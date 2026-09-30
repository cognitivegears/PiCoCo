#pragma once
#include <stdint.h>
#include <stddef.h>

/* Ring-buffered, leveled logger. log_write() formats a line and pushes it
 * into a 4096-byte ring; it never blocks. The main loop drains the ring with
 * log_drain() at its own pace. log_write() is core0-only: no locks, so it
 * must not be called from core1 or an ISR that races core0's calls. */

enum { LOG_OFF = 0, LOG_ERROR = 1, LOG_INFO = 2, LOG_DEBUG = 3 };
enum { LOG_M_MAIN, LOG_M_BUS, LOG_M_DW, LOG_M_BECKER, LOG_M_CONSOLE, LOG_M_FS, LOG_M_NET, LOG_M_COUNT };

extern const char *const log_module_names[LOG_M_COUNT];
extern uint32_t log_dropped;

void log_init(void);                          /* all modules LOG_INFO, ring reset, log_dropped = 0 */
void log_set_level(int module, int level);
int  log_level(int module);
int  log_module_by_name(const char *name);    /* -1 unknown */
void log_write(int module, int level, const char *fmt, ...);
size_t log_drain(char *out, size_t max);      /* copies out bytes, returns count */

#ifndef PICOCO_LOG_LEVEL
#define PICOCO_LOG_LEVEL LOG_INFO
#endif

#define LOG_AT(m, lvl, ...) do { if ((lvl) <= PICOCO_LOG_LEVEL && (lvl) <= log_level(m)) log_write((m), (lvl), __VA_ARGS__); } while (0)
#define LOG_E(m, ...) LOG_AT(m, LOG_ERROR, __VA_ARGS__)
#define LOG_I(m, ...) LOG_AT(m, LOG_INFO, __VA_ARGS__)
#define LOG_D(m, ...) LOG_AT(m, LOG_DEBUG, __VA_ARGS__)
