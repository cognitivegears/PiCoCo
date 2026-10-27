/* Sampler timing: the bus engine's serve budget, recomputed from the one
 * clock constant (bus_timing.h, bound to the SDK's SYS_CLK_HZ in
 * bus_engine.c). Run this before any clock change; it prints the margins at
 * the configured clock and fails when the budget no longer closes. Expected
 * values are the 2026-10-02 bench census (docs/firmware-architecture.md §8). */
#include "test.h"
#include "bus_timing.h"

TEST(serve_budget_closes_at_both_speeds) {
    printf("  clk_sys %u MHz, clkdiv %d: serve %d clk = %d ns\n",
           (unsigned)(BUS_SYS_CLK_HZ / 1000000), BUS_PIO_CLKDIV, BUS_SERVE_CLK, BUS_SERVE_NS);
    printf("  1.79 MHz: sample point %d clk, margin %d clk\n", BUS_LATCH_CLK_FAST, BUS_MARGIN_CLK_FAST);
    printf("  0.89 MHz: sample point %d clk, margin %d clk\n", BUS_LATCH_CLK_SLOW, BUS_MARGIN_CLK_SLOW);
    ASSERT(BUS_MARGIN_CLK_FAST > 0);                 /* the budget: the byte lands before the sample point */
    ASSERT(BUS_MARGIN_CLK_SLOW > 0);
}

/* The census, and the literal 150 MHz counts `bus selftest` is written in
 * (fake6809.c/.pio: 84-clk cycle, S=36/72, `* 20 / 3` for ns). */
TEST(matches_the_150mhz_census) {
    if (BUS_SYS_CLK_HZ != 150000000u) {
        printf("  skipped: not the census clock. bus selftest (fake6809) still counts at 150 MHz; rewrite it against bus_timing.h\n");
        return;
    }
    ASSERT_EQ(BUS_SERVE_NS, 146);
    ASSERT_EQ(BUS_CYCLE_CLK_FAST, 84);
    ASSERT_EQ(BUS_LATCH_CLK_FAST, 36);
    ASSERT_EQ(BUS_LATCH_CLK_SLOW, 72);
    ASSERT_EQ(BUS_MARGIN_CLK_FAST, 14);
    ASSERT_EQ(BUS_MARGIN_CLK_SLOW, 50);
}

int main(void) {
    RUN(serve_budget_closes_at_both_speeds);
    RUN(matches_the_150mhz_census);
    TEST_MAIN_END
}
