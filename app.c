/* KIST — the app: the FM-1's panel and screen around the organ engine (dsp/organ.c).
 *
 * Panel: the top row of buttons draws the stops (Gedackt, Rohrflöte, Principal: bass | treble), ARP the treble
 * Quint, SEQ the tremulant, PLAY the blower, REC cancels every stop (again: back), SAVE keeps the setup for the
 * next power-on, HOME the Console. SELECT turns the pages, PRESETS the room's mix, ALGORITHM the temperament
 * (from any page), K1..K4 what the page's footer says. OCT- / OCT+ move the FM-1's own keys; both: back to 0.
 * Part of the unity build (device/unity.c, host/plat_host.c), after the platform: gfx.c and announce.c are in scope. */
#include "fastmath.h"
#include "organ.h"

#define SET_MAGIC 0x4B495331u            /* "KIS1" */
#define SET_VERSION 1u
typedef struct {
    uint32_t magic;
    uint16_t version, size;
    organ_params_t par;
    organ_voicing_t voi;
    uint8_t halves;                      /* the stops drawn: HALF_BIT */
    uint8_t trem;
    uint8_t midi_ch;                     /* 0 omni, 1..16 */
    int8_t midi_transpose;               /* -12..+12 semitones */
    int8_t octave;                       /* the FM-1's keys: -2..+1 */
    uint8_t rsv[3];
} settings_t;
static settings_t set;
static uint8_t set_dirty;

enum { PG_CONSOLE, PG_TUNING, PG_ROOM, PG_WIND, PG_VOICING, PG_SYSTEM, NPAGES };
static const char *const PAGE_NAME[NPAGES] = {"CONSOLE", "TUNING", "ROOM", "WIND & TOUCH", "VOICING", "SYSTEM"};
static const char *const STOP_SHORT[NSTOPS] = {"GED 8'", "ROHR 4'", "PRIN 2'", "QUINT"};
static const char *const SYS_SHORT[NTS] = {"EDO", "WERCKM. III", "1/4-COMMA", "KIRNB. III", "VALLOTTI"};
static const char *const PC_NAME[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "G#", "A", "Bb", "B"};
/* the stop halves on the panel, by button: FX SEL ENV LFO EDIT GLO, and ARP (the Quint's treble) */
static const uint8_t BTN_HALF[NB] = {0, 1, 2, 3, 4, 5, 0xFF, 0xFF, 7, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static uint32_t flash_id;                /* the flash part (0x856014 the older FM-1s, 0xB36014 the newer) */
static int cpu2_started;

/* --------------------------------------------------------------------------------------- the engine --- */
static void engine_all(void)
{
    organ_set_params(&set.par);
    organ_set_voicing(&set.voi);
    organ_set_stops(set.halves);
    organ_set_tremulant(set.trem);
}

static volatile uint32_t midi_count;
static void midi_in(void)                /* audio interrupt: what came in over USB and the TRS jack */
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = pkt >> 24, ch = st & 0x0Fu;
        int key;
        if (st < 0x80u || st >= 0xF0u)
            continue;
        if (set.midi_ch && ch != (uint32_t)(set.midi_ch - 1u))
            continue;
        midi_count++;
        key = (int)d1 + set.midi_transpose;
        switch (st & 0xF0u) {
        case 0x90u:
            if (d2)
                organ_note_on_isr(key, (int)d2);
            else
                organ_note_off_isr(key);
            break;
        case 0x80u:
            organ_note_off_isr(key);
            break;
        case 0xB0u:                      /* all notes off, all sound off; the sustain pedal is ignored (an organ) */
            if (d1 == 123u || d1 == 120u)
                organ_all_off_isr();
            break;
        default:
            break;
        }
    }
}

static float out_l[APP_BLOCK], out_r[APP_BLOCK];
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    static float gain_now;
    float gain = (float)master_q12 / 4096.0f * 7000000.0f;
    uint32_t i;
    midi_in();
    organ_render(out_l, out_r, n);
    for (i = 0; i < n; i++) {
        gain_now += (gain - gain_now) * 0.004f;   /* MASTER glides: no step at a block's edge */
        lr[2 * i] = (int32_t)(fm_tanhf(out_l[i] * 0.7f) * gain_now);
        lr[2 * i + 1] = (int32_t)(fm_tanhf(out_r[i] * 0.7f) * gain_now);
    }
}

