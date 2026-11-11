#define _POSIX_C_SOURCE 199309L
/* KIST — what the engine needs from the platform, for the tools and tests that run it alone on a computer.
 * KIST_STUB_CPU2=1 in the environment pretends a second core (jobs run at once, in order, as fm1-base's host does). */
#include <stdint.h>
#include <stdlib.h>
static int cpu2 = -1;
int plat_cpu2_on(void)
{
    if (cpu2 < 0) {
        const char *e = getenv("KIST_STUB_CPU2");
        cpu2 = e && *e == '1';
    }
    return cpu2;
}
int plat_cpu2_run(void (*fn)(uint32_t), uint32_t arg) { fn(arg); return 1; }
int plat_cpu2_wait(void) { return 0; }
#include <time.h>
uint32_t plat_ticks24(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 24000000u + (uint64_t)t.tv_nsec * 24u / 1000u);
}
