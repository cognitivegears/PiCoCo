#include "bus.h"
#include "hardware/sync.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/structs/busctrl.h"
#include "bus_engine.pio.h"
#include PICOCO_BOARD_H
#include <stdio.h>

/* The PIO + DMA engine (bus_engine.pio) serves ROM-window and I/O-page reads
 * with no CPU in the per-cycle path, on both boards; core1 has nothing to do.
 * The CPU loops this replaced are at tag fw-1.4-cpu-loop. Spike scope: no read
 * hooks, no write capture, no trace or counters, so the Becker port does not
 * work on this build, and the Plus-W's firmware-decoded $FF60-$FF7F
 * (bus_fw_mask) is gone.
 *
 * Read SM: PIO0 (GPIO base 0). Pico 2: it waits on OE_BUS (GP26) itself.
 * Plus-W: OE_BUS is GP40, outside PIO0's window, so bus_sel_pw watches it from
 * PIO2 (GPIO base 16, shared with the cyw43 driver) and raises PIO0 flags 0
 * (start), 1 (event SM, later) and 2 (end) across blocks.
 * DMA A: PIO0 RX (the table byte's address) -> DMA B READ_ADDR_TRIG, endless.
 * DMA B: that byte -> PIO0 TX, 8-bit, count 1, re-armed by every A write.
 * Everything below runs on core0. */

static PIO const epio = pio0;
static PIO hpio;                 /* helper's block: pio0 (Pico 2), pio2 (Plus-W) */
static int esm = -1, hsm, edma_a, edma_b;
static uint eoff, hoff;
static const pio_program_t *eprog;
static bool erunning;
static bool ebypass = true, eprio = true;   /* measured defaults, see docs/superpowers/specs/2026-10-02-pio-engine-spike.md */
/* Spike knobs. eorder: 0 = A (enable before pull), 1 = B (enable after).
 * etrig: 0 = pin (Pico 2: OE_BUS; Plus-W: E rise/fall, a calibration mode with
 * no select decode at all), 1 = helper one flag, 2 = helper two flags,
 * 3 = helper two flags started mid-cycle with no flag clear (naive form). */
static int eorder = 1;
#ifdef PICOCO_BOARD_PLUSW
static int etrig = 2;            /* the Plus-W design: helper on OE_BUS */
#define HELPER_PROG      bus_sel_pw_program
#define HELPER_CFG       bus_sel_pw_program_get_default_config
#define HELPER_FLAG1     bus_sel_pw_offset_flag1
#define HELPER_FALL      bus_sel_pw_offset_fall
#define HELPER_INSTR     bus_sel_pw_program_instructions
#define ENG_IN_MASK ((0x3FFFu << PIN_A0) | (1u << PIN_RW) | (1u << PIN_E))
#else
static int etrig = 0;            /* recommended: B on the pin (spike part 3) */
#define HELPER_PROG      bus_sel_p2_program
#define HELPER_CFG       bus_sel_p2_program_get_default_config
#define HELPER_FLAG1     bus_sel_p2_offset_flag1
#define HELPER_FALL      bus_sel_p2_offset_fall
#define HELPER_INSTR     bus_sel_p2_program_instructions
#define ENG_IN_MASK ((0x3FFFu << PIN_A0) | (1u << PIN_RW) | (1u << PIN_OE_BUS))
#endif

