/* KIST — the room (see reverb.h). The delay lines are 16-bit (the FM-1 has 512 KB of RAM in all), the rest float. */
#include "reverb.h"
#include "organ.h"
#include "fastmath.h"
#include "plat.h"
#include <string.h>

#define RV_FS KIST_FS
#define RV_SIZE_MAX 1.3f
#define RV_MOD_MAX 12                      /* samples of modulation either way */
#define RV_Q 8192.0f                       /* line scaling: +-4 full scale before the 16 bits saturate */
#define ER_CAP 6144                        /* 139 ms: the longest early tap plus pre-delay */
#define NAP 4                              /* input diffusers (2 plain, 4 rich) */

static const float LINE_MS[RV_LINES] = {29.7f, 37.1f, 41.1f, 43.7f, 53.3f, 59.9f, 67.7f, 73.1f};
/* a line's capacity at the largest size: tenths of a ms x 1.3 x 44.12 samples a ms, plus the modulation's room */
#define CAP(ms10) ((ms10) * 13 * 4412 / 100000 + 2 * RV_MOD_MAX + 8)
static const int LINE_CAP[RV_LINES] = {CAP(297), CAP(371), CAP(411), CAP(437), CAP(533), CAP(599), CAP(677), CAP(731)};
#define LINES_TOTAL (CAP(297) + CAP(371) + CAP(411) + CAP(437) + CAP(533) + CAP(599) + CAP(677) + CAP(731))
static int16_t lines[LINES_TOTAL] PLAT_POOL;
static int16_t er_buf[ER_CAP] PLAT_POOL;
static const int AP_LEN[NAP] = {142, 107, 379, 277};
static float ap_buf[142 + 107 + 379 + 277] PLAT_POOL;

/* early taps: time (ms at size 1), gain, side (-1 left .. +1 right) */
static const float ER_T[12] = {7.1f, 11.3f, 17.9f, 23.3f, 31.7f, 41.9f, 5.3f, 13.7f, 19.6f, 27.1f, 36.2f, 47.3f};
static const float ER_G[12] = {0.55f, 0.48f, 0.42f, 0.36f, 0.30f, 0.25f, 0.40f, 0.36f, 0.31f, 0.27f, 0.22f, 0.18f};
static const float ER_P[12] = {-0.8f, 0.7f, -0.4f, 0.9f, -0.9f, 0.5f, 0.6f, -0.6f, 0.8f, -0.3f, 0.4f, -0.7f};
/* the lines' signs into the left and right outputs, and from the input */
static const float SL[RV_LINES] = {1, -1, 1, -1, 1, -1, 1, -1};
static const float SR[RV_LINES] = {1, 1, -1, -1, 1, 1, -1, -1};
static const float SI[RV_LINES] = {1, -1, -1, 1, 1, 1, -1, -1};
static const float MOD_HZ[RV_LINES] = {0.31f, 0.0f, 0.43f, 0.37f, 0.53f, 0.0f, 0.67f, 0.0f};

static struct {
    int16_t *line[RV_LINES];
    int len[RV_LINES], w[RV_LINES];
    float g[RV_LINES], lp[RV_LINES], a_damp;
    float mod_ph[RV_LINES], mod_inc[RV_LINES], mod_depth[RV_LINES];
    int er_w, er_n, er_d[12], fdn_d;
    float er_gl[12], er_gr[12];
    int ap_w[NAP], nap;
    float *ap[NAP];
    float mix;
    reverb_cfg_t want, have;
    volatile int pending;
} rv;

static inline __attribute__((always_inline)) int16_t sat16(float x)
{
    x *= RV_Q;
    if (x > 32767.0f)
        x = 32767.0f;
    if (x < -32768.0f)
        x = -32768.0f;
    return (int16_t)x;
}

void reverb_clear(void)
{
    memset(lines, 0, sizeof lines);
    memset(er_buf, 0, sizeof er_buf);
    memset(ap_buf, 0, sizeof ap_buf);
    memset(rv.lp, 0, sizeof rv.lp);
}

