# PIO + DMA read engine: feasibility spike (2026-10-02)

Branch `pio-engine`. Commits: `01c89cc` (fast self-test), `a15d0f1` (engine),
`e3fa9e2` (mixed r/w and drive-off bursts). The old CPU loop is at tag
`fw-1.4-cpu-loop`. All numbers below come from a bare Pico 2 module (RP2350A,
150 MHz, no carrier, no CoCo), firmware 1.4 built from this branch.

## Question

Can one PIO state machine plus two chained DMA channels serve cartridge ROM
reads with no CPU in the per-cycle path, fast enough for back-to-back read
cycles at 1.79 MHz? What is the select-to-data latency?

**Answer: yes.** 0 mismatches in every realistic 1.79 MHz burst (155 bursts of
4096 back-to-back cycles, 634,880 reads). Data is on the pins **19-20 clk
(126-133 ns) after OE_BUS falls**, against a 36 clk (240 ns) requirement:
**16-17 clk (107-113 ns) of margin**. The CPU loop it replaces fails almost
every cycle of the same burst.

## The test: `bus selftest fast [stress]`

`fake6809_fast` (in `firmware/src/bus/fake6809.pio`) runs on PIO1 at clkdiv 1.
DMA feeds it one 32-bit TX word per cycle from an array, and a second DMA
channel copies its RX samples into another array. No CPU runs in the burst,
so nothing on core0 can stretch a cycle. Side-set is OE_BUS (GP26). Out pins
are GP8-GP22 (A0-A13, R/W). In pins are GP0-GP7. The fake's input sync is
bypassed on D0-D7, so a sample happens at the stated clk and not 2 clk later.

TX word: `[14:0]` address and R/W, `[17:15]` e (high-phase extension),
`[24:18]` pre, `[31:25]` post. RX word: `[15:8]` = D0-D7 at the sample point,
`[7:0]` = D0-D7 shortly after OE_BUS rises (the release check).

Timing, counted from the instruction listing. t=0 is the OE_BUS rise that ends
the previous cycle; f=0 is the OE_BUS fall.

| | 1.79 MHz (e=0) | 0.89 MHz (e=3) |
|---|---|---|
| cycle | 84 clk (560 ns) | 168 clk |
| OE_BUS high | 42 clk = 17 + 1 + 14(e+1) + 1 + 8 | 84 clk |
| OE_BUS low | 42 clk = pre + post + 3 (pre + post = 39) | 84 clk (pre + post = 81) |
| address + R/W setup to the fall | 25 clk (out pins at t=17, fall at t=42) | 67 clk |
| address hold after the rise | 17 clk | 17 clk |
| sample point S | pre + 1 clk after the fall (1..40) | 1..82 |
| release check R | k + 1 clk after the rise, k patched into the PIO program at run time (1..14) | same |

Cycles run back to back with no gap. A burst is 4096 cycles (`FAST_N`). Each
run does the following:

- (a) Five bursts at the realistic point S=36 (240 ns) at 1.79 MHz.
- A mixed burst: every third cycle is a CoCo write, which must never be driven.
- A burst with `bus drive off`, where nothing may be driven.
- (b) A sweep of S = 1..40. `response_clk` is the smallest S from which every
  later S reads the whole burst correctly.
- A release sweep, which reports the earliest R at which D0-D7 read 0x00 on
  every cycle.
- (c) One burst at S=72 (480 ns) at 0.89 MHz, then a sweep of S = 1..82.

