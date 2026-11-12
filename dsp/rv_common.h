/* KIST's rooms (dsp/reverb.c, dsp/plate.c): the half rate, its halfbands, delay lines (float, and 16-bit floating
 * point), a slow phasor, a hash. Included by each; everything static inline. */
#pragma once
#include <stdint.h>
#include "fastmath.h"
#include "organ.h"

#define HR_FS (0.5f * KIST_FS)             /* the rooms' own rate: half the organ's */

/* the halfband (15 taps, Kaiser 6: flat to 7 kHz, -3.6 dB at 10 kHz, -38 at 16): its odd taps either side of 0.5 */
#define HB1 0.300794f
#define HB3 (-0.06271f)
#define HB5 0.012712f
#define HB7 (-0.000676f)
/* its histories doubled (each sample written twice, 15 or 8 apart), so a window is always contiguous: no shifting */
typedef struct { float d[30], ul[16], ur[16]; int p, q, ph; } hb_t;
static inline void hb_in(hb_t *h, float x)          /* a full-rate sample into the decimator */
{
    int p = h->p ? h->p - 1 : 14;
    h->d[p] = h->d[p + 15] = x;
    h->p = p;
}
static inline float hb_down_out(const hb_t *h)      /* the decimated sample (its window newest first) */
{
    const float *d = h->d + h->p;
    return 0.5f * d[7] + HB1 * (d[6] + d[8]) + HB3 * (d[4] + d[10]) + HB5 * (d[2] + d[12]) + HB7 * (d[0] + d[14]);
}
static inline float hb_down(hb_t *h, float x)       /* (both: a sample in, the decimated one out) */
{
    hb_in(h, x);
    return hb_down_out(h);
}
static inline void hb_push(hb_t *h, float l, float r)
{
    int q = h->q ? h->q - 1 : 7;
    h->ul[q] = h->ul[q + 8] = l;
    h->ur[q] = h->ur[q + 8] = r;
    h->q = q;
}
static inline void hb_up(const hb_t *h, int even, float *l, float *r)
{
    const float *a = h->ul + h->q, *b = h->ur + h->q;
    if (even) {
        *l = a[4];
        *r = b[4];
        return;
    }
    *l = 2.0f * (HB1 * (a[3] + a[4]) + HB3 * (a[2] + a[5]) + HB5 * (a[1] + a[6]) + HB7 * (a[0] + a[7]));
    *r = 2.0f * (HB1 * (b[3] + b[4]) + HB3 * (b[2] + b[5]) + HB5 * (b[1] + b[6]) + HB7 * (b[0] + b[7]));
}

typedef struct { float *b; int cap, w; } dl_t;
static inline void dl_write(dl_t *d, float x) { d->b[d->w] = x; if (++d->w >= d->cap) d->w = 0; }
static inline float dl_tap(const dl_t *d, int n)   /* n samples back (1 = the last written) */
{
    int r = d->w - n;
    if (r < 0) r += d->cap;
    return d->b[r];
}
static inline float dl_tapf(const dl_t *d, float n)
{
    int i = (int)n;
    float f = n - (float)i, a = dl_tap(d, i), b = dl_tap(d, i + 1);
    return a + f * (b - a);
}

/* a slow phasor: cos, sin turned by a fixed step each sample */
typedef struct { float c, s, rc, rs; } ph_t;
static inline void ph_init(ph_t *p, float hz, float phase, float fs)
{
    p->c = fm_cosf(FM_TWO_PI * phase); p->s = fm_sinf(FM_TWO_PI * phase);
    p->rc = fm_cosf(FM_TWO_PI * hz / fs); p->rs = fm_sinf(FM_TWO_PI * hz / fs);
}
static inline float ph_step(ph_t *p)
{
    float c = p->c * p->rc - p->s * p->rs, s = p->s * p->rc + p->c * p->rs;
    p->c = c; p->s = s;
    return s;
}
static inline void ph_norm(ph_t *p) { float m = 1.5f - 0.5f * (p->c * p->c + p->s * p->s); p->c *= m; p->s *= m; }

static uint32_t rv_hash(uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }

/* a 16-bit floating-point delay line (IEEE half: sign, 5-bit exponent, 10-bit mantissa, rounded): 11 bits of precision
 * at any level, its error proportional to the signal and fading with it. Values scaled by 2^12 into the half's range
 * on the way (x 2^-112 x 2^12 rebiases the float's exponent to the half's, then a shift): a multiply and a few
 * integer operations each way, no branch. Range: 1.5e-8 (-156 dB; below, the half's subnormals) to 15.9 (held). */
typedef struct { uint16_t *b; int cap, w; } hdl_t;
static inline uint16_t h_enc(float x)
{
    union { float f; uint32_t u; } v;
    uint32_t sign;
    v.f = x;
    sign = (v.u >> 16) & 0x8000u;
    v.u &= 0x7FFFFFFFu;
    v.f = fm_minf(v.f, 15.9f) * 7.8886091e-31f;   /* 2^-112 (the exponents' bias) x 2^12 (the scaling) */
    v.u += 0x1000u;                         /* round the mantissa at its 10th bit (a carry into the exponent is right) */
    return (uint16_t)(sign | (v.u >> 13));
}
static inline float h_dec(uint16_t h)
{
    union { float f; uint32_t u; } v;
    v.u = (uint32_t)(h & 0x7FFFu) << 13;
    v.f *= 1.2676506e30f;                   /* 2^112 x 2^-12 */
    v.u |= (uint32_t)(h & 0x8000u) << 16;
    return v.f;
}
static inline void hdl_write(hdl_t *d, float x) { d->b[d->w] = h_enc(x); if (++d->w >= d->cap) d->w = 0; }
static inline float hdl_tap(const hdl_t *d, int n) { int r = d->w - n; if (r < 0) r += d->cap; return h_dec(d->b[r]); }
static inline float hdl_tapf(const hdl_t *d, float n)
{
    int i = (int)n;
    float f = n - (float)i, a = hdl_tap(d, i), b = hdl_tap(d, i + 1);
    return a + f * (b - a);
}
