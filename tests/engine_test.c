#define _POSIX_C_SOURCE 199309L
/* KIST — engine tests on a computer: tuning accuracy (every stop, every key, several references and systems),
 * temperaments, a full render (finite, bounded, polyphony limits), the cost per pipe, and WAV files to listen to.
 *   tools/build_tools.sh && build/engine_test [wavdir]        (exit 1 on a failure) */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "organ.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
static const char *SN[NSTOPS] = {"Gedackt 8'", "Rohrflote 4'", "Principal 2'", "Quint 2 2/3'"};

static organ_params_t defaults(void)
{
    organ_params_t p;
    memset(&p, 0, sizeof p);
    p.pitch_hz = 440; p.system = TS_EQUAL; p.edo = 12; p.split = 60; p.touch = 2; p.steadiness = 1;
    p.blower_noise = 3; p.trem_speed = 52; p.room = ROOM_CHAPEL; p.mix = 35; p.distance = 6; p.decay_ds = 16;
    return p;
}

/* ------------------------------------------------------------------------------------------- tuning --- */
static void tuning(void)
{
    static const struct { int hz, cent, sys, edo; const char *name; } C[] = {
        {300, 0, TS_EQUAL, 12, "A300 equal"}, {392, 0, TS_EQUAL, 12, "A392 equal"},
        {415, 0, TS_MEANTONE, 12, "A415 1/4-comma"}, {440, 0, TS_EQUAL, 12, "A440 equal"},
        {440, 0, TS_WERCKMEISTER3, 12, "A440 Werckmeister III"}, {465, 0, TS_VALLOTTI, 12, "A465 Vallotti"},
        {500, 99, TS_EQUAL, 12, "A500.99 equal"}, {440, 0, TS_EQUAL, 31, "A440 31-EDO"},
        {432, 37, TS_KIRNBERGER3, 12, "A432.37 Kirnberger III"},
    };
    int c, k, s;
    double worst_all = 0.0;
    printf("== tuning: measured against asked, cents (pipes alone, steady wind)\n");
    for (c = 0; c < (int)(sizeof C / sizeof C[0]); c++) {
        organ_params_t p = defaults();
        double worst = 0.0, sum2 = 0.0, worst_hi = 0.0;
        int n = 0, skipped = 0, worst_key = 0, worst_stop = 0;
        p.pitch_hz = (uint16_t)C[c].hz; p.pitch_cent = (uint8_t)C[c].cent; p.system = (uint8_t)C[c].sys;
        p.edo = (uint8_t)C[c].edo;
        for (s = 0; s < NSTOPS; s++)
            for (k = KIST_KEY_LO; k <= KIST_KEY_HI; k++) {
                float want, got, fs_mul;
                double e;
                if (s == ST_QUINT && k < 60)
                    continue;
                want = organ_key_hz(&p, k) * organ_stop_ratio(s);
                fs_mul = 1.0f;
                if (want < 28.0f || want > 0.15f * KIST_FS * fs_mul) { skipped++; continue; }
                got = organ_probe_run(s, want, fs_mul, 1, fmaxf(0.8f, 120.0f / want), 0, 0);
                if (got <= 0.0f) { CHECK(0, "%s %s key %d (%.1f Hz) did not speak", C[c].name, SN[s], k, want); continue; }
                e = 1200.0 * log2((double)got / (double)want);
                sum2 += e * e; n++;
                if (want > 1500.0f) { if (fabs(e) > fabs(worst_hi)) worst_hi = e; continue; }
                if (fabs(e) > fabs(worst)) { worst = e; worst_key = k; worst_stop = s; }
            }
        printf("  %-24s %3d pipes  rms %.3f  worst %+.3f (%s key %d), above 1.5 kHz at 1x %+.3f  out of range %d\n",
               C[c].name, n, sqrt(sum2 / (n ? n : 1)), worst, SN[worst_stop], worst_key, worst_hi, skipped);
        CHECK(fabs(worst) < 1.0, "%s: worst error %.3f cents", C[c].name, worst);
        CHECK(fabs(worst_hi) < 1.5, "%s: worst error above 1.5 kHz %.3f cents", C[c].name, worst_hi);
        if (fabs(worst) > worst_all) worst_all = fabs(worst);
    }
    /* the top of the compass at 2x (the second core's pipes) */
    {
        organ_params_t p = defaults();
        double worst = 0.0;
        for (s = 0; s < NSTOPS; s++)
            for (k = 72; k <= KIST_KEY_HI; k++) {
                float want = organ_key_hz(&p, k) * organ_stop_ratio(s), got;
                double e;
                if (want <= 1500.0f) continue;
                got = organ_probe_run(s, want, 2.0f, 1, 0.8f, 0, 0);
                e = got > 0 ? 1200.0 * log2((double)got / want) : 99.0;
                if (fabs(e) > fabs(worst)) worst = e;
            }
        printf("  %-24s worst %+.3f\n", "A440 2x oversampled", worst);
        CHECK(fabs(worst) < 1.0, "2x oversampled: worst %.3f cents", worst);
    }
    printf("  worst overall %.3f cents\n", worst_all);
}

