#include "bus.h"
#include "bus_engine.h"
#include "hardware/sync.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/structs/busctrl.h"
#include "hardware/structs/sio.h"
#include "bus_engine.pio.h"
#include PICOCO_BOARD_H
#include <stdio.h>

/* The PIO + DMA engine (bus_engine.pio) serves ROM-window and I/O-page reads
 * with no CPU in the per-cycle path, on both boards, and records every
 * selected cycle for core1. The CPU loops it replaced are at tag
 * fw-1.4-cpu-loop.
 *
 * Read SM: PIO0 (GPIO base 0). Pico 2: it waits on OE_BUS (GP26) itself.
 * Plus-W: OE_BUS is GP40, outside PIO0's window, so bus_sel_pw watches it from
 * PIO2 (GPIO base 16, shared with the cyw43 driver) and raises PIO0 flags 0
 * (start), 1 (event SM) and 2 (end) across blocks. It runs from engine_init
 * on, never stopped.
 * DMA A: PIO0 RX (the table byte's address) -> DMA B READ_ADDR_TRIG, endless.
 * DMA B: that byte -> PIO0 TX, 8-bit, count 1, re-armed by every A write.
 * Event SM: PIO0, one word per selected cycle; DMA C: its RX -> bus_events,
 * an 8 KB write ring, endless. Both start once (engine_init) and never stop.
 * Input sync bypass on A0-A13 and R/W only: the strobe (GP26) and D0-D7 keep
 * their synchronisers. DMA has bus priority.
 * Everything here runs on core0 except the bank calls. */

_Static_assert(PIN_D0 == 0 && PIN_A0 == 8 && PIN_RW == 22, "event word layout (bus.h) is GP0..GP22");

static PIO const epio = pio0;
static int esm = -1, edma_a, edma_b;
static int evsm = -1, edma_c;
static uint eoff;
static spin_lock_t *elock;       /* stop/start vs the bank calls (the two cores) */
#define ENG_BYPASS ((0x3FFFu << PIN_A0) | (1u << PIN_RW))
#ifdef PICOCO_BOARD_PLUSW
static PIO hpio;                 /* the helper's block: pio2 */
static int hsm = -1;
static uint hoff;
#endif

static void event_init(void) {
    evsm = (int)pio_claim_unused_sm(epio, true);
#ifdef PICOCO_BOARD_PLUSW
    uint off = pio_add_program(epio, &bus_event_pw_program);
    pio_sm_config c = bus_event_pw_program_get_default_config(off);
    sm_config_set_jmp_pin(&c, PIN_E);
#else
    uint off = pio_add_program(epio, &bus_event_p2_program);
    pio_sm_config c = bus_event_p2_program_get_default_config(off);
    sm_config_set_jmp_pin(&c, PIN_OE_BUS);
#endif
    sm_config_set_in_pins(&c, PIN_D0);
    sm_config_set_in_pin_count(&c, 23);              /* D0-D7, A0-A13, R/W; bits 23-31 read 0 */
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&c, 1.0f);
    pio_sm_init(epio, (uint)evsm, off, &c);

    /* The ring must hold only empty slots before DMA C writes its first word:
     * core1 reads slot 0 first (bus_core1.c). */
    for (int i = 0; i < BUS_EVENTS; i++) bus_events[i] = BUS_EV_NONE;
    edma_c = dma_claim_unused_channel(true);
    dma_channel_config cc = dma_channel_get_default_config((uint)edma_c);
    channel_config_set_transfer_data_size(&cc, DMA_SIZE_32);
    channel_config_set_read_increment(&cc, false);
    channel_config_set_write_increment(&cc, true);
    channel_config_set_ring(&cc, true, 13);          /* 2048 words: the write address wraps in bus_events */
    channel_config_set_dreq(&cc, pio_get_dreq(epio, (uint)evsm, false));
    channel_config_set_high_priority(&cc, true);
    dma_channel_configure((uint)edma_c, &cc, (void *)bus_events, &epio->rxf[evsm],
                          dma_encode_endless_transfer_count(), true);
    pio_sm_set_enabled(epio, (uint)evsm, true);
}

static void engine_init(void) {
    hard_assert(((uintptr_t)bus_mem & 0x1FFFF) == 0);
    hard_assert(((uintptr_t)bus_events & (sizeof bus_events - 1)) == 0);
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
    /* The whole register, so the strobe's bit (GP26) is known to be clear.
     * Nothing else writes PIO0's. */
    epio->input_sync_bypass = ENG_BYPASS;
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
    busctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_R_BITS | BUSCTRL_BUS_PRIORITY_DMA_W_BITS;
    event_init();
#ifdef PICOCO_BOARD_PLUSW
    /* The helper runs from here on, through every stop, start and bus drive
     * off, so the event SM (flag 1) never misses a cycle. GP40 keeps its
     * synchroniser (an asynchronous strobe); set once, here. */
    pio_set_input_sync_bypass_with_mask64(hpio, 0, 1ull << PIN_OE_BUS);
    pio_sm_set_enabled(hpio, (uint)hsm, true);
#endif
}

