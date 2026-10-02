#include "bus.h"
#include "bus_engine.h"
#include "hardware/sync.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/structs/busctrl.h"
#include "bus_engine.pio.h"
#include PICOCO_BOARD_H
#include <stdio.h>

/* The PIO + DMA engine (bus_engine.pio) serves ROM-window and I/O-page reads
 * with no CPU in the per-cycle path, on both boards. The CPU loops it
 * replaced are at tag fw-1.4-cpu-loop.
 *
 * Read SM: PIO0 (GPIO base 0). Pico 2: it waits on OE_BUS (GP26) itself.
 * Plus-W: OE_BUS is GP40, outside PIO0's window, so bus_sel_pw watches it from
 * PIO2 (GPIO base 16, shared with the cyw43 driver) and raises PIO0 flags 0
 * (start), 1 (event SM, later) and 2 (end) across blocks.
 * DMA A: PIO0 RX (the table byte's address) -> DMA B READ_ADDR_TRIG, endless.
 * DMA B: that byte -> PIO0 TX, 8-bit, count 1, re-armed by every A write.
 * Input sync bypass on A0-A13 and R/W only: OE_BUS keeps its synchroniser (an
 * asynchronous strobe). DMA has bus priority.
 * Everything here runs on core0 except bus_engine_set_bank. */

static PIO const epio = pio0;
static int esm = -1, edma_a, edma_b;
static uint eoff;
static volatile bool erunning;
static spin_lock_t *elock;       /* stop/start vs bus_engine_set_bank (the two cores) */
#define ENG_BYPASS ((0x3FFFu << PIN_A0) | (1u << PIN_RW))
#ifdef PICOCO_BOARD_PLUSW
static PIO hpio;                 /* the helper's block: pio2 */
static int hsm = -1;
static uint hoff;
#endif

static void engine_init(void) {
    elock = spin_lock_init((uint)spin_lock_claim_unused(true));
    esm = (int)pio_claim_unused_sm(epio, true);
    eoff = pio_add_program(epio, &bus_read_program);
    pio_sm_config c = bus_read_program_get_default_config(eoff);
    sm_config_set_in_pins(&c, PIN_A0);
    sm_config_set_out_pins(&c, PIN_D0, 8);
    sm_config_set_jmp_pin(&c, PIN_RW);
    sm_config_set_in_shift(&c, false, false, 32);    /* shift left: isr = y << 17 | x << 14 | A13..A0 */
    sm_config_set_out_shift(&c, true, false, 32);
    sm_config_set_clkdiv(&c, 1.0f);
    pio_sm_init(epio, (uint)esm, eoff, &c);
    pio_sm_set_enabled(epio, (uint)esm, false);
#ifdef PICOCO_BOARD_PLUSW
    /* flags 0 (start) and 2 (end) from bus_sel_pw */
    epio->instr_mem[eoff + bus_read_offset_trig] = (uint16_t)pio_encode_wait_irq(true, false, 0);
    epio->instr_mem[eoff + bus_read_offset_wend] = epio->instr_mem[eoff + bus_read_offset_rend] =
        (uint16_t)pio_encode_wait_irq(true, false, 2);
    /* `irq next` from PIO2 lands in PIO0. The cyw43 driver normally got here
     * first and set PIO2's GPIO base to 16; if not, set it (PIO2 still empty). */
    hpio = pio2;
    if (pio_get_gpio_base(hpio) != 16) hard_assert(pio_set_gpio_base(hpio, 16) == PICO_OK);
    hsm = (int)pio_claim_unused_sm(hpio, true);
    hoff = pio_add_program(hpio, &bus_sel_pw_program);
    pio_sm_config hc = bus_sel_pw_program_get_default_config(hoff);
    sm_config_set_clkdiv(&hc, 1.0f);
    pio_sm_init(hpio, (uint)hsm, hoff, &hc);
    pio_sm_set_enabled(hpio, (uint)hsm, false);
#endif
    pio_sm_set_pins_with_mask(epio, (uint)esm, 0, 0xFFu << PIN_D0);
    pio_sm_set_pindirs_with_mask(epio, (uint)esm, 0, 0xFFu << PIN_D0);
    for (int g = PIN_D0; g < PIN_D0 + 8; g++) pio_gpio_init(epio, (uint)g);

    edma_a = dma_claim_unused_channel(true);
    edma_b = dma_claim_unused_channel(true);
    dma_channel_config b = dma_channel_get_default_config((uint)edma_b);
    channel_config_set_transfer_data_size(&b, DMA_SIZE_8);
    channel_config_set_read_increment(&b, false);
    channel_config_set_write_increment(&b, false);
    channel_config_set_dreq(&b, pio_get_dreq(epio, (uint)esm, true));
    channel_config_set_high_priority(&b, true);
    dma_channel_configure((uint)edma_b, &b, &epio->txf[esm], bus_mem, 1, false);
    dma_channel_config a = dma_channel_get_default_config((uint)edma_a);
    channel_config_set_transfer_data_size(&a, DMA_SIZE_32);
    channel_config_set_read_increment(&a, false);
    channel_config_set_write_increment(&a, false);
    channel_config_set_dreq(&a, pio_get_dreq(epio, (uint)esm, false));
    channel_config_set_high_priority(&a, true);
    dma_channel_configure((uint)edma_a, &a, &dma_hw->ch[edma_b].al3_read_addr_trig, &epio->rxf[esm],
                          dma_encode_endless_transfer_count(), true);
}

