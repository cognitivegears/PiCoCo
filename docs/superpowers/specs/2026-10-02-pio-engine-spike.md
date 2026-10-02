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

## Part 3: helper-flag trigger

The question: moving the select decision into a helper state machine lets one
read program serve both boards. On the Plus-W, OE_BUS (GP40) is outside
PIO0's GPIO window, but /CTS (GP24), /SCS (GP25) and E (GP26) are inside it.
What does that cost on the read path, alone and combined with enabling the
outputs only after the pull?

Commit: `a354b38`. Everything below is from the same bare Pico 2, measured with
`bus selftest fast`, and `bus cycles 0` was confirmed before every run.

### What was built

The read program and the helper are both in `firmware/src/bus/bus_engine.pio`.

```
; read SM, variation A (bus_engine): enable before the pull
top:  mov isr, x
trig: wait 1 irq 0          ; patched at load to `wait 0 gpio 26` for the pin trigger
      jmp pin rd
      wait 1 gpio 26
      jmp top
rd:   in pins, 14
      push noblock
      mov pindirs, ~null
      pull block
      out pins, 8
      wait 1 gpio 26
      mov pins, null [1]
      mov pindirs, null

; read SM, variation B (bus_engine_late): rd = in pins,14 / push noblock / pull block /
;                                        out pins,8 / mov pindirs,~null / (rest as A)

; helper (bus_sel_p2), PIO0, no pins of its own
.wrap_target
      wait 1 gpio 26        ; starts here: a start while OE_BUS is low skips that cycle
fall: wait 0 gpio 26
      irq set 0
flag1: irq set 1            ; patched to a nop for the one-flag variant
.wrap
```

How the flag behaves:

- The helper raises the flag once per OE_BUS-low cycle, for reads and writes
  alike.
- The read SM's `wait 1 irq 0` clears the flag, and it does so on both its
  read and write branches. A write therefore never leaves the flag set.
- Unselected cycles never raise it.
- Flag 1 has no consumer here, so it simply stays set. An event SM will have to
  consume it every cycle, or events merge silently, because a flag is a bit and
  not a count.

`engine_start` handles a restart as follows:

1. Wait for RX to be empty and DMA B to be idle.
2. Clear the FIFOs.
3. Restart both SMs.
4. Clear IRQ flags 0 and 1.
5. Enable the read SM and the helper with one register write.

The `naive` knob starts the helper at `fall` instead and does not clear the
flags. That is the coordinator's form, `wait 0` / `irq set` / `wait 1`.

The console knobs are `bus engine order a|b` and
`bus engine trig pin|irq1|irq2|naive`. The build is left on **B with the pin
trigger**.

### Test additions (fake6809_fast)

- The TX word gained an unselected bit (bit 17). e shrank to 2 bits, since only
  0 and 3 are used. An unselected cycle keeps OE_BUS high for the whole "low"
  phase and is 1 clk longer.
- A **gaps** burst: `k%4` = read, unselected, read, write. It covers
  write->read, read->gap, gap->read and write->read->gap.
- The **mixed** burst (every third cycle a write) stays.
- A **restart** burst: the gaps pattern with `bus_engine_rebase()` called every
  50 us under it. That is 46 restarts, each landing at an arbitrary point in a
  cycle.
- Mismatches are now split into four classes:
  - `spurious`: nonzero on a cycle that must not be driven, a write or an
    unselected cycle.
  - `wrong`: a nonzero byte that is not the expected one, which indicates a
    desync.
  - `zero`: 0x00 on a read, which only means the cycle was missed.
  - `stale`.

  The restart burst passes with zero `spurious` and zero `wrong`. Each restart
  may lose one or two cycles as `zero`.

### Found and fixed: the stop path drove the bus for about 1 us

The first restart run, with variation A and the pin trigger (no helper
involved), showed:

```
fast 1.79MHz restarts 46 under a gaps burst: spurious 9 wrong 0 (zero 135 = cycles lost to a restart)
```

