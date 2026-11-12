/* KIST — the room (see reverb.h): a 16-line feedback delay network (Jot) at half the rate, its lines 16-bit floating
 * point. Each line: a delay that wanders slowly (two phasors at its own rates, so its modes never ring as fixed
 * pitches), an allpass inside the loop (the echoes multiply each pass), and an absorbent filter designed for its own
 * length, so every line loses the same dB a second at 500 Hz (the room's RT60) and at 4 kHz (shorter, by the room's
 * damp_hz). Mixed by a 16 x 16 Hadamard. Fed through four diffusers from the pre-delayed sound and from the early
 * reflections: velvet noise (24 sparse random taps a side over 70 ms x the size, a float line), darkened.
 *
 * Why so: the earlier room (8 lines, 0.5 s of delay, 16-bit fixed, two lines modulated) left its modes ringing
 * apart (overlap 0.25 in a 4.5 s nave, a room's is over 3), its highs dying in 0.2 s, and its 16-bit truncation
 * cutting its tails off with a grit above 8 kHz. Half the rate is no loss: every room's tail is damped below
 * 7.5 kHz. Memory at the largest size: 31400 line samples (61 KB) and the early line (10 KB). */
#include "reverb.h"
#include "rv_common.h"
#include "plat.h"
#define memset __builtin_memset

#define NL RV_LINES
#define SZ_MAX 1.3f
#define NV 24                              /* early taps a side */
#define ER_MS 70.0f
#define PRE_MAX 0.045f
#define MOD_MAX 10                         /* samples, at the half rate */
#define RV_WET 2.0f                        /* the wet's level: the rooms' mixes as they were */
#define BLK_HALF 160                       /* half-rate samples a call at most (the organ's 256-frame blocks: 128) */

/* the lines, ms at size 1: spread 1.07x apart, nudged off any common period; the allpass inside each its own length */
static const float LMS[NL] = {31.3f, 33.7f, 36.1f, 38.9f, 41.3f, 44.3f, 47.9f, 51.1f, 54.7f, 58.3f, 62.9f, 67.1f, 71.9f, 76.7f, 82.3f, 88.1f};
static const float AMS[NL] = {4.1f, 5.3f, 3.7f, 6.1f, 4.7f, 3.3f, 5.9f, 4.3f, 6.7f, 3.9f, 5.1f, 4.9f, 3.1f, 6.3f, 5.7f, 4.5f};
static const float DMS[4] = {4.77f, 3.60f, 12.73f, 9.31f};   /* the input diffusers (Dattorro's lengths) */
static const float DG[4] = {0.75f, 0.75f, 0.625f, 0.625f};
/* capacities, from tenths of a ms (integers: the array's size is a constant): x 1.3 x 2.206 samples a tenth */
#define CAPT(t10) ((t10) * 13 * 2206 / 10000)
#define CAPL(t10) (CAPT(t10) + MOD_MAX + 4)   /* (the wander's reach beyond the line's length) */
#define CAPA(t10) (CAPT(t10) + 4)
static const int LINE_CAP[NL] = {CAPL(313), CAPL(337), CAPL(361), CAPL(389), CAPL(413), CAPL(443), CAPL(479), CAPL(511),
                                 CAPL(547), CAPL(583), CAPL(629), CAPL(671), CAPL(719), CAPL(767), CAPL(823), CAPL(881)};
#define PRE_CAP (450 * 2206 / 1000)        /* 45 ms: the most a room asks (16 + 1.2 x 20 m) */
#define ER_CAP (CAPT(700) + 8)
#define MEM_N (CAPL(313) + CAPL(337) + CAPL(361) + CAPL(389) + CAPL(413) + CAPL(443) + CAPL(479) + CAPL(511) +        \
               CAPL(547) + CAPL(583) + CAPL(629) + CAPL(671) + CAPL(719) + CAPL(767) + CAPL(823) + CAPL(881) +        \
               CAPA(67) * NL + 4 * CAPA(68) + PRE_CAP)
static uint16_t mem[MEM_N] PLAT_POOL;
static float er_mem[ER_CAP] PLAT_POOL;   /* the early line float: 80 taps a step read it */

static struct {
    hdl_t line[NL], apl[NL], dif[4], pre;
    dl_t er;
    int len[NL], alen[NL], dlen[4], pre_n;
    float g[NL], a[NL], lp[NL], depth[NL];
    ph_t m1[NL];
    int er_d[2][NV];
    float er_g[2][NV], er_lp[2], er_a;
    float mix;
    hb_t hb;
    reverb_cfg_t want, have;
    volatile int pending;
} B;

void reverb_clear(void)
{
    memset(mem, 0, sizeof mem);
    memset(er_mem, 0, sizeof er_mem);
    memset(B.lp, 0, sizeof B.lp);
    B.er_lp[0] = B.er_lp[1] = 0.0f;
    memset(&B.hb, 0, sizeof B.hb);
}