/* The read SM only: the event SM and the Plus-W helper never stop. */
static inline __attribute__((always_inline)) void engine_enable(bool on) {
    pio_sm_set_enabled(epio, (uint)esm, on);
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
    spin_unlock(elock, irq);
}

/* SRAM, like engine_stop: the enable must not wait on a flash fetch. */
static void __no_inline_not_in_flash_func(engine_start)(void) {
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
    uint32_t irq = spin_lock_blocking(elock);
    pio_sm_exec(epio, (uint)esm, pio_encode_set(pio_x, bus_bank));   /* under the lock: a bank switch meanwhile is not lost */
#ifdef PICOCO_BOARD_PLUSW
    /* While the read SM was stopped the running helper left flags 0 and 2 set
     * (they live in PIO0; flag 1 is the event SM's). Clear them only between
     * cycles: a cleared start whose end flag comes later would leave that end
     * flag pending and release every later read at once. So: OE_BUS (GP40,
     * read through SIO) high for 16 clk, so the last cycle's end flag is in
     * (the helper raises it a few clk after the rise), clear, and still high
     * after the clear, so no new cycle's start was cleared. A cycle that starts
     * after that is served from its start flag, late, as on a Pico 2. Bounded
     * by one bus cycle while the CoCo runs; no wait at all when it is idle. */
    const uint32_t oe = 1u << (PIN_OE_BUS - 32);
    for (;;) {
        while (!(sio_hw->gpio_hi_in & oe)) { }
        busy_wait_at_least_cycles(16);
        if (!(sio_hw->gpio_hi_in & oe)) continue;
        epio->irq = 5u;
        if (sio_hw->gpio_hi_in & oe) break;
    }
#endif
    engine_enable(true);
    spin_unlock(elock, irq);
}

void bus_engine_drive(bool on) {
    if (esm < 0) engine_init();
    engine_stop();
    if (on) engine_start();
}

void bus_engine_init(void) {
    if (esm < 0) bus_engine_drive(bus_drive_get());
}

void bus_engine_check_drops(void) {
    if (evsm < 0) return;
    /* RXSTALL is also set by a `push noblock` that found the RX FIFO full,
     * i.e. a dropped event (RP2350 datasheet, FDEBUG; `bus selftest fast`
     * confirms it on the chip). One count per check that finds it set. */
    uint32_t bit = 1u << (PIO_FDEBUG_RXSTALL_LSB + evsm);
    if (epio->fdebug & bit) { epio->fdebug = bit; bus_stats.event_drop++; }
}

void bus_engine_tick(uint32_t now_ms) {
    static uint32_t last;
    if (now_ms == last) return;
    last = now_ms;
    bus_engine_check_drops();
}

/* Before engine_init only core0 runs (core1 starts after bus_engine_init),
 * so interrupts off is the whole lock. */
BUS_HOT uint32_t bus_engine_lock(void) {
    return elock ? spin_lock_blocking(elock) : save_and_disable_interrupts();
}

BUS_HOT void bus_engine_unlock(uint32_t saved) {
    if (elock) spin_unlock(elock, saved);
    else restore_interrupts(saved);
}

/* Bank for the next read: one exec'd `set x, bank`, under the lock with
 * the bus_bank store, so the record and the register never disagree. No
 * pause, no PC read, no jump: an exec'd instruction runs at an instruction
 * boundary; an SM stalled in a `wait` or `pull` runs it and stays stalled, a
 * running one is delayed by 1 clk (`bus selftest fast` switches at every
 * phase of the cycle). The read program takes the bank (`in x, 3`) right
 * after the start wait, so a bank write takes effect for every cycle whose
 * `in x, 3` has not yet run; a write that lands after it leaves that one read
 * served from the old bank. Nothing is ever desynchronised or served from a
 * wrong address. SRAM: core1's $FF40 hook calls it. */
BUS_HOT void bus_engine_set_bank_locked(uint8_t bank) {
    bank &= BUS_BANKS - 1;                           /* bus_peek indexes bus_mem with it */
    bus_bank = bank;
    if (esm >= 0) epio->sm[esm].instr = pio_encode_set(pio_x, bank);   /* never started: engine_start loads bus_bank */
}

BUS_HOT void bus_engine_set_bank(uint8_t bank) {
    uint32_t irq = bus_engine_lock();
    bus_engine_set_bank_locked(bank);
    bus_engine_unlock(irq);
}

/* Who holds what: the engine's own SMs/channels and every claimed SM and DMA
 * channel in the chip (the cyw43 driver's included). */
void bus_engine_resources(void (*line)(const char *s)) {
    char buf[128];
    if (esm < 0) { line("bus engine not started"); return; }
#ifdef PICOCO_BOARD_PLUSW
    snprintf(buf, sizeof buf, "bus engine read pio0 sm%d, event sm%d, helper pio%u sm%d, dma A %d B %d C %d",
             esm, evsm, pio_get_index(hpio), hsm, edma_a, edma_b, edma_c);
#else
    snprintf(buf, sizeof buf, "bus engine read pio0 sm%d, event sm%d, dma A %d B %d C %d", esm, evsm, edma_a, edma_b, edma_c);
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
