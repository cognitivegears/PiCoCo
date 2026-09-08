#pragma once
#include <stdint.h>

/* Crash record survives a watchdog_reboot() in an __uninitialized_ram
 * variable (SRAM not cleared by that reset path). Pico only: never built
 * into the host target. */
typedef struct { uint32_t magic, reason, pc, lr, cfsr, mode, uptime_ms; } crash_rec_t;

/* Reason codes stored in crash_rec_t.reason. */
#define CRASH_REASON_HARDFAULT 1u
#define CRASH_REASON_PANIC     2u

/* Set by main.c to a wrapper around mode_get(); called from the hardfault/
 * panic path with interrupts already off, so it must not block, log, or
 * write flash. NULL is handled (mode field is left 0). */
extern uint32_t (*crash_mode_hook)(void);

void crash_init(void);                  /* validates the record left by a previous run, clears it */
const crash_rec_t *crash_last(void);    /* NULL if the last reset was clean */
void crash_clear(void);
