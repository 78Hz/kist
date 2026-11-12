/* KIST-P — the plate (dsp/plate.c): Dattorro's plate with its parameters free. It also provides reverb.h's interface,
 * so the organ engine drives it as it drives the room (in KIST-P it is built in place of reverb.c); the room's own
 * settings are ignored but its mix. */
#pragma once
#include <stdint.h>

typedef struct {
    uint8_t size_pc;                       /* the plate's size, % of Dattorro's: 30..130 (it glides there) */
    uint8_t decay_ds;                      /* RT60, tenths of a second: 2..120 */
    uint8_t damp_k10;                      /* the plate's damping: its highs' cut, tenths of a kHz: 10..100 */
    uint8_t predelay_ms;                   /* 0..100 */
    uint8_t bandwidth_k10;                 /* the drive's bandwidth (what goes in), tenths of a kHz: 10..100 */
    uint8_t in_diff;                       /* input diffusion, %: 0..100 */
    uint8_t dec_diff;                      /* decay diffusion, %: 0..100 */
    uint8_t lowcut_h10;                    /* the plate's low cut, tens of Hz: 0 (off) ..50 */
    uint8_t rate_ds;                       /* the motion's rate, tenths of a Hz: 1..30 */
    uint8_t depth;                         /* the motion's depth, %: 0..100 */
    uint8_t width;                         /* stereo width, %: 0..100 */
    uint8_t rsv;
} plate_params_t;

void plate_defaults(plate_params_t *p);
int plate_params_ok(const plate_params_t *p);
void plate_set(const plate_params_t *p);   /* main loop: taken at the render's next block */
float plate_size_now(void);                /* the size as it glides (for the screen), 0.3 .. 1.3 */