static void engine_load(void) {
    if (eprog) pio_remove_program(epio, eprog, eoff);
    eprog = eorder ? &bus_engine_late_program : &bus_engine_program;
    eoff = pio_add_program(epio, eprog);
    pio_sm_config c = eorder ? bus_engine_late_program_get_default_config(eoff) : bus_engine_program_get_default_config(eoff);
    sm_config_set_in_pins(&c, PIN_A0);
    sm_config_set_out_pins(&c, PIN_D0, 8);
    sm_config_set_jmp_pin(&c, PIN_RW);
    sm_config_set_in_shift(&c, false, false, 32);    /* shift left: isr = x << 14 | A13..A0 */
    sm_config_set_out_shift(&c, true, false, 32);
    sm_config_set_clkdiv(&c, 1.0f);
    pio_sm_init(epio, (uint)esm, eoff, &c);
    pio_sm_set_enabled(epio, (uint)esm, false);
    uint trig = eoff + (eorder ? bus_engine_late_offset_trig : bus_engine_offset_trig);
    uint wend = eoff + (eorder ? bus_engine_late_offset_wend : bus_engine_offset_wend);
    uint rend = eoff + (eorder ? bus_engine_late_offset_rend : bus_engine_offset_rend);
#ifdef PICOCO_BOARD_PLUSW
    if (!etrig) {                /* calibration: E itself, no decode */
        epio->instr_mem[trig] = (uint16_t)pio_encode_wait_gpio(true, PIN_E);
        epio->instr_mem[wend] = epio->instr_mem[rend] = (uint16_t)pio_encode_wait_gpio(false, PIN_E);
    } else {                     /* flags 0 (start) and 2 (end) from bus_sel_pw */
        epio->instr_mem[wend] = epio->instr_mem[rend] = (uint16_t)pio_encode_wait_irq(true, false, 2);
    }
#else
    (void)wend; (void)rend;
    if (!etrig) epio->instr_mem[trig] = (uint16_t)pio_encode_wait_gpio(false, PIN_OE_BUS);
#endif
    hpio->instr_mem[hoff + HELPER_FLAG1] = etrig >= 2 ? HELPER_INSTR[HELPER_FLAG1] : (uint16_t)pio_encode_nop();
}

