/* KIST — the screens, drawn as a Baroque engraving: ink on paper, red rubrication, Fraktur titles, IM Fell text.
 * Included by app.c (the unity build: gfx.c's canvas, cv_rect / cv_line / cv_text, is in scope). The screen is drawn
 * in two halves of 120 rows; every routine here takes whole-screen coordinates. */
#include "kist_fonts.h"

#define P_PAPER RGB(238, 231, 214)
#define P_INK RGB(30, 26, 22)
#define P_RED RGB(158, 42, 31)
#define P_MID RGB(107, 98, 82)                 /* secondary text */
#define P_RULE RGB(184, 174, 152)              /* light rules under rows */

/* ------------------------------------------------------------------------------------------ primitives --- */
static void hline(int32_t x, int32_t y, int32_t w, uint16_t c) { cv_rect(x, y, w, 1, c); }
static void vline(int32_t x, int32_t y, int32_t h, uint16_t c) { cv_rect(x, y, 1, h, c); }
static void dotted(int32_t x, int32_t y, int32_t w, int32_t step, uint16_t c)
{
    int32_t i;
    for (i = 0; i < w; i += step)
        cv_pset(x + i, y, c);
}
static void frame(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    hline(x, y, w, c);
    hline(x, y + h - 1, w, c);
    vline(x, y, h, c);
    vline(x + w - 1, y, h, c);
}

/* a circle's outline (midpoint), every pixel or, dotted, every other */
static void circle(int32_t cx, int32_t cy, int32_t r, uint16_t c, int dots)
{
    int32_t x = r, y = 0, e = 1 - r, n = 0;
    while (x >= y) {
        if (!dots || (n++ & 1)) {
            cv_pset(cx + x, cy + y, c); cv_pset(cx - x, cy + y, c); cv_pset(cx + x, cy - y, c); cv_pset(cx - x, cy - y, c);
            cv_pset(cx + y, cy + x, c); cv_pset(cx - y, cy + x, c); cv_pset(cx + y, cy - x, c); cv_pset(cx - y, cy - x, c);
        }
        y++;
        if (e < 0) {
            e += 2 * y + 1;
        } else {
            x--;
            e += 2 * (y - x) + 1;
        }
    }
}

/* a filled circle's engraved shading: horizontal lines every other row, inside radius r */
static void circle_hatch(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    int32_t y;
    for (y = -r + 1; y < r; y += 2) {
        int32_t w = (int32_t)fm_sqrtf((float)(r * r - y * y));
        hline(cx - w, cy + y, 2 * w + 1, c);
    }
}

/* an arc of radius r..r+t-1 from angle a0 to a1 (degrees, clockwise from the top) */
static void arc(int32_t cx, int32_t cy, int32_t r, int32_t t, float a0, float a1, uint16_t c)
{
    int32_t k;
    for (k = 0; k < t; k++) {
        float rr = (float)(r + k), step = 57.29578f / rr * 0.7f, a;
        for (a = a0; a <= a1; a += step) {
            float rad = (a - 90.0f) * 0.01745329f;
            cv_pset(cx + (int32_t)(rr * fm_cosf(rad) + 0.5f), cy + (int32_t)(rr * fm_sinf(rad) + 0.5f), c);
        }
    }
}

static int32_t text_c(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)   /* centred */
{
    return cv_text(cx - text_w(f, s) / 2, y, f, s, c);
}
static void text_r(int32_t rx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)      /* right-aligned */
{
    cv_text(rx - text_w(f, s), y, f, s, c);
}

static void roman(uint32_t v, char *b)        /* lower-case Roman numerals, 0 as "none" */
{
    static const uint16_t V[] = {100, 90, 50, 40, 10, 9, 5, 4, 1};
    static const char *const S[] = {"c", "xc", "l", "xl", "x", "ix", "v", "iv", "i"};
    uint32_t i;
    b[0] = 0;
    if (!v) {
        text_cat(b, "none");
        return;
    }
    for (i = 0; i < 9u; i++)
        while (v >= V[i]) {
            text_cat(b, S[i]);
            v -= V[i];
        }
}

