/* KIST — the room: velvet-noise early reflections, input diffusion and a 16-line feedback delay network, every line
 * wandering slowly and absorbing for its own length; at half the rate, its lines 16-bit floating point (see
 * reverb.c). Algorithmic only. "rich" is kept for the engine's sake and no longer changes the room. */
#pragma once
#include <stdint.h>

#define RV_LINES 16

typedef struct {
    float size, rt60, damp_hz, er_gain, predelay_ms, mix;
    int rich;
} reverb_cfg_t;

void reverb_init(void);
void reverb_configure(const reverb_cfg_t *c);   /* main loop or render: a new room clears the lines */
/* in: the dry stereo signal; adds the wet signal (scaled by the mix) into out_l / out_r */
void reverb_process(const float *in_l, const float *in_r, float *out_l, float *out_r, uint32_t n);
void reverb_clear(void);