static void engine_init(void) {
    esm = (int)pio_claim_unused_sm(epio, true);
#ifdef PICOCO_BOARD_PLUSW
    /* `irq next` from PIO2 lands in PIO0. The cyw43 driver normally got here
     * first and set PIO2's GPIO base to 16; if not, set it (PIO2 still empty). */
    hpio = pio2;
    if (pio_get_gpio_base(hpio) != 16) hard_assert(pio_set_gpio_base(hpio, 16) == PICO_OK);
#else
    hpio = epio;
#endif
    hsm = (int)pio_claim_unused_sm(hpio, true);
    hoff = pio_add_program(hpio, &HELPER_PROG);
    pio_sm_config hc = HELPER_CFG(hoff);
    sm_config_set_clkdiv(&hc, 1.0f);
    pio_sm_init(hpio, (uint)hsm, hoff, &hc);
    pio_sm_set_enabled(hpio, (uint)hsm, false);
    pio_sm_set_pins_with_mask(epio, (uint)esm, 0, 0xFFu << PIN_D0);
    pio_sm_set_pindirs_with_mask(epio, (uint)esm, 0, 0xFFu << PIN_D0);
    for (int g = PIN_D0; g < PIN_D0 + 8; g++) pio_gpio_init(epio, (uint)g);
    engine_load();

    edma_a = dma_claim_unused_channel(true);
    edma_b = dma_claim_unused_channel(true);
    dma_channel_config b = dma_channel_get_default_config((uint)edma_b);
    channel_config_set_transfer_data_size(&b, DMA_SIZE_8);
    channel_config_set_read_increment(&b, false);
    channel_config_set_write_increment(&b, false);
    channel_config_set_dreq(&b, pio_get_dreq(epio, (uint)esm, true));
    channel_config_set_high_priority(&b, true);
    dma_channel_configure((uint)edma_b, &b, &epio->txf[esm], bus_table, 1, false);
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
static inline __attribute__((always_inline)) void engine_enable(bool on, bool helper) {
#ifdef PICOCO_BOARD_PLUSW
    pio_set_sm_multi_mask_enabled(epio, helper ? (1u << hsm) : 0, 1u << esm, 0, on);
#else
    pio_set_sm_mask_enabled(epio, (1u << esm) | (helper ? (1u << hsm) : 0), on);
#endif
}

/* From the disable to the release must be a few clk: whatever was on D0-D7
 * stays driven until the two execs land. pio_sm_set_pins_with_mask() went pin
 * by pin and left them driven ~1 us (spike part 3); then, from flash, the
 * first stop after the XIP cache had been cleared out stalled on the fetch of
 * the execs and held a byte through the next two cycles (Plus-W, part 4). So:
 * SRAM, interrupts off. */
static void __no_inline_not_in_flash_func(engine_stop)(void) {
    uint32_t irq = save_and_disable_interrupts();
    engine_enable(false, true);
    epio->sm[esm].instr = pio_encode_mov(pio_pins, pio_null);       /* E9: low first... */
    epio->sm[esm].instr = 0xA063u;                                  /* ...then `mov pindirs, null` */
    restore_interrupts(irq);
    erunning = false;
}

static void engine_start(void) {
    /* A pointer still on its way from RX through DMA A to B would land in the
     * TX FIFO after the clear below and serve every later read one cycle late:
     * let both channels go quiet first. */
    while (!pio_sm_is_rx_fifo_empty(epio, (uint)esm) || dma_channel_is_busy((uint)edma_b)) { }
    busy_wait_at_least_cycles(64);
    pio_sm_clear_fifos(epio, (uint)esm);
    pio_sm_restart(epio, (uint)esm);
    pio_sm_restart(hpio, (uint)hsm);
    pio_sm_put(epio, (uint)esm, (uint32_t)(uintptr_t)bus_rom_base >> 14);
    pio_sm_exec(epio, (uint)esm, pio_encode_pull(false, true));
    pio_sm_exec(epio, (uint)esm, pio_encode_mov(pio_x, pio_osr));
    pio_sm_exec(epio, (uint)esm, pio_encode_jmp(eoff + (eorder ? bus_engine_late_wrap_target : bus_engine_wrap_target)));
    pio_sm_exec(hpio, (uint)hsm, pio_encode_jmp(hoff + (etrig == 3 ? HELPER_FALL : 0)));
    if (etrig != 3) epio->irq = 7u;                  /* no flag left from before the stop (the flags live in PIO0) */
    epio->input_sync_bypass = ebypass ? (epio->input_sync_bypass | ENG_IN_MASK) : (epio->input_sync_bypass & ~ENG_IN_MASK);
#ifdef PICOCO_BOARD_PLUSW
    pio_set_input_sync_bypass_with_mask64(hpio, ebypass ? (1ull << PIN_OE_BUS) : 0, 1ull << PIN_OE_BUS);
#endif
    busctrl_hw->priority = eprio ? (BUSCTRL_BUS_PRIORITY_DMA_R_BITS | BUSCTRL_BUS_PRIORITY_DMA_W_BITS) : 0;
    engine_enable(true, etrig != 0);                 /* together: no flag raised before the read SM runs */
    erunning = true;
}

void bus_engine_drive(bool on) {
    if (esm < 0) engine_init();
    if (erunning) engine_stop();
    if (on) engine_start();
}

/* core0, after bus_rom_base changed: x must point at the new window. The SM is
 * stopped for a few us; a cycle in that gap reads 0x00. */
void bus_engine_rebase(void) {
    if (erunning) { engine_stop(); engine_start(); }
}

void bus_engine_tune(int bypass, int prio) {
    if (bypass >= 0) ebypass = bypass;
    if (prio >= 0) eprio = prio;
    if (erunning) { engine_stop(); engine_start(); }
}
void bus_engine_get(bool *bypass, bool *prio) { *bypass = ebypass; *prio = eprio; }

void bus_engine_variant(int order, int trig) {
    bool run = erunning;
    if (esm < 0) engine_init();
    if (erunning) engine_stop();
    if (order >= 0) eorder = order;
    if (trig >= 0) etrig = trig;
    engine_load();
    if (run) engine_start();
}
const char *bus_engine_desc(void) {
#ifdef PICOCO_BOARD_PLUSW
    static const char *const t[] = { "trigger E pin (calibration, no decode)", "trigger helper 1 flag", "trigger helper 2 flags", "trigger helper 2 flags naive start" };
#else
    static const char *const t[] = { "trigger OE_BUS pin", "trigger helper 1 flag", "trigger helper 2 flags", "trigger helper 2 flags naive start" };
#endif
    static char buf[64];
    snprintf(buf, sizeof buf, "order %s, %s", eorder ? "B (enable after pull)" : "A (enable before pull)", t[etrig]);
    return buf;
}

/* Who holds what: the engine's own SMs/channels and every claimed SM and DMA
 * channel in the chip (the cyw43 driver's included). */
void bus_engine_resources(void (*line)(const char *s)) {
    char buf[128];
    if (esm < 0) { line("bus engine not started"); return; }
    snprintf(buf, sizeof buf, "bus engine read pio0 sm%d, helper pio%u sm%d, dma A %d B %d",
             esm, pio_get_index(hpio), hsm, edma_a, edma_b);
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

BUS_HOT void bus_core1_main(void) {
    (void)save_and_disable_interrupts();
    for (;;) __wfe();
}
