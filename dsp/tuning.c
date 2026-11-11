/* KIST — tuning: the A reference, equal divisions of the octave and four historical temperaments.
 * Everything in single precision (the FM-1 has no double-precision FPU); fm_exp2f is good to 3e-7, which is
 * 0.0005 cents, far finer than the 0.01 Hz the panel sets. */
#include "organ.h"
#include "fastmath.h"

#define PURE_FIFTH 701.955001f             /* 1200 log2(3/2) */
#define PYTH_COMMA 23.460010f              /* 1200 log2(3^12 / 2^19) */
#define SYNT_COMMA 21.506290f              /* 1200 log2(81/80) */
#define SCHISMA 1.953721f                  /* the Pythagorean less the syntonic comma */

/* the twelve fifths of the circle, in order C-G, G-D, D-A, A-E, E-B, B-F#, F#-C#, C#-G#, G#-D#, D#-A#, A#-F,
 * F-C, in cents. Each temperament's fifths sum to 7 octaves (8400 cents) */
static void fifths(int system, float f[12])
{
    int i;
    for (i = 0; i < 12; i++)
        f[i] = PURE_FIFTH;
    switch (system) {
    case TS_WERCKMEISTER3:                 /* C-G, G-D, D-A and B-F# narrowed by 1/4 Pythagorean comma */
        f[0] = f[1] = f[2] = f[5] = PURE_FIFTH - PYTH_COMMA / 4.0f;
        break;
    case TS_MEANTONE:                      /* every fifth 1/4 syntonic comma narrow; the wolf on G#-Eb */
        for (i = 0; i < 12; i++)
            f[i] = PURE_FIFTH - SYNT_COMMA / 4.0f;
        f[8] = 8400.0f - 11.0f * (PURE_FIFTH - SYNT_COMMA / 4.0f);
        break;
    case TS_KIRNBERGER3:                   /* C-G-D-A-E narrowed by 1/4 syntonic comma, F#-C# by a schisma */
        f[0] = f[1] = f[2] = f[3] = PURE_FIFTH - SYNT_COMMA / 4.0f;
        f[6] = PURE_FIFTH - SCHISMA;
        break;
    case TS_VALLOTTI:                      /* F-C-G-D-A-E-B narrowed by 1/6 Pythagorean comma, the rest pure */
        f[11] = f[0] = f[1] = f[2] = f[3] = f[4] = PURE_FIFTH - PYTH_COMMA / 6.0f;
        break;
    default:                               /* equal: every fifth 700 */
        for (i = 0; i < 12; i++)
            f[i] = 700.0f;
        break;
    }
}

/* each pitch class's distance in cents from equal temperament, C at 0 (before A is pinned) */
static void positions(int system, float off[12])
{
    float f[12], c = 0.0f;
    int i, pc = 0;
    fifths(system, f);
    off[0] = 0.0f;
    for (i = 0; i < 11; i++) {             /* up the circle from C: G, D, A, E, B, F#, C#, G#, D#, A#, F */
        c += f[i];
        pc = (pc + 7) % 12;
        while (c >= 1200.0f)
            c -= 1200.0f;
        off[pc] = c - 100.0f * (float)pc;
    }
}

void organ_temperament_cents(int system, int root, float out[12])
{
    float off[12];
    int pc, a;
    if (system == TS_EQUAL) {
        for (pc = 0; pc < 12; pc++)
            out[pc] = 0.0f;
        return;
    }
    positions(system, off);
    root = ((root % 12) + 12) % 12;
    a = (9 - root + 12) % 12;              /* A, counted from the root */
    for (pc = 0; pc < 12; pc++)
        out[pc] = off[(pc - root + 12) % 12] - off[a];
}

float organ_key_hz(const organ_params_t *p, int key)
{
    float ref = (float)p->pitch_hz + 0.01f * (float)p->pitch_cent;
    if (p->system == TS_EQUAL) {
        int n = p->edo < 5 ? 5 : p->edo > 72 ? 72 : p->edo;
        return ref * fm_exp2f((float)(key - 69) / (float)n);
    } else {
        float t[12];
        organ_temperament_cents(p->system, p->root, t);
        return ref * fm_exp2f((float)(key - 69) / 12.0f + t[((key % 12) + 12) % 12] / 1200.0f);
    }
}

float organ_stop_ratio(int stop)
{
    static const float R[NSTOPS] = {1.0f, 2.0f, 4.0f, 3.0f};   /* 8', 4', 2', 2 2/3' (a pure twelfth) */
    return R[stop];
}