/* ---------------------------------------------------------------------------------------------- the sheet --- */
/* the border: a double rule with a flourish in each corner */
static void sheet(void)
{
    static const int8_t SX[4] = {1, -1, 1, -1}, SY[4] = {1, 1, -1, -1};
    int k;
    cv_rect(4, 4, 232, 1, P_INK);
    cv_rect(4, 5, 232, 1, P_INK);
    cv_rect(4, 234, 232, 1, P_INK);
    cv_rect(4, 235, 232, 1, P_INK);
    cv_rect(4, 4, 1, 232, P_INK);
    cv_rect(5, 4, 1, 232, P_INK);
    cv_rect(234, 4, 1, 232, P_INK);
    cv_rect(235, 4, 1, 232, P_INK);
    frame(8, 8, 224, 224, P_INK);
    for (k = 0; k < 4; k++) {              /* two quarter arcs and a small ring, turned into each corner */
        int32_t ox = SX[k] > 0 ? 10 : 229, oy = SY[k] > 0 ? 10 : 229, i;
        for (i = 0; i <= 12; i++) {
            float a = 1.5708f * (float)i / 12.0f, s = fm_sinf(a), c = fm_cosf(a);
            cv_pset(ox + SX[k] * (int32_t)(7.0f - 7.0f * c + 0.5f), oy + SY[k] * (int32_t)(7.0f - 7.0f * s + 0.5f), P_INK);
            cv_pset(ox + SX[k] * (int32_t)(2.0f + 9.0f - 9.0f * c + 0.5f), oy + SY[k] * (int32_t)(2.0f + 9.0f - 9.0f * s + 0.5f), P_INK);
        }
        circle(ox + SX[k] * 7, oy + SY[k] * 7, 1, P_INK, 0);
    }
}

/* the title in Fraktur (its first letter red) between double hairlines, and the caption under it: the page's own,
 * or for a moment the last thing done. The type keeps the old scale on the organ's intervals (tools/gen_kist_fonts.py):
 * labels 12, captions and lists 16, values 18, titles 24. The page's own part and the knobs' part below it stand
 * about as 4 : 3. */
#ifndef KIST_CLOCK
#define KIST_CLOCK 1                           /* the Tuning page as a clock face (0: the Quinten-Circul) */
#endif
#define Y0 54                                  /* the page's own rows begin here */
#define FOOT_Y 155                             /* the knobs: two rows of a label over its value */
static void title(const char *t, const char *caption)
{
    char first[2] = {t[0], 0};
    int32_t w = text_w(&KF_TITLE, t), x = 120 - w / 2;
    hline(16, 20, x - 22, P_INK);
    hline(16, 22, x - 22, P_INK);
    hline(x + w + 6, 20, 224 - (x + w + 6), P_INK);
    hline(x + w + 6, 22, 224 - (x + w + 6), P_INK);
    x = cv_text(x, 7, &KF_TITLE, first, P_RED);
    cv_text(x, 7, &KF_TITLE, t + 1, P_INK);
    if (plat_ms() < ui.msg_until) {
        char m[56];
        m[0] = 0;
        text_cat(m, ui.msg[0]);
        if (ui.msg[1][0]) {
            text_cat(m, " ^ ");
            text_cat(m, ui.msg[1]);
        }
        {                                   /* the panel's words are capitals: here in the hand's small letters */
            int i, cap = 1;
            for (i = 0; m[i]; i++) {
                char c = m[i];
                if (c >= 'A' && c <= 'Z' && !cap && !(i && m[i - 1] >= '0' && m[i - 1] <= '9'))
                    m[i] = (char)(c + 32);
                cap = (c == '^');
                if (c != ' ' && c != '^')
                    cap = 0;
                if (c == '^')
                    cap = 1;
            }
        }
        text_c(120, 33, &KF_ITAL, m, P_RED);
    } else {
        text_c(120, 33, &KF_ITAL, caption, P_INK);
    }
}

/* the four knobs at the foot, as a table: the numeral and the name small, in red; the value under them, clear */
static void foot(int k, const char *label, const char *value)
{
    static const char *const N[4] = {"i", "ii", "iii", "iv"};
    int32_t x = (k & 1) ? 128 : 22, rx = (k & 1) ? 218 : 114, y = FOOT_Y + (k >> 1) * 37;
    const felucca_font_t *f = &KF_NAME;
    x = cv_text(x, y, &KF_SC, N[k], P_RED) + 4;
    cv_text(x, y, &KF_SC, label, P_RED);
    if (text_w(f, value) > rx - x)
        f = &KF_VITAL;                      /* narrower, the same size */
    if (text_w(f, value) > rx - x)
        f = &KF_ITAL;
    cv_text(x, y + 12, f, value, P_INK);
}
static void foot_rules(void)
{
    hline(12, 150, 216, P_INK);
    hline(12, 151, 216, P_INK);
    hline(20, 190, 200, P_RULE);
    vline(120, 154, 70, P_RULE);
}