Addresses are a pseudo-random walk over table indices 0x0000-0x3EFF. The test
fills the window itself with nonzero bytes (an undriven pin reads 0x00) and
redraws the walk so that each cycle's byte differs from the previous one. The
expected byte comes from `bus_peek()`. The test clears the ROM afterwards
(`rom_off()`), as the old self-test does. A mismatch is classed as `zero`
(read 0x00) or `stale` (read the previous cycle's byte).

`stress` runs a 2 KB memcpy loop on core0 for the whole burst, so core0 and
the DMA compete for SRAM.

Two parts of the brief were not built:

- The variant with unselected gaps between reads. The word format has no
  spare select bit, so it is not cheap.
- Measuring D0-D7 after the rise cannot tell "driven low" from "released".
  Both read 0x00.

## Baseline: the CPU loop (fw-1.4 core1 loop, before Part 2)

Result at 1.79 MHz back-to-back: **fails, as predicted.** About 4095 of every
4096 cycles read the wrong byte at 240 ns, and no sample point in the low phase
reads a whole burst correctly.

```
fast timing: cycle 84 clk (high 42, low 42), address setup 25 clk, 4096 cycles back to back
fast 1.79MHz S=36 (240 ns): mismatches 4094/4096 (zero 2046 stale 2048) rel_nonzero 4095 lost 0
fast 1.79MHz first bad cycle 1 idx 01b0 want 14 got 2e
fast 1.79MHz S=36 (240 ns): mismatches 4095/4096 (zero 2047 stale 2048) rel_nonzero 4096 lost 0
  (x3 more, identical)
fast 1.79MHz sweep 1:4096 ... 27:4096 28:4095 29:4095 ... 40:4095
fast 1.79MHz response_clk -1 (-1 ns after OE_BUS fell)
fast 1.79MHz release_clk -1 (... -1 = not by 14)
fast 0.89MHz S=72 (480 ns): mismatches 0/4096 (zero 0 stale 0) rel_nonzero 4096 lost 0
fast 0.89MHz sweep ... 18:4095 19:4094 20:4093 21:3965 ... 36:1242 ... 41:31 42:5 43:2 44:0 ... 82:0
fast 0.89MHz response_clk 44 (293 ns after OE_BUS fell)
```

With core0 stress the 1.79 MHz result is 3778-3825/4096 mismatches (zero
~1750, stale 2048), and 0.89 MHz gives `response_clk 48` (320 ns).

The failure pattern is exact: half the cycles read 0x00 and half read the
previous cycle's byte. The loop serves only every second cycle. Its read path
(detect, drive, trace and counters at ~55 clk, the 3-sample end filter,
discharge, hook scan) takes longer than the 42 clk high phase. So it drives
byte k late in cycle k, holds it through cycle k+1 (the stale reads), and only
catches cycle k+2. This matches the corrupt graphics on the bench from a cart
game running from ROM in fast mode. At 0.89 MHz the loop passes at 480 ns,
but its worst case is 44-48 clk, with a long tail from S=18.

## The engine as built

### PIO (PIO0, one SM): `firmware/src/bus/bus_engine.pio`

```
.program bus_engine
.pio_version 1
.wrap_target
top:
    mov isr, x              ; isr = base >> 14, shift count 0
    wait 0 gpio 26          ; OE_BUS low: selected and E high
    jmp pin rd              ; R/W high = CoCo read
    wait 1 gpio 26          ; a write: never drive
    jmp top
rd:
    in pins, 14             ; isr = base | A13..A0: the table byte's address
    push noblock
    mov pindirs, ~null      ; drive now: the pads still hold 0x00 from the last release
    pull block              ; the byte, from DMA B
    out pins, 8
    wait 1 gpio 26          ; end of the cycle
    mov pins, null [1]      ; RP2350-E9: drive low two clk...
    mov pindirs, null       ; ...then release
.wrap
```

SM configuration:

- `in_base` GP8, `out_base` GP0 with a count of 8, `jmp_pin` GP22, clkdiv 1.
- In-shift left with no autopush, so `isr = x << 14 | A13..A0`. Out-shift
  right with no autopull.
- X = `bus_rom_base >> 14`, loaded by core0 while the SM is stopped
  (`put`, `exec pull`, `exec mov x, osr`).

`bus_table` and `rom_banks` are 16 KB aligned (`BUS_WINDOW_ALIGN`, only when
`PICOCO_PIO_ENGINE` is defined). The linker placed them at 0x20004000 and
0x20008000. `bus_set_rom_base()` is the core0 store of `bus_rom_base`. On this
build it also re-points the SM: stop, reload X, restart, a gap of a few µs.
`rom.c` now uses it in `unbank()` and `rom_publish_banks()`.

### DMA (`firmware/src/bus/bus_core1.c`, Pico 2 half)

| ch | read | write | size | DREQ | count | notes |
|---|---|---|---|---|---|---|
| A | `pio0->rxf[sm]`, fixed | `dma_hw->ch[B].al3_read_addr_trig`, fixed | 32 | PIO0 RX sm | `dma_encode_endless_transfer_count()` | high priority, started once |
| B | set by A, fixed | `pio0->txf[sm]`, fixed | 8 | PIO0 TX sm | 1, reloaded on every trigger | high priority, re-armed by each write from A |

Knobs, applied on every engine start:

- `pio0->input_sync_bypass` for GP8-GP22 and GP26.
- `busctrl_hw->priority = DMA_R | DMA_W`.

Both are on by default. The console command `bus engine bypass|prio on|off`
changes them.

**8-bit DMA write into the 32-bit TX FIFO.** It is replicated across the word.
A probe (an 8-bit DMA write to an idle PIO2 SM, then `pull` / `mov isr, osr` /
`push` by exec) prints `fast dma 8-bit write of a7 reaches the TX FIFO as
a7a7a7a7`. `out pins, 8` takes bits 7:0, which works either way.

**bus drive off** stops the SM and sets D0-D7 low, then releases them as inputs.
`bus_drive_set()` calls `bus_engine_drive()`. The SM is started only while
drive is on. The drive-off burst confirms that nothing is driven.

**core1** now runs only `for (;;) __wfe();` with interrupts off. `main.c` is
unchanged. The entry list in `check_core1_flash_free.py` did not need changing:
`bus_core1_main` and the three hooks still exist, and the check passes.

These features are out of scope on this build and do not work:

- Read hooks, the write path, the write ring, trace and `bus_stats`.
- The Becker port.
- `$FF40` bank switching. The hook is still registered but never runs, so the
  engine stays on bank 0.

Because `bus cycles` never counts, the self-tests' "cycles not moving" guard
is vacuous here. The 10 ms pin-activity guard still applies. The legacy
`bus selftest` prints `skipped (spike)` for the checks that depend on writes
or hooks, and for `becker_status_read`. That check is skipped because a banked
image's I/O page is served from the bank buffer (see Recommendations). The
`pad_hold` probe is skipped too, because SIO cannot drive pins that PIO0 owns.

## Measurements on the engine

Realistic point at 1.79 MHz (S=36, 240 ns): **0/4096 in every burst**. That is
5 bursts per run, in 31 runs across all knob settings, with and without stress
(155 bursts, 634,880 cycles). Five consecutive runs with the default settings:

```
=== run 1
bus cycles 0 reads 0 writes 0 write_overrun 0
fast dma 8-bit write of a7 reaches the TX FIFO as a7a7a7a7
fast engine: PIO0 + DMA, input sync bypass on, DMA bus priority on
fast timing: cycle 84 clk (high 42, low 42), address setup 25 clk, 4096 cycles back to back
fast 1.79MHz S=36 (240 ns): mismatches 0/4096 (zero 0 stale 0) rel_nonzero 4096 lost 0   (x5)
fast 1.79MHz mixed r/w S=36 (240 ns): mismatches 0/4096 (zero 0 stale 0) rel_nonzero 2731 lost 0
fast 1.79MHz drive off S=36 (240 ns): mismatches 0/4096 (zero 0 stale 0) rel_nonzero 0 lost 0
fast 1.79MHz sweep 1:4096 2:4096 ... 17:4096 18:4096
fast 1.79MHz sweep 19:1 20:0 21:0 ... 40:0
fast 1.79MHz response_clk 20 (133 ns after OE_BUS fell)
fast 1.79MHz release_clk 5 (D0-D7 low on every cycle 5 clk after OE_BUS rose; -1 = not by 14)
fast 0.89MHz S=72 (480 ns): mismatches 0/4096 (zero 0 stale 0) rel_nonzero 4096 lost 0
fast 0.89MHz sweep 1:4096 ... 18:4096
fast 0.89MHz sweep 19:0 20:0 ... 82:0
fast 0.89MHz response_clk 19 (126 ns after OE_BUS fell)
selftest fast pass
=== runs 2-5: identical except response_clk 19 (126 ns) at 1.79 MHz and 19 at 0.89 MHz
```

How to read these lines:

- `rel_nonzero 4096` on the S=36 lines only means that the default release
  sample (R=4) comes before the release. `release_clk` is the measured value.
- In the mixed burst, `rel_nonzero 2731` is exactly the number of read cycles,
  so write cycles were never driven.

The edge is sharp. S=18 reads 0x00 on every cycle, because the pins are
already driving 0x00 before the byte lands. S=19 is all correct or has 1
mismatch. S=20 and later are always correct. The latency is therefore
deterministic within 1 clk, and no cycle position or address stood out.

| setting (each: 2 runs plain, 2 runs core0 stress) | response_clk 1.79 MHz | response_clk 0.89 MHz | release_clk |
|---|---|---|---|
| bypass on, prio on (default) | 19-20 (126-133 ns) | 19-20 | 5 (33 ns) |
| bypass off, prio on | 20 | 20 | 6 |
| bypass on, prio off | 19 plain, 20 stress | 19 plain, 20 stress | 5 |
| bypass off, prio off | 20 plain, 21 stress | 20 plain, 21 stress | 6 |

- **Input sync bypass**: saves about 1 clk on the read and 1 clk on the
  release, not the 2 clk that the synchroniser depth suggests. The PIO-only
  experiment below shows the same 1 clk.
- **Bus priority**: no measurable effect with core0 alone hammering SRAM. It
  saved 1 clk only in the worst corner (bypass off and stress).
- **Release**: D0-D7 read 0x00 by 5 clk (33 ns) after OE_BUS rises, and by
  6 clk without bypass. On the board U10 stops passing data once OE_BUS is
  high, so this only has to finish before the next cycle (42 clk), which it
  does by a wide margin. The test cannot separate "driven low" from
  "released". By the listing, release follows the low drive 2 clk later.

**Margin**: 36 - 20 = 16 clk (107 ns) worst case at 1.79 MHz. At 0.89 MHz the
latency is the same in clk while the requirement is twice as long.

## Tried and discarded

The criteria are 1.79 MHz `response_clk`, bypass on, prio on.

1. **The lead's drive ordering**: `pull; out pins, 8; mov osr, ~null; out
   pindirs, 8`. It measured 21 clk (140 ns), against 19-20 for the built
   version. The built version enables the outputs before the `pull`, while
   DMA is still fetching, so they drive 0x00 until the byte lands. That takes
   2 instructions off the critical path, and `mov pindirs` (PIO v1) saves one
   more instruction on the release. Kept.
2. **Autopull (threshold 8) instead of `pull block` + `out`**: 19/20 clk with
   bypass on and off, no gain. It also needs an `out null, 32` exec at start,
   or the first `out` uses the stale base word. Discarded.
3. **PIO-only experiment** (temporary, not committed): `mov osr, isr` in place
   of push/pull, driving A7..A0. It measured 9 clk with bypass and 10 without.
   So the DMA A to DMA B round trip costs about **10 clk (67 ns)** of the 19-20.
   The PIO side costs about 9 clk, of which roughly 3-4 are pad and sample
   overhead.

No tuning was needed. The first build passed.

## Unknowns

- **Real-board delays.** No U10/U15 or edge-connector delays are included.
  OE_BUS reaches the Pico late by U15's two gate delays plus R11, and data
  goes back through U10, roughly 10-20 ns in total. Expect a real-board margin
  of about 13-14 clk.
- **Heavier contention.** Untested: both cores busy (core1 idles here), USB
  bulk traffic, and flash programming (FatFS writes stall XIP but not SRAM or
  DMA).
- **OE_BUS glitches with bypass on.** A metastable or runt OE_BUS edge could
  start a false read. On the board U10 is off whenever OE_BUS is high, so the
  likely result is harmless, but this is untested.
- **A stall in DMA A.** If DMA A ever stops (aborted, misconfigured, or
  starved), the SM blocks in `pull` with D0-D7 driven. A following write cycle
  would then put U10's B to A drive against the Pico pads. Nothing in the
  spike guards against this.
- **Bank switch and I/O-page reads on a banked image.** Not exercised on this
  build.

## Recommendations for the full engine

- **Keep this read path as is.** It runs on the 150 MHz clock with 16 clk
  spare. Every instruction added between `wait 0 gpio 26` and `push`, or
  between `pull` and `out`, costs 1 clk. Anything that can be precomputed into
  ISR before the `wait` is free. A faster clk_sys scales the whole 19-20 clk,
  DMA included, if more margin is wanted.
- **Sync bypass.** Leave the synchroniser on for OE_BUS (it costs 1 clk) and
  bypass only A0-A13/R/W if wanted. The measured difference is 0-1 clk, which
  is not worth the metastability exposure on an async strobe. Keep DMA bus
  priority on. It is free, and it matters once both cores and USB compete.
- **Bank switching.** Put the bank number in X and the constant high part of
  the address in Y, with 8 banks × 16 KB in one 128 KB-aligned block. The read
  SM then builds `mov isr, y; in x, 3` before the `wait`, at zero latency cost.
  A bank change is a single `pio_sm_exec(set x, n)` between cycles, with no
  stop and restart. The `$FF40` write still has to be decoded somewhere:
  - from the write-capture stream by core1, with a deadline of about 280 ns
    (the next E-high at 1.79 MHz) if the next fetch is from the new bank;
  - or in PIO, if a free SM can compare the address. That is hard with only
    X and Y.

  Find out whether real banked carts fetch from the new bank on the cycle
  right after the `$FF40` write before committing to the core1 route.
- **I/O page on banked images.** The pointer trick serves `$FF00-$FFFF` from
  the current bank's top 256 bytes. Either mirror `bus_table[0x3F00..]` into
  every bank (each bank buffer is a full 16 KB, so the room exists), or have
  the Becker writer update every mirror. That is at most 8 single-byte stores.
- **Write capture.** Use a second SM with `jmp_pin = OE_BUS`. It loops on
  `mov y, pins` (in_base GP0, so it reads D0-D7, A0-A13 and R/W in one word)
  until OE_BUS rises, then pushes the last sample. A DMA ring buffer drains
  it. The deadline is the write's data valid before the end of E; nothing is
  driven, so latency does not matter. It must ignore reads, by jumping on R/W
  through a separate wait or an extra pin test. It shares no registers with
  the read SM.
- **Event stream to the CPU, for Becker read side effects and trace.** Chain a
  third DMA channel C from B. C copies `dma_hw->ch[B].read_addr` (B does not
  increment, so this still holds the pointer just served) into a ring buffer
  in SRAM. That gives a record of every read with zero added latency, because
  C runs after B has fed the FIFO. core1 consumes the ring to run the Becker
  status/data refresh. The refresh has to land before the CoCo's next
  `$FF41`/`$FF42` poll, which is microseconds away and not one bus cycle.
  Unmeasured.
- **Robustness.** Add a guard against the `pull block` hang, for example a
  core0 watchdog that checks DMA A is busy and the SM PC is not stuck at
  `pull` across a 1 ms window, then stops the SM. Keep the drive-off path:
  stop the SM, drive low, release.
- **Self-test.** Keep `bus selftest fast` as the regression gate. Add a
  select-gap variant (it needs a select bit in the TX word, for example by
  shrinking `post` to 6 bits) and write-data checks once write capture
  exists.
