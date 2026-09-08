#include "test.h"
#include "log.h"
#include <string.h>

TEST(level_filter) {
    log_init();
    LOG_D(LOG_M_DW, "hidden");
    char out[4096];
    size_t n = log_drain(out, sizeof(out));
    ASSERT_EQ(n, 0);

    log_set_level(LOG_M_DW, LOG_DEBUG);
    LOG_D(LOG_M_DW, "shown");
    n = log_drain(out, sizeof(out));
    ASSERT(n > 0);
}

TEST(format_has_module_and_level) {
    log_init();
    LOG_I(LOG_M_DW, "hello %d", 42);
    char out[4096];
    size_t n = log_drain(out, sizeof(out));
    out[n] = '\0';
    ASSERT(strstr(out, " dw I hello 42\n") != NULL);
}

TEST(drop_when_full) {
    log_init();
    char msg[101];
    memset(msg, 'x', 100);
    msg[100] = '\0';
    for (int i = 0; i < 200; i++) {
        LOG_I(LOG_M_MAIN, "%s", msg);
    }
    ASSERT(log_dropped > 0);

    char out[4096];
    size_t n = log_drain(out, sizeof(out));
    ASSERT(n <= 4095);
    ASSERT(n > 0);
    ASSERT_EQ(out[n - 1], '\n');

    /* every line must start with a digit timestamp: no partial line pushed */
    size_t i = 0;
    while (i < n) {
        ASSERT(out[i] >= '0' && out[i] <= '9');
        char *nl = memchr(out + i, '\n', n - i);
        ASSERT(nl != NULL);
        i = (size_t)(nl - out) + 1;
    }
}

TEST(module_by_name) {
    ASSERT_EQ(log_module_by_name("dw"), LOG_M_DW);
    ASSERT_EQ(log_module_by_name("x"), -1);
}

TEST(truncates_long_line) {
    log_init();
    char msg[301];
    memset(msg, 'y', 300);
    msg[300] = '\0';
    LOG_I(LOG_M_MAIN, "%s", msg);
    char out[4096];
    size_t n = log_drain(out, sizeof(out));
    /* exactly one line, 159 chars total, ending "...\n" */
    ASSERT_EQ(n, 159);
    ASSERT_EQ(out[n - 1], '\n');
    ASSERT_EQ(out[n - 2], '.');
    ASSERT_EQ(out[n - 3], '.');
    ASSERT_EQ(out[n - 4], '.');
}

int main(void) {
    RUN(level_filter);
    RUN(format_has_module_and_level);
    RUN(drop_when_full);
    RUN(module_by_name);
    RUN(truncates_long_line);
    TEST_MAIN_END
}
