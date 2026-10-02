# PIO Bus Engine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the CPU-polled bus loops with one PIO + DMA engine on both boards: reads served with no CPU in the path, everything else (writes, device hooks, trace) on an event stream, so back-to-back cartridge cycles at 1.79 MHz work and future devices cost no read latency.

**Architecture:** A read state machine captures the address and two chained DMA channels fetch the byte from a 128 KB-aligned table whose bank number is part of the pointer. An event state machine records each selected cycle at its end; a third DMA channel drains it into a ring that core1 walks, running hooks and feeding core0's write queue. The spike on branch `pio-engine` already has the read path running on both bare modules; this plan turns it into the full engine and removes the spike's knobs.

**Tech Stack:** C11 (Pico SDK, RP2350), PIO (pioasm, PIO version 1), DMA, the on-chip fake 6809 self-test, host build with ctest.

**Spec:** `docs/superpowers/specs/2026-10-02-pio-bus-engine-design.md`. Measured basis: `docs/superpowers/specs/2026-10-02-pio-engine-spike.md`.

## Global Constraints

- Branch `pio-engine`. No build switch for the old loops (tag `fw-1.4-cpu-loop` holds them).
- Read latency budget: 36 clk at 1.79 MHz. Measured today: Pico 2 20 clk, Plus-W 23 clk (corrected). **No task may add an instruction between the read program's start wait and `push`, or between `pull` and `mov pindirs`.** Any change to `bus_engine.pio` is followed by `bus selftest fast` on both boards with the response figure in the task report.
- Outputs are enabled only after the byte has been pulled.
- Pico 2: read and event state machines wait on `OE_BUS` (GP26) directly. Plus-W: `bus_sel_pw` in PIO2 (GPIO base 16) watches GP40 and raises PIO0 flags 0 (read start), 1 (event start), 2 (end). The helper must stay in PIO2.
- DMA channels and PIO state machines are claimed through the SDK allocators, never by number (the cyw43 driver holds PIO2 sm0 and DMA 0-1 on a Plus-W).
- core1 runs only SRAM code: the event loop, hooks and anything they call are `BUS_HOT` or `static inline`; `check_core1_flash_free.py` must print `ok` on both targets with its entry list updated to the new entry points. core1 reads no PIO register per event (DMA registers are fine).
- Engine start/stop and the bank-switch register writes run from SRAM with interrupts off.
- Single-writer rule: only core1 hooks write the Becker status/data table entries; core0 only pushes bytes to the ring.
- Device-owned I/O entries (index `0x3F40-0x3F5F`) are written only through `bus_io_set`, which stores into all eight banks. ROM loaders never write that range.
- Plus-W: `OE_FW` (GP31) held high; A14/A15 unused; no firmware-decoded addresses.
- Self-tests run on **bare modules only**. Before any `bus selftest`, `status` must show no live bus; never run one on a PCB or with a CoCo attached.
- Host suite green and warning-free: `cmake --build build-host && ctest --test-dir build-host --output-on-failure`. Both Pico targets link: `ninja -C build-pico && ninja -C build-pico-plusw`.
- Every commit message ends with:
  `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_018Kxf6M2Y3x8KSxMVEtm3kk`

## Hardware for Tasks 1-5

Two bare modules on USB at once: a Pico 2 and a Waveshare RP2350B-Plus-W. Tell them apart by console (`net status` answers with radio details only on the Plus-W; `version` shows the build). Flash the Pico 2 with `build-pico/picoco.uf2` and the Plus-W with `build-pico-plusw/picoco.uf2`, one at a time through `bootsel` (only one module is ever in BOOTSEL). The Plus-W's saved config has `becker net`; after a reboot it can take ~10 s to answer.

## Review Focus