void app_panic(void) { organ_panic(); }
void app_overload(void) { organ_shed(); }

/* -------------------------------------------------------------------------------------------- setup --- */
static void settings_defaults(void)
{
    int s, k;
    memset(&set, 0, sizeof set);
    set.magic = SET_MAGIC;
    set.version = SET_VERSION;
    set.size = sizeof set;
    set.par.pitch_hz = 440;
    set.par.system = TS_EQUAL;
    set.par.edo = 12;
    set.par.split = 60;
    set.par.touch = 2;
    set.par.steadiness = 1;
    set.par.blower_noise = 3;
    set.par.trem_speed = 52;
    set.par.trem_depth = 0;
    set.par.room = ROOM_CHAPEL;
    set.par.mix = organ_room_mix(ROOM_CHAPEL);
    set.par.distance = 6;
    set.par.decay_ds = organ_room_decay_ds(ROOM_CHAPEL);
    for (s = 0; s < NSTOPS; s++) {
        for (k = 0; k < KIST_NKEYS; k++)
            set.voi.level_half_db[s][k] = 0;
        set.voi.chiff[s] = 2;
        set.voi.drift_dc[s] = 15;
    }
    set.halves = (uint8_t)(HALF_BIT(ST_GEDACKT, 0) | HALF_BIT(ST_GEDACKT, 1));
}

static int settings_ok(void)
{
    const organ_params_t *p = &set.par;
    return set.magic == SET_MAGIC && set.version == SET_VERSION && set.size == sizeof set &&
           p->pitch_hz >= 300 && p->pitch_hz <= 500 && p->pitch_cent <= 99 && p->system < NTS && p->edo >= 5 &&
           p->edo <= 72 && p->root < 12 && p->split >= 48 && p->split <= 72 && p->touch <= 4 && p->pressure >= -10 &&
           p->pressure <= 10 && p->steadiness <= 2 && p->blower_noise <= 10 && p->trem_speed >= 30 &&
           p->trem_speed <= 80 && p->trem_depth <= 2 && p->room < NROOMS && p->mix <= 100 && p->distance >= 1 &&
           p->distance <= 20 && p->decay_ds >= 3 && p->decay_ds <= 80 && set.midi_ch <= 16 &&
           set.midi_transpose >= -12 && set.midi_transpose <= 12 && set.octave >= -2 && set.octave <= 1;
}

static uint32_t read_flash_id(void)
{
#ifdef PLAT_HOST
    return 0x856014u;
#else
    uint32_t f = irq_save(), id = FL_FAR(fl_jedec_ram)();
    irq_restore(f);
    return id;
#endif
}

void app_init(void)
{
    flash_id = read_flash_id();          /* before the second core starts: nothing else is reading the flash */
    organ_init();
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set || !settings_ok())
        settings_defaults();
    engine_all();
    cpu2_started = plat_cpu2_set(1) == 0;   /* the 2x top pipes and the richer room, where the unit allows */
}

static uint8_t backup_hold;              /* a backup streaming: SAVE waits */

