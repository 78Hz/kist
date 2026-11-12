/* KIST — the organ: wooden flue pipes as digital waveguides, their valves, the wind, and the mix.
 *
 * A flue pipe (after the jet-drive model of Verge / Fletcher, as digital waveguides): the jet from the windway
 * crosses the mouth in jet_d samples and is deflected by the acoustic flow; the deflected jet (a cubic: it
 * saturates either side of the upper lip) drives the resonator, a delay line of bore_d samples (a whole period
 * for an open pipe, half for a stopped one, with the sign of the reflection) with a one-pole wall/radiation loss.
 * Wood: lower loss frequencies than metal, softer cubic drive. The Rohrflöte's chimney adds a resonance near
 * its fifth harmonic.
 *
 * The model's sounding pitch is not the resonator's: the jet pulls it. calib_table.h (tools/calibrate.c, from
 * this exact model) gives the pull in cents for each stop across log2(f / fs); a pipe is built at the frequency
 * that makes it sound where the tuning asks.
 *
 * Memory: each pipe's two delay lines come from pools of five sizes (64 .. 1024 samples), so 48 pipes fit in
 * about 200 KB; 2x-oversampled pipes (the top of the compass, on the second core) are short anyway. */
#include "organ.h"
#include "reverb.h"
#include "calib_table.h"
#include "fastmath.h"
#include "plat.h"
#define memset __builtin_memset   /* no libc headers in the firmware build */
#define memcpy __builtin_memcpy

#define FS KIST_FS
#define CR 16u                             /* control rate: valves, wind and smoothing every 16 samples */
#define BLOCK_MAX 256u
#define JET_STRETCH 1.35f                  /* the slowest jet (a valve just opening) as a multiple of the voiced one */
#define OS_HZ 1500.0f                      /* pipes above this run at 2x on the second core, */
#define OS_MAX 16                          /* at most this many (the core must finish well inside a block) */
#define HZ_MIN 28.0f                       /* a pipe below this, or above HZ_MAX of its rate, is not built */
#define HZ_MAX_FRAC 0.15f

/* ------------------------------------------------------------------------------------------- stops --- */
typedef struct {
    float sign;                            /* -1 stopped, +1 open */
    float jr;                              /* jet transit / period */
    float bore_fc, jet_fc;                 /* loss filters, multiples of the pipe's frequency */
    float loop_g;
    float out_fc, out_min;                 /* radiation lowpass: multiple of f, at least out_min Hz */
    float gain, noise, chiff, chiff_ms;
    float chim_ratio, chim_q, chim_mix;    /* the Rohrflöte's chimney resonance */
    float tap;                             /* the sound taken at the mouth: jet flow + tap x the bore's return
                                            * (+1 stopped: mostly odd harmonics; negative open: the full series) */
    float flow;                            /* wind it takes, relative */
    uint8_t prio;                          /* 3 kept longest .. 0 dropped first */
} stop_model_t;

static const stop_model_t STOP[NSTOPS] = {
    /* Gedackt 8': stopped wood, mild, almost no even harmonics */
    {-1.0f, 0.45f, 4.0f, 3.0f, 0.985f, 6.0f, 1200.0f, 0.0506f, 0.012f, 0.06f, 28.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.00f, 3},
    /* Rohrflöte 4': stopped with a chimney, a little brighter, a lifted fifth harmonic */
    {-1.0f, 0.40f, 5.0f, 3.0f, 0.985f, 9.0f, 1500.0f, 0.0396f, 0.014f, 0.07f, 20.0f, 5.0f, 6.0f, 1.20f, 1.0f, 0.60f, 2},
    /* Principal 2': open wood */
    {+1.0f, 0.36f, 6.0f, 3.0f, 0.985f, 10.0f, 2000.0f, 0.136f, 0.018f, 0.05f, 15.0f, 0.0f, 0.0f, 0.0f, -0.6f, 0.45f, 1},
    /* Quint 2 2/3': open wood, softer */
    {+1.0f, 0.38f, 6.0f, 3.0f, 0.985f, 8.0f, 2000.0f, 0.0876f, 0.018f, 0.04f, 12.0f, 0.0f, 0.0f, 0.0f, -0.6f, 0.30f, 0},
};
#define FLUE_BREATH 0.9f                   /* jet drive at the voiced pressure */
#define SURGE 0.22f                        /* the foot pressure's overshoot as a pallet opens, decaying in ~18 ms */
#define JET_REFL 0.5f
#define END_REFL 0.5f

/* ------------------------------------------------------------------------------------- delay pools --- */
#define NCLASS 5
static const uint32_t CLASS_LEN[NCLASS] = {64, 128, 256, 512, 1024};
#define C0N 16
#define C1N 16
#define C2N 16
#define C3N 20
#define C4N 8
static const uint32_t CLASS_N[NCLASS] = {C0N, C1N, C2N, C3N, C4N};
#define POOL_FLOATS (2u * (C0N * 64u + C1N * 128u + C2N * 256u + C3N * 512u + C4N * 1024u))
static float delay_pool[POOL_FLOATS] PLAT_POOL;
static float *class_base[NCLASS];
static uint32_t class_used[NCLASS];        /* bit per buffer */

static int buf_take(uint32_t need, uint8_t *cls, uint8_t *idx)
{
    uint32_t c, i;
    for (c = 0; c < NCLASS; c++) {
        if (CLASS_LEN[c] < need)
            continue;
        for (i = 0; i < CLASS_N[c]; i++)
            if (!(class_used[c] >> i & 1u)) {
                class_used[c] |= 1u << i;
                *cls = (uint8_t)c;
                *idx = (uint8_t)i;
                return 1;
            }
    }
    return 0;
}
static void buf_give(uint8_t cls, uint8_t idx) { class_used[cls] &= ~(1u << idx); }
static float *buf_ptr(uint8_t cls, uint8_t idx) { return class_base[cls] + (uint32_t)idx * 2u * CLASS_LEN[cls]; }

/* ------------------------------------------------------------------------------------------- pipes --- */
typedef struct {
    float *bore, *jet;
    uint32_t mask, wb, wj;
    float bore_d, jet_d, bore_t, jet_t, bore_0, jet_0;
    float sign, loop_g, a_bore, a_jet, a_dc, a_out, a_hp, tap;
    float lpb, lpj, dc_in, dc_out, out_lp, hp_x, hp_y;
    float c_b0, c_a1, c_a2, c_z1, c_z2, c_mix;
    float breath, noise, chiff_now, chiff_dec, noise_base;
    float valve, open_step, close_step, surge, surge_dec;
    float gain, gain_t, gl, gr;
    float hz;                              /* the sounding frequency asked for */
    uint32_t rng, quiet, age;
    uint8_t active, closing, stop, os, cls, idx, side, core2, dead;
    int8_t note;
} pipe_t;

static pipe_t pipes[KIST_MAX_PIPES];

