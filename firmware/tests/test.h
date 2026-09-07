#pragma once
#include <stdio.h>
#include <string.h>
#include <stdint.h>
static int t_fail = 0, t_run = 0;
#define TEST(name) static void name(void)
#define ASSERT(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); t_fail++; return; } } while (0)
#define ASSERT_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { printf("  FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); t_fail++; return; } } while (0)
#define ASSERT_MEMEQ(a, b, n) do { if (memcmp((a), (b), (n)) != 0) { printf("  FAIL %s:%d: memcmp %s %s\n", __FILE__, __LINE__, #a, #b); t_fail++; return; } } while (0)
#define RUN(name) do { t_run++; printf("%s\n", #name); name(); } while (0)
#define TEST_MAIN_END printf("%d tests, %d failed\n", t_run, t_fail); return t_fail ? 1 : 0;