static void temperaments(void)
{
    static const char *TN[NTS] = {"equal", "Werckmeister III", "1/4-comma meantone", "Kirnberger III", "Vallotti"};
    static const char *PC[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "G#", "A", "Bb", "B"};
    int t, i;
    printf("== temperaments: cents from equal, A = 0, root C\n     ");
    for (i = 0; i < 12; i++) printf("%6s", PC[i]);
    printf("\n");
    for (t = 0; t < NTS; t++) {
        float c[12];
        organ_temperament_cents(t, 0, c);
        printf("  %-3.3s", TN[t]);
        for (i = 0; i < 12; i++) printf("%+6.1f", c[i]);
        printf("  %s\n", TN[t]);
        CHECK(fabsf(c[9]) < 1e-3f, "%s: A is not at 0", TN[t]);
    }
    {   /* meantone's major thirds from C are pure (5/4 = 386.31 cents) */
        float c[12];
        organ_temperament_cents(TS_MEANTONE, 0, c);
        CHECK(fabsf(400.0f + c[4] - c[0] - 386.3137f) < 0.01f, "meantone C-E is not pure");
        organ_temperament_cents(TS_MEANTONE, 2, c);   /* root D: D-F# pure */
        CHECK(fabsf(400.0f + c[6] - c[2] - 386.3137f) < 0.01f, "meantone on D: D-F# is not pure");
    }
}

/* -------------------------------------------------------------------------------------------- render --- */
static float L[256], R[256];
static short wav[44100 * 60 * 2];
static int wav_n;
static float peak_all;
static int nan_seen;

static void render(float secs)
{
    int blocks = (int)(secs * 44100.0f / 256.0f), b, i;
    for (b = 0; b < blocks; b++) {
        organ_render(L, R, 256);
        for (i = 0; i < 256; i++) {
            float l = L[i], r = R[i];
            if (!isfinite(l) || !isfinite(r)) nan_seen++;
            if (fabsf(l) > peak_all) peak_all = fabsf(l);
            if (fabsf(r) > peak_all) peak_all = fabsf(r);
            if (wav_n < 44100 * 60) {
                l = tanhf(l * 1.2f) * 0.9f; r = tanhf(r * 1.2f) * 0.9f;
                wav[2 * wav_n] = (short)(l * 32767.0f); wav[2 * wav_n + 1] = (short)(r * 32767.0f);
                wav_n++;
            }
        }
    }
}

static void wav_write(const char *dir, const char *name)
{
    char path[512];
    FILE *f;
    unsigned int data = (unsigned int)wav_n * 4u, v;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if (!(f = fopen(path, "wb"))) { wav_n = 0; return; }
    fwrite("RIFF", 1, 4, f); v = 36 + data; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f); v = 0x00020001u; fwrite(&v, 4, 1, f); v = 44100; fwrite(&v, 4, 1, f);
    v = 44100 * 4; fwrite(&v, 4, 1, f); v = 0x00100004u; fwrite(&v, 4, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f); fwrite(wav, 4, (size_t)wav_n, f); fclose(f);
    printf("  wrote %s (%.1f s)\n", path, wav_n / 44100.0);
    wav_n = 0;
}

static void chord(const int *k, int n, int vel, float hold, float tail)
{
    int i;
    for (i = 0; i < n; i++) organ_note_on(k[i], vel);
    render(hold);
    for (i = 0; i < n; i++) organ_note_off(k[i]);
    render(tail);
}