`engine_stop` released D0-D7 with `pio_sm_set_pins_with_mask()` and
`pio_sm_set_pindirs_with_mask()`. Those go pin by pin through exec and pinctrl
rewrites. A byte being driven at the moment of the stop therefore stayed on
the pads for about 1 us, into the following unselected and write cycles. On
the board that would put the Pico's pads against U10's write drive.

The fix is two direct execs on the stopped SM: `mov pins, null`, then
`mov pindirs, null`, a few clk in total. After the fix, every restart run
reads `spurious 0 wrong 0`. The same issue affected `bus_engine_rebase()` on
the Part 2 build.

### Results

Each configuration ran 2 plain runs and 2 runs with core0 stress. The naive
form ran 2 plain runs per order. That is 28 runs in all, and every one passed.

| variation | response_clk 1.79 MHz | response_clk 0.89 MHz | margin vs 36 clk (1.79) |
|---|---|---|---|
| A, OE_BUS pin (Part 2 engine) | 19-20 (126-133 ns) | 19-20 | 16 clk (107 ns) |
| A, helper, 1 flag | 21 (140 ns) | 22 | 15 clk (100 ns) |
| **A, helper, 2 flags (Variation A)** | **21 (140 ns)** | **22** | **15 clk (100 ns)** |
| B, OE_BUS pin | 20 (133 ns) | 20-21 | 16 clk (107 ns) |
| B, helper, 1 flag | 22 (146 ns) | 23 | 14 clk (93 ns) |
| **B, helper, 2 flags (Variation B)** | **22 (146 ns)** | **23** | **14 clk (93 ns)** |
| A / B, helper 2 flags, naive start | 21 / 22 | 22 / 23 | same |

Core0 stress changed none of these numbers. In every run, all S=36 bursts read
0/4096. That covers 252 bursts: the 5 realistic ones, mixed, gaps, gaps after
the restarts, and drive-off, in each of the 28 runs. Every restart burst read
`spurious 0 wrong 0`, with 54-67 cycles lost as `zero`. `release_clk` was 5 in
every run.

- **The helper costs 2 clk.** One clk is the helper's `irq set` after its
  `wait` completes. The other is the flag reaching the read SM's `wait irq`.
- **The second flag costs 0 clk** on the read path. It is raised one
  instruction after flag 0, which the read SM has already seen.
- **Enabling the outputs after the pull (B) costs 1 clk** against A, with
  either trigger. That is one instruction, `mov pindirs, ~null`, after `out`.
  Part 2's 21 clk was the two-instruction `mov osr`/`out pindirs` form.
- With the helper, 0.89 MHz consistently reads 1 clk later than 1.79 MHz. I
  assume this is a phase effect between the helper, the flag and the fake's
  clocking, but it is unexplained and within 1 clk.

Raw lines, variation A (helper, 2 flags):

```
bus cycles 0 reads 0 writes 0 write_overrun 0
fast engine: PIO0 + DMA, input sync bypass on, DMA bus priority on, order A (enable before pull), trigger helper 2 flags
fast 1.79MHz S=36 (240 ns): mismatches 0/4096 (zero 0 stale 0 spurious 0 wrong 0) rel_nonzero 4096 lost 0   (x5)
fast 1.79MHz mixed r/w S=36 (240 ns): mismatches 0/4096 (zero 0 stale 0 spurious 0 wrong 0) rel_nonzero 2731 lost 0
fast 1.79MHz gaps r/w/unsel S=36 (240 ns): mismatches 0/4096 (zero 0 stale 0 spurious 0 wrong 0) rel_nonzero 2048 lost 0
fast 1.79MHz restarts 46 under a gaps burst: spurious 0 wrong 0 (zero 64 = cycles lost to a restart)
fast 1.79MHz gaps after restarts S=36 (240 ns): mismatches 0/4096 (zero 0 stale 0 spurious 0 wrong 0) rel_nonzero 2048 lost 0
fast 1.79MHz drive off S=36 (240 ns): mismatches 0/4096 (zero 0 stale 0 spurious 0 wrong 0) rel_nonzero 0 lost 0
fast 1.79MHz sweep 19:4096 20:4096 21:0 22:0 ... 40:0
fast 1.79MHz response_clk 21 (140 ns after OE_BUS fell)
fast 1.79MHz release_clk 5 (D0-D7 low on every cycle 5 clk after OE_BUS rose; -1 = not by 14)
fast 0.89MHz S=72 (480 ns): mismatches 0/4096 (zero 0 stale 0 spurious 0 wrong 0) rel_nonzero 4096 lost 0
fast 0.89MHz sweep 19:4096 20:4096 21:1 22:0 ... 82:0
fast 0.89MHz response_clk 22 (146 ns after OE_BUS fell)
selftest fast pass
```

