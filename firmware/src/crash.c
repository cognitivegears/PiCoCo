#include "crash.h"
#include <stdbool.h>
#include "pico/platform/sections.h"
#include "pico/time.h"
#include "hardware/watchdog.h"
#include "hardware/structs/scb.h"

#define CRASH_MAGIC 0xC0C0DEADu

uint32_t (*crash_mode_hook)(void);

static crash_rec_t __uninitialized_ram(rec);
static crash_rec_t last;
static bool have_last;

void crash_init(void) {
    if (rec.magic == CRASH_MAGIC) { last = rec; have_last = true; }
    rec.magic = 0;
}

const crash_rec_t *crash_last(void) { return have_last ? &last : NULL; }
void crash_clear(void) { have_last = false; }

static void __attribute__((noreturn)) record_and_reboot(uint32_t reason, uint32_t pc, uint32_t lr) {
    rec.magic = CRASH_MAGIC;
    rec.reason = reason;
    rec.pc = pc;
    rec.lr = lr;
    rec.cfsr = scb_hw->cfsr;
    rec.mode = crash_mode_hook ? crash_mode_hook() : 0;
    rec.uptime_ms = to_ms_since_boot(get_absolute_time());
    watchdog_reboot(0, 0, 0);
    for (;;) { }
}

void __attribute__((naked)) isr_hardfault(void) {
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "b hardfault_c\n"
    );
}

/* not inlined: hardfault_c must be a real callable symbol for the naked
 * isr_hardfault's "b hardfault_c" to branch to. */
void __attribute__((noinline)) hardfault_c(uint32_t *frame) {
    record_and_reboot(CRASH_REASON_HARDFAULT, frame[6], frame[5]);
}

/* Installed as PICO_PANIC_FUNCTION: replaces the SDK's default panic() body
 * (which prints and breakpoints) so an assert/panic also survives as a
 * crash record instead of hanging with the debugger detached. */
void picoco_panic(const char *fmt, ...) {
    (void)fmt;
    record_and_reboot(CRASH_REASON_PANIC, (uint32_t)__builtin_return_address(0), 0);
}