static const char *const PAGE_TITLE[NPAGES] = {"Con_ole", "Tuning", "The Room", "The Wind", "Intonation", "The Works"};
static const char *const STOP_DE[NSTOPS] = {"Gedackt", "Rohrfl{te", "Principal", "Quinta"};
static const char *const STOP_FUSS[NSTOPS] = {"8 Fu~", "4 Fu~", "2 Fu~", "2` Fu~"};

static const char *caption(const char *what)     /* "what ^ iv" */
{
    static char b[64];
    char r[8];
    b[0] = 0;
    text_cat(b, what);
    text_cat(b, " ^ ");
    roman((uint32_t)ui.page + 1u, r);
    text_cat(b, r);
    return b;
}

static void pitch_dot(char *b)                    /* 415^37 */
{
    char t[4];
    int_text(set.par.pitch_hz, b);
    text_cat(b, "^");
    t[0] = (char)('0' + set.par.pitch_cent / 10u);
    t[1] = (char)('0' + set.par.pitch_cent % 10u);
    t[2] = 0;
    text_cat(b, t);
}

static void dots_to_mid(char *t)                 /* 2.4 -> 2^4 */
{
    for (; *t; t++)
        if (*t == '.')
            *t = '^';
}

/* ------------------------------------------------------------------------------------------------ pages --- */
static void knob_head(int32_t cx, int32_t cy, int drawn, int exists)
{
    if (!exists) {
        circle(cx, cy, 9, P_MID, 1);
        return;
    }
    if (drawn)
        circle_hatch(cx, cy, 9, P_INK);
    circle(cx, cy, 9, P_INK, 0);
    circle(cx, cy, 10, P_INK, 0);
    circle(cx, cy, 5, drawn ? P_PAPER : P_INK, 0);
}

static void draw_console(void)
{
    char t[64];
    int s;
    t[0] = 0;
    text_cat(t, ui.blower ? "the wind well supplied" : "the wind wanting");
    if (set.trem)
        text_cat(t, ", trembling");
    title(PAGE_TITLE[0], caption(t));
    cv_text(14, Y0, &KF_SC, "Register", P_RED);
    text_c(164, Y0, &KF_SC, "Bass", P_RED);
    text_c(206, Y0, &KF_SC, "Treble", P_RED);
    for (s = 0; s < NSTOPS; s++) {
        int32_t y = Y0 + 14 + s * 20, x;
        x = cv_text(14, y, &KF_NAME, STOP_DE[s], P_INK);
        cv_text(x + 5, y + 2, &KF_ITAL, STOP_FUSS[s], P_INK);
        knob_head(164, y + 10, set.halves >> (2 * s) & 1u, s != ST_QUINT);
        knob_head(206, y + 10, set.halves >> (2 * s + 1) & 1u, 1);
        if (s < NSTOPS - 1)
            hline(12, y + 19, 216, P_RULE);
    }
    foot_rules();
    foot(0, "Room", organ_room_name(set.par.room));
    int_text(set.par.mix, t);
    text_cat(t, " in 100");
    foot(1, "its share", t);
    pitch_dot(t);
    text_cat(t, " Hz");
    foot(2, "Pitch", t);
    foot(3, "Touch", TOUCH_B[set.par.touch]);
}