/* a line's absorbent filter: y = g x - a y1; its gain at w is g / |1 + a e^-jw| */
static float mag(float a, float w) { return 1.0f / fm_sqrtf(1.0f + 2.0f * a * fm_cosf(w) + a * a); }
static void absorb(int n, float t_lo, float t_hi, float *pa, float *pg)
{
    float wl = FM_TWO_PI * 500.0f / HR_FS, wh = FM_TWO_PI * 4000.0f / HR_FS;
    float gl = fm_exp2f(-3.321928f * 3.0f * (float)n / (t_lo * HR_FS)), gh = fm_exp2f(-3.321928f * 3.0f * (float)n / (t_hi * HR_FS));
    float want = gh / gl, lo = -0.95f, hi = 0.0f, a = 0.0f;
    int i;
    for (i = 0; i < 30; i++) {
        a = 0.5f * (lo + hi);
        if (mag(a, wh) / mag(a, wl) > want)
            hi = a;
        else
            lo = a;
    }
    *pa = a;
    *pg = gl / mag(a, wl);
}

static void apply(const reverb_cfg_t *c)
{
    float size = fm_clampf(c->size, 0.3f, SZ_MAX), rt = fm_clampf(c->rt60, 0.2f, 12.0f);
    /* the highs' RT against the room's: damp_hz says how dark it is (4.2 kHz a stone nave, 7.5 kHz a bright hall) */
    float hi_ratio = fm_clampf(0.5f + (c->damp_hz - 4200.0f) / 11000.0f, 0.35f, 0.8f);
    int i, k;
    for (i = 0; i < NL; i++) {
        B.len[i] = (int)(LMS[i] * size * HR_FS / 1000.0f);
        if (B.len[i] > B.line[i].cap - MOD_MAX - 3)
            B.len[i] = B.line[i].cap - MOD_MAX - 3;
        B.alen[i] = (int)(AMS[i] * fm_maxf(size, 0.5f) * HR_FS / 1000.0f);
        if (B.alen[i] > B.apl[i].cap - 2)
            B.alen[i] = B.apl[i].cap - 2;
        absorb(B.len[i] + B.alen[i], rt, rt * hi_ratio, &B.a[i], &B.g[i]);
        B.depth[i] = (float)MOD_MAX * (0.45f + 0.5f * (float)(rv_hash((uint32_t)i * 77u + 3u) & 0xFFu) / 255.0f);
    }
    for (i = 0; i < 4; i++) {
        B.dlen[i] = (int)(DMS[i] * fm_clampf(size, 0.5f, 0.68f) * HR_FS / 1000.0f);
        if (B.dlen[i] > B.dif[i].cap - 2)
            B.dlen[i] = B.dif[i].cap - 2;
    }
    B.pre_n = (int)(fm_clampf(c->predelay_ms, 0.5f, PRE_MAX * 1000.0f - 1.0f) * HR_FS / 1000.0f);
    for (k = 0; k < 2; k++)
        for (i = 0; i < NV; i++) {
            uint32_t h = rv_hash((uint32_t)(i * 2 + k) * 2654435761u + 17u);
            float t = ((float)i + 0.2f + 0.6f * (float)(h & 0xFFFFu) / 65536.0f) / (float)NV;   /* a jittered grid */
            float ms = 2.0f + t * ER_MS * size;
            B.er_d[k][i] = 1 + (int)((ms + c->predelay_ms * 0.5f) * HR_FS / 1000.0f);
            if (B.er_d[k][i] >= B.er.cap - 2)
                B.er_d[k][i] = B.er.cap - 2;
            B.er_g[k][i] = ((h >> 16) & 1u ? 1.0f : -1.0f) * c->er_gain * 0.5f * fm_expf(-3.0f * t) / fm_sqrtf((float)NV * 0.25f);
        }
    B.er_a = 1.0f - fm_expf(-FM_TWO_PI * fm_clampf(c->damp_hz, 2000.0f, 9000.0f) / HR_FS);
    B.mix = fm_clampf(c->mix, 0.0f, 1.0f);
    if (c->size != B.have.size)
        reverb_clear();
    B.have = *c;
}