/* ------------------------------------------------------------------------------------------- text --- */
static void text_cat(char *d, const char *s)
{
    while (*d)
        d++;
    while (*s)
        *d++ = *s++;
    *d = 0;
}
static void int_text(int32_t v, char *b)
{
    char t[12];
    int n = 0, neg = v < 0;
    uint32_t u = neg ? (uint32_t)-v : (uint32_t)v;
    do {
        t[n++] = (char)('0' + u % 10u);
        u /= 10u;
    } while (u);
    if (neg)
        *b++ = '-';
    while (n)
        *b++ = t[--n];
    *b = 0;
}
static void signed_text(int32_t v, char *b)       /* +3, 0, -2 */
{
    if (v > 0)
        *b++ = '+';
    int_text(v, b);
}
static void dec_text(int32_t v10, char *b)        /* tenths: 1.6 */
{
    char t[12];
    int_text(v10 / 10, b);
    text_cat(b, ".");
    int_text(v10 % 10 < 0 ? -(v10 % 10) : v10 % 10, t);
    text_cat(b, t);
}
static void pitch_text(char *b)                   /* 415.37 */
{
    char t[8];
    int_text(set.par.pitch_hz, b);
    text_cat(b, ".");
    t[0] = (char)('0' + set.par.pitch_cent / 10u);
    t[1] = (char)('0' + set.par.pitch_cent % 10u);
    t[2] = 0;
    text_cat(b, t);
}
static void note_text(int key, char *b)           /* c', f''' (Helmholtz, as organists name the compass) */
{
    static const char *const N[12] = {"c", "c#", "d", "d#", "e", "f", "f#", "g", "g#", "a", "a#", "b"};
    int oct = key / 12 - 4, i;
    b[0] = 0;
    if (oct < 0) {                       /* C (36) .. B (47): capitals */
        b[0] = (char)(N[key % 12][0] - 32);
        b[1] = N[key % 12][1] == '#' ? '#' : 0;
        b[2] = 0;
        return;
    }
    text_cat(b, N[key % 12]);
    for (i = 0; i < oct; i++)
        text_cat(b, "'");
}
static void system_text(char *b, int sys)         /* "31-EDO" or "1/4-COMMA" */
{
    b[0] = 0;
    if (sys == TS_EQUAL) {
        int_text(set.par.edo, b);
        text_cat(b, "-EDO");
    } else {
        text_cat(b, SYS_SHORT[sys]);
    }
}

/* ------------------------------------------------------------------------------------------- the UI --- */
static struct {
    int page;
    char msg[2][28];
    uint32_t msg_until;
    uint32_t keys, btn;
    uint8_t undo_halves;
    int sys_pending;                     /* a temperament turned to and not yet applied: -1 none */
    uint32_t sys_turned, overlay_until;
    int voice_stop, voice_key;           /* the VOICING page's stop and note */
    int blower;
    uint32_t catch_until;                /* the pitch knob holds at a common pitch for a moment */
    uint8_t trace[60];                   /* the reservoir's pressure, the last 6 s */
    uint32_t trace_t;
    uint32_t meter_t, lit_t;
    uint8_t redraw;
} ui;

void app_say(const char *a, const char *b)
{
    uint32_t i;
    char t[64];
    for (i = 0; i < 27u && a && a[i]; i++)
        ui.msg[0][i] = a[i];
    ui.msg[0][i] = 0;
    for (i = 0; i < 27u && b && b[i]; i++)
        ui.msg[1][i] = b[i];
    ui.msg[1][i] = 0;
    ui.msg_until = plat_ms() + 1300u;
    ui.redraw = 1;
    if (ann_on()) {
        t[0] = 0;
        text_cat(t, ui.msg[0]);
        if (ui.msg[1][0]) {
            text_cat(t, " ");
            text_cat(t, ui.msg[1]);
        }
        ann_say(t, 0, ANN_NOW);
    }
}

static void changed(void)
{
    set_dirty = 1;
    ui.redraw = 1;
}

static void params_changed(void)
{
    organ_set_params(&set.par);
    changed();
}

/* the pitch: K1 whole hertz (a quick turn moves 10 at a time and holds a moment at 392, 415, 432, 440 and 465),
 * K2 hundredths, carried into the hertz */
static void pitch_turn(int32_t e, int hundredths)
{
    static const int CATCH[5] = {392, 415, 432, 440, 465};
    int32_t v = (int32_t)set.par.pitch_hz * 100 + set.par.pitch_cent, nv, i, now = (int32_t)plat_ms();
    if (hundredths) {
        nv = v + e;
    } else {
        int32_t step = e > 2 || e < -2 ? 10 : 1;
        if ((int32_t)(ui.catch_until - (uint32_t)now) > 0)
            return;                      /* held at a common pitch */
        nv = v + e * step * 100;
        for (i = 0; i < 5; i++) {        /* passing a common pitch: stop on it */
            if ((v / 100 < CATCH[i] && nv / 100 > CATCH[i]) || (v / 100 > CATCH[i] && nv / 100 < CATCH[i]) ||
                (nv / 100 == CATCH[i] && v / 100 != CATCH[i])) {
                nv = CATCH[i] * 100 + (int32_t)set.par.pitch_cent;
                ui.catch_until = (uint32_t)now + 350u;
                break;
            }
        }
    }
    nv = clamp(nv, 30000, 50099);
    set.par.pitch_hz = (uint16_t)(nv / 100);
    set.par.pitch_cent = (uint8_t)(nv % 100);
    params_changed();
}

