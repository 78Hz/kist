/* KIST-P — the plate (see plate.h): J. Dattorro's plate ("Effect Design, Part 1: Reverberator and Other Filters",
 * JAES 45(9), 1997) at half the rate, float, every parameter free.
 *
 * The drive: pre-delay, a bandwidth lowpass, four input diffusers. The plate (the tank): two loops, each a
 * modulated decay diffuser, a delay, the damping lowpass, the decay, a second (modulated) diffuser and a delay,
 * crossing into the other; the sound tapped from both loops at Dattorro's points. Then a low cut and the width.
 * The size scales the plate's delays and taps; a new size glides there (0.3 of the range a second), every read
 * fractional, so its tail bends in pitch as it goes (a plate grown or shrunk while it rings). The decay is
 * recomputed from the plate's length as it glides. Its lines 16-bit floating point: 50 KB at the largest size. */
#include "reverb.h"
#include "plate.h"
#include "rv_common.h"
#include "plat.h"
#define memset __builtin_memset

#define SR_D 29761.0f                      /* Dattorro's rate: his lengths are at it */
#define SZ_MIN 0.3f
#define SZ_MAX 1.3f
#define GLIDE 0.3f                         /* size units a second */
#define EXC_MAX 26.0f                      /* the motion's excursion at 100 %, samples (Dattorro's 16 at his rate: 62 %) */

/* the plate's lengths at 29761 Hz: decay diffuser 1, delay, decay diffuser 2, delay; left loop, then right */
static const int TL[8] = {672, 4453, 1800, 3720, 908, 4217, 2656, 3163};
static const int IL[4] = {142, 107, 379, 277};   /* the input diffusers (at size 1, fixed) */
/* capacities: at the largest size, at half the organ's rate (x 1.3 x 22058.8 / 29761 = x 0.96354), and room */
#define CAPF(n) ((n) * 96354 / 100000 + 60)   /* (+ the motion's full excursion, 2 x 26) */
#define CAPI(n) ((n) * 74119 / 100000 + 4)
#define PRE_CAP (100 * 2206 / 100 + 8)     /* 100 ms */
static const int TCAP[8] = {CAPF(672), CAPF(4453), CAPF(1800), CAPF(3720), CAPF(908), CAPF(4217), CAPF(2656), CAPF(3163)};
static const int ICAP[4] = {CAPI(142), CAPI(107), CAPI(379), CAPI(277)};
#define MEM_N (CAPF(672) + CAPF(4453) + CAPF(1800) + CAPF(3720) + CAPF(908) + CAPF(4217) + CAPF(2656) + CAPF(3163) + \
               CAPI(142) + CAPI(107) + CAPI(379) + CAPI(277) + PRE_CAP)
static uint16_t mem[MEM_N] PLAT_POOL;   /* 16-bit floating point (rv_common.h): its error fades with the sound */

static struct {
    hdl_t t[8], in[4], pre;
    int ilen[4], pre_n;
    float size, size_to;                   /* now, and where it glides */
    float decay, rt, damp, bw, bw_lp, lp[2], loopx;
    float g_in1, g_in2, g_d1, g_d2;
    float exc, hp_a, hp_l, hp_r, hpx_l, hpx_r, width, mix;
    ph_t mod[4];
    hb_t hb;
    plate_params_t want, have;
    volatile int pending;
    float cfg_mix;
    volatile int cfg_pending;
    float cfg_mix_want;
} P;

void plate_defaults(plate_params_t *p)     /* a medium plate, as Dattorro gives it */
{
    memset(p, 0, sizeof *p);
    p->size_pc = 100;
    p->decay_ds = 25;
    p->damp_k10 = 60;
    p->predelay_ms = 10;
    p->bandwidth_k10 = 80;
    p->in_diff = 100;
    p->dec_diff = 100;
    p->lowcut_h10 = 8;
    p->rate_ds = 10;
    p->depth = 60;
    p->width = 100;
}

int plate_params_ok(const plate_params_t *p)
{
    return p->size_pc >= 30 && p->size_pc <= 130 && p->decay_ds >= 2 && p->decay_ds <= 120 && p->damp_k10 >= 10 &&
           p->damp_k10 <= 100 && p->predelay_ms <= 100 && p->bandwidth_k10 >= 10 && p->bandwidth_k10 <= 100 &&
           p->in_diff <= 100 && p->dec_diff <= 100 && p->lowcut_h10 <= 50 && p->rate_ds >= 1 && p->rate_ds <= 30 &&
           p->depth <= 100 && p->width <= 100;
}

float plate_size_now(void) { return P.size; }

void reverb_clear(void)
{
    memset(mem, 0, sizeof mem);
    P.lp[0] = P.lp[1] = P.bw_lp = P.loopx = 0.0f;
    P.hp_l = P.hp_r = P.hpx_l = P.hpx_r = 0.0f;
    memset(&P.hb, 0, sizeof P.hb);
}