void reverb_init(void)
{
    uint16_t *m = mem;
    int i;
    for (i = 0; i < NL; i++) {
        B.line[i].b = m;
        B.line[i].cap = LINE_CAP[i];
        B.line[i].w = 0;
        m += B.line[i].cap;
    }
    for (i = 0; i < NL; i++) {
        B.apl[i].b = m;
        B.apl[i].cap = CAPA(67);
        B.apl[i].w = 0;
        m += B.apl[i].cap;
        /* each line its own slow wander, its rate between 0.11 and 0.83 Hz, no two alike */
        ph_init(&B.m1[i], 0.11f + 0.048f * (float)((i * 7) % NL), 0.37f * (float)i, HR_FS);
    }
    for (i = 0; i < 4; i++) {
        B.dif[i].b = m;
        B.dif[i].cap = CAPA(68);   /* (12.73 ms x 0.68 at most) */
        B.dif[i].w = 0;
        m += B.dif[i].cap;
    }
    B.pre.b = m;
    B.pre.cap = PRE_CAP;
    B.pre.w = 0;
    m += B.pre.cap;
    B.er.b = er_mem;
    B.er.cap = ER_CAP;
    B.er.w = 0;
    B.have.size = -1.0f;
    B.want.size = 0.6f;
    B.want.rt60 = 1.6f;
    B.want.damp_hz = 6000.0f;
    B.want.er_gain = 0.6f;
    B.want.predelay_ms = 8.0f;
    B.want.mix = 0.35f;
    B.want.rich = 0;
    apply(&B.want);
}

void reverb_configure(const reverb_cfg_t *c)
{
    B.want = *c;
    B.pending = 1;                         /* the render takes it at its next block */
}

static void hadamard16(float *x)
{
    int h, i, j;
    for (h = 1; h < NL; h <<= 1)
        for (i = 0; i < NL; i += h << 1)
            for (j = i; j < i + h; j++) {
                float a = x[j], b = x[j + h];
                x[j] = a + b;
                x[j + h] = a - b;
            }
}

/* the room in blocks of KB half-rate samples: every delay in it is longer than a block (the shortest, an input
 * diffuser at the smallest size, 40 samples; a line, 200), so each part runs a block at a time, its state in
 * registers: a line read for the whole block, the mixing per sample, the line written for the whole block. The
 * same sound as sample by sample; the wander's delay moves in a straight line across a block (32 samples, 1.5 ms). */
#define KB 32
static float bx[KB], bel[KB], ber[KB], bin[KB], bwl[KB], bwr[KB], bo[KB][NL];