static void apply(const reverb_cfg_t *c)
{
    int i, n;
    float size = fm_clampf(c->size, 0.2f, RV_SIZE_MAX), rt = fm_clampf(c->rt60, 0.2f, 12.0f);
    int new_room = c->size != rv.have.size;
    for (i = 0; i < RV_LINES; i++) {
        int len = (int)(LINE_MS[i] * size * RV_FS / 1000.0f);
        float rich_mod = c->rich ? 1.0f : 0.0f;
        if (len > LINE_CAP[i] - 2 * RV_MOD_MAX - 4)
            len = LINE_CAP[i] - 2 * RV_MOD_MAX - 4;
        rv.len[i] = len;
        /* the gain for an RT60 of rt: -60 dB after rt seconds, so -60 L/(rt fs) dB per pass */
        rv.g[i] = fm_exp2f(-3.321928f * 3.0f * (float)len / (rt * RV_FS));
        /* plain: lines 0 and 4 modulated; rich: 0, 2, 4, 6 and 3 */
        rv.mod_depth[i] = 0.0f;
        if (i == 0 || i == 4)
            rv.mod_depth[i] = c->rich ? 9.0f : 6.0f;
        else if (MOD_HZ[i] > 0.0f)
            rv.mod_depth[i] = 8.0f * rich_mod;
        rv.mod_inc[i] = MOD_HZ[i] > 0.0f ? MOD_HZ[i] / RV_FS : 0.0f;
    }
    rv.a_damp = 1.0f - fm_expf(-FM_TWO_PI * fm_clampf(c->damp_hz, 500.0f, 16000.0f) / RV_FS);
    n = c->rich ? 12 : 6;
    rv.er_n = n;
    for (i = 0; i < n; i++) {
        int d = (int)((ER_T[i] * size + c->predelay_ms) * RV_FS / 1000.0f);
        float pan = ER_P[i], g = ER_G[i] * c->er_gain;
        if (d >= ER_CAP)
            d = ER_CAP - 1;
        if (d < 1)
            d = 1;
        rv.er_d[i] = d;
        rv.er_gl[i] = g * fm_sqrtf(0.5f * (1.0f - pan));
        rv.er_gr[i] = g * fm_sqrtf(0.5f * (1.0f + pan));
    }
    rv.fdn_d = (int)((c->predelay_ms + 9.0f * size) * RV_FS / 1000.0f);
    if (rv.fdn_d >= ER_CAP)
        rv.fdn_d = ER_CAP - 1;
    if (rv.fdn_d < 1)
        rv.fdn_d = 1;
    rv.nap = c->rich ? 4 : 2;
    rv.mix = fm_clampf(c->mix, 0.0f, 1.0f);
    if (new_room)
        reverb_clear();
    rv.have = *c;
}

void reverb_init(void)
{
    int i, off = 0;
    for (i = 0; i < RV_LINES; i++) {
        rv.line[i] = lines + off;
        off += LINE_CAP[i];
        rv.w[i] = 0;
        rv.mod_ph[i] = 0.13f * (float)i;
    }
    off = 0;
    for (i = 0; i < NAP; i++) {
        rv.ap[i] = ap_buf + off;
        off += AP_LEN[i];
        rv.ap_w[i] = 0;
    }
    rv.have.size = -1.0f;
    reverb_clear();
    rv.want.size = 0.6f;
    rv.want.rt60 = 1.6f;
    rv.want.damp_hz = 6000.0f;
    rv.want.er_gain = 0.6f;
    rv.want.predelay_ms = 8.0f;
    rv.want.mix = 0.35f;
    rv.want.rich = 0;
    apply(&rv.want);
}

void reverb_configure(const reverb_cfg_t *c)
{
    rv.want = *c;
    rv.pending = 1;                        /* the render takes it at its next block */
}