/* the 2x pipes' sum and the decimator to 1x */
static float os_l[2 * BLOCK_MAX], os_r[2 * BLOCK_MAX], hb_l[2 * BLOCK_MAX + 32], hb_r[2 * BLOCK_MAX + 32];
static float dec_l[BLOCK_MAX], dec_r[BLOCK_MAX];
#define HB_TAPS 31
static float hb_h[HB_TAPS];
static uint32_t job_n, job_prev_n;
static int os_count;

static inline __attribute__((always_inline)) float rnd(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

/* a fractional delay's taps, worked out once per control step (16 samples) and applied to every sample of it */
typedef struct { uint32_t i; float c0, c1, c2, c3; } frac_t;
static inline __attribute__((always_inline)) void frac_set(frac_t *q, float d)
{
    int32_t i = (int32_t)d;
    float f = d - (float)i, fm1 = f - 1.0f, fm2 = f - 2.0f, fp1 = f + 1.0f;
    q->i = (uint32_t)i;
    q->c0 = -f * fm1 * fm2 * (1.0f / 6.0f);
    q->c1 = fp1 * fm1 * fm2 * 0.5f;
    q->c2 = -fp1 * f * fm2 * 0.5f;
    q->c3 = fp1 * f * fm1 * (1.0f / 6.0f);
}
static inline __attribute__((always_inline)) float frac_read(const float *b, uint32_t mask, uint32_t w, const frac_t *q)
{
    uint32_t r = w - q->i;
    return q->c0 * b[(r + 1u) & mask] + q->c1 * b[r & mask] + q->c2 * b[(r - 1u) & mask] + q->c3 * b[(r - 2u) & mask];
}

/* one sample of a pipe: the bore read, losses, the jet, the cubic, the bore written; returns the mouth's output */
static inline __attribute__((always_inline)) float pipe_tick_q(pipe_t *p, float breath, const frac_t *qb, const frac_t *qj,
                                                               float noise)
{
    float b = frac_read(p->bore, p->mask, p->wb, qb), t, pd, pj, jt, in;
    p->lpb += p->a_bore * (b - p->lpb);
    t = p->sign * p->loop_g * p->lpb;
    p->dc_out = t - p->dc_in + (1.0f - p->a_dc) * p->dc_out;
    p->dc_in = t;
    t = p->dc_out;
    pd = breath * (1.0f + noise * rnd(&p->rng)) - JET_REFL * t;
    p->wj = (p->wj + 1u) & p->mask;
    p->jet[p->wj] = pd;
    pj = frac_read(p->jet, p->mask, p->wj, qj);
    p->lpj += p->a_jet * (pj - p->lpj);
    jt = p->lpj * (p->lpj * p->lpj - 1.0f);
    if (jt > 1.0f)
        jt = 1.0f;
    if (jt < -1.0f)
        jt = -1.0f;
    in = jt + END_REFL * t;
    p->wb = (p->wb + 1u) & p->mask;
    p->bore[p->wb] = in + 1e-20f;
    return in + p->tap * t;
}

static float one_pole(float fc, float fs)
{
    if (fc > 0.45f * fs)
        fc = 0.45f * fs;
    return 1.0f - fm_expf(-FM_TWO_PI * fc / fs);
}

/* the calibration: the model's pull in cents at log2(f / fs) */
static float calib_cents(int stop, float lf)
{
    float x = (lf - CAL_LF_MIN) / CAL_LF_STEP;
    int i;
    if (x <= 0.0f)
        return CAL[stop][0];
    if (x >= (float)(CAL_N - 1))
        return CAL[stop][CAL_N - 1];
    i = (int)x;
    return CAL[stop][i] + (x - (float)i) * (CAL[stop][i + 1] - CAL[stop][i]);
}

/* the frequency to build a pipe at so that it sounds at hz */
static float build_hz(int stop, float hz, float fs, int use_cal)
{
    float fe = hz;
    int it;
    if (!use_cal)
        return hz;
    for (it = 0; it < 3; it++)
        fe = hz * fm_exp2f(-calib_cents(stop, fm_log2f(fe / fs)) / 1200.0f);
    return fe;
}

/* geometry and filters for a pipe sounding at hz at rate fs; 0 if its delays do not fit max_len */
static int pipe_shape(pipe_t *p, int stop, float hz, float fs, int use_cal, uint32_t max_len)
{
    const stop_model_t *m = &STOP[stop];
    float fe = build_hz(stop, hz, fs, use_cal), per = fs / fe, w, bw, r;
    p->bore_t = m->sign < 0.0f ? 0.5f * per : per;
    p->jet_t = m->jr * per;
    if (max_len && (p->bore_t * 1.06f + 4.0f > (float)max_len || p->jet_t * JET_STRETCH + 4.0f > (float)max_len))
        return 0;
    if (p->bore_t < 2.0f || p->jet_t < 1.2f)
        return 0;
    p->bore_0 = p->bore_t;
    p->jet_0 = p->jet_t;
    p->sign = m->sign;
    p->tap = m->tap;
    p->loop_g = m->loop_g;
    p->a_bore = one_pole(m->bore_fc * fe, fs);
    p->a_jet = one_pole(m->jet_fc * fe, fs);
    p->a_dc = one_pole(0.25f * fe, fs);
    w = m->out_fc * hz;
    p->a_out = one_pole(w > m->out_min ? w : m->out_min, fs);
    p->a_hp = 1.0f - FM_TWO_PI * 18.0f / fs;
    p->c_mix = 0.0f;
    if (m->chim_mix > 0.0f && m->chim_ratio * hz < 0.4f * fs) {
        w = FM_TWO_PI * m->chim_ratio * hz / fs;
        bw = w / m->chim_q;
        r = 1.0f - 0.5f * bw;
        p->c_a1 = -2.0f * r * fm_cosf(w);
        p->c_a2 = r * r;
        p->c_b0 = 0.5f * (1.0f - r * r);
        p->c_mix = m->chim_mix;
    }
    p->hz = hz;
    return 1;
}

/* ------------------------------------------------------------------------------------------- notes --- */
typedef struct {
    int8_t key, held, used;
    uint8_t vel;
    uint32_t age;
    int8_t pipe[NSTOPS];
    float open_step;                       /* valve opening per sample, from the velocity */
} note_t;

static note_t notes[KIST_MAX_NOTES];
static uint32_t age_counter;

/* -------------------------------------------------------------------------------------- the state --- */
static organ_params_t par_buf[2];
static volatile uint32_t par_cur, par_new;
static organ_params_t P;                   /* the render's copy */
static organ_voicing_t voicing_buf[2];
static volatile uint32_t voi_cur, voi_new;
static organ_voicing_t V;
static float key_hz[KIST_NKEYS];
static uint32_t halves;                    /* stops drawn: HALF_BIT */
static volatile int blower_want = 1, trem_want;
static organ_meter_t meter;

/* main loop -> render: a small single-producer queue */
enum { EV_NONE, EV_ON, EV_OFF, EV_STOPS, EV_PANIC };
#define QN 64u
static volatile uint32_t q_buf[QN], q_w, q_r;
static void q_push(uint32_t type, uint32_t a, uint32_t b)
{
    uint32_t w = q_w, nw = (w + 1u) & (QN - 1u);
    if (nw == q_r)
        return;                            /* full: dropped (64 events within one 5.8 ms block) */
    q_buf[w] = type | a << 8 | b << 16;
    q_w = nw;
}

/* wind */
static struct {
    float p, v;                            /* reservoir pressure (1 voiced) and its rate */
    float spin;                            /* the blower's speed, 0..1 */
    float demand;
    float trem_ph, trem_amt;
    float noise_lp1, noise_lp2;
    uint32_t rng;
    float p_sub[BLOCK_MAX / CR + 1];       /* the pressure the pipes see, at each control step of the block */
} wind;

/* ---------------------------------------------------------------------------------- pipes: start/stop --- */
static uint32_t hash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static float detune_cents(int stop, int key)
{
    float u = (float)(hash32((uint32_t)(stop * 977 + key * 131 + 7)) & 0xFFFFu) / 32767.5f - 1.0f;
    return 0.1f * (float)V.drift_dc[stop] * u;
}

static float reg_gain(int stop, int key)
{
    float db = 0.5f * (float)V.level_half_db[stop][key - KIST_KEY_LO];
    return STOP[stop].gain * fm_exp2f(db * (3.321928f / 20.0f));
}

static int pipes_active(void)
{
    int i, n = 0;
    for (i = 0; i < KIST_MAX_PIPES; i++)
        n += pipes[i].active;
    return n;
}

static void pipe_free(int i)
{
    pipe_t *p = &pipes[i];
    int k;
    if (!p->active)
        return;
    buf_give(p->cls, p->idx);
    p->active = 0;
    if (p->note >= 0)
        for (k = 0; k < NSTOPS; k++)
            if (notes[p->note].pipe[k] == i)
                notes[p->note].pipe[k] = -1;
}

static void pipe_close(int i, float ms)
{
    pipe_t *p = &pipes[i];
    float fs = p->os ? 2.0f * FS : FS;
    if (!p->active)
        return;
    p->closing = 1;
    p->close_step = 1.0f / (ms * 0.001f * fs);
}

/* a free pipe slot: one free, else the oldest closing, else the least important of the oldest notes (when
 * stop's priority is higher) */
static int pipe_cap_now(void);
static int pipe_slot(int stop)
{
    int i, best = -1;
    if (pipe_cap_now())
        for (i = 0; i < KIST_MAX_PIPES; i++)
            if (!pipes[i].active)
                return i;
    for (i = 0; i < KIST_MAX_PIPES; i++)   /* the oldest pipe already closing */
        if (pipes[i].closing && (best < 0 || pipes[i].age < pipes[best].age))
            best = i;
    if (best < 0)                          /* else the oldest of the least important stop, if it ranks below this one */
        for (i = 0; i < KIST_MAX_PIPES; i++) {
            int pr = STOP[pipes[i].stop].prio;
            if (pr >= STOP[stop].prio)
                continue;
            if (best < 0 || pr < STOP[pipes[best].stop].prio ||
                (pr == STOP[pipes[best].stop].prio && pipes[i].age < pipes[best].age))
                best = i;
        }
    if (best >= 0)
        pipe_free(best);
    return best;
}

static void pipe_start(int n, int stop)
{
    note_t *nt = &notes[n];
    int key = nt->key, i;
    pipe_t tmp, *p;
    float hz = key_hz[key - KIST_KEY_LO] * organ_stop_ratio(stop), fs;
    uint32_t need;
    uint8_t cls, idx;
    int os;
    if (nt->pipe[stop] >= 0) {             /* still sounding (a key played again, a stop drawn back) */
        p = &pipes[nt->pipe[stop]];
        p->closing = 0;
        p->open_step = nt->open_step * (p->os ? 0.5f : 1.0f);
        p->chiff_now = p->noise_base * 0.5f;
        return;
    }
    hz *= fm_exp2f(detune_cents(stop, key) / 1200.0f);
    os = hz > OS_HZ && plat_cpu2_on() && os_count < OS_MAX;
    fs = os ? 2.0f * FS : FS;
    if (hz < HZ_MIN || hz > HZ_MAX_FRAC * fs)
        return;                            /* out of the model's range (an extreme EDO, a very high pitch) */
    memset(&tmp, 0, sizeof tmp);
    if (!pipe_shape(&tmp, stop, hz, fs, 1, 0))
        return;
    need = (uint32_t)(fm_maxf(tmp.bore_t * 1.06f, tmp.jet_t * JET_STRETCH) + 5.0f);
    i = pipe_slot(stop);
    if (i < 0)
        return;
    if (!buf_take(need, &cls, &idx))
        return;
    p = &pipes[i];
    *p = tmp;
    p->cls = cls;
    p->idx = idx;
    p->bore = buf_ptr(cls, idx);
    p->jet = p->bore + CLASS_LEN[cls];
    p->mask = CLASS_LEN[cls] - 1u;
    memset(p->bore, 0, 2u * CLASS_LEN[cls] * sizeof(float));
    p->bore_d = p->bore_t;
    p->jet_d = p->jet_t * JET_STRETCH;
    p->stop = (uint8_t)stop;
    p->os = (uint8_t)os;
    p->note = (int8_t)n;
    p->rng = hash32((uint32_t)(stop * 131071 + key * 31 + 1)) | 1u;
    p->noise_base = STOP[stop].noise;
    {
        float ch = STOP[stop].chiff * 0.5f * (float)V.chiff[stop];   /* chiff 0..4: 0 .. 2x the stop's own */
        float tau = STOP[stop].chiff_ms * (hz > 1000.0f ? 0.6f : 1.0f);
        p->chiff_now = ch;
        p->chiff_dec = fm_expf(-(float)CR / (tau * 0.001f * fs));
    }
    p->open_step = nt->open_step * (os ? 0.5f : 1.0f);
    p->surge = SURGE;
    p->surge_dec = fm_expf(-(float)CR / (0.018f * fs));
    p->gain_t = reg_gain(stop, key);
    p->gain = p->gain_t;
    p->side = (uint8_t)(key & 1);          /* the chest: the C side and the C# side, each a bus (panned at the mix) */
    p->age = ++age_counter;
    p->active = 1;
    os_count += os;
    nt->pipe[stop] = (int8_t)i;
}

static int half_drawn(int stop, int key)
{
    int treble = key >= P.split;
    return (halves & HALF_BIT(stop, treble)) && !(stop == ST_QUINT && !treble);
}

static float open_step_for(int vel)
{
    static const float FAST[5] = {6.0f, 4.0f, 3.0f, 2.5f, 2.0f}, SLOW[5] = {40.0f, 30.0f, 22.0f, 16.0f, 12.0f};
    int t = P.touch > 4 ? 4 : P.touch;
    float x = (float)(vel < 1 ? 1 : vel > 127 ? 127 : vel) / 127.0f;
    float ms = SLOW[t] * fm_powf(FAST[t] / SLOW[t], x);
    meter.last_vel = (uint32_t)vel;
    meter.last_open_us = (uint32_t)(ms * 1000.0f);
    return 1.0f / (ms * 0.001f * FS);
}

static void note_on_now(int key, int vel)
{
    int i, n = -1, s;
    if (key < KIST_KEY_LO || key > KIST_KEY_HI)
        return;
    for (i = 0; i < KIST_MAX_NOTES; i++)
        if (notes[i].used && notes[i].key == key)
            n = i;
    if (n < 0) {
        uint32_t oldest = 0xFFFFFFFFu;
        for (i = 0; i < KIST_MAX_NOTES && n < 0; i++)
            if (!notes[i].used)
                n = i;
        if (n < 0) {                       /* else the oldest released note, else the oldest held */
            for (i = 0; i < KIST_MAX_NOTES; i++)
                if (!notes[i].held && notes[i].age < oldest) {
                    oldest = notes[i].age;
                    n = i;
                }
            if (n < 0)
                for (i = 0; i < KIST_MAX_NOTES; i++)
                    if (notes[i].age < oldest) {
                        oldest = notes[i].age;
                        n = i;
                    }
            for (s = 0; s < NSTOPS; s++)
                if (notes[n].pipe[s] >= 0)
                    pipe_free(notes[n].pipe[s]);
        }
        notes[n].used = 1;
        notes[n].key = (int8_t)key;
        for (s = 0; s < NSTOPS; s++)
            notes[n].pipe[s] = -1;
    }
    notes[n].held = 1;
    notes[n].vel = (uint8_t)vel;
    notes[n].age = ++age_counter;
    notes[n].open_step = open_step_for(vel);
    for (s = 0; s < NSTOPS; s++)
        if (half_drawn(s, key))
            pipe_start(n, s);
}

static void note_off_now(int key)
{
    int i, s;
    for (i = 0; i < KIST_MAX_NOTES; i++)
        if (notes[i].used && notes[i].key == key && notes[i].held) {
            notes[i].held = 0;
            for (s = 0; s < NSTOPS; s++)
                if (notes[i].pipe[s] >= 0)
                    pipe_close(notes[i].pipe[s], 6.0f);   /* a tracker pallet shuts in a few ms */
        }
}

static void stops_now(uint32_t h)
{
    int i, s;
    halves = h & HALVES_ALL & ~QUINT_BASS_BIT;
    for (i = 0; i < KIST_MAX_NOTES; i++) {
        if (!notes[i].used || !notes[i].held)
            continue;
        for (s = 0; s < NSTOPS; s++) {
            int want = half_drawn(s, notes[i].key), have = notes[i].pipe[s] >= 0 && !pipes[notes[i].pipe[s]].closing;
            if (want && !have)
                pipe_start(i, s);
            else if (!want && have)
                pipe_close(notes[i].pipe[s], 25.0f);   /* a slider closing */
        }
    }
}

static void all_off_now(void)
{
    int i;
    for (i = 0; i < KIST_MAX_NOTES; i++)
        if (notes[i].used && notes[i].held)
            note_off_now(notes[i].key);
}

static void panic_now(void)
{
    int i;
    for (i = 0; i < KIST_MAX_PIPES; i++)
        pipe_free(i);
    memset(notes, 0, sizeof notes);
    reverb_clear();
}

/* --------------------------------------------------------------------------------------------- tuning --- */
static void retune(void)
{
    int k, i;
    for (k = 0; k < KIST_NKEYS; k++)
        key_hz[k] = organ_key_hz(&P, KIST_KEY_LO + k);
    for (i = 0; i < KIST_MAX_PIPES; i++) {
        pipe_t *p = &pipes[i], t;
        int key;
        if (!p->active || p->note < 0)
            continue;
        key = notes[p->note].key;
        memset(&t, 0, sizeof t);
        if (!pipe_shape(&t, p->stop, key_hz[key - KIST_KEY_LO] * organ_stop_ratio(p->stop)
                        * fm_exp2f(detune_cents(p->stop, key) / 1200.0f),
                        p->os ? 2.0f * FS : FS, 1, CLASS_LEN[p->cls])) {
            pipe_close(i, 10.0f);          /* the new pitch does not fit its lines: it stops */
            continue;
        }
        p->bore_t = t.bore_t;              /* a held pipe glides to its new length */
        p->jet_0 = t.jet_0;
        p->bore_0 = t.bore_0;
        p->a_bore = t.a_bore;
        p->a_jet = t.a_jet;
        p->a_dc = t.a_dc;
        p->a_out = t.a_out;
        p->c_a1 = t.c_a1;
        p->c_a2 = t.c_a2;
        p->c_b0 = t.c_b0;
        p->hz = t.hz;
    }
}

static int tuning_differs(const organ_params_t *a, const organ_params_t *b)
{
    return a->pitch_hz != b->pitch_hz || a->pitch_cent != b->pitch_cent || a->system != b->system ||
           a->edo != b->edo || a->root != b->root;
}

/* ------------------------------------------------------------------------------------------ the room --- */
static const struct {
    const char *name;
    float size, damp_hz, er_gain, predelay_ms;
    uint8_t decay_ds, mix;
} ROOMS[NROOMS] = {
    {"Dry", 0.42f, 7000.0f, 0.22f, 2.0f, 4, 7},
    {"Chapel", 0.60f, 6000.0f, 0.60f, 6.0f, 16, 35},
    {"Village church", 0.90f, 5200.0f, 0.55f, 10.0f, 26, 35},
    {"Abbey nave", 1.25f, 4200.0f, 0.45f, 16.0f, 45, 40},
    {"Concert hall", 1.10f, 7500.0f, 0.55f, 14.0f, 22, 30},
};
const char *organ_room_name(int room) { return ROOMS[room % NROOMS].name; }
uint8_t organ_room_decay_ds(int room) { return ROOMS[room % NROOMS].decay_ds; }
uint8_t organ_room_mix(int room) { return ROOMS[room % NROOMS].mix; }

static int rich_now;
static void room_now(int rich)
{
    reverb_cfg_t c;
    int r = P.room % NROOMS;
    c.size = ROOMS[r].size;
    c.rt60 = 0.1f * (float)(P.decay_ds < 3 ? 3 : P.decay_ds);
    c.damp_hz = ROOMS[r].damp_hz;
    c.er_gain = ROOMS[r].er_gain * (1.0f - 0.025f * (float)(P.distance > 20 ? 20 : P.distance));
    c.predelay_ms = ROOMS[r].predelay_ms + 1.2f * (float)P.distance;
    c.mix = 0.01f * (float)(P.mix > 100 ? 100 : P.mix);
    c.rich = rich;
    rich_now = rich;
    reverb_configure(&c);
}

/* -------------------------------------------------------------------------------------- the interface --- */
void organ_init(void)
{
    uint32_t c, off = 0;
    int i, k;
    for (c = 0; c < NCLASS; c++) {
        class_base[c] = delay_pool + off;
        off += 2u * CLASS_LEN[c] * CLASS_N[c];
        class_used[c] = 0;
    }
    memset(pipes, 0, sizeof pipes);
    memset(notes, 0, sizeof notes);
    P.pitch_hz = 440;
    P.pitch_cent = 0;
    P.system = TS_EQUAL;
    P.edo = 12;
    P.root = 0;
    P.split = 60;
    P.touch = 2;
    P.pressure = 0;
    P.steadiness = 1;
    P.blower_noise = 3;
    P.trem_speed = 52;
    P.trem_depth = 0;
    P.room = ROOM_CHAPEL;
    P.mix = 35;
    P.distance = 6;
    P.decay_ds = 16;
    par_buf[0] = par_buf[1] = P;
    for (i = 0; i < NSTOPS; i++) {
        for (k = 0; k < KIST_NKEYS; k++)
            V.level_half_db[i][k] = 0;
        V.chiff[i] = 2;
        V.drift_dc[i] = 15;
    }
    voicing_buf[0] = voicing_buf[1] = V;
    for (k = 0; k < KIST_NKEYS; k++)
        key_hz[k] = organ_key_hz(&P, KIST_KEY_LO + k);
    halves = HALF_BIT(ST_GEDACKT, 0) | HALF_BIT(ST_GEDACKT, 1);
    wind.p = 1.0f;
    wind.spin = 1.0f;
    wind.rng = 0x2545F491u;
    for (k = 0; k < HB_TAPS; k++) {        /* the decimator: a windowed-sinc halfband */
        int m = k - HB_TAPS / 2;
        float w = 0.5f + 0.5f * fm_cosf(FM_PI * (float)m / (float)(HB_TAPS / 2 + 1));
        hb_h[k] = m == 0 ? 0.5f : (m & 1) ? w * fm_sinf(0.5f * FM_PI * (float)m) / (FM_PI * (float)m) : 0.0f;
    }
    memset(hb_l, 0, sizeof hb_l);
    memset(hb_r, 0, sizeof hb_r);
    reverb_init();
    room_now(0);
}

void organ_set_params(const organ_params_t *p)
{
    uint32_t w = par_cur ^ 1u;
    par_buf[w] = *p;
    par_cur = w;
    par_new = 1;
}

void organ_set_voicing(const organ_voicing_t *v)
{
    uint32_t w = voi_cur ^ 1u;
    voicing_buf[w] = *v;
    voi_cur = w;
    voi_new = 1;
}

void organ_set_stops(uint32_t h) { q_push(EV_STOPS, h & 0xFFu, 0); }
void organ_set_blower(int on) { blower_want = on; }
void organ_set_tremulant(int on) { trem_want = on; }
void organ_note_on(int key, int vel) { q_push(EV_ON, (uint32_t)key & 0xFFu, (uint32_t)vel & 0xFFu); }
void organ_note_off(int key) { q_push(EV_OFF, (uint32_t)key & 0xFFu, 0); }
void organ_note_on_isr(int key, int vel) { note_on_now(key, vel); }
void organ_note_off_isr(int key) { note_off_now(key); }
void organ_all_off_isr(void) { all_off_now(); }
void organ_panic(void) { q_push(EV_PANIC, 0, 0); }
void organ_use_cpu2(int on) { (void)on; }
const organ_meter_t *organ_meter(void) { return &meter; }

static volatile int shed_req;
void organ_shed(void) { shed_req = 1; }   /* any context: the render does it at its next block */

static void shed_now(void)
{
    int i, k;
    for (k = 0; k < 4; k++) {              /* the four least important, oldest pipes close */
        int best = -1;
        for (i = 0; i < KIST_MAX_PIPES; i++) {
            if (!pipes[i].active || pipes[i].closing)
                continue;
            if (best < 0 || STOP[pipes[i].stop].prio < STOP[pipes[best].stop].prio ||
                (STOP[pipes[i].stop].prio == STOP[pipes[best].stop].prio && pipes[i].age < pipes[best].age))
                best = i;
        }
        if (best >= 0)
            pipe_close(best, 15.0f);
    }
}

/* -------------------------------------------------------------------------------------------- render --- */
static void events(void)
{
    while (q_r != q_w) {
        uint32_t e = q_buf[q_r];
        q_r = (q_r + 1u) & (QN - 1u);
        switch (e & 0xFFu) {
        case EV_ON: note_on_now((int)(e >> 8 & 0xFFu), (int)(e >> 16 & 0xFFu)); break;
        case EV_OFF: note_off_now((int)(e >> 8 & 0xFFu)); break;
        case EV_STOPS: stops_now(e >> 8 & 0xFFu); break;
        case EV_PANIC: panic_now(); break;
        default: break;
        }
    }
}

/* the wind for one block: the reservoir against the pipes' demand, the blower's speed, the tremulant */
static void wind_block(uint32_t n)
{
    static const float SAG[3] = {0.010f, 0.035f, 0.070f}, TAU[3] = {0.015f, 0.040f, 0.090f}, ZETA[3] = {1.0f, 0.8f, 0.5f};
    static const float TDEPTH[3] = {0.025f, 0.05f, 0.09f};
    int s = P.steadiness > 2 ? 2 : P.steadiness, i;
    float dt = (float)CR / FS, trim = 1.0f + 0.01f * (float)P.pressure, d = 0.0f;
    float om = 1.0f / TAU[s], ze = ZETA[s];
    float trem_hz = 0.1f * (float)(P.trem_speed < 30 ? 30 : P.trem_speed > 80 ? 80 : P.trem_speed);
    float tdepth = TDEPTH[P.trem_depth > 2 ? 2 : P.trem_depth];
    uint32_t steps = (n + CR - 1u) / CR;
    for (i = 0; i < KIST_MAX_PIPES; i++)
        if (pipes[i].active)
            d += STOP[pipes[i].stop].flow * pipes[i].valve * (pipes[i].hz < 200.0f ? 1.0f : 200.0f / pipes[i].hz + 0.3f);
    wind.demand = d;
    for (i = 0; i < (int)steps; i++) {
        float target, acc;
        /* the blower spins up in about 1.5 s and down in about 2 s */
        wind.spin += blower_want ? dt / 1.5f : -dt / 2.0f;
        wind.spin = fm_clampf(wind.spin, 0.0f, 1.0f);
        target = trim * wind.spin * (1.0f - SAG[s] * d / 24.0f);
        if (wind.spin < 0.05f && !blower_want) {   /* off: the reservoir empties as the pipes take it */
            float leak = (0.15f + 0.08f * d) * dt;
            wind.p -= wind.p * leak;
            wind.v = 0.0f;
        } else {
            acc = om * om * (target - wind.p) - 2.0f * ze * om * wind.v;
            wind.v += acc * dt;
            wind.p += wind.v * dt;
        }
        if (wind.p < 0.0f)
            wind.p = 0.0f;
        wind.trem_amt += ((trem_want ? 1.0f : 0.0f) - wind.trem_amt) * dt / 0.3f;
        wind.trem_ph += trem_hz * dt;
        if (wind.trem_ph >= 1.0f)
            wind.trem_ph -= 1.0f;
        wind.p_sub[i] = wind.p * (1.0f + wind.trem_amt * tdepth * fm_sinf(FM_TWO_PI * wind.trem_ph));
    }
    meter.pressure = wind.p;
}

/* CR samples of a pipe, its state in locals (the delay lines' writes cannot alias it) and no branch per sample but
 * the jet's clamp. CHIM: the chimney's resonator (the Rohrflöte) */
#define PIPE_RUN(NAME, CHIM)                                                                                          \
static void NAME(pipe_t *p, float *__restrict bus, uint32_t cr, float breath, float bnoise, const frac_t *qb,      \
                 const frac_t *qj, float g)                                                                          \
{                                                                                                                    \
    float *__restrict d = p->bore;         /* the bore line, then the jet line (p->jet = d + len) */                 \
    const uint32_t mask = p->mask, len = mask + 1u, ib = qb->i, ij = qj->i;                                          \
    const float sg = p->sign * p->loop_g;  /* the reflection folded into the bore's taps */                           \
    const float b0 = sg * qb->c0, b1 = sg * qb->c1, b2 = sg * qb->c2, b3 = sg * qb->c3, jfr = qj->c0;                 \
    const float a_bore = p->a_bore, a_jet = p->a_jet, k_dc = 1.0f - p->a_dc, a_out = p->a_out, tap = p->tap;         \
    const float cb0 = p->c_b0, ca1 = p->c_a1, ca2 = p->c_a2, cmix = p->c_mix;                                         \
    float u = p->lpb, lpj = p->lpj, dc_in = p->dc_in, dc_out = p->dc_out, out_lp = p->out_lp;                         \
    float z1 = p->c_z1, z2 = p->c_z2;                                                                                \
    uint32_t w = p->wb, rng = p->rng, j;   /* the bore and the jet are written at the same index each sample */       \
    (void)cb0; (void)ca1; (void)ca2; (void)cmix; (void)z1; (void)z2;                                                 \
    for (j = 0; j < cr; j++) {                                                                                       \
        uint32_t rb = w - ib, rj = w - ij;                                                                           \
        float t, pd, pj, jt, in, y;                                                                                  \
        u += a_bore * (b0 * d[(rb + 1u) & mask] + b1 * d[rb & mask] + b2 * d[(rb - 1u) & mask] +                     \
                       b3 * d[(rb - 2u) & mask] - u);                                                                \
        dc_out = u - dc_in + k_dc * dc_out;                                                                          \
        dc_in = u;                                                                                                   \
        t = dc_out;                                                                                                  \
        rng = rng * 1664525u + 1013904223u;                                                                          \
        pd = breath + bnoise * (float)(int32_t)rng - JET_REFL * t;                                                   \
        w = (w + 1u) & mask;                                                                                         \
        d[len + w] = pd;                                                                                             \
        pj = d[len + ((rj + 1u) & mask)];                                                                            \
        pj += jfr * (d[len + (rj & mask)] - pj);                                                                     \
        lpj += a_jet * (pj - lpj);                                                                                   \
        jt = lpj * (lpj * lpj - 1.0f);                                                                               \
        jt = jt > 1.0f ? 1.0f : jt < -1.0f ? -1.0f : jt;                                                             \
        in = jt + END_REFL * t;                                                                                      \
        d[w] = in + 1e-20f;                                                                                          \
        y = in + tap * t;                                                                                            \
        out_lp += a_out * (y - out_lp);                                                                              \
        y = out_lp;                                                                                                  \
        if (CHIM) {                                                                                                  \
            float c = cb0 * y + z1;                                                                                  \
            z1 = -ca1 * c + z2;                                                                                      \
            z2 = -cb0 * y - ca2 * c;                                                                                 \
            y += cmix * c;                                                                                           \
        }                                                                                                            \
        bus[j] += y * g;                                                                                             \
    }                                                                                                                \
    p->lpb = u; p->lpj = lpj; p->dc_in = dc_in; p->dc_out = dc_out; p->out_lp = out_lp;                              \
    p->c_z1 = z1; p->c_z2 = z2; p->wb = w; p->rng = rng;                                                             \
}
PIPE_RUN(pipe_run_plain, 0)
PIPE_RUN(pipe_run_chimney, 1)

/* the render's pipe budget: a pipe is started only while fewer than this sound (it follows the measured cost) */
static int pipe_cap = KIST_MAX_PIPES;
static int pipe_cap_now(void) { return pipes_active() < pipe_cap; }
static uint32_t cap_calm;                  /* blocks in a row with room to spare */

/* after each block: the time it took against its budget. Over 70 %: no more pipes than now; over 85 %: the least
 * important close; under 50 % for a second: one more allowed again, up to 48 */
static void budget(uint32_t ticks, uint32_t n, int np)
{
    uint32_t avail = (uint32_t)((float)n * (1e6f * (float)PLAT_TICKS_PER_US) / FS), pct = ticks * 100u / (avail ? avail : 1u);
    if (pct > 70u) {
        pipe_cap = np > 16 ? np : 16;
        cap_calm = 0;
        if (pct > 85u)
            shed_now();
    } else if (pct < 50u) {
        if (++cap_calm > 170u && pipe_cap < KIST_MAX_PIPES) {
            pipe_cap++;
            cap_calm = 150u;
        }
    } else {
        cap_calm = 0;
    }
    meter.load_pct = pct;
    meter.pipe_cap = (uint32_t)pipe_cap;
}

/* render one pipe into l/r (n frames at its own rate: 2n when oversampled): its valve, wind and smoothing every CR */
static void pipe_block(int i, float *l, float *r, uint32_t n)
{
    pipe_t *p = &pipes[i];
    uint32_t mul = p->os ? 2u : 1u, total = n * mul, cr = CR * mul, s, k = 0;
    float peak = 0.0f;
    for (s = 0; s < total; s += cr, k++) {
        float pf, jt, bt, a;
        frac_t qb, qj;
        if (p->closing) {
            p->valve -= p->close_step * (float)cr;
            if (p->valve < 0.0f)
                p->valve = 0.0f;
        } else if (p->valve < 1.0f) {
            p->valve += p->open_step * (float)cr;
            if (p->valve > 1.0f)
                p->valve = 1.0f;
        }
        pf = wind.p_sub[k] * p->valve;
        bt = FLUE_BREATH * pf * (1.0f + p->surge * p->valve);
        p->surge *= p->surge_dec;
        jt = pf > 0.30f ? p->jet_0 / fm_sqrtf(pf) : p->jet_0 * JET_STRETCH;
        if (jt > p->jet_0 * JET_STRETCH)
            jt = p->jet_0 * JET_STRETCH;
        p->noise = p->noise_base + p->chiff_now;
        p->chiff_now *= p->chiff_dec;
        p->bore_d += 0.1f * (p->bore_t - p->bore_d);
        p->gain += 0.25f * (p->gain_t - p->gain);
        frac_set(&qb, p->bore_d);
        qj.i = (uint32_t)(int32_t)jt;
        qj.c0 = jt - (float)(int32_t)jt;
        (p->c_mix > 0.0f ? pipe_run_chimney : pipe_run_plain)(p, (p->side ? r : l) + s, cr, bt,
                                                                bt * p->noise * (1.0f / 2147483648.0f), &qb, &qj, p->gain);
        a = fm_fabsf(p->out_lp) * p->gain;
        if (a > peak)
            peak = a;
        a = fm_fabsf(p->lpb) * p->gain;
        if (a > peak)
            peak = a;
    }
    if (p->closing && p->valve <= 0.0f) {
        if (peak < 3e-5f) {
            if (++p->quiet > 3u)
                p->dead = 1;               /* freed by the first core after the block (either core may run this) */
        } else {
            p->quiet = 0;
        }
    }
}

/* the second core's job, and the first's when it has none: the 2x pipes, then the decimation */

static void decimate(const float *in, float *hist, float *out, uint32_t n)
{
    uint32_t s, k;
    /* hist keeps the last HB_TAPS - 1 input samples before this block */
    memcpy(hist + HB_TAPS - 1, in, 2u * n * sizeof(float));
    for (s = 0; s < n; s++) {
        const float *x = hist + 2u * s;
        float acc = 0.5f * x[HB_TAPS / 2];
        for (k = 1; k < HB_TAPS / 2; k += 2)
            acc += hb_h[HB_TAPS / 2 + k] * (x[HB_TAPS / 2 + k] + x[HB_TAPS / 2 - k]);
        out[s] = acc;
    }
    for (s = 0; s < HB_TAPS - 1u; s++)   /* (no memmove in the firmware: the ranges do not overlap for n >= 15) */
        hist[s] = hist[s + 2u * n];
}

static float dry_l[BLOCK_MAX], dry_r[BLOCK_MAX];          /* this block's dry sum */
static float prev_l[BLOCK_MAX], prev_r[BLOCK_MAX];        /* the last block's, for the room on the second core */
static float wet_l[BLOCK_MAX], wet_r[BLOCK_MAX];          /* the room's output from the second core */
static float c2_l[BLOCK_MAX], c2_r[BLOCK_MAX];            /* the 1x pipes the second core took, by chest side */

static int finite_bounded(const float *a, uint32_t n)
{
    uint32_t s;
    for (s = 0; s < n; s++)
        if (!(a[s] == a[s]) || a[s] > 8.0f || a[s] < -8.0f)
            return 0;
    return 1;
}

/* the 2x pipes of this block, decimated into dec_l / dec_r */
static void os_pipes(uint32_t n)
{
    int i;
    memset(os_l, 0, 2u * n * sizeof(float));
    memset(os_r, 0, 2u * n * sizeof(float));
    for (i = 0; i < KIST_MAX_PIPES; i++)
        if (pipes[i].active && pipes[i].os)
            pipe_block(i, os_l, os_r, n);
    decimate(os_l, hb_l, dec_l, n);
    decimate(os_r, hb_r, dec_r, n);
}

/* the second core's job: this block's 2x pipes, and the (richer) room over the last block's sound. What it hands
 * back is checked before it reaches the speakers: a misread on a unit whose second core is unreliable must not */
static void cpu2_job(uint32_t arg)
{
    uint32_t n = job_n, m = job_prev_n;
    int i;
    (void)arg;
    if (os_count)
        os_pipes(n);
    memset(c2_l, 0, n * sizeof(float));
    memset(c2_r, 0, n * sizeof(float));
    for (i = 0; i < KIST_MAX_PIPES; i++)
        if (pipes[i].active && pipes[i].core2)
            pipe_block(i, c2_l, c2_r, n);
    memset(wet_l, 0, m * sizeof(float));
    memset(wet_r, 0, m * sizeof(float));
    reverb_process(prev_l, prev_r, wet_l, wet_r, m);
}

void organ_render(float *l, float *r, uint32_t n)
{
    uint32_t s, t0 = plat_ticks24();
    int i, use2, np = 0, nos = 0, handed;
    if (n > BLOCK_MAX)
        n = BLOCK_MAX;
    if (par_new) {
        organ_params_t np_ = par_buf[par_cur];
        int tun = tuning_differs(&np_, &P), room = np_.room != P.room || np_.mix != P.mix ||
                  np_.distance != P.distance || np_.decay_ds != P.decay_ds;
        par_new = 0;
        P = np_;
        if (tun)
            retune();
        if (room)
            room_now(rich_now);
    }
    if (voi_new) {
        voi_new = 0;
        V = voicing_buf[voi_cur];
        for (i = 0; i < KIST_MAX_PIPES; i++)
            if (pipes[i].active && pipes[i].note >= 0)
                pipes[i].gain_t = reg_gain(pipes[i].stop, notes[pipes[i].note].key);
    }
    events();
    if (shed_req) {
        shed_req = 0;
        shed_now();
    }
    for (i = 0; i < KIST_MAX_NOTES; i++) { /* a released note whose pipes have all gone is free again */
        int k, any = 0;
        if (!notes[i].used || notes[i].held)
            continue;
        for (k = 0; k < NSTOPS; k++)
            any |= notes[i].pipe[k] >= 0;
        if (!any)
            notes[i].used = 0;
    }
    wind_block(n);
    use2 = plat_cpu2_on();
    if (use2 != rich_now) {                /* the richer room while the second core runs it; the plain one without */
        room_now(use2);
        job_prev_n = 0;
    }
    for (i = 0; i < KIST_MAX_PIPES; i++)
        if (pipes[i].active) {
            np++;
            nos += pipes[i].os;
        }
    os_count = nos;
    job_n = n;
    {   /* the second core's share of the 1x pipes: enough that both cores have about as much to do (a 2x pipe counts
         * twice, the room about six) */
        int n1 = np - nos, give = use2 ? (n1 - 2 * nos - 6) / 2 : 0, k = 0;
        if (give < 0)
            give = 0;
        for (i = 0; i < KIST_MAX_PIPES; i++) {
            pipes[i].core2 = 0;
            if (pipes[i].active && !pipes[i].os && k < give && (i & 1)) {
                pipes[i].core2 = 1;
                k++;
            }
        }
        for (i = 0; i < KIST_MAX_PIPES && k < give; i++)
            if (pipes[i].active && !pipes[i].os && !pipes[i].core2) {
                pipes[i].core2 = 1;
                k++;
            }
    }
    memset(dry_l, 0, n * sizeof(float));
    memset(dry_r, 0, n * sizeof(float));
    handed = use2 && plat_cpu2_run(cpu2_job, 0);
    for (i = 0; i < KIST_MAX_PIPES; i++)   /* the first core: its 1x pipes (all of them without the second) */
        if (pipes[i].active && !pipes[i].os && (!handed || !pipes[i].core2))
            pipe_block(i, dry_l, dry_r, n);
    if (handed && plat_cpu2_wait() != 0) {
        handed = 0;                        /* given up and the job dropped: the first core does its pipes now */
        job_prev_n = 0;
        for (i = 0; i < KIST_MAX_PIPES; i++)
            if (pipes[i].active && pipes[i].core2) {
                pipes[i].core2 = 0;
                pipe_block(i, dry_l, dry_r, n);
            }
    }
    if (handed) {                          /* the second core's 1x pipes, checked like the rest of what it hands back */
        if (finite_bounded(c2_l, n) && finite_bounded(c2_r, n)) {
            for (s = 0; s < n; s++) {
                dry_l[s] += c2_l[s];
                dry_r[s] += c2_r[s];
            }
        } else {
            for (i = 0; i < KIST_MAX_PIPES; i++)
                if (pipes[i].active && pipes[i].core2)
                    pipes[i].dead = 1;
        }
    }
    for (i = 0; i < KIST_MAX_PIPES; i++)   /* the pipes that died this block, freed here (one core only) */
        if (pipes[i].active && pipes[i].dead)
            pipe_free(i);
    if (nos) {
        if (!handed)
            os_pipes(n);                   /* no second core: the 2x pipes that are left, here */
        if (!finite_bounded(dec_l, n) || !finite_bounded(dec_r, n)) {
            for (i = 0; i < KIST_MAX_PIPES; i++)
                if (pipes[i].active && pipes[i].os)
                    pipe_free(i);          /* their state is not to be trusted: they stop (the first core frees them) */
            memset(dec_l, 0, n * sizeof(float));
            memset(dec_r, 0, n * sizeof(float));
        }
        for (s = 0; s < n; s++) {
            dry_l[s] += dec_l[s];
            dry_r[s] += dec_r[s];
        }
    }
    {   /* the two chest sides: their DC out (once per side, not per pipe), then panned C side left, C# side right */
        static float hx[2], hy[2];
        const float a = 1.0f - FM_TWO_PI * 18.0f / FS, gn = 0.8367f, gf = 0.5477f;
        for (s = 0; s < n; s++) {
            float c = dry_l[s], d = dry_r[s], vc = c - hx[0] + a * hy[0], vd = d - hx[1] + a * hy[1];
            hx[0] = c;
            hy[0] = vc;
            hx[1] = d;
            hy[1] = vd;
            dry_l[s] = gn * vc + gf * vd;
            dry_r[s] = gf * vc + gn * vd;
        }
    }
    {   /* the dry sound and the blower's own noise, quiet, while it turns */
        float lvl = P.blower_noise ? 0.0009f * fm_exp2f(0.55f * (float)P.blower_noise) * wind.spin : 0.0f;
        float dry = 1.0f - 0.35f * 0.01f * (float)P.mix;
        for (s = 0; s < n; s++) {
            float z = rnd(&wind.rng);
            wind.noise_lp1 += 0.03f * (z - wind.noise_lp1);
            wind.noise_lp2 += 0.12f * (wind.noise_lp1 - wind.noise_lp2);
            l[s] = dry * dry_l[s] + lvl * wind.noise_lp2;
            r[s] = dry * dry_r[s] + lvl * wind.noise_lp1;
        }
    }
    if (handed) {                          /* the room the second core made from the last block (5.8 ms later) */
        uint32_t m = job_prev_n < n ? job_prev_n : n;
        if (finite_bounded(wet_l, m) && finite_bounded(wet_r, m)) {
            for (s = 0; s < m; s++) {
                l[s] += wet_l[s];
                r[s] += wet_r[s];
            }
        } else {
            reverb_clear();
        }
        memcpy(prev_l, dry_l, n * sizeof(float));
        memcpy(prev_r, dry_r, n * sizeof(float));
        job_prev_n = n;
    } else {
        reverb_process(dry_l, dry_r, l, r, n);
    }
    budget(plat_ticks24() - t0, n, np);
    meter.pipes = (uint32_t)np;
    if ((uint32_t)np > meter.pipes_peak)
        meter.pipes_peak = (uint32_t)np;
    meter.os_pipes = (uint32_t)nos;
    meter.rich_reverb = (uint32_t)rich_now;
    {
        uint32_t lo = 0, hi = 0, nn = 0;
        for (i = 0; i < KIST_MAX_NOTES; i++)
            if (notes[i].used) {
                int k = notes[i].key - KIST_KEY_LO;
                nn++;
                if (k < 32)
                    lo |= 1u << k;
                else
                    hi |= 1u << (k - 32);
            }
        meter.sounding_lo = lo;
        meter.sounding_hi = hi;
        meter.notes = nn;
    }
}

/* ------------------------------------------------------------------------------------- probe (tools) --- */
/* one pipe alone at steady wind, for the calibration and the tests: returns its measured frequency. Not in the
 * firmware (its 32 KB buffer would not fit beside the organ) */
#ifdef PLAT_HOST
static float probe_buf[2u * 4096u];
float organ_probe_run(int stop, float hz, float fs_mul, int use_cal, float seconds, float *out, uint32_t nout)
{
    pipe_t p;
    float fs = FS * fs_mul, lp1 = 0.0f, lp2 = 0.0f, prev = 0.0f, a, t_first = -1.0f, t_last = 0.0f, hx = 0.0f, hy = 0.0f;
    float ahp = 1.0f - FM_TWO_PI * 18.0f / fs, bus[CR];
    uint32_t total = (uint32_t)(seconds * fs), from = total / 2u, s, len = 64, crossings = 0, j;
    frac_t qb, qj;
    memset(&p, 0, sizeof p);
    if (!pipe_shape(&p, stop, hz, fs, use_cal, 0))
        return 0.0f;
    while ((float)len < fm_maxf(p.bore_t * 1.06f, p.jet_t * JET_STRETCH) + 5.0f)
        len <<= 1;
    if (len > 4096u)
        return 0.0f;
    memset(probe_buf, 0, sizeof probe_buf);
    p.bore = probe_buf;
    p.jet = probe_buf + len;
    p.mask = len - 1u;
    p.rng = 0x1234567u;
    frac_set(&qb, p.bore_t);               /* the same code as the render, at steady wind */
    qj.i = (uint32_t)(int32_t)p.jet_t;
    qj.c0 = p.jet_t - (float)(int32_t)p.jet_t;
    a = one_pole(1.3f * hz, fs);
    for (s = 0; s < total; s += CR) {
        float v = s < (uint32_t)(0.01f * fs) ? (float)s / (0.01f * fs) : 1.0f, bt = FLUE_BREATH * v;
        memset(bus, 0, sizeof bus);
        (p.c_mix > 0.0f ? pipe_run_chimney : pipe_run_plain)(&p, bus, CR, bt, bt * STOP[stop].noise * (1.0f / 2147483648.0f),
                                                              &qb, &qj, STOP[stop].gain);
        for (j = 0; j < CR; j++) {
            float y = bus[j], h = y - hx + ahp * hy;
            hx = y;
            hy = h;
            if (out && s + j < nout)
                out[s + j] = h;
            lp1 += a * (h - lp1);          /* the fundamental, for the zero crossings */
            lp2 += a * (lp1 - lp2);
            if (s + j >= from && prev < 0.0f && lp2 >= 0.0f) {
                float t = (float)(s + j) - lp2 / (lp2 - prev);
                if (t_first < 0.0f)
                    t_first = t;
                t_last = t;
                crossings++;
            }
            prev = lp2;
        }
    }
    if (crossings < 3u)
        return 0.0f;
    return fs / ((t_last - t_first) / (float)(crossings - 1u));
}
#endif