/* a temperament turned to: shown at once, applied when the knob has rested 0.6 s */
static void system_turn(int32_t e)
{
    int s = ui.sys_pending >= 0 ? ui.sys_pending : set.par.system;
    s = ((s + (int)e) % NTS + NTS) % NTS;
    ui.sys_pending = s;
    ui.sys_turned = plat_ms();
    ui.redraw = 1;
}

static void room_turn(int32_t e)
{
    int r = ((int)set.par.room + (int)e) % NROOMS;
    if (r < 0)
        r += NROOMS;
    set.par.room = (uint8_t)r;
    set.par.decay_ds = organ_room_decay_ds(r);   /* a new room brings its own decay */
    params_changed();
    app_say("ROOM", organ_room_name(r));
}

static void knob(int k, int32_t e)
{
    organ_params_t *p = &set.par;
    switch (ui.page) {
    case PG_CONSOLE:
        if (k == 0)
            room_turn(e);
        else if (k == 1) {
            p->mix = (uint8_t)clamp((int32_t)p->mix + e, 0, 100);
            params_changed();
        } else if (k == 2)
            pitch_turn(e, 0);
        else {
            p->touch = (uint8_t)clamp((int32_t)p->touch + e, 0, 4);
            params_changed();
        }
        break;
    case PG_TUNING:
        if (k == 0)
            pitch_turn(e, 0);
        else if (k == 1)
            pitch_turn(e, 1);
        else if (k == 2)
            system_turn(e);
        else if (p->system == TS_EQUAL) {
            p->edo = (uint8_t)clamp((int32_t)p->edo + e, 5, 72);
            params_changed();
        } else {
            p->root = (uint8_t)(((int32_t)p->root + e % 12 + 12) % 12);
            params_changed();
        }
        break;
    case PG_ROOM:
        if (k == 0)
            room_turn(e);
        else if (k == 1)
            p->blower_noise = (uint8_t)clamp((int32_t)p->blower_noise + e, 0, 10);
        else if (k == 2)
            p->distance = (uint8_t)clamp((int32_t)p->distance + e, 1, 20);
        else
            p->decay_ds = (uint8_t)clamp((int32_t)p->decay_ds + e, 3, 80);
        if (k)
            params_changed();
        break;
    case PG_WIND:
        if (k == 0)
            p->pressure = (int8_t)clamp((int32_t)p->pressure + e, -10, 10);
        else if (k == 1)
            p->steadiness = (uint8_t)clamp((int32_t)p->steadiness + e, 0, 2);
        else if (k == 2)
            p->trem_speed = (uint8_t)clamp((int32_t)p->trem_speed + e, 30, 80);
        else
            p->trem_depth = (uint8_t)clamp((int32_t)p->trem_depth + e, 0, 2);
        params_changed();
        break;
    case PG_VOICING:
        if (k == 0) {
            ui.voice_stop = ((ui.voice_stop + (int)e) % NSTOPS + NSTOPS) % NSTOPS;
            if (ui.voice_stop == ST_QUINT && ui.voice_key < set.par.split)
                ui.voice_key = set.par.split;
            ui.redraw = 1;
        } else {
            if (k == 1) {
                int8_t *lv = &set.voi.level_half_db[ui.voice_stop][ui.voice_key - KIST_KEY_LO];
                *lv = (int8_t)clamp((int32_t)*lv + e, -24, 24);
            } else if (k == 2)
                set.voi.chiff[ui.voice_stop] = (uint8_t)clamp((int32_t)set.voi.chiff[ui.voice_stop] + e, 0, 4);
            else
                set.voi.drift_dc[ui.voice_stop] = (uint8_t)clamp((int32_t)set.voi.drift_dc[ui.voice_stop] + e, 0, 50);
            organ_set_voicing(&set.voi);
            changed();
        }
        break;
    case PG_SYSTEM:
        if (k == 0)
            set.midi_ch = (uint8_t)clamp((int32_t)set.midi_ch + e, 0, 16);
        else if (k == 1) {
            p->split = (uint8_t)clamp((int32_t)p->split + e, 48, 72);
            organ_set_params(p);
            organ_set_stops(set.halves);
        } else if (k == 2)
            set.midi_transpose = (int8_t)clamp((int32_t)set.midi_transpose + e, -12, 12);
        changed();
        break;
    }
}