static inline __attribute__((always_inline)) float line_read(int i)
{
    int16_t *b = rv.line[i];
    int cap = LINE_CAP[i];
    float d = (float)rv.len[i];
    if (rv.mod_depth[i] > 0.0f) {
        float ph = rv.mod_ph[i];
        d += rv.mod_depth[i] * fm_sinf(FM_TWO_PI * ph);
        ph += rv.mod_inc[i];
        if (ph >= 1.0f)
            ph -= 1.0f;
        rv.mod_ph[i] = ph;
        {
            int di = (int)d;
            float fr = d - (float)di;
            int r0 = rv.w[i] - di, r1 = r0 - 1;
            if (r0 < 0)
                r0 += cap;
            if (r1 < 0)
                r1 += cap;
            return ((float)b[r0] + fr * (float)(b[r1] - b[r0])) * (1.0f / RV_Q);
        }
    } else {
        int r = rv.w[i] - rv.len[i];
        if (r < 0)
            r += cap;
        return (float)b[r] * (1.0f / RV_Q);
    }
}

void reverb_process(const float *in_l, const float *in_r, float *out_l, float *out_r, uint32_t n)
{
    uint32_t s;
    int i;
    if (rv.pending) {
        rv.pending = 0;
        apply(&rv.want);
    }
    if (rv.mix <= 0.0f)
        return;
    for (s = 0; s < n; s++) {
        float x = 0.5f * (in_l[s] + in_r[s]), el = 0.0f, er = 0.0f, o[RV_LINES], h[RV_LINES], yl = 0.0f, yr = 0.0f;
        int k;
        /* the early reflections and the pre-delayed feed into the network, from one mono line */
        er_buf[rv.er_w] = sat16(x);
        for (k = 0; k < rv.er_n; k++) {
            int r = rv.er_w - rv.er_d[k];
            float v;
            if (r < 0)
                r += ER_CAP;
            v = (float)er_buf[r] * (1.0f / RV_Q);
            el += v * rv.er_gl[k];
            er += v * rv.er_gr[k];
        }
        {
            int r = rv.er_w - rv.fdn_d;
            if (r < 0)
                r += ER_CAP;
            x = (float)er_buf[r] * (1.0f / RV_Q);
        }
        if (++rv.er_w >= ER_CAP)
            rv.er_w = 0;
        /* input diffusion: allpasses */
        for (k = 0; k < rv.nap; k++) {
            float *b = rv.ap[k];
            int w = rv.ap_w[k];
            float d = b[w], v = x + 0.6f * d;
            b[w] = v;
            x = d - 0.6f * v;
            if (++w >= AP_LEN[k])
                w = 0;
            rv.ap_w[k] = w;
        }
        /* the network: read, damp, mix (Hadamard), write */
        for (i = 0; i < RV_LINES; i++) {
            o[i] = line_read(i);
            rv.lp[i] += rv.a_damp * (o[i] - rv.lp[i]);
            h[i] = rv.lp[i] * rv.g[i];
            yl += o[i] * SL[i];
            yr += o[i] * SR[i];
        }
        {
            float a0 = h[0] + h[1], a1 = h[0] - h[1], a2 = h[2] + h[3], a3 = h[2] - h[3];
            float a4 = h[4] + h[5], a5 = h[4] - h[5], a6 = h[6] + h[7], a7 = h[6] - h[7];
            float b0 = a0 + a2, b1 = a1 + a3, b2 = a0 - a2, b3 = a1 - a3;
            float b4 = a4 + a6, b5 = a5 + a7, b6 = a4 - a6, b7 = a5 - a7;
            h[0] = b0 + b4; h[1] = b1 + b5; h[2] = b2 + b6; h[3] = b3 + b7;
            h[4] = b0 - b4; h[5] = b1 - b5; h[6] = b2 - b6; h[7] = b3 - b7;
        }
        for (i = 0; i < RV_LINES; i++) {
            int w = rv.w[i];
            rv.line[i][w] = sat16(h[i] * 0.35355339f + x * SI[i] * 0.5f);
            if (++w >= LINE_CAP[i])
                w = 0;
            rv.w[i] = w;
        }
        out_l[s] += rv.mix * (el + 0.30f * yl);
        out_r[s] += rv.mix * (er + 0.30f * yr);
    }
}