/* the decay a pass for an RT60 of rt, the plate's loop as long as it is now: the decay comes twice a half-loop */
static void decay_now(void)
{
    float loop = 0.0f;
    int i;
    for (i = 0; i < 4; i++)
        loop += (float)TL[i] * P.size * HR_FS / SR_D;
    P.decay = fm_exp2f(-3.321928f * 1.5f * loop / (P.rt * HR_FS));
    if (P.decay > 0.985f)
        P.decay = 0.985f;
}

static void apply(const plate_params_t *p)
{
    float in = 0.01f * (float)p->in_diff, dd = 0.01f * (float)p->dec_diff;
    int i;
    P.size_to = fm_clampf(0.01f * (float)p->size_pc, SZ_MIN, SZ_MAX);
    P.rt = 0.1f * (float)(p->decay_ds < 2 ? 2 : p->decay_ds);
    P.damp = fm_expf(-FM_TWO_PI * 100.0f * (float)p->damp_k10 / HR_FS);
    P.bw = 1.0f - fm_expf(-FM_TWO_PI * 100.0f * (float)p->bandwidth_k10 / HR_FS);
    P.g_in1 = 0.75f * in;                  /* Dattorro's: 0.75, 0.625; the decay's: 0.70, 0.50 */
    P.g_in2 = 0.625f * in;
    P.g_d1 = 0.70f * dd;
    P.g_d2 = 0.50f * dd;
    P.pre_n = 1 + (int)((float)p->predelay_ms * HR_FS / 1000.0f);
    if (P.pre_n > PRE_CAP - 2)
        P.pre_n = PRE_CAP - 2;
    P.exc = EXC_MAX * 0.01f * (float)p->depth;
    for (i = 0; i < 4; i++) {              /* the motion: two pairs, one at the rate, one a little off; its phase kept */
        float hz = 0.1f * (float)p->rate_ds * (i < 2 ? (i ? 0.81f : 1.0f) : (i == 2 ? 0.63f : 1.27f));
        P.mod[i].rc = fm_cosf(FM_TWO_PI * hz / HR_FS);
        P.mod[i].rs = fm_sinf(FM_TWO_PI * hz / HR_FS);
    }
    P.hp_a = p->lowcut_h10 ? fm_expf(-FM_TWO_PI * 10.0f * (float)p->lowcut_h10 / HR_FS) : 1.0f;
    P.width = 0.01f * (float)p->width;
    decay_now();
    P.have = *p;
}

/* -------------------------------------------------------------------------- reverb.h, for the engine --- */
void reverb_init(void)
{
    uint16_t *m = mem;
    int i;
    for (i = 0; i < 8; i++) {
        P.t[i].b = m;
        P.t[i].cap = TCAP[i];
        P.t[i].w = 0;
        m += TCAP[i];
    }
    for (i = 0; i < 4; i++) {
        P.in[i].b = m;
        P.in[i].cap = ICAP[i];
        P.in[i].w = 0;
        m += ICAP[i];
        P.ilen[i] = (int)((float)IL[i] * HR_FS / SR_D);
    }
    P.pre.b = m;
    P.pre.cap = PRE_CAP;
    P.pre.w = 0;
    for (i = 0; i < 4; i++)
        ph_init(&P.mod[i], 1.0f, 0.25f * (float)i, HR_FS);
    plate_defaults(&P.want);
    P.size = 0.01f * (float)P.want.size_pc;
    apply(&P.want);
    P.cfg_mix = 0.35f;
    reverb_clear();
}

void reverb_configure(const reverb_cfg_t *c)   /* the engine's room: only its mix is the plate's */
{
    P.cfg_mix_want = c->mix;
    P.cfg_pending = 1;
}

void plate_set(const plate_params_t *p)
{
    P.want = *p;
    P.pending = 1;
}

/* an allpass on a delay line (Schroeder): v = x + g d, out = d - g v; its delay fractional */
static inline float ap_f(hdl_t *d, float n, float g, float x)
{
    float z = hdl_tapf(d, n), v = x + g * z;
    hdl_write(d, v);
    return z - g * v;
}
static inline float ap_i(hdl_t *d, int n, float g, float x)
{
    float z = hdl_tap(d, n), v = x + g * z;
    hdl_write(d, v);
    return z - g * v;
}