static void draw_tuning(void)
{
    char t[64], u[24];
    int sys = ui.sys_pending >= 0 ? ui.sys_pending : set.par.system;
    if (sys == TS_EQUAL && set.par.edo != 12) {
        int32_t step100 = (int32_t)(120000 / set.par.edo);
        t[0] = 0;
        text_cat(t, "each part ");
        int_text(step100 / 100, u);
        text_cat(t, u);
        text_cat(t, "^");
        u[0] = (char)('0' + step100 % 100 / 10);
        u[1] = (char)('0' + step100 % 10);
        u[2] = 0;
        text_cat(t, u);
        text_cat(t, " cents");
        title(PAGE_TITLE[1], t);
    } else {
        title(PAGE_TITLE[1], sys == TS_EQUAL ? "twelve equal semitones" : CIRCUL_NOTE[sys]);
    }
#if KIST_CLOCK
    {
        /* the clock face: the octave round the dial, C at the top. The ticks are the equal division's steps (twelve
         * equal semitones, or the division's own parts); the dotted hands are where this tuning puts each note,
         * their offsets from the ticks shown three times over (1 cent = 0.9 degrees) */
        static const char *const NOTE[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "G#", "A", "Bb", "B"};
        const int32_t cx = 120, cy = 102, R = 32;
        int n = sys == TS_EQUAL ? set.par.edo : 12, k, i;
        float c[12];
        if (sys == TS_EQUAL)
            for (i = 0; i < 12; i++)
                c[i] = 0.0f;
        else
            organ_temperament_cents(sys, set.par.root, c);
        circle(cx, cy, R, P_INK, 0);
        circle(cx, cy, R + 1, P_INK, 0);
        if (!(sys == TS_EQUAL && n != 12))
            circle(cx, cy, 2, P_INK, 0);
        for (k = 0; k < n; k++) {           /* the equal steps, outside the ring */
            float rad = (360.0f * (float)k / (float)n - 90.0f) * 0.01745329f, co = fm_cosf(rad), si = fm_sinf(rad);
            int32_t len = n == 12 || k * 12 % n == 0 ? 5 : 3;
            cv_line(cx + (int32_t)((float)(R + 2) * co + 0.5f), cy + (int32_t)((float)(R + 2) * si + 0.5f),
                    cx + (int32_t)((float)(R + 1 + len) * co + 0.5f), cy + (int32_t)((float)(R + 1 + len) * si + 0.5f),
                    k == 0 ? P_RED : P_INK);
        }
        for (i = 0; i < 12; i++) {          /* the notes' names round the outside, and their hands */
            float base = 30.0f * (float)i, ang, rad, co, si;
            int root = sys != TS_EQUAL && i == (int)set.par.root;
            int32_t r;
            rad = (base - 90.0f) * 0.01745329f;
            text_c(cx + (int32_t)(46.0f * fm_cosf(rad) + 0.5f) + 1, cy + (int32_t)(43.0f * fm_sinf(rad) + 0.5f) - 7, &KF_NOTE,
                   NOTE[i], root ? P_RED : P_MID);
            if (sys == TS_EQUAL && n != 12)
                continue;                   /* a division: its own steps, no hands */
            ang = base + 3.0f * 0.3f * c[i];
            rad = (ang - 90.0f) * 0.01745329f;
            co = fm_cosf(rad);
            si = fm_sinf(rad);
            for (r = 4; r < R; r += 2)       /* dotted, from the centre to the ring */
                cv_pset(cx + (int32_t)((float)r * co + 0.5f), cy + (int32_t)((float)r * si + 0.5f), root ? P_RED : P_INK);
        }
        if (sys == TS_EQUAL && n != 12) {
            int_text(n, t);
            text_c(cx, cy - 25, &KF_BIG, t, P_INK);
            text_c(cx, cy + 10, &KF_SC, "parts", P_RED);
            cv_text(14, Y0 + 68, &KF_SC, "ticks:", P_MID);
            cv_text(14, Y0 + 80, &KF_SC, "the parts", P_MID);
        } else {
            cv_text(14, Y0, &KF_SC, "the notes", P_RED);
            cv_text(14, Y0 + 68, &KF_SC, "ticks:", P_MID);
            cv_text(14, Y0 + 80, &KF_SC, "equal", P_MID);
            text_r(226, Y0 + 68, &KF_SC, "hands:", P_MID);
            text_r(226, Y0 + 80, &KF_SC, "each cent x3", P_MID);
        }
    }
#else
    if (sys == TS_EQUAL && set.par.edo != 12) {
        /* an equal division: the octave as an engraved scale of its parts */
        int n = set.par.edo, k;
        int32_t cx = 120, cy = 101;
        circle(cx, cy, 45, P_INK, 0);
        circle(cx, cy, 43, P_INK, 0);
        for (k = 0; k < n; k++) {
            float a = 360.0f * (float)k / (float)n, rad = (a - 90.0f) * 0.01745329f, c = fm_cosf(rad), s = fm_sinf(rad);
            int32_t len = (k % 12 == 0 || n % 12 == 0) && k * 12 % n == 0 ? 12 : 6;
            cv_line(cx + (int32_t)(43.0f * c), cy + (int32_t)(43.0f * s), cx + (int32_t)((float)(43 - len) * c),
                    cy + (int32_t)((float)(43 - len) * s), k == 0 ? P_RED : P_INK);
        }
        int_text(n, t);
        text_c(cx, cy - 25, &KF_BIG, t, P_INK);
        text_c(cx, cy + 13, &KF_SC, "parts", P_RED);
    } else {
        /* the Quinten-Circul: the twelve notes round the circle of fifths, each fifth an arc between its notes.
         * The dotted ring is the pure fifth; a tempered fifth is drawn in towards the centre by how much it is
         * narrowed (1.6 px a cent), so the comma's squashing shows where it was put; a fifth wider than pure bows
         * out, in red: the wolf */
        static const int FIFTHS[12] = {0, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10, 5};
        static const char *const NOTE[12] = {"C", "G", "D", "A", "E", "B", "F#", "C#", "G#", "Eb", "Bb", "F"};
        const int32_t cx = 120, cy = 102, R = 33;
        float c[12];
        int i, k;
        organ_temperament_cents(sys, set.par.root, c);
        circle(cx, cy, R, P_MID, 1);
        for (i = 0; i < 12; i++) {
            int a = FIFTHS[i], b = FIFTHS[(i + 1) % 12];
            float narrow = 701.955f - (700.0f + c[b] - c[a]), sag = narrow * 1.6f;
            uint16_t ink = narrow < -3.0f ? P_RED : P_INK;
            int32_t px = 0, py = 0;
            if (sag > 10.0f)
                sag = 10.0f;
            if (sag < -8.0f)
                sag = -8.0f;
            for (k = 0; k <= 16; k++) {
                float u = (float)k / 16.0f, ang = (30.0f * ((float)i + u) - 90.0f) * 0.01745329f;
                float r = (float)R - sag * fm_sinf(3.14159265f * u);
                int32_t x = cx + (int32_t)(r * fm_cosf(ang) + 0.5f), y = cy + (int32_t)(r * fm_sinf(ang) + 0.5f);
                int32_t x2 = cx + (int32_t)((r - 1.0f) * fm_cosf(ang) + 0.5f), y2 = cy + (int32_t)((r - 1.0f) * fm_sinf(ang) + 0.5f);
                if (k) {
                    cv_line(px, py, x, y, ink);
                    cv_pset(x2, y2, ink);
                }
                px = x;
                py = y;
            }
        }
        for (i = 0; i < 12; i++) {
            float ang = (30.0f * (float)i - 90.0f) * 0.01745329f;
            int32_t nx = cx + (int32_t)(43.0f * fm_cosf(ang) + 0.5f), ny = cy + (int32_t)(43.0f * fm_sinf(ang) + 0.5f);
            int root = FIFTHS[i] == (int)set.par.root;
            cv_rect(cx + (int32_t)((float)R * fm_cosf(ang) + 0.5f) - 1, cy + (int32_t)((float)R * fm_sinf(ang) + 0.5f) - 1, 3, 3,
                    root ? P_RED : P_INK);
            text_c(nx + 1, ny - 7, &KF_SC, NOTE[i], root ? P_RED : P_INK);
        }
        cv_text(14, Y0, &KF_SC, "the fifths", P_RED);
        cv_text(14, Y0 + 68, &KF_SC, "dotted:", P_MID);
        cv_text(14, Y0 + 80, &KF_SC, "pure", P_MID);
        text_r(226, Y0 + 68, &KF_SC, "drawn in:", P_MID);
        text_r(226, Y0 + 80, &KF_SC, "narrowed", P_MID);
        {
            float worst = 0.0f;
            for (i = 0; i < 12; i++) {
                float w = (700.0f + c[FIFTHS[(i + 1) % 12]] - c[FIFTHS[i]]) - 701.955f;
                if (w > worst)
                    worst = w;
            }
            if (worst > 3.0f) {
                text_r(226, Y0, &KF_SC, "the wolf", P_RED);
                text_r(226, Y0 + 12, &KF_SC, "bows out", P_RED);
            }
        }
    }
#endif
    foot_rules();
    int_text(set.par.pitch_hz, t);
    text_cat(t, " Hz");
    foot(0, "A", t);
    t[0] = '+';
    t[1] = '^';
    t[2] = (char)('0' + set.par.pitch_cent / 10u);
    t[3] = (char)('0' + set.par.pitch_cent % 10u);
    t[4] = 0;
    text_cat(t, " Hz");
    foot(1, "fine", t);
    foot(2, "Temperament", sys == TS_EQUAL ? "Equal" : SYS_SHORT_B[sys]);
    if (sys == TS_EQUAL) {
        int_text(set.par.edo, t);
        text_cat(t, " parts");
        foot(3, "Division", t);
    } else {
        foot(3, "upon", PC_NAME[set.par.root]);
    }
}