Raw lines, variation B (helper, 2 flags):

```
fast engine: PIO0 + DMA, input sync bypass on, DMA bus priority on, order B (enable after pull), trigger helper 2 flags
fast 1.79MHz S=36 (240 ns): mismatches 0/4096 (zero 0 stale 0 spurious 0 wrong 0) rel_nonzero 4096 lost 0   (x5)
fast 1.79MHz mixed r/w S=36 (240 ns): mismatches 0/4096 (zero 0 stale 0 spurious 0 wrong 0) rel_nonzero 2731 lost 0
fast 1.79MHz gaps r/w/unsel S=36 (240 ns): mismatches 0/4096 (zero 0 stale 0 spurious 0 wrong 0) rel_nonzero 2048 lost 0
fast 1.79MHz restarts 46 under a gaps burst: spurious 0 wrong 0 (zero 67 = cycles lost to a restart)
fast 1.79MHz gaps after restarts S=36 (240 ns): mismatches 0/4096 (zero 0 stale 0 spurious 0 wrong 0) rel_nonzero 2048 lost 0
fast 1.79MHz drive off S=36 (240 ns): mismatches 0/4096 (zero 0 stale 0 spurious 0 wrong 0) rel_nonzero 0 lost 0
fast 1.79MHz sweep 19:4096 20:4096 21:4096 22:0 23:0 ... 40:0
fast 1.79MHz response_clk 22 (146 ns after OE_BUS fell)
fast 1.79MHz release_clk 5 (D0-D7 low on every cycle 5 clk after OE_BUS rose; -1 = not by 14)
fast 0.89MHz S=72 (480 ns): mismatches 0/4096 (zero 0 stale 0 spurious 0 wrong 0) rel_nonzero 4096 lost 0
fast 0.89MHz sweep 19:4096 20:4096 21:4096 22:1 23:0 ... 82:0
fast 0.89MHz response_clk 23 (153 ns after OE_BUS fell)
selftest fast pass
```

B with the OE_BUS pin trigger, as left on the module:

```
fast engine: PIO0 + DMA, input sync bypass on, DMA bus priority on, order B (enable after pull), trigger OE_BUS pin
fast 1.79MHz sweep 19:4096 20:0 21:0 ... 40:0
fast 1.79MHz response_clk 20 (133 ns after OE_BUS fell)
fast 0.89MHz sweep 19:4096 20:0 ... 82:0
fast 0.89MHz response_clk 20 (133 ns after OE_BUS fell)
selftest fast pass
```

### Flag correctness

No stale or missed flag was seen in any of these cases:

- write followed immediately by a read (mixed and gaps bursts);
- an unselected gap before and after a read;
- write -> read -> gap;
- 46 restarts landing mid-cycle, including the naive start with no flag clear.

