/* KIST — a physically modelled continuo organ for the M-VAVE FM-1.
 * The engine's interface: one manual (C..f''', MIDI 36..89), four wooden stops split bass | treble,
 * 12 notes, 48 pipes, a modelled wind supply with tremulant, and an algorithmic room.
 *
 * Threads: organ_render() runs in the audio interrupt. Everything else is called from the main loop and
 * reaches the audio side through a small lock-free queue (notes, stops) or through the parameter block,
 * which the render reads once per block. MIDI read inside the render calls organ_note_*_isr() directly. */
#pragma once
#include <stdint.h>

#define KIST_FS 44117.647f                 /* the FM-1's real rate: 24 MHz / 544 (felucca-bsp fm1_audio.h) */
#define KIST_KEY_LO 36                     /* C (MIDI 36) */
#define KIST_KEY_HI 89                     /* f''' (MIDI 89) */
#define KIST_NKEYS (KIST_KEY_HI - KIST_KEY_LO + 1)   /* 54 */
#define KIST_MAX_NOTES 12
#define KIST_MAX_PIPES 48

/* stops: one rank each; the Quint has treble pipes only */
enum { ST_GEDACKT, ST_ROHRFLOTE, ST_PRINCIPAL, ST_QUINT, NSTOPS };
/* the seven stop halves as the panel draws them: bit (2 * stop + half), half 0 bass, 1 treble */
#define HALF_BIT(stop, treble) (1u << (2u * (uint32_t)(stop) + ((treble) ? 1u : 0u)))
#define HALVES_ALL 0xBFu                   /* every half but bit 6: the Quint has no bass */
#define QUINT_BASS_BIT (1u << 6)

/* tuning systems: an equal division of the octave (size set separately), or a 12-note temperament */
enum { TS_EQUAL, TS_WERCKMEISTER3, TS_MEANTONE, TS_KIRNBERGER3, TS_VALLOTTI, NTS };

enum { ROOM_DRY, ROOM_CHAPEL, ROOM_CHURCH, ROOM_ABBEY, ROOM_HALL, NROOMS };

typedef struct {
    /* tuning */
    uint16_t pitch_hz;                     /* A reference, whole hertz: 300..500 */
    uint8_t pitch_cent;                    /* hundredths of a hertz: 0..99 */
    uint8_t system;                        /* TS_* */
    uint8_t edo;                           /* 5..72, used when system == TS_EQUAL */
    uint8_t root;                          /* temperament root pitch class, 0 = C */
    /* keyboard */
    uint8_t split;                         /* first treble key (MIDI), default 60 = c' */
    uint8_t touch;                         /* 0 soft .. 4 crisp: how velocity sets the valve's speed */
    /* wind */
    int8_t pressure;                       /* -10..+10 %, around the voiced pressure */
    uint8_t steadiness;                    /* 0 steady, 1 lively, 2 loose */
    uint8_t blower_noise;                  /* 0 off .. 10 */
    uint8_t trem_speed;                    /* tenths of Hz: 30..80 */
    uint8_t trem_depth;                    /* 0 gentle, 1 medium, 2 deep */
    /* room */
    uint8_t room;                          /* ROOM_* */
    uint8_t mix;                           /* 0..100 % */
    uint8_t distance;                      /* metres, 1..20 */
    uint8_t decay_ds;                      /* RT60 in tenths of a second, 3..80 */
} organ_params_t;

/* voicing, per stop: what the VOICING page edits */
typedef struct {
    int8_t level_half_db[NSTOPS][KIST_NKEYS];   /* per-note regulation, 0.5 dB steps, -24..+24 */
    uint8_t chiff[NSTOPS];                 /* 0 none .. 4 strong */
    uint8_t drift_dc[NSTOPS];              /* per-pipe random tuning spread, tenths of a cent: 0..50 */
} organ_voicing_t;

/* what the screens show (written by the render, read by the main loop) */
typedef struct {
    volatile uint32_t pipes, pipes_peak, notes;
    volatile float pressure;               /* reservoir, 1 = voiced */
    volatile uint32_t sounding_lo, sounding_hi;   /* keys 36..67 and 68..89 sounding, bit per key */
    volatile uint32_t last_vel, last_open_us;   /* the last note's velocity and its valve opening time */
    volatile uint32_t os_pipes;            /* pipes running 2x oversampled on the second core */
    volatile uint32_t rich_reverb;         /* 1 while the second core runs the richer room */
    volatile uint32_t load_pct;            /* the last block's render time, % of its budget */
    volatile uint32_t pipe_cap;            /* pipes allowed now (48, less when the render runs long) */
} organ_meter_t;

void organ_init(void);
void organ_set_params(const organ_params_t *p);    /* main loop: takes a copy; tuning changes retune held notes */
void organ_set_voicing(const organ_voicing_t *v);  /* main loop */
void organ_set_stops(uint32_t halves);             /* main loop: HALF_BIT mask; held notes follow, as on an organ */
void organ_set_blower(int on);
void organ_set_tremulant(int on);
void organ_note_on(int key, int vel);              /* main loop (the FM-1's keys) */
void organ_note_off(int key);
void organ_note_on_isr(int key, int vel);          /* audio interrupt (MIDI read in the render) */
void organ_note_off_isr(int key);
void organ_all_off_isr(void);                      /* audio interrupt: every valve closed (MIDI all notes off) */
void organ_panic(void);                            /* silence now */
void organ_shed(void);                             /* the render ran long: drop the least important pipes */
void organ_use_cpu2(int on);                       /* main loop: the second core runs the extras when on */
void organ_render(float *l, float *r, uint32_t n); /* audio interrupt: n frames, about +-1 full scale */
const organ_meter_t *organ_meter(void);

/* tuning: the sounding frequency of a key on the 8' (the Gedackt's), for the screens and the tests */
float organ_key_hz(const organ_params_t *p, int key);
/* cents from equal temperament for each pitch class of a temperament, A held at 0: for the TUNING graph */
void organ_temperament_cents(int system, int root, float out[12]);
/* a pipe's sounding frequency: key frequency times the stop's ratio (Gedackt 1, Rohrflöte 2, Principal 4, Quint 3) */
float organ_stop_ratio(int stop);

/* the rooms' names and defaults */
const char *organ_room_name(int room);
uint8_t organ_room_decay_ds(int room);
uint8_t organ_room_mix(int room);

/* host tools and tests: a single pipe, steady, for calibration and pitch checks */
typedef struct pipe_probe pipe_probe_t;
float organ_probe_run(int stop, float hz, float fs_mul, int use_calibration, float seconds, float *out, uint32_t nout);