static void step(float x, float *wl, float *wr)
{
    float s = P.size * HR_FS / SR_D, a, b, m0, m1, m2, m3, l, r;
    /* the drive */
    hdl_write(&P.pre, x);
    x = hdl_tap(&P.pre, P.pre_n);
    P.bw_lp += P.bw * (x - P.bw_lp);
    x = P.bw_lp;
    x = ap_i(&P.in[0], P.ilen[0], P.g_in1, x);
    x = ap_i(&P.in[1], P.ilen[1], P.g_in1, x);
    x = ap_i(&P.in[2], P.ilen[2], P.g_in2, x);
    x = ap_i(&P.in[3], P.ilen[3], P.g_in2, x);
    /* the plate */
    m0 = P.exc * (1.0f + ph_step(&P.mod[0]));
    m1 = P.exc * (1.0f + ph_step(&P.mod[1]));
    m2 = 0.6f * P.exc * (1.0f + ph_step(&P.mod[2]));
    m3 = 0.6f * P.exc * (1.0f + ph_step(&P.mod[3]));
    a = x + P.decay * P.loopx;
    a = ap_f(&P.t[0], (float)TL[0] * s + m0, -P.g_d1, a);
    hdl_write(&P.t[1], a);
    a = hdl_tapf(&P.t[1], (float)TL[1] * s);
    P.lp[0] = a + P.damp * (P.lp[0] - a);
    a = ap_f(&P.t[2], (float)TL[2] * s + m2, P.g_d2, P.lp[0] * P.decay);
    hdl_write(&P.t[3], a);
    b = x + P.decay * hdl_tapf(&P.t[3], (float)TL[3] * s);
    b = ap_f(&P.t[4], (float)TL[4] * s + m1, -P.g_d1, b);
    hdl_write(&P.t[5], b);
    b = hdl_tapf(&P.t[5], (float)TL[5] * s);
    P.lp[1] = b + P.damp * (P.lp[1] - b);
    b = ap_f(&P.t[6], (float)TL[6] * s + m3, P.g_d2, P.lp[1] * P.decay);
    hdl_write(&P.t[7], b);
    P.loopx = hdl_tapf(&P.t[7], (float)TL[7] * s);
    /* the sound, at Dattorro's taps */
    if (P.size != P.size_to) {             /* gliding: the taps slide with it */
#define T(i, n) hdl_tapf(&P.t[i], 1.0f + (float)(n) * s)
        l = T(5, 266) + T(5, 2974) - T(6, 1913) + T(7, 1996) - T(1, 1990) - T(2, 187) - T(3, 1066);
        r = T(1, 353) + T(1, 3627) - T(2, 1228) + T(3, 2673) - T(5, 2111) - T(6, 335) - T(7, 121);
#undef T
    } else {                               /* still: whole samples (half the reads) */
#define T(i, n) hdl_tap(&P.t[i], 1 + (int)((float)(n) * s + 0.5f))
        l = T(5, 266) + T(5, 2974) - T(6, 1913) + T(7, 1996) - T(1, 1990) - T(2, 187) - T(3, 1066);
        r = T(1, 353) + T(1, 3627) - T(2, 1228) + T(3, 2673) - T(5, 2111) - T(6, 335) - T(7, 121);
#undef T
    }
    {   /* the low cut, then the width (mid and side) */
        float hl = P.hp_a * (P.hp_l + l - P.hpx_l), hr = P.hp_a * (P.hp_r + r - P.hpx_r), mid, side;
        P.hpx_l = l;
        P.hpx_r = r;
        P.hp_l = hl;
        P.hp_r = hr;
        mid = 0.5f * (hl + hr);
        side = 0.5f * (hl - hr) * P.width;
        *wl = P.cfg_mix * 0.6f * (mid + side);
        *wr = P.cfg_mix * 0.6f * (mid - side);
    }
}

void reverb_process(const float *in_l, const float *in_r, float *out_l, float *out_r, uint32_t n)
{
    uint32_t s;
    int i;
    if (P.pending) {
        P.pending = 0;
        apply(&P.want);
    }
    if (P.cfg_pending) {
        P.cfg_pending = 0;
        P.cfg_mix = fm_clampf(P.cfg_mix_want, 0.0f, 1.0f);
    }
    if (P.cfg_mix <= 0.0f)
        return;
    if (P.size != P.size_to) {             /* the glide: this block's share */
        float step = GLIDE * (float)n / KIST_FS, d = P.size_to - P.size;
        P.size = fm_fabsf(d) <= step ? P.size_to : P.size + (d > 0.0f ? step : -step);
        decay_now();
    }
    for (i = 0; i < 4; i++)
        ph_norm(&P.mod[i]);
    for (s = 0; s < n; s++) {
        float x = hb_down(&P.hb, 0.5f * (in_l[s] + in_r[s])), l, r;
        if (P.hb.ph) {
            float wl, wr;
            step(x, &wl, &wr);
            hb_push(&P.hb, wl, wr);
        }
        hb_up(&P.hb, P.hb.ph, &l, &r);
        out_l[s] += l;
        out_r[s] += r;
        P.hb.ph ^= 1;
    }
    P.lp[0] = fm_flush(P.lp[0]);
    P.lp[1] = fm_flush(P.lp[1]);
    P.bw_lp = fm_flush(P.bw_lp);
}
