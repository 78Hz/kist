/* KIST — the room: early reflections, input diffusion and an 8-line feedback delay network with slow modulation.
 * Algorithmic only. The "rich" mode (the second core's) adds more early reflections, two more diffusers and
 * modulation on four lines instead of two; both modes share the same buffers. */
#pragma once
#include <stdint.h>

#define RV_LINES 8

typedef struct {
    float size, rt60, damp_hz, er_gain, predelay_ms, mix;
    int rich;
} reverb_cfg_t;

void reverb_init(void);
void reverb_configure(const reverb_cfg_t *c);   /* main loop or render: a new room clears the lines */
/* in: the dry stereo signal; adds the wet signal (scaled by the mix) into out_l / out_r */
void reverb_process(const float *in_l, const float *in_r, float *out_l, float *out_r, uint32_t n);
void reverb_clear(void);