static void draw_room(void)
{
    static const char *const NUM[5] = {"i", "ii", "iii", "iv", "v"};
    char t[40];
    int r, i;
    title(PAGE_TITLE[2], caption("where the organ stands"));
    for (r = 0; r < NROOMS; r++) {
        int32_t y = Y0 + r * 19, sel = r == set.par.room;
        cv_text(14, y + 4, &KF_SC, sel ? "}" : NUM[r], sel ? P_RED : P_MID);
        cv_text(32, y, sel ? &KF_ITAL : &KF_BODY, organ_room_name(r), sel ? P_RED : P_INK);
        if (r < NROOMS - 1)
            hline(12, y + 18, 124, P_RULE);
    }
    {   /* the decay: an engraved curve over three seconds */
        float rt = 0.1f * (float)set.par.decay_ds;
        int32_t x0 = 146, w = 80, base = 129;
        cv_text(x0, Y0, &KF_SC, "its dying", P_RED);
        for (i = 0; i < w; i++) {
            float tt = 3.0f * (float)i / (float)w, v = 1.0f - tt / rt;
            int32_t h = v > 0.0f ? (int32_t)(54.0f * v) : 0, y;
            for (y = 0; y < h; y += 2)
                cv_pset(x0 + i, base - y, P_INK);
            if (h)
                cv_pset(x0 + i, base - h, P_INK);
        }
        hline(x0, base + 1, w, P_INK);
        for (i = 0; i < 4; i++)
            vline(x0 + i * (w - 1) / 3, base + 1, 3, P_INK);
        cv_text(x0 - 2, base + 4, &KF_SC, "0", P_MID);
        text_r(x0 + w + 2, base + 4, &KF_SC, "3 s", P_MID);
    }
    foot_rules();
    foot(0, "Room", organ_room_name(set.par.room));
    if (set.par.blower_noise) {
        int_text(set.par.blower_noise, t);
        text_cat(t, " of 10");
    } else {
        t[0] = 0;
        text_cat(t, "silent");
    }
    foot(1, "Blower", t);
    int_text(set.par.distance, t);
    text_cat(t, " metres");
    foot(2, "Distance", t);
    dec_text(set.par.decay_ds, t);
    dots_to_mid(t);
    text_cat(t, " s");
    foot(3, "Decay", t);
}