static void renders(const char *dir)
{
    organ_params_t p = defaults();
    const organ_meter_t *m = organ_meter();
    static const int SCALE[] = {48, 50, 52, 53, 55, 57, 59, 60, 62, 64, 65, 67, 69, 71, 72};
    int i, s;
    printf("== render\n");
    organ_init();
    organ_set_params(&p);
    organ_set_stops(HALF_BIT(ST_GEDACKT, 0) | HALF_BIT(ST_GEDACKT, 1));
    render(0.6f);
    for (s = 0; s < NSTOPS; s++) {         /* each stop alone: a scale and a chord */
        char name[64];
        organ_set_stops(HALF_BIT(s, 0) | HALF_BIT(s, 1));
        if (s == ST_QUINT) for (i = 0; i < 15; i++) { int k = SCALE[i] + 12; organ_note_on(k, 90); render(0.32f); organ_note_off(k); render(0.05f); }
        else for (i = 0; i < 15; i++) { organ_note_on(SCALE[i], 90); render(0.32f); organ_note_off(SCALE[i]); render(0.05f); }
        { int c[] = {48, 55, 64, 72}; if (s == ST_QUINT) { c[0] = 60; c[1] = 67; c[2] = 76; c[3] = 84; } chord(c, 4, 90, 2.0f, 1.6f); }
        snprintf(name, sizeof name, "stop%d_%s.wav", s + 1, s == 0 ? "gedackt8" : s == 1 ? "rohrflote4" : s == 2 ? "principal2" : "quint");
        wav_write(dir, name);
    }
    /* the full organ: a cadence, then 14 keys at once (more than the 12 notes it has) */
    organ_set_stops(HALVES_ALL);
    { int a[] = {36, 48, 55, 64, 67}; chord(a, 5, 100, 1.6f, 0.05f); }
    { int a[] = {41, 53, 57, 65, 69}; chord(a, 5, 100, 1.6f, 0.05f); }
    { int a[] = {43, 55, 59, 62, 67, 71}; chord(a, 6, 100, 1.6f, 0.05f); }
    { int a[] = {36, 48, 55, 60, 64, 72}; chord(a, 6, 100, 3.0f, 2.5f); }
    wav_write(dir, "full_organ_cadence.wav");
    { int a[] = {36, 40, 43, 48, 52, 55, 60, 64, 67, 72, 76, 79, 84, 88};
      for (i = 0; i < 14; i++) organ_note_on(a[i], 100);
      render(1.5f);
      CHECK(m->notes <= KIST_MAX_NOTES, "notes %u > %d", m->notes, KIST_MAX_NOTES);
      CHECK(m->pipes <= KIST_MAX_PIPES, "pipes %u > %d", m->pipes, KIST_MAX_PIPES);
      printf("  14 keys, all stops: %u notes, %u pipes (peak %u)\n", m->notes, m->pipes, m->pipes_peak);
      for (i = 0; i < 14; i++) organ_note_off(a[i]);
      render(2.0f); }
    wav_write(dir, "polyphony_14_keys.wav");
    /* velocity: the valve's speed. Soft then hard, Principal 2' + Gedackt */
    organ_set_stops(HALF_BIT(ST_GEDACKT, 0) | HALF_BIT(ST_GEDACKT, 1) | HALF_BIT(ST_PRINCIPAL, 0) | HALF_BIT(ST_PRINCIPAL, 1));
    for (i = 0; i < 6; i++) { organ_note_on(60 + 2 * i, i & 1 ? 127 : 15); render(0.45f); organ_note_off(60 + 2 * i); render(0.2f); }
    wav_write(dir, "velocity_soft_hard.wav");
    /* the tremulant, then the blower switched off under a chord */
    organ_set_stops(HALF_BIT(ST_GEDACKT, 0) | HALF_BIT(ST_GEDACKT, 1) | HALF_BIT(ST_ROHRFLOTE, 0) | HALF_BIT(ST_ROHRFLOTE, 1));
    organ_set_tremulant(1);
    { int a[] = {53, 60, 65, 69}; chord(a, 4, 90, 3.0f, 0.8f); }
    organ_set_tremulant(0);
    render(0.5f);
    { int a[] = {48, 55, 64, 67}; for (i = 0; i < 4; i++) organ_note_on(a[i], 90);
      render(1.5f); organ_set_blower(0); render(4.0f); for (i = 0; i < 4; i++) organ_note_off(a[i]);
      printf("  blower off under a chord: pressure %.3f after 4 s\n", m->pressure);
      CHECK(m->pressure < 0.3f, "the wind did not run out (%.3f)", m->pressure);
      organ_set_blower(1); render(2.0f); }
    wav_write(dir, "tremulant_and_blower_off.wav");
    /* the rooms: the same phrase in each */
    for (i = 0; i < NROOMS; i++) {
        char name[64];
        p.room = (uint8_t)i; p.decay_ds = organ_room_decay_ds(i); p.mix = organ_room_mix(i);
        organ_set_params(&p);
        organ_set_stops(HALF_BIT(ST_GEDACKT, 0) | HALF_BIT(ST_GEDACKT, 1) | HALF_BIT(ST_ROHRFLOTE, 1));
        { int a[] = {60, 64, 67}; chord(a, 3, 100, 0.8f, 0.05f); }
        { int a[] = {59, 62, 67}; chord(a, 3, 100, 0.8f, 0.05f); }
        { int a[] = {60, 64, 67, 72}; chord(a, 4, 100, 1.0f, 4.0f); }
        snprintf(name, sizeof name, "room%d_%s.wav", i + 1, organ_room_name(i));
        for (char *c = name; *c; c++) if (*c == ' ') *c = '_';
        wav_write(dir, name);
    }
    /* meantone vs equal: the same C major and E major triads */
    p = defaults();
    organ_set_stops(HALF_BIT(ST_GEDACKT, 0) | HALF_BIT(ST_GEDACKT, 1) | HALF_BIT(ST_PRINCIPAL, 0) | HALF_BIT(ST_PRINCIPAL, 1));
    for (i = 0; i < 2; i++) {
        p.system = i ? TS_MEANTONE : TS_EQUAL; p.pitch_hz = 415;
        organ_set_params(&p); render(0.1f);
        { int a[] = {48, 60, 64, 67}; chord(a, 4, 100, 2.0f, 0.3f); }
        { int a[] = {52, 64, 68, 71}; chord(a, 4, 100, 2.0f, 1.0f); }
    }
    wav_write(dir, "equal_then_meantone_A415.wav");
    printf("  peak %.3f, non-finite samples %d\n", peak_all, nan_seen);
    CHECK(nan_seen == 0, "%d non-finite samples", nan_seen);
    CHECK(peak_all < 4.0f, "peak %.2f", peak_all);
}