static int fm1_note(uint32_t k) { return 53 + (int)k + 12 * set.octave; }   /* key 0 is F3 */

static void octave_step(int32_t d)
{
    char t[8];
    set.octave = (int8_t)clamp((int32_t)set.octave + d, -2, 1);
    signed_text(set.octave, t);
    app_say("OCTAVE", t);
    changed();
}

static void stop_button(int half)
{
    int stop = half / 2, treble = half & 1;
    uint8_t bit = (uint8_t)(1u << half);
    char t[24];
    set.halves ^= bit;
    organ_set_stops(set.halves);
    t[0] = 0;
    text_cat(t, STOP_SHORT[stop]);
    text_cat(t, stop == ST_QUINT ? "" : treble ? " T" : " B");
    app_say(t, set.halves & bit ? "ON" : "OFF");
    changed();
}

static void input(void)
{
    uint32_t keys, btn = plat_buttons(), k, now = plat_ms();
    int32_t e;
    touch_poll();
    keys = touch_keys();
    for (k = 0; k < NKEYS; k++) {
        uint32_t bit = 1u << k;
        int note = fm1_note(k);
        if ((keys & bit) && !(ui.keys & bit)) {
            uint32_t v = touch_vel(k);
            organ_note_on(note, (int)v);
            plat_midi_send(0x09u | 0x90u << 8 | (uint32_t)note << 16 | v << 24, TO_USB);
            if (ui.page == PG_VOICING && note >= KIST_KEY_LO && note <= KIST_KEY_HI &&
                !(ui.voice_stop == ST_QUINT && note < set.par.split)) {
                ui.voice_key = note;
                ui.redraw = 1;
            }
        } else if (!(keys & bit) && (ui.keys & bit)) {
            organ_note_off(note);
            plat_midi_send(0x08u | 0x80u << 8 | (uint32_t)note << 16, TO_USB);
        }
    }
    for (k = 0; k < NB; k++) {
        uint32_t bit = 1u << k;
        if (!(btn & bit) || (ui.btn & bit))
            continue;
        if (BTN_HALF[k] != 0xFF) {
            stop_button(BTN_HALF[k]);
        } else if (k == B_OCTDN || k == B_OCTUP) {
            if (btn >> (k == B_OCTDN ? B_OCTUP : B_OCTDN) & 1u) {
                set.octave = 0;          /* both: back to 0 */
                app_say("OCTAVE", "0");
                changed();
            } else {
                octave_step(k == B_OCTDN ? -1 : 1);
            }
        } else if (k == B_HOME) {
            ui.page = PG_CONSOLE;
            ui.overlay_until = 0;
            ui.redraw = 1;
            app_say(PAGE_NAME[ui.page], "");
        } else if (k == B_SAVE) {
            int r = backup_hold ? -1 : plat_store_save(OBJ_SETTINGS, &set, sizeof set);
            if (r == 0) {
                set_dirty = 0;
                app_say("SAVED", "for power-on");
            } else {
                app_say("NOT SAVED", r == -9 ? "flash unknown" : backup_hold ? "backup running" : "store full");
            }
        } else if (k == B_SEQ) {
            set.trem ^= 1u;
            organ_set_tremulant(set.trem);
            app_say("TREMULANT", set.trem ? "ON" : "OFF");
            changed();
        } else if (k == B_PLAY) {
            ui.blower ^= 1;
            organ_set_blower(ui.blower);
            app_say("BLOWER", ui.blower ? "ON" : "OFF");
        } else if (k == B_REC) {
            if (set.halves) {            /* cancel: every stop in, kept for the undo */
                ui.undo_halves = set.halves;
                set.halves = 0;
                app_say("CANCEL", "all stops off");
            } else if (ui.undo_halves) {
                set.halves = ui.undo_halves;
                app_say("CANCEL", "undone");
            }
            organ_set_stops(set.halves);
            changed();
        }
    }
    for (k = 0; k < NE; k++) {
        if ((e = plat_enc((int)k)) == 0)
            continue;
        if (k == EN_SELECT) {
            ui.page = ((ui.page + (int)e) % NPAGES + NPAGES) % NPAGES;
            ui.redraw = 1;
            app_say(PAGE_NAME[ui.page], "");
        } else if (k == EN_PRESET) {
            char t[8];
            set.par.mix = (uint8_t)clamp((int32_t)set.par.mix + e, 0, 100);
            params_changed();
            int_text(set.par.mix, t);
            text_cat(t, "%");
            app_say("ROOM MIX", t);
        } else if (k == EN_ALGO) {
            system_turn(e);
            ui.overlay_until = now + 2000u;
        } else {
            knob((int)(k - EN_K1), e);
        }
    }
    if (ui.sys_pending >= 0 && now - ui.sys_turned >= 600u) {   /* the temperament, once the knob rests */
        char t[24];
        set.par.system = (uint8_t)ui.sys_pending;
        ui.sys_pending = -1;
        params_changed();
        system_text(t, set.par.system);
        app_say("TUNING", t);
    }
    if (keys != ui.keys || btn != ui.btn)
        ui.redraw = 1;
    ui.keys = keys;
    ui.btn = btn;
    {   /* the lights: stops drawn, the tremulant, SAVE while unsaved; the keys while their note sounds */
        const organ_meter_t *m = organ_meter();
        uint32_t lit = 0, kl = 0, b;
        for (b = 0; b < NB; b++)
            if (BTN_HALF[b] != 0xFF && (set.halves >> BTN_HALF[b] & 1u))
                lit |= 1u << b;
        if (set.trem)
            lit |= 1u << B_SEQ;
        if (set_dirty)
            lit |= 1u << B_SAVE;
        if (ui.blower)
            lit |= LED_PLAY_GREEN;
        for (b = 0; b < NKEYS; b++) {
            int n = fm1_note(b) - KIST_KEY_LO;
            if (n >= 0 && n < KIST_NKEYS && ((n < 32 ? m->sounding_lo >> n : m->sounding_hi >> (n - 32)) & 1u))
                kl |= 1u << b;
        }
        plat_glow(1);
        plat_leds(lit, kl);
    }
}