static void draw_wind(void)
{
    const organ_meter_t *m = organ_meter();
    char t[48], u[16];
    int i;
    if (m->last_vel) {                     /* the caption: the last key's touch, once there is one */
        t[0] = 0;
        text_cat(t, "the last key ");
        int_text((int32_t)m->last_vel, u);
        text_cat(t, u);
        text_cat(t, ", open in ");
        dec_text((int32_t)(m->last_open_us / 100u), u);
        dots_to_mid(u);
        text_cat(t, u);
        text_cat(t, " ms");
        title(PAGE_TITLE[3], t);
    } else {
        title(PAGE_TITLE[3], caption(set.trem ? "the tremulant beating" : "the bellows"));
    }
    {   /* a wedge bellows, opened by the reservoir's pressure */
        int32_t p = clamp((int32_t)(m->pressure * 100.0f), 0, 110), rise = 6 + p * 22 / 100, y;
        cv_rect(14, 100, 56, 6, P_INK);
        for (y = 101; y < 105; y += 2)
            hline(15, y, 54, P_PAPER);
        cv_line(14, 100, 70, 100 - rise, P_INK);
        cv_line(14, 98, 70, 98 - rise, P_INK);
        vline(70, 100 - rise, rise, P_INK);
        for (i = 1; i < 4; i++)
            vline(14 + i * 14, 100 - rise * i / 4, rise * i / 4, P_RULE);
    }
    cv_text(86, Y0, &KF_SC, "the reservoir", P_RED);
    int_text((int32_t)(m->pressure * 100.0f + 0.5f), t);
    i = cv_text(86, Y0 + 10, &KF_BIG, t, P_INK);
    cv_text(i + 5, Y0 + 27, &KF_ITAL, "in 100", P_INK);
    dotted(12, 117, 216, 3, P_MID);        /* the voiced pressure; under it, the last six seconds */
    for (i = 0; i < 59; i++) {
        int32_t a = ui.trace[(ui.trace_t / 100u + 1u + (uint32_t)i) % 60u], b = ui.trace[(ui.trace_t / 100u + 2u + (uint32_t)i) % 60u];
        a = 117 + clamp(100 - a, -6, 16);
        b = 117 + clamp(100 - b, -6, 16);
        cv_line(12 + i * 216 / 59, a, 12 + (i + 1) * 216 / 59, b, P_INK);
    }
    hline(12, 134, 216, P_RULE);
    cv_text(14, 136, &KF_SC, "six seconds past", P_MID);
    text_r(226, 136, &KF_SC, "now", P_MID);
    foot_rules();
    if (set.par.pressure) {
        signed_text(set.par.pressure, t);
        text_cat(t, " in 100");
    } else {
        t[0] = 0;
        text_cat(t, "as voiced");
    }
    foot(0, "Wind", t);
    foot(1, "Steadiness", STEADY_B[set.par.steadiness]);
    dec_text(set.par.trem_speed, t);
    dots_to_mid(t);
    text_cat(t, " a second");
    foot(2, "Tremulant", t);
    foot(3, "its depth", DEPTH_B[set.par.trem_depth]);
}