/* Read SM and helper start and stop in one register write. Plus-W: PIO2 is
 * PIO0's "prev" neighbour. */
static inline __attribute__((always_inline)) void engine_enable(bool on) {
#ifdef PICOCO_BOARD_PLUSW
    pio_set_sm_multi_mask_enabled(epio, 1u << hsm, 1u << esm, 0, on);
#else
    pio_sm_set_enabled(epio, (uint)esm, on);
#endif
}

/* From the disable to the release must be a few clk: whatever was on D0-D7
 * stays driven until the two execs land. pio_sm_set_pins_with_mask() went pin
 * by pin and left them driven ~1 us (spike part 3); then, from flash, the
 * first stop after the XIP cache had been cleared out stalled on the fetch of
 * the execs and held a byte through the next two cycles (Plus-W, part 4). So:
 * SRAM, interrupts off. */
static void __no_inline_not_in_flash_func(engine_stop)(void) {
    uint32_t irq = spin_lock_blocking(elock);
    engine_enable(false);
    epio->sm[esm].instr = pio_encode_mov(pio_pins, pio_null);       /* E9: low first... */
    epio->sm[esm].instr = 0xA063u;                                  /* ...then `mov pindirs, null` */
    erunning = false;
    spin_unlock(elock, irq);
}

static void engine_start(void) {
    /* A pointer still on its way from RX through DMA A to B would land in the
     * TX FIFO after the clear below and serve every later read one cycle late:
     * let both channels go quiet first. */
    while (!pio_sm_is_rx_fifo_empty(epio, (uint)esm) || dma_channel_is_busy((uint)edma_b)) { }
    busy_wait_at_least_cycles(64);
    pio_sm_clear_fifos(epio, (uint)esm);
    pio_sm_restart(epio, (uint)esm);
    pio_sm_put(epio, (uint)esm, (uint32_t)(uintptr_t)bus_mem >> 17);
    pio_sm_exec(epio, (uint)esm, pio_encode_pull(false, true));
    pio_sm_exec(epio, (uint)esm, pio_encode_mov(pio_y, pio_osr));
    pio_sm_exec(epio, (uint)esm, pio_encode_jmp(eoff + bus_read_offset_top));
    epio->input_sync_bypass |= ENG_BYPASS;
#ifdef PICOCO_BOARD_PLUSW
    pio_sm_restart(hpio, (uint)hsm);
    pio_sm_exec(hpio, (uint)hsm, pio_encode_jmp(hoff));   /* the `wait 1`: a cycle already under way is skipped */
    pio_set_input_sync_bypass_with_mask64(hpio, 0, 1ull << PIN_OE_BUS);
    epio->irq = 7u;                                  /* no flag left from before the stop (the flags live in PIO0) */
#endif
    busctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_R_BITS | BUSCTRL_BUS_PRIORITY_DMA_W_BITS;
    uint32_t irq = spin_lock_blocking(elock);
    pio_sm_exec(epio, (uint)esm, pio_encode_set(pio_x, bus_bank));   /* under the lock: a bank switch meanwhile is not lost */
    erunning = true;
    engine_enable(true);                             /* Plus-W: together, so no flag is raised before the read SM runs */
    spin_unlock(elock, irq);
}