/* the pitch of a note through the whole render (wind, valve, smoothing), dry */
static void render_pitch(void)
{
    organ_params_t p = defaults();
    static float buf[44100 * 3];
    int i, n = 0, cross = 0, keys[] = {36, 57, 69, 81, 89};
    p.mix = 0; p.blower_noise = 0;
    printf("== pitch through the render (Gedackt 8', A440, dry)\n");
    for (int kk = 0; kk < 5; kk++) {
        int key = keys[kk];
        float want, lp1 = 0, lp2 = 0, prev = 0, a, t0 = -1, t1 = 0, got;
        organ_init(); organ_set_params(&p);
        organ_set_stops(HALF_BIT(ST_GEDACKT, 0) | HALF_BIT(ST_GEDACKT, 1));
        want = organ_key_hz(&p, key);
        organ_note_on(key, 100);
        n = 0;
        for (i = 0; i < (int)(3.0f * 44100 / 256); i++) { organ_render(L, R, 256); for (int j = 0; j < 256; j++) buf[n++] = L[j] + R[j]; }
        a = 1.0f - expf(-6.2831853f * 1.3f * want / KIST_FS);
        cross = 0;
        for (i = 0; i < n; i++) {
            lp1 += a * (buf[i] - lp1); lp2 += a * (lp1 - lp2);
            if (i > n / 2 && prev < 0 && lp2 >= 0) { float t = i - lp2 / (lp2 - prev); if (t0 < 0) t0 = t; t1 = t; cross++; }
            prev = lp2;
        }
        got = KIST_FS / ((t1 - t0) / (cross - 1));
        printf("  key %d: want %.3f Hz, got %.3f Hz (%+.2f cents, includes this pipe's drift and the wind)\n", key, want, got,
               1200.0 * log2(got / want));
        CHECK(fabs(1200.0 * log2(got / want)) < 4.0, "key %d off by more than 4 cents through the render", key);
    }
}

static void cost(void)
{
    organ_params_t p = defaults();
    int i, k = 0;
    struct timespec a, b;
    double ns;
    static const int K[12] = {36, 40, 43, 48, 52, 55, 60, 64, 67, 72, 76, 79};
    organ_init(); organ_set_params(&p); organ_set_stops(HALVES_ALL);
    for (i = 0; i < 12; i++) organ_note_on(K[i], 100);
    render(0.5f); wav_n = 0;
    k = (int)organ_meter()->pipes;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (i = 0; i < 400; i++) organ_render(L, R, 256);
    clock_gettime(CLOCK_MONOTONIC, &b);
    ns = ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / (400.0 * 256.0);
    printf("== cost on this computer: %d pipes, %.1f ns a frame (%.2f ns a pipe-sample, room included)\n", k, ns, ns / k);
    wav_n = 0;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "build/wav";
    char cmd[600];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir);
    if (system(cmd)) {}
    temperaments();
    tuning();
    render_pitch();
    renders(dir);
    cost();
    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