static void draw_voicing(void)
{
    char t[40], u[16];
    int k, st = ui.voice_stop;
    title(PAGE_TITLE[4], caption("strike a key to choose its pipe"));
    {                                       /* the register being voiced, and where it sits among the four */
        int32_t x;
        t[0] = 0;
        text_cat(t, STOP_DE[st]);
        text_cat(t, " ");
        text_cat(t, STOP_FUSS[st]);
        x = text_c(120, Y0 - 2, &KF_VITAL, t, P_RED);
        hline(240 - x, Y0 + 19, 2 * x - 240, P_RED);
        for (k = 0; k < NSTOPS; k++)        /* four marks under it, the register's own filled */
            if (k == st)
                circle_hatch(105 + k * 10, Y0 + 26, 3, P_RED);
            else
                circle(105 + k * 10, Y0 + 26, 2, P_MID, 0);
    }
    hline(12, 109, 216, P_INK);
    dotted(12, 97, 216, 3, P_MID);
    dotted(12, 121, 216, 3, P_MID);
    for (k = 0; k < KIST_NKEYS; k++) {
        int key = KIST_KEY_LO + k, db2 = set.voi.level_half_db[st][k];
        int32_t px = 14 + k * 4, py = 109 - db2 * 6 / 5, sel = key == ui.voice_key;
        if (st == ST_QUINT && key < set.par.split)
            continue;
        if (py < 109)
            vline(px, py, 109 - py, P_INK);
        else if (py > 109)
            vline(px, 110, py - 109, P_INK);
        if (sel) {
            circle_hatch(px, py, 3, P_RED);
            circle(px, py, 3, P_RED, 0);
        } else {
            cv_rect(px - 1, py - 1, 2, 2, P_INK);
        }
    }
    vline(14 + (set.par.split - KIST_KEY_LO) * 4 - 2, 85, 50, P_RED);    /* the division */
    cv_text(12, 134, &KF_SC, "C", P_MID);
    text_c(120, 134, &KF_SC, "the strength of each pipe", P_MID);
    text_r(228, 134, &KF_SC, "f'''", P_MID);
    foot_rules();
    foot(0, "Register", STOP_DE[st]);
    {
        int v = set.voi.level_half_db[st][ui.voice_key - KIST_KEY_LO];
        char lab[16];
        note_text(ui.voice_key, lab);
        signed_text(v / 2, t);
        if (v & 1)
            text_cat(t, "^5");
        text_cat(t, " dB");
        foot(1, lab, t);
    }
    if (set.voi.chiff[st]) {
        int_text(set.voi.chiff[st], t);
        text_cat(t, " of 4");
    } else {
        t[0] = 0;
        text_cat(t, "none");
    }
    foot(2, "Chiff", t);
    t[0] = '\\';
    t[1] = 0;
    dec_text(set.voi.drift_dc[st], u);
    dots_to_mid(u);
    text_cat(t, u);
    text_cat(t, " cents");
    foot(3, "Drift", t);
}

static void ledger(int32_t y, const char *k, const char *v, uint16_t vc)   /* a row: name, dotted leader, value */
{
    int32_t x = cv_text(14, y, &KF_BODY, k, P_INK) + 4, vx = 226 - text_w(&KF_ITAL, v) - 4;
    if (vx > x)
        dotted(x, y + 14, vx - x, 3, P_MID);
    text_r(226, y, &KF_ITAL, v, vc);
}