static void block(int k)
{
    int i, j, s;
    {   /* the early reflections: the dry into its line, the taps read along it */
        float *b = B.er.b;
        int cap = B.er.cap, w0 = B.er.w, w = w0;
        for (s = 0; s < k; s++) {
            b[w] = bx[s];
            if (++w >= cap)
                w = 0;
        }
        B.er.w = w;
        for (s = 0; s < k; s++)
            bel[s] = ber[s] = 0.0f;
        for (j = 0; j < 2 * NV; j++) {
            float g = B.er_g[j & 1][j >> 1], *acc = (j & 1) ? ber : bel;
            int r = w0 + 1 - B.er_d[j & 1][j >> 1];
            while (r < 0)
                r += cap;
            {   /* in at most two straight runs: no wrap test a sample */
                int n1 = cap - r < k ? cap - r : k;
                const float *p = b + r;
                for (s = 0; s < n1; s++)
                    acc[s] += g * p[s];
                for (p = b - n1; s < k; s++)
                    acc[s] += g * p[s];
            }
        }
        {
            float l0 = B.er_lp[0], l1 = B.er_lp[1], a = B.er_a;
            for (s = 0; s < k; s++) {
                l0 += a * (bel[s] - l0);
                l1 += a * (ber[s] - l1);
                bel[s] = l0;
                ber[s] = l1;
            }
            B.er_lp[0] = l0;
            B.er_lp[1] = l1;
        }
    }
    {   /* the pre-delayed dry with the early part: the late part's input */
        uint16_t *b = B.pre.b;
        int cap = B.pre.cap, w = B.pre.w, r;
        for (s = 0; s < k; s++) {
            b[w] = h_enc(bx[s]);
            if (++w >= cap)
                w = 0;
        }
        r = B.pre.w + 1 - B.pre_n;
        while (r < 0)
            r += cap;
        B.pre.w = w;
        for (s = 0; s < k; s++) {
            bin[s] = 0.5f * h_dec(b[r]) + 0.35f * (bel[s] + ber[s]);
            if (++r >= cap)
                r = 0;
        }
    }
    for (j = 0; j < 4; j++) {              /* the input diffusers */
        uint16_t *b = B.dif[j].b;
        int cap = B.dif[j].cap, w = B.dif[j].w, r = w - B.dlen[j];
        float g = DG[j];
        if (r < 0)
            r += cap;
        for (s = 0; s < k; s++) {
            float z = h_dec(b[r]), v = bin[s] + g * z;
            b[w] = h_enc(v);
            bin[s] = z - g * v;
            if (++w >= cap)
                w = 0;
            if (++r >= cap)
                r = 0;
        }
        B.dif[j].w = w;
    }
    for (s = 0; s < k; s++)
        bwl[s] = bwr[s] = 0.0f;
    for (i = 0; i < NL; i++) {             /* the lines read: wandering, absorbing; their sound to the outputs */
        uint16_t *b = B.line[i].b;
        int cap = B.line[i].cap, w = B.line[i].w;
        float d0 = (float)B.len[i] + B.depth[i] * B.m1[i].s, d1, dd, g = B.g[i], a = B.a[i], lp = B.lp[i];
        float sl = (i & 1) ? -0.25f : 0.25f, sr = (i & 2) ? -0.25f : 0.25f;
        for (s = 0; s < k; s++)            /* the phasor a block on */
            ph_step(&B.m1[i]);
        d1 = (float)B.len[i] + B.depth[i] * B.m1[i].s;
        dd = (d1 - d0) / (float)k;
        for (s = 0; s < k; s++) {
            float d = d0 + dd * (float)s, f, v;
            int di = (int)d, r0 = w + s - di, r1 = r0 - 1;
            f = d - (float)di;
            if (r0 < 0)
                r0 += cap;
            if (r1 < 0)
                r1 += cap;
            {
                float x0 = h_dec(b[r0]), x1 = h_dec(b[r1]);
                v = x0 + f * (x1 - x0);
            }
            lp = g * v - a * lp;
            bo[s][i] = lp;
            bwl[s] += sl * v;
            bwr[s] += sr * v;
        }
        B.lp[i] = fm_flush(lp);
    }
    for (s = 0; s < k; s++)                /* the mixing: a 16 x 16 Hadamard, a sample at a time */
        hadamard16(bo[s]);
    for (i = 0; i < NL; i++) {             /* the lines written, through their allpasses */
        uint16_t *b = B.line[i].b, *ab = B.apl[i].b;
        int cap = B.line[i].cap, w = B.line[i].w, acap = B.apl[i].cap, aw = B.apl[i].w, ar = aw - B.alen[i];
        float sgn = ((i * 5 + 3) & 4) ? 1.0f : -1.0f;
        if (ar < 0)
            ar += acap;
        for (s = 0; s < k; s++) {
            float x_ = 0.25f * bo[s][i] + sgn * bin[s], z = h_dec(ab[ar]), v = x_ + 0.5f * z;
            ab[aw] = h_enc(v);
            b[w] = h_enc(z - 0.5f * v);
            if (++aw >= acap)
                aw = 0;
            if (++ar >= acap)
                ar = 0;
            if (++w >= cap)
                w = 0;
        }
        B.line[i].w = w;
        B.apl[i].w = aw;
    }
    {
        float m = B.mix * RV_WET;
        for (s = 0; s < k; s++) {
            bwl[s] = m * (bel[s] + bwl[s]);
            bwr[s] = m * (ber[s] + bwr[s]);
        }
    }
}

void reverb_process(const float *in_l, const float *in_r, float *out_l, float *out_r, uint32_t n)
{
    static float hl[BLK_HALF], hr[BLK_HALF];
    uint32_t s, m = 0, k0;
    int i;
    if (B.pending) {
        B.pending = 0;
        apply(&B.want);
    }
    if (B.mix <= 0.0f)
        return;
    for (i = 0; i < NL; i++)               /* the phasors kept on the circle */
        ph_norm(&B.m1[i]);
    /* down: the half-rate samples of this block (the phase carried across blocks) */
    {
        hb_t *h = &B.hb;
        int ph = h->ph;
        static float xs[BLK_HALF];
        for (s = 0; s < n; s++) {
            hb_in(h, 0.5f * (in_l[s] + in_r[s]));
            if (ph && m < BLK_HALF)
                xs[m++] = hb_down_out(h);   /* (only the samples kept are computed) */
            ph ^= 1;
        }
        for (k0 = 0; k0 < m; k0 += KB) {   /* the room, a block at a time */
            int k = (int)(m - k0 < KB ? m - k0 : KB), t;
            for (t = 0; t < k; t++)
                bx[t] = xs[k0 + t];
            block(k);
            for (t = 0; t < k; t++) {
                hl[k0 + t] = bwl[t];
                hr[k0 + t] = bwr[t];
            }
        }
        /* up: the same phase walk, the room's samples pushed as they were made */
        ph = h->ph;
        m = 0;
        for (s = 0; s < n; s++) {
            float l, r;
            if (ph && m < BLK_HALF) {
                hb_push(h, hl[m], hr[m]);
                m++;
            }
            hb_up(h, ph, &l, &r);
            out_l[s] += l;
            out_r[s] += r;
            ph ^= 1;
        }
        h->ph = ph;
    }
    B.er_lp[0] = fm_flush(B.er_lp[0]);
    B.er_lp[1] = fm_flush(B.er_lp[1]);
}
