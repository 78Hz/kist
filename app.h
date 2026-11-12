/* KIST — the app's half of the contract with fm1-base (device/unity.c, host/plat_host.c). */
#pragma once
#include <stdint.h>
#include "plat.h"

#ifdef KIST_PLATE
#define APP_NAME "KIST-P"
#else
#define APP_NAME "KIST"
#endif
#define APP_SPLASH_RGB RGB(240, 180, 76)
#define APP_BLOCK 256u                   /* frames per audio block: 5.8 ms at 44.1 kHz */
#define APP_TOUCH                        /* velocity from the FM-1's own keys (lib/touch.h) */
#define APP_OVERLOAD                     /* the render ran long: shed pipes */
#define APP_CANVAS_PX (240u * 120u)      /* the screen drawn in two halves: 57.6 KB of the pool, not 69 */

/* the store: the setup (SAVE) and the panel calibration (the platform's) */
enum { OBJ_SETTINGS, OBJ_PANEL, OBJ_NOBJ };
#define APP_STORE_MAP {0xFC000u, 0xDC000u}
/* backup over USB (fm1.toml backup = true): the setup alone; the panel's calibration stays the unit's */
#define APP_BACKUP_OBJS {OBJ_SETTINGS}
#ifdef KIST_PLATE
#define APP_BACKUP_MAGIC 0x4250494Bu            /* "KIPB" */
#else
#define APP_BACKUP_MAGIC 0x4253494Bu            /* "KISB" */
#endif
#define APP_BACKUP_VERSION 1u
#define APP_BACKUP_WHAT "replaces the saved setup"
int app_backup_playing(void);
void app_backup_flush(void);
void app_backup_hold(int on);
void app_backup_restored(void);

void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);
void app_overload(void);

/* the host simulator's checks (host/plat_host.c "expect") */
#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