static void draw_system(void)
{
    const organ_meter_t *m = organ_meter();
    char t[64], u[16];
    t[0] = 0;                               /* the caption: what the organ is doing */
    text_cat(t, "work ");
    int_text((int32_t)m->load_pct, u);
    text_cat(t, u);
    text_cat(t, "%, pipes ");
    int_text((int32_t)m->pipes, u);
    text_cat(t, u);
    text_cat(t, " of ");
    int_text((int32_t)m->pipe_cap, u);
    text_cat(t, u);
    title(PAGE_TITLE[5], t);
    ledger(Y0, "MIDI received", "by jack and USB", P_INK);
    note_text(set.par.split - 1, t);
    text_cat(t, " | ");
    note_text(set.par.split, u);
    text_cat(t, u);
    ledger(Y0 + 19, "the division", t, P_INK);
    ledger(Y0 + 38, "the second core", plat_cpu2_on() ? "at work" : cpu2_started ? "retired" : "at rest",
           plat_cpu2_on() ? P_RED : P_INK);
    ledger(Y0 + 57, "the memory", flash_id == 0x856014u ? "P25Q80, the older" : flash_id == 0xB36014u ? "UC25HQ80, the newer"
                                                                                                 : "unknown: no saving",
           flash_id == 0x856014u || flash_id == 0xB36014u ? P_INK : P_RED);
    int_text((int32_t)m->pipes_peak, u);
    ledger(Y0 + 76, "the most pipes at once", u, P_INK);
    foot_rules();
    if (set.midi_ch) {
        int_text(set.midi_ch, t);
    } else {
        t[0] = 0;
        text_cat(t, "any");
    }
    foot(0, "Channel", t);
    t[0] = 0;
    text_cat(t, "at ");
    note_text(set.par.split, u);
    text_cat(t, u);
    foot(1, "Division", t);
    if (set.midi_transpose) {
        signed_text(set.midi_transpose, t);
        text_cat(t, " semitones");
    } else {
        t[0] = 0;
        text_cat(t, "none");
    }
    foot(2, "Transpose", t);
}

static void draw_temperament(void)       /* the ALGORITHM pop-up, over the page */
{
    static const char *const NUM[NTS] = {"i", "ii", "iii", "iv", "v"};
    static const char *const YEAR[NTS] = {"", "1691", "1/4 comma", "1779", "c. 1750"};
    int sel = ui.sys_pending >= 0 ? ui.sys_pending : set.par.system, i;
    cv_rect(9, 9, 222, 222, P_PAPER);
    sheet();
    title("Temperament", "turn the Algorithm to choose");
    for (i = 0; i < NTS; i++) {
        int32_t y = Y0 + 2 + i * 27;
        char t[32];
        t[0] = 0;
        if (i == TS_EQUAL) {
            text_cat(t, "in ");
            int_text(set.par.edo, t + 3);
            text_cat(t, " parts");
        } else {
            text_cat(t, YEAR[i]);
        }
        cv_text(16, y + 5, &KF_SC, i == sel ? "}" : NUM[i], i == sel ? P_RED : P_MID);
        cv_text(34, y, i == sel ? &KF_VITAL : &KF_NAME, i == TS_EQUAL ? "Equal" : SYS_LONG_B[i], i == sel ? P_RED : P_INK);
        text_r(226, y + 5, &KF_SC, t, P_MID);
        hline(12, y + 24, 216, P_RULE);
    }
    text_c(120, 196, &KF_ITAL, "it takes hold when the knob rests;", P_INK);
    text_c(120, 211, &KF_SC, "the key and the parts on page ii", P_MID);
}

static void draw(void)
{
    uint32_t half, now = plat_ms();
    for (half = 0; half < 2u; half++) {  /* the screen in two halves of 120 rows */
        cv_begin(240, 120, P_PAPER);
        cv_oy = -(int32_t)(120u * half);
        cv_y0 = 0;
        cv_y1 = 120;
        sheet();
        if (now < ui.overlay_until)
            draw_temperament();
        else
            switch (ui.page) {
            case PG_CONSOLE: draw_console(); break;
            case PG_TUNING: draw_tuning(); break;
            case PG_ROOM: draw_room(); break;
            case PG_WIND: draw_wind(); break;
            case PG_VOICING: draw_voicing(); break;
            default: draw_system(); break;
            }
        cv_blit(0, 120u * half);
    }
}