void app_input_only(void) { input(); }

/* -------------------------------------------------------------------------------------------- drawing --- */
/* the words the engraved screens use (ui/engraving.c: its fonts map ^ to a middle dot, ` to two-thirds) */
static const char *const SYS_SHORT_B[NTS] = {"equal", "Werckmeister", "Meantone", "Kirnberger", "Vallotti"};
static const char *const SYS_LONG_B[NTS] = {"Equal", "Werckmeister III", "the Meantone", "Kirnberger III", "Vallotti"};
static const char *const CIRCUL_NOTE[NTS] = {"each fifth 1/12 comma narrow", "four fifths 1/4 comma narrow",
                                             "each fifth 1/4 comma narrow", "four fifths 1/4, one a schisma",
                                             "six fifths 1/6 comma narrow"};
static const char *const TOUCH_B[5] = {"soft", "gentle", "moderate", "firm", "crisp"};
static const char *const STEADY_B[3] = {"steady", "lively", "loose"};
static const char *const DEPTH_B[3] = {"gentle", "moderate", "deep"};
#include "ui/engraving.c"

/* a screen reader that has just started listening (or asks) hears the page */
static void speak(void)
{
    if (ann_on() && ann_where()) {
        char t[96], u[16];
        t[0] = 0;
        text_cat(t, PAGE_NAME[ui.page]);
        text_cat(t, ". A ");
        pitch_text(u);
        text_cat(t, u);
        text_cat(t, ". ");
        system_text(u, set.par.system);
        text_cat(t, u);
        ann_say(t, 0, ANN_NOW);
    }
    ann_tick(plat_ms());
}