1. **A byte the CoCo wrote must never be lost or reordered**, including two writes on consecutive cycles and a write straight after a read. Test: write capture in Task 2.
2. **A Becker byte must be delivered exactly once**: no phantom read when the status said "not ready", no duplicate, no loss, at the tightest real poll spacing at 1.79 MHz. Test in Task 3.
3. **A device read must return the same value whichever bank is selected.** Test in Task 3.
4. **The data pins must not stay driven when the engine is stopped, restarted or stalled.** Tests in Task 4 (and the spike's restart bursts, kept).
5. **core1 must keep up when every cycle is a cart cycle at 1.79 MHz**, with Becker traffic mixed in. Test in Task 2 and again in Task 3.

---

## File Structure

| File | Responsibility |
|---|---|
| `firmware/src/bus/bus_engine.pio` | Read program, event programs (one per board), Plus-W select helper |
| `firmware/src/bus/bus_engine.c` (new, from `bus_core1.c`'s engine half) | core0 side: load, start, stop, bank register writes, stall guard, resources |
| `firmware/src/bus/bus_core1.c` | core1 event loop only |
| `firmware/src/bus/bus.c`, `bus.h` | `bus_mem`, `bus_io_set`, `bus_event`, hooks, write queue, trace, counters |
| `firmware/src/dev/rom.c` | Loads into `bus_mem`; `$FF40` hook calls `bus_engine_set_bank` |
| `firmware/src/dev/becker.c` | Hooks unchanged in logic; table writes through `bus_io_set` |
| `firmware/src/bus/fake6809.c`, `.pio` | The self-test, extended; the legacy test removed in Task 5 |
| `firmware/host/sim_bus.c`, `firmware/tests/*` | Host simulator and tests on `bus_event` |
| `firmware/tools/tracedump.py`, `bench.py` | New trace line format; `bench.py` runs `bus selftest fast` |

---

### Task 1: One engine, table with the bank in the pointer

**Files:**
- Create: `firmware/src/bus/bus_engine.c`, `firmware/src/bus/bus_engine.h`
- Modify: `firmware/src/bus/bus_engine.pio`, `bus_core1.c`, `bus.c`, `bus.h`, `firmware/src/dev/rom.c`, `firmware/src/dev/becker.c`, `firmware/src/console/console.c`, `firmware/src/bus/fake6809.c`, `firmware/CMakeLists.txt`, `firmware/tests/test_bus.c`, `test_rom.c`

**Interfaces:**
- Produces:

```c
/* bus.h */
#define BUS_BANKS 8
#define BUS_IO_LO 0x3F40           /* device-owned entries, mirrored in every bank */
#define BUS_IO_HI 0x3F5F
extern uint8_t bus_mem[BUS_BANKS][BUS_TABLE_SIZE];   /* aligned to 128 KB */
#define bus_table (bus_mem[0])
extern volatile uint8_t bus_bank;                     /* current bank, 0 when unbanked */
static inline uint8_t bus_peek(uint16_t idx) { return bus_mem[bus_bank][idx]; }
void bus_io_set(uint16_t idx, uint8_t v);             /* BUS_HOT: stores into all BUS_BANKS banks */

/* bus_engine.h */
void bus_engine_drive(bool on);          /* core0: start/stop the read path (bus_drive) */
void bus_engine_set_bank(uint8_t bank);  /* BUS_HOT, core1 or core0: bank for the next cycle */
void bus_engine_resources(void (*line)(const char *s));
```

- [ ] **Step 1: Collapse the spike's variants.** In `bus_engine.pio` keep one read program (the late-enable ordering), the Plus-W helper `bus_sel_pw`, and delete `bus_engine` (variation A) and `bus_sel_p2`. Delete `etrig`, `eorder`, `bus_engine_variant`, `bus_engine_tune`, `bus_engine_get`, `bus_engine_desc` and the console switches that drive them. Input sync bypass: off for the select strobe, on for A0-A13 and R/W; DMA bus priority on. Move the core0 engine code from `bus_core1.c` to `bus_engine.c`.

- [ ] **Step 2: The read program with the bank in the pointer.**

```
.program bus_read
.pio_version 1
.wrap_target
top:
    mov isr, y              ; y = &bus_mem >> 17 (constant)
    in x, 3                 ; x = bank
public trig:
    wait 0 gpio 26          ; Pico 2 as assembled; Plus-W: patched to `wait 1 irq 0`
    jmp pin rd              ; R/W high = CoCo read
public wend:
    wait 1 gpio 26          ; a write: never driven. Plus-W: `wait 1 irq 2`
    jmp top
rd:
    in pins, 14             ; isr = &bus_mem[x][A13..A0]
    push noblock
    pull block              ; the byte, from DMA B
    out pins, 8
    mov pindirs, ~null      ; drive only once the byte is in the latch
public rend:
    wait 1 gpio 26          ; end of the cycle. Plus-W: `wait 1 irq 2`
    mov pins, null [1]      ; RP2350-E9: drive low two clk...
    mov pindirs, null       ; ...then release
.wrap
```

`in_shift` left, no autopush; `Y` and `X` are loaded with `pio_sm_put` + exec'd `pull` / `mov` while the state machine is stopped, in `engine_start`.

- [ ] **Step 3: `bus_mem` and the loaders.** `uint8_t bus_mem[BUS_BANKS][BUS_TABLE_SIZE] __attribute__((aligned(BUS_BANKS * BUS_TABLE_SIZE)))` in `bus.c`; remove `bus_rom_base`, `bus_set_rom_base`, `rom_banks`, `BUS_WINDOW_ALIGN`. `bus_init` fills all banks with `0xFF` and sets `bus_bank = 0`. `rom.c`: 8K/16K images and banked images load into `bus_mem[b]`, skipping `BUS_IO_LO..BUS_IO_HI` in every bank; `rom_off`/`rom_pattern` likewise; the `$FF40` hook becomes

```c
static BUS_HOT void rom_bank_hook(uint8_t data) {
    if (rom_nbanks) { bus_bank = data & rom_bank_mask; bus_engine_set_bank(bus_bank); }
}
```

and unbanking sets bank 0. `becker_refresh` and the data hook write through `bus_io_set`. `bus_set_read` on an index in `BUS_IO_LO..BUS_IO_HI` goes through `bus_io_set`.

- [ ] **Step 4: `bus_engine_set_bank`.** In SRAM, interrupts off around two writes to the read state machine's `instr` register: `set x, bank`, then `jmp top` (so the precomputed pointer is rebuilt). Host build: a stub that only stores the bank.

- [ ] **Step 5: Host tests.** In `firmware/tests/test_bus.c` add:

```c
TEST(io_set_reaches_every_bank) {
    bus_init();
    bus_io_set(BUS_IDX_BECKER_STATUS, 0x02);
    for (int b = 0; b < BUS_BANKS; b++) ASSERT_EQ(bus_mem[b][BUS_IDX_BECKER_STATUS], 0x02);
}

TEST(peek_follows_the_bank) {
    bus_init();
    bus_mem[0][0x10] = 0xA0; bus_mem[3][0x10] = 0xA3;
    ASSERT_EQ(bus_peek(0x10), 0xA0);
    bus_bank = 3;
    ASSERT_EQ(bus_peek(0x10), 0xA3);
    bus_bank = 0;
}
```

and in `test_rom.c` a test that a banked load leaves `bus_mem[b][BUS_IDX_BECKER_STATUS]` untouched in every bank. Update existing tests that used `bus_rom_base`. Run them red, then green.

- [ ] **Step 6: Self-test, both boards.** `bus selftest fast` gains a banked burst: fill each of the 8 banks with a distinct pattern, `bus_engine_set_bank(b)`, run a burst, expect that bank's bytes; and a line confirming the I/O page entry reads the same in every bank. Expected output on each board, in addition to the spike's lines:

```
fast banks: 8/8 banks read their own pattern, 0 mismatches
fast io page: same in 8 banks
fast 1.79MHz response_clk <n>
selftest fast pass
```

Record `response_clk` for both boards in the report: it must not exceed 20 (Pico 2) and 28 raw / 23 corrected (Plus-W).

- [ ] **Step 7: Build everything and commit.**

```bash
cmake --build build-host && ctest --test-dir build-host --output-on-failure
ninja -C build-pico && ninja -C build-pico-plusw
git add firmware
git commit -m "engine: one read program, bank in the pointer, bus_mem with mirrored I/O entries"
```

---

### Task 2: Event stream — capture, ring, core1 loop

**Files:**
- Modify: `firmware/src/bus/bus_engine.pio`, `bus_engine.c`, `bus_core1.c`, `bus.c`, `bus.h`, `firmware/host/sim_bus.c`, `firmware/src/console/console.c`, `firmware/src/bus/fake6809.c`, `firmware/tools/tracedump.py`, `firmware/tools/check_core1_flash_free.py` (entry list in `CMakeLists.txt`), `firmware/tests/test_bus.c`

**Interfaces:**
- Consumes: Task 1's engine start/stop.
- Produces:

```c
/* bus.h */
#define BUS_EVENTS 2048                       /* ring entries, 32-bit each */
extern uint32_t bus_events[BUS_EVENTS];        /* aligned to its size in bytes */
#define BUS_EV_DATA(w) ((uint8_t)(w))
#define BUS_EV_IDX(w)  ((uint16_t)(((w) >> 8) & 0x3FFF))
#define BUS_EV_RD(w)   (((w) >> 22) & 1u)
void bus_event(uint32_t w);                    /* BUS_HOT: one cycle; hooks, write queue, counters */
typedef struct { uint32_t seq; uint16_t idx; uint8_t rw; uint8_t data; } bus_trace_entry;
/* bus_stats_t: cycles, reads, writes, write_overrun, whooks_run, engine_stall, event_lag_max, event_drop */
uint32_t bus_engine_event_head(void);          /* BUS_HOT: ring index DMA C will write next */
```

- [ ] **Step 1: Event programs.** `in_base` GP0 so one `mov isr, pins` takes D0-D7, A0-A13 and R/W.

```
; Pico 2: jmp_pin = OE_BUS (GP26)
.program bus_event_p2
.pio_version 1
.wrap_target
    wait 1 gpio 26          ; between cycles
    wait 0 gpio 26          ; a selected cycle
smp:
    mov isr, pins
    jmp pin done            ; OE_BUS high: over; this sample may be from after the end
    mov x, isr              ; a sample taken while the cycle was still on
    jmp smp
done:
    mov isr, x
    push noblock
.wrap

; Plus-W: jmp_pin = E (GP26); flag 1 from bus_sel_pw starts it
.program bus_event_pw
.pio_version 1
.wrap_target
    wait 1 irq 1
smp:
    mov isr, pins
    jmp pin keep            ; E still high
    jmp done
keep:
    mov x, isr
    jmp smp
done:
    mov isr, x
    push noblock
.wrap
```

- [ ] **Step 2: DMA C and the ring.** Channel C: RX FIFO of the event state machine to `bus_events`, 32-bit, write increment, ring on the write address (`channel_config_set_ring(&c, true, 13)` for 8 KB), endless count, high priority. Started and stopped with the engine; the event state machine runs even when bus drive is off. `bus_engine_event_head()` reads channel C's write address.

- [ ] **Step 3: `bus_event` and the host simulator.** In `bus.c`:

```c
BUS_HOT void bus_event(uint32_t w) {
    uint16_t idx = BUS_EV_IDX(w);
    uint8_t data = BUS_EV_DATA(w);
    bus_stats.cycles++;
    if (BUS_EV_RD(w)) {
        bus_stats.reads++;
        bus_run_read_hooks(idx);
    } else {
        bus_stats.writes++;
        bus_run_write_hooks(idx, data);      /* existing loop, split out of bus_on_write */
        bus_queue_write(idx, data);          /* existing ring push, counts write_overrun */
    }
}
```

`bus_on_read_done` and `bus_on_write` are removed; `sim_bus.c` builds the event word (`data | idx << 8 | rd << 22`) and calls `bus_event`, after serving the read from `bus_peek`. Host tests that called the old pair are updated; add:

```c
TEST(event_runs_hooks_and_queues_writes) {
    bus_init();
    hook_n = 0;
    bus_add_read_hook(0x3F42, hook_incr);
    bus_event(0x00 | (0x3F42u << 8) | (1u << 22));     /* a read of $FF42 */
    ASSERT_EQ(hook_n, 1);
    bus_event(0x5A | (0x3F42u << 8));                   /* a write of $5A */
    uint16_t idx; uint8_t d;
    ASSERT(bus_pop_write(&idx, &d));
    ASSERT_EQ(idx, 0x3F42); ASSERT_EQ(d, 0x5A);
    ASSERT_EQ(bus_stats.reads, 1); ASSERT_EQ(bus_stats.writes, 1);
}
```

- [ ] **Step 4: core1 event loop.** `bus_core1.c` becomes:

```c
BUS_HOT void bus_core1_main(void) {
    (void)save_and_disable_interrupts();
    uint32_t r = bus_engine_event_head();
    for (;;) {
        uint32_t w = bus_engine_event_head();
        uint32_t lag = (w - r) & (BUS_EVENTS - 1);
        if (lag > bus_stats.event_lag_max) bus_stats.event_lag_max = lag;
        while (r != w) {
            bus_event(bus_events[r]);
            r = (r + 1) & (BUS_EVENTS - 1);
        }
    }
}
```

Update the flash-free checker's entry list to `bus_core1_main`, the hooks, and `bus_engine_set_bank`.

- [ ] **Step 5: Trace.** The ring is the trace. `bus_trace_copy` returns the last `n` ring entries with `seq` counted from `bus_stats.cycles`. `bus_trace_freeze_hot` (called from a hook on core1) copies the last 512 entries to a frozen buffer that `trace dump` prints until `trace run`. Console line format: `seq idx R|W data` (e.g. `104233 3f42 R a7`). `tools/tracedump.py` parses a sequence number where the timestamp was. Counters in `status`: drop `addr_resample`, `late_precompute`, `oe_glitch`, `resample_*`; add `bus engine_stall <n> event_lag_max <n> event_drop <n>`.

- [ ] **Step 6: Self-test additions, both boards.** In `fake6809.c`'s fast test:
  - **Write capture:** a burst of writes with known data to known addresses, including two on consecutive cycles and a write straight after a read; every one must arrive through `bus_pop_write`, in order, with the right index and data.
  - **Event order and content:** a mixed burst; the ring's entries match the driven sequence (index, R/W, and for reads the byte served).
  - **Event rate:** 4096 back-to-back reads at 1.79 MHz, five times, then `bus cycles` has grown by exactly the number of selected cycles, `event_drop 0`; print `event_lag_max`.

Expected lines:

```
fast writes: 0 lost, 0 out of order, 0 wrong data of <n>
fast events: 0 mismatches of <n>
fast event rate 1.79MHz: counted <n> of <n>, lag max <k>, drop 0
selftest fast pass
```

If core1 cannot keep up (counted < expected, or lag grows towards the ring size), stop and report with the numbers: the spec's fallback (record only writes and I/O-page reads unless tracing) is a decision for the controller, not to be built unasked.

- [ ] **Step 7: Build, run the host suite, both self-tests, commit.**

```bash
git commit -m "engine: event stream (capture SM, DMA ring, core1 event loop), trace from the ring"
```

---

### Task 3: Devices on events — Becker and the bank switch at speed

**Files:**
- Modify: `firmware/src/dev/becker.c`, `firmware/src/dev/rom.c`, `firmware/src/bus/fake6809.c`, `firmware/tests/test_becker.c`

**Interfaces:**
- Consumes: `bus_event`, `bus_io_set`, `bus_engine_set_bank`, the event loop.
- Produces: the Becker port and `$FF40` banking working on the engine; no API change.

- [ ] **Step 1: Becker on the engine.** The hooks' logic is unchanged (status read: refresh; data read: pop and refresh when the table said ready, else count an underrun and freeze the trace). Confirm every table write goes through `bus_io_set` and that the order is safe for a reader between the two stores: when publishing a byte, data first then status; when emptying, status first then data. Add that ordering as a comment and a host test:

```c
TEST(publish_order_never_shows_ready_with_stale_data) {
    setup();
    uint8_t b = 0x41;
    becker_write(&b, 1);
    sim_read(0xFF41);                       /* status poll publishes */
    ASSERT_EQ(bus_table[BUS_IDX_BECKER_STATUS], 0x02);
    ASSERT_EQ(bus_table[BUS_IDX_BECKER_DATA], 0x41);
    sim_read(0xFF42);                       /* pops */
    ASSERT_EQ(bus_table[BUS_IDX_BECKER_STATUS], 0x00);
}
```

- [ ] **Step 2: Self-test, Becker at speed (both boards).** The fake 6809 runs the real client pattern as cart cycles: status read, 4 filler ROM fetches, data read if the status byte had bit 1, repeat, at 1.79 and 0.89 MHz, with core0 pushing a known 2 KB stream through `becker_write`. Pass: the bytes the fake collected equal the stream exactly, `underrun 0`, `overrun 0`. Then the back-to-back case: status and data on consecutive cycles; print what it returns for a ready byte and for an empty port, and require the result to be the same on five runs. Expected lines:

```
fast becker 1.79MHz: 2048 bytes, 0 lost, 0 duplicated, 0 phantom
fast becker 0.89MHz: 2048 bytes, 0 lost, 0 duplicated, 0 phantom
fast becker back-to-back: ready -> <st>,<data>; empty -> <st>,<data> (stable)
```

- [ ] **Step 3: Self-test, bank switch at speed.** With 8 patterned banks: write `$FF40` = n, then read the same ROM address on the next cycle, and at +1 and +2 cycles; report which read first returns bank n's byte, at both speeds.

```
fast bank switch 0.89MHz: new bank at +0 cycles
fast bank switch 1.79MHz: new bank at +<k> cycles
```

Pass condition: +0 at 0.89 MHz. The 1.79 MHz figure is reported, not gated. If it is not +0 at 0.89 MHz, stop and report.

- [ ] **Step 4: Event rate with Becker traffic.** Repeat Task 2's event-rate check with the Becker stream running: `drop 0`, counts exact.

- [ ] **Step 5: Build, host suite, both self-tests, commit.**

```bash
git commit -m "engine: Becker and bank switching on the event stream, measured at speed"
```

---

### Task 4: Stall guard, capture-only mode, engine lifecycle

**Files:**
- Modify: `firmware/src/bus/bus_engine.c`, `bus_engine.h`, `firmware/src/main.c`, `firmware/src/console/console.c`, `firmware/src/bus/fake6809.c`

**Interfaces:**
- Produces: `void bus_engine_tick(uint32_t now_ms);` called from core0's main loop.

- [ ] **Step 1: Stall guard.** `bus_engine_tick`: once per millisecond, if the read state machine's program counter is at its `pull` and DMA B is idle, remember it; if the same holds on the next tick, stop and restart the engine and count `bus_stats.engine_stall`. Reads a PIO register from core0 only.

- [ ] **Step 2: Capture-only.** `bus drive off`: the read state machine is stopped with pins released; the event state machine and DMA C keep running, so writes and the trace still work. `bus drive on` restarts the read path.

- [ ] **Step 3: Self-test.**
  - **Forced stall:** a test hook disables DMA A for one burst; the pins read `0x00`/released on every cycle of it (never a stale byte), `engine_stall` increments, and the following burst reads correctly.
  - **Drive off:** nothing driven, events still counted, writes still captured.
  - The spike's restart-mid-cycle bursts stay.

```
fast stall: detected 1, pins never driven during the stall, next burst 0 mismatches
fast drive off: 0 driven, events <n> of <n>, writes 0 lost
```

- [ ] **Step 4: Build, host suite, both self-tests, commit.**

```bash
git commit -m "engine: stall guard, capture-only mode"
```

---

### Task 5: Fold the old self-test in and remove what the CPU loops left

**Files:**
- Modify: `firmware/src/bus/fake6809.c`, `fake6809.pio`, `fake6809.h`, `firmware/src/console/console.c`, `firmware/tools/bench.py`, `firmware/tests/*`, `firmware/src/net/net_selftest.c`

- [ ] **Step 1:** `bus selftest` with no argument runs the fast test. Move anything the legacy test checked that still applies (ROM read of a loaded image, banked image marker, `$FF40` write hook, write events reaching core0) into it; delete the legacy program `fake6809_p2`/`fake6809_pw` slow paths, `pad_hold`, the latency sweeps that measured the CPU loop, and the spike-only console options. `bus selftest net`: rebuild it on the fast test if it is a small change (WiFi Becker round trip with the link up); otherwise keep the "not supported by this engine" message and say so in the report.
- [ ] **Step 2:** `tools/bench.py` runs `bus selftest` and passes on `selftest fast pass`.
- [ ] **Step 3:** Grep for leftovers and remove them: `late_precompute`, `addr_resample`, `oe_glitch`, `bus_rom_base`, `bus_fw_`, `BUS_OE_REG`, `bus_on_read_done`, `bus_record_read`, `PICOCO_PIO_ENGINE` (the engine is unconditional now).

Run: `grep -rn 'late_precompute\|addr_resample\|oe_glitch\|bus_rom_base\|bus_fw_\|BUS_OE_REG\|bus_on_read_done\|PICOCO_PIO_ENGINE' firmware/src firmware/host firmware/tests firmware/tools`
Expected: no output.

- [ ] **Step 4:** Build, host suite, both self-tests (full output in the report), commit.

```bash
git commit -m "engine: one self-test; remove the CPU loops' leftovers"
```

---

### Task 6: Documentation

**Files:** as listed in the spec, section 13.

- [ ] **Step 1:** Rewrite `docs/firmware-architecture.md` sections 3.2.2-3.4, 4 and 8 to describe this engine with the measured numbers; remove the old PIO sketch and the CPU loop text.
- [ ] **Step 2:** `firmware/README.md`: `status` counters, trace line format, `bus selftest`, the Plus-W limits (JP2 1-2, no A14/A15, no firmware-decoded addresses).
- [ ] **Step 3:** `firmware/TEST_PLAN.md`: a new section K with the self-test's expected lines for each board and the bench rows of Task 7 (empty results table).
- [ ] **Step 4:** `docs/pcb-bringup.md`: a short pointer to the new timing numbers; `docs/ADDITIONAL_ROADMAP.md` section 6 item on read-path margin: closed by this engine; section 9: note that the Plus-W firmware-decoded addresses were removed and return with the respin.
- [ ] **Step 5:** Commit: `docs: PIO bus engine`.

`CLAUDE.md` is untracked in this repo; the controller updates it by hand after the bench (spec section 13 lists the rules).

---

### Task 7: Bench (controller with the user)

- [ ] Flash the PCB (Pico 2 build) only after `status` shows what is running there.
- [ ] CoCo 3: HDB-DOS at 0.89 and 1.79 MHz with loads and a save; the manager, HDB-DOS and pak launches; double RESET; **Tetris displays correctly**; `status`: `engine_stall 0`, `event_drop 0`, DriveWire counters clean.
- [ ] CoCo 2 (16K, no Extended BASIC): manager, `carttest.rom` 30 minutes clean.
- [ ] Record everything in `firmware/TEST_PLAN.md` section K and update `CLAUDE.md`.
