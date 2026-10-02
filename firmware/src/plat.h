#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
/* Platform functions. host/plat_host.c on the Mac, src/plat_pico.c on the RP2350 (Plan B). */
uint32_t plat_now_us(void);
uint32_t plat_now_ms(void);
int  plat_fs_list(void (*cb)(const char *name, uint32_t size, void *ctx), void *ctx); /* 0 ok */
int  plat_fs_remove(const char *name);
int  plat_fs_format(void);
int  plat_fs_export(bool on);                 /* USB MSC export; host: no-op returning -1 */
bool plat_fs_exporting(void);                 /* true while exported over USB MSC; host: always false */
bool plat_usb_ejected(void);                  /* host has ejected the MSC volume; host: always false */
int  plat_cfg_read(char *buf, size_t max);    /* bytes read, <0 none */
int  plat_cfg_write(const char *buf, size_t n);
void plat_reboot(bool bootsel);
void plat_halt(bool assert_halt);
void plat_smoke(void);                        /* GPIO toggle test; host: no-op */
void plat_crash_test(void);                   /* deliberately fault (Pico); no-op (host) */
const char *plat_last_reset(void);            /* "power-on" | "watchdog" | "reboot" | "hardfault pc=0x... ..." | "panic pc=0x... ..." | "host" */
void plat_reset_latch(void);                  /* Pico: call once, before watchdog_enable() re-arms and clobbers the
                                                  marker plat_last_reset() needs; host: no-op */
size_t plat_bridge_read(uint8_t *buf, size_t n);   /* CDC0 in bridge mode; host: 0 */
size_t plat_bridge_write(const uint8_t *buf, size_t n);
size_t plat_bridge_write_free(void);             /* bytes CDC0 can take now; 0 while nobody is listening */
/* Real milliseconds since boot. The Pico is up and has released /HALT about
 * 3 ms after a reset (plus the debounce below), so this window is what the
 * person pressing RESET experiences. Not in `becker net` mode, where /HALT is
 * held until the network is up: there the window closes while the screen is
 * still dark, and only two quick presses count. */
#define PICOCO_DOUBLE_RESET_MS 2000
#define PICOCO_RESET_DEBOUNCE_MS 150   /* boots shorter than this never touch the marker */
/* Spec 2026-10-01 §6.5: RESET twice inside the window brings the manager up
 * for that boot. The CoCo's /RESET drives the Pico's RUN pin, so every CoCo
 * reset is a Pico reboot; the marker is a small log in flash (plat_pico.c). */
bool plat_double_reset(void);                    /* call once, first thing in main */
void plat_double_reset_tick(uint32_t now_ms);    /* main loop: disarm once now_ms >= the window */
uint32_t plat_double_reset_seen(void);           /* armed/disarmed bytes found in the marker log at boot (odd = double RESET); host: 0 */
bool plat_rtc_get(int64_t *unix_secs);        /* true if a clock that survives a CoCo reset is running; host: false */
void plat_rtc_set(int64_t unix_secs);         /* host: no-op */
const char *plat_fs_dir(void);                /* host only: directory backing the "filesystem" */
void plat_host_set_dir(const char *dir);      /* host only */