Writes and gaps were never driven: `spurious 0` everywhere, and in the mixed
burst `rel_nonzero` equals the read count. The naive start did not fail
either. Because the stop and start each act on both SMs with one register
write, the helper cannot raise a flag while the read SM is stopped. The
rotated helper (beginning at `wait 1`) plus clearing the flags at start is
still the safe form, since it skips a cycle already in progress instead of
serving it late. I recommend keeping it.

### Recommendation

Keep one read program, with B ordering, and patch its first `wait` at load:

- **Pico 2:** `wait 0 gpio 26`, the OE_BUS pin directly. 20 clk, margin
  16 clk (107 ns). No helper SM, and the event SM can wait on the same pin.
- **Plus-W:** `wait 1 irq 0` from a helper. 22 clk on this measurement,
  margin 14 clk (93 ns), plus whatever its select decode adds (see below).

The helper's 2 clk is affordable, but on the Pico 2 it buys nothing: the
program has to be patched per board anyway, because the end-of-cycle wait has
the opposite polarity on the two boards (OE_BUS high on the Pico 2, E low on
the Plus-W). The helper would also cost one of PIO0's four SMs. B costs 1 clk
over A and means a stalled DMA leaves D0-D7 released, so take it on both
boards.

### Plus-W helper sketch (not built; no Plus-W attached)

The coordinator's sketch: two SMs, `jmp_pin` = /CTS (GP24) and /SCS (GP25)
respectively.

```
.wrap_target
    wait 1 gpio 26          ; E high
    jmp pin skip            ; this select high: not ours
    irq set 0
    irq set 1
skip:
    wait 0 gpio 26          ; E low
.wrap
```

Read SM end-of-cycle: `wait 0 gpio 26` (E falls). Problems with this sketch:

1. **SM budget.** Two helpers, the read SM and the event SM take all four
   PIO0 SMs, leaving nothing for a separate write-capture or bank SM. One
   helper can test both selects instead. RP2350's `SHIFTCTRL.IN_COUNT` masks
   `mov x, pins`: with in_base GP24 and in_count 2, and Y preloaded to 3,
   `wait 1 gpio 26; mov x, pins; jmp x!=y sel` covers both selects at about
   +1 clk over `jmp pin`. Because /CTS and /SCS are never both low, merged
   flags are never ambiguous in either form.
2. **Late /CTS.** It samples /CTS and /SCS 1-2 clk after E rises. The current
   Plus-W CPU loop resamples at E rise precisely because the selects can
   settle late (bench), so this sketch can miss a selected cycle and the CPU
   reads 0x00. A robust helper loops on the selects while E is high, which
   costs clk and logic. An alternative avoids this and the SM budget:
   - Put the helper in **PIO1 with GPIOBASE = 16**, so GP40 is in its window.
   - It waits on the hardware OE_BUS (U15, already E-qualified and late-select
     safe): `wait 0 gpio 40`.
   - It sets PIO0's flag with **`irq set 0 prev`**. PIO v1 IRQ index modes
     reach the neighbouring block.

   The latency of that cross-block flag is unmeasured. This route needs
   JP2 1-2.
3. **Writes.** The helper raises the flag for writes as well. The read SM's
   `jmp pin` on R/W keeps writes undriven, as on the Pico 2, and the write
   branch consumes the flag. With **JP2 2-3**, though, U10's /OE is PIN_OE_FW
   (GP31). The read SM would have to side-set GP31 low on both its read and
   write branches, and high at the end of the cycle. GP31 is in the window, so
   that is possible, but it is new work and touches both branches.
4. **Firmware-decoded `$FF60-$FF7F`** (`bus_fw_mask`). Neither /CTS nor /SCS
   asserts for these addresses, so no select helper sees them. Serving them
   needs a full 16-bit address match against a per-address mask, which is not
   practical in PIO. That feature stays on a CPU path or is dropped from the
   PIO build.
5. **End-of-cycle polarity.** It differs between boards (see Recommendation).
   It is one patched instruction.
6. **Flag 1.** The event SM must consume flag 1 within the same cycle, or two
   cycles' events merge into one.
