#include "log.h"
#include "ring.h"
#include "plat.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* log_write() is core0-only: it touches log_ring and log_levels with no
 * locking. Do not call it from core1 or an ISR that races core0. */

const char *const log_module_names[LOG_M_COUNT] = {"main", "bus", "dw", "becker", "console", "fs"};
uint32_t log_dropped;

static uint8_t log_buf[4096];
static ring_t log_ring;
static int log_levels[LOG_M_COUNT];

void log_init(void) {
    ring_init(&log_ring, log_buf, sizeof(log_buf));
    for (int i = 0; i < LOG_M_COUNT; i++) log_levels[i] = LOG_INFO;
    log_dropped = 0;
}

void log_set_level(int module, int level) { log_levels[module] = level; }
int log_level(int module) { return log_levels[module]; }

int log_module_by_name(const char *name) {
    for (int i = 0; i < LOG_M_COUNT; i++) {
        if (strcmp(log_module_names[i], name) == 0) return i;
    }
    return -1;
}

void log_write(int module, int level, const char *fmt, ...) {
    static const char levelch[] = {'?', 'E', 'I', 'D'};
    char line[160];

    int n = snprintf(line, sizeof(line), "%u %s %c ", plat_now_us(), log_module_names[module],
                      levelch[level & 3]);
    if (n < 0) return;
    if ((size_t)n >= sizeof(line)) n = (int)sizeof(line) - 1; /* ponytail: prefix is always short; belt-and-suspenders only */

    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;

    size_t len;
    if ((size_t)n + (size_t)m + 1 < sizeof(line)) {
        len = (size_t)n + (size_t)m;
        line[len++] = '\n';
    } else {
        len = sizeof(line) - 1; /* 159: truncated line, ends "...\n" */
        memcpy(line + len - 4, "...\n", 4);
    }

    if (ring_free(&log_ring) < len) {
        log_dropped++;
        return;
    }
    for (size_t i = 0; i < len; i++) ring_push(&log_ring, (uint8_t)line[i]);
}

size_t log_drain(char *out, size_t max) {
    size_t i = 0;
    uint8_t b;
    while (i < max && ring_pop(&log_ring, &b)) out[i++] = (char)b;
    return i;
}