void app_ui_init(void)
{
    touch_on(1);
    ui.sys_pending = -1;
    ui.voice_key = 60;
    ui.blower = 1;
    organ_set_blower(1);
    lcd_fill(0, 0, 240, 240, P_PAPER);
    ui.redraw = 1;
}

void app_frame(void)
{
    static uint32_t last_notes, last_pipes, last_msg;
    const organ_meter_t *m = organ_meter();
    uint32_t now = plat_ms();
    input();
    speak();
    if (now / 100u != ui.trace_t / 100u) {   /* the reservoir's trace, 10 a second: every slot since the last */
        uint32_t k = ui.trace_t / 100u, n = 0;
        uint8_t v = (uint8_t)clamp((int32_t)(m->pressure * 100.0f), 0, 255);
        while (k != now / 100u && n++ < 60u)
            ui.trace[++k % 60u] = v;
        ui.trace_t = now;
        if (ui.page == PG_WIND)
            ui.redraw = 1;
    }
    if (now - ui.meter_t >= 250u) {      /* the counts on the CONSOLE and SYSTEM pages */
        ui.meter_t = now;
        if ((ui.page == PG_CONSOLE && (m->pipes != last_pipes || m->notes != last_notes)) || ui.page == PG_SYSTEM)
            ui.redraw = 1;
        last_pipes = m->pipes;
        last_notes = m->notes;
    }
    if ((ui.msg_until && now >= ui.msg_until) || (ui.overlay_until && now >= ui.overlay_until)) {
        if (now >= ui.msg_until)
            ui.msg_until = 0;
        if (now >= ui.overlay_until)
            ui.overlay_until = 0;
        ui.redraw = 1;
    }
    if (ui.sys_pending >= 0 && now - last_msg > 50u) {
        last_msg = now;
        ui.redraw = 1;
    }
    if (ui.redraw) {
        ui.redraw = 0;
        draw();
    }
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    const organ_meter_t *m = organ_meter();
    if (!strcmp(what, "page"))
        *got = ui.page;
    else if (!strcmp(what, "halves"))
        *got = set.halves;
    else if (!strcmp(what, "pitch"))     /* hundredths of a hertz */
        *got = set.par.pitch_hz * 100 + set.par.pitch_cent;
    else if (!strcmp(what, "system"))
        *got = set.par.system;
    else if (!strcmp(what, "edo"))
        *got = set.par.edo;
    else if (!strcmp(what, "room"))
        *got = set.par.room;
    else if (!strcmp(what, "mix"))
        *got = set.par.mix;
    else if (!strcmp(what, "trem"))
        *got = set.trem;
    else if (!strcmp(what, "blower"))
        *got = ui.blower;
    else if (!strcmp(what, "octave"))
        *got = set.octave;
    else if (!strcmp(what, "dirty"))
        *got = set_dirty;
    else if (!strcmp(what, "notes"))
        *got = (int)m->notes;
    else if (!strcmp(what, "pipes"))
        *got = (int)m->pipes;
    else if (!strcmp(what, "pressure"))  /* percent */
        *got = (int)(m->pressure * 100.0f + 0.5f);
    else
        return 0;
    return 1;
}
#endif

/* backup and restore over USB (fm1-base device/backup.c): the setup SAVE stored, nothing else */
int app_backup_playing(void) { return 0; }            /* a restore only swaps the setup: fine while playing */
void app_backup_flush(void) {}                          /* the setup is saved by SAVE alone: a backup takes that */
void app_backup_hold(int on) { backup_hold = (uint8_t)on; }
void app_backup_restored(void)
{
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set || !settings_ok())
        settings_defaults();
    engine_all();
    set_dirty = 0;
    ui.redraw = 1;
}