void bus_engine_drive(bool on) {
    if (esm < 0) engine_init();
    if (erunning) engine_stop();
    if (on) engine_start();
}

/* Bank for the next cycle: `set x, bank`, and, if the SM is idle at the start
 * wait, `jmp top` so the pointer it precomputed is rebuilt. Anywhere else in
 * the program the SM passes `top` before its next cycle and picks up the new
 * X by itself. A `jmp top` that lands between a read's push and its pull
 * leaves DMA B's byte in the TX FIFO for the next cycle and serves every
 * later read one cycle late (measured: 1551-1770 of 2048 reads wrong under
 * a switch burst), so the jump is issued only with the SM paused at `trig`.
 * The pause covers one PC read and the jump: a cycle that starts in it is
 * served a few clk late. SRAM, interrupts off (the lock): it must never
 * stall on a flash fetch with the SM paused. */
BUS_HOT void bus_engine_set_bank(uint8_t bank) {
    if (esm < 0) return;                             /* engine never started: engine_start loads bus_bank */
    pio_sm_hw_t *s = &epio->sm[esm];
    io_rw_32 *pause = hw_clear_alias(&epio->ctrl), *resume = hw_set_alias(&epio->ctrl);
    uint32_t set = pio_encode_set(pio_x, bank), jmp = pio_encode_jmp(eoff + bus_read_offset_top);
    uint32_t trig = eoff + bus_read_offset_trig, mask = 1u << esm;
    uint32_t irq = spin_lock_blocking(elock);
    bool run = erunning;
    s->instr = set;                                  /* safe anywhere in the program */
    if (s->addr == trig) {                           /* idle at the start wait (or just leaving it) */
        *pause = mask;
        if (s->addr == trig) s->instr = jmp;         /* paused: it cannot leave `trig` before the jump */
        if (run) *resume = mask;
    }
    spin_unlock(elock, irq);
}

/* Who holds what: the engine's own SMs/channels and every claimed SM and DMA
 * channel in the chip (the cyw43 driver's included). */
void bus_engine_resources(void (*line)(const char *s)) {
    char buf[128];
    if (esm < 0) { line("bus engine not started"); return; }
#ifdef PICOCO_BOARD_PLUSW
    snprintf(buf, sizeof buf, "bus engine read pio0 sm%d, helper pio%u sm%d, dma A %d B %d",
             esm, pio_get_index(hpio), hsm, edma_a, edma_b);
#else
    snprintf(buf, sizeof buf, "bus engine read pio0 sm%d, dma A %d B %d", esm, edma_a, edma_b);
#endif
    line(buf);
    for (uint i = 0; i < NUM_PIOS; i++) {
        PIO p = pio_get_instance(i);
        int pos = snprintf(buf, sizeof buf, "pio%u gpio_base %u claimed sm", i, pio_get_gpio_base(p));
        for (uint s = 0; s < 4; s++) if (pio_sm_is_claimed(p, s)) pos += snprintf(buf + pos, sizeof buf - pos, " %u", s);
        line(buf);
    }
    int pos = snprintf(buf, sizeof buf, "dma claimed");
    for (uint ch = 0; ch < NUM_DMA_CHANNELS; ch++) if (dma_channel_is_claimed(ch)) pos += snprintf(buf + pos, sizeof buf - pos, " %u", ch);
    line(buf);
}
