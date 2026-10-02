# PIO bus engine: design

Status: draft for review, 2026-10-02. Brainstormed with the user. Measured
basis: `2026-10-02-pio-engine-spike.md` (same directory), Parts 1-4.
Replaces the CPU-polled core1 loop on both boards. The last build with that
loop is tag `fw-1.4-cpu-loop`; there is no build switch back to it.

## 1. Goals

1. Serve cartridge reads on every cycle, including consecutive ones, at
   1.79 MHz. Bench 2026-10-02: Tetris selects fast mode (`$FFD9`) and runs
   from the cart, and shows corrupt graphics; the CPU loop reads 4,094 of
   4,096 back-to-back cycles wrong at that speed (spike Part 1).
2. Take the CPU out of the timing path for good. A read is served by PIO
   and DMA alone. Device emulation runs on events after the cycle, with a
   deadline of "before the CoCo next touches that device", not "inside
   this bus cycle". Adding a device must not change read latency.
3. One engine for the Pico 2 and the Plus-W. No CPU bus loop remains on
   either board.
4. Keep what the bench has proven: the Becker port and DriveWire in every
   mode, banked ROMs, the ROM manager, capture-only mode, the trace, the
   core1 flash-free rule, the single-writer rule for the Becker table.

Non-goals are in section 12.

## 2. Decisions

| Topic | Decision | Why |
|---|---|---|
| Read path | PIO state machine + two chained DMA channels, table lookup by pointer | Measured 20 clk (133 ns) on the Pico 2, 23 clk on the Plus-W, identical on every cycle; the requirement is 36 clk |
| Output ordering | Outputs enabled only after the byte has been pulled | A stalled DMA leaves the pins released. Costs 1 clk |
| Pico 2 trigger | Waits on `OE_BUS` (GP26) directly | A helper flag costs 2 clk and a state machine and buys no uniformity: the end-of-cycle wait differs per board anyway |
| Plus-W trigger | One helper in PIO2 (GPIO base 16) watches `OE_BUS` (GP40) and raises flags in PIO0 across blocks | `OE_BUS` is outside PIO0's pin window. Rebuilding select from `/CTS`, `/SCS`, E in PIO would miss the late selects a CoCo 3 produces; the decode chip already handles them. Measured: 3 clk |
| Everything else | One event stream: a second state machine records each selected cycle at its end, DMA drains it to a ring, core1 walks the ring | Write capture, hook feed and trace are the same data |
| Banking | Bank number is part of the pointer; eight banks in one 128 KB-aligned block | A bank switch is one register update, no copy, no latency |
| Old loops | Deleted on both boards | User's ruling: source control is the fallback |
| Plus-W extras | A14/A15 unused, firmware-decoded addresses removed, JP2 must be 1-2 | Same limits as the Pico 2 until the respin (section 11) |

## 3. Architecture

```
            PIO0 sm0 "read"                DMA A            DMA B
select ---> capture A0-A13, build  --RX--> pointer -------> fetch byte --TX--> drive D0-D7
            pointer                         into B's         from table         until end of
                                            read address                         cycle, release

            PIO0 sm1 "event"               DMA C
select ---> sample D0-D7,A0-A13,R/W --RX--> ring[] in SRAM ---> core1 event loop:
            until the cycle ends,                                hooks, write queue to
            push the last sample                                 core0, counters, trace

Plus-W only: PIO2 sm "select" watches OE_BUS (GP40), raises flag 0 (read) and flag 1 (event) in PIO0.
```

core0 is unchanged in role: DriveWire, console, USB, WiFi, the `ui` module.

## 4. Read path

### 4.1 Program (identical on both boards except two waits)

```
top:  mov isr, y            ; constant high part of the table address
      in x, 3               ; bank number
      <start wait>          ; Pico 2: wait 0 gpio 26     Plus-W: wait 1 irq 0
      jmp pin rd            ; R/W high = read
      <end wait>            ; a write: never driven
      jmp top
rd:   in pins, 14           ; A0-A13: ISR is now the address of the byte
      push noblock
      pull block            ; the byte, fetched by DMA
      out pins, 8
      mov osr, ~null
      out pindirs, 8        ; drive
      <end wait>            ; Pico 2: wait 1 gpio 26     Plus-W: wait 0 gpio 26 (E falls)
      mov pins, null        ; RP2350-E9: drive low...
      mov pindirs, null     ; ...then release
```

The two per-board waits are set when the program is loaded. `jmp_pin` is
R/W (GP22). The precompute (`mov isr, y` / `in x, 3`) sits before the start
wait and costs no latency.

### 4.2 DMA

- **A**: reads the read state machine's RX FIFO (32-bit, paced by its DREQ,
  endless), writes channel B's `READ_ADDR_TRIG`.
- **B**: one 8-bit transfer from that address to the state machine's TX
  FIFO. An 8-bit write reaches the FIFO replicated across the word, so
  `out pins, 8` takes the right byte (measured).
- Both `HIGH_PRIORITY`; DMA given bus priority. Input synchronisers stay on
  for `OE_BUS` (an asynchronous strobe; bypass saves 1 clk and is not worth
  it).

### 4.3 Measured latency and margin (bare modules, spike Parts 3-4)

| Board | Select to data | Margin against 36 clk | Expected on the PCB |
|---|---|---|---|
| Pico 2 | 20 clk (133 ns) | 16 clk | about 13 (U10/U15 delays) |
| Plus-W | 23 clk (153 ns) from `OE_BUS` | 13 clk | about 10 |

Same in clk at 0.89 MHz, where the requirement doubles. Release: D0-D7 low
within 5-6 clk of the end of the cycle. Unchanged with core0 under load and
with the radio scanning or joining. Every instruction added between the
start wait and `push`, or between `pull` and `out pindirs`, costs 1 clk.

### 4.4 Capture-only mode (`bus drive off`)

The read state machine is stopped with its pins released; the event state
machine keeps running. Starting and stopping the engine runs from SRAM with
interrupts off (spike Part 4: from flash, a cache miss left the pins driven
for two cycles).

## 5. Table and banking

- One block `bus_mem[8][0x4000]`, aligned to 128 KB. The pointer is
  `Y << 17 | X << 14 | A13..A0`: `Y` is the block's address shifted, `X`
  the bank.
- Bank 0 is today's `bus_table`: an 8K or 16K ROM lives there, unbanked.
  A banked image fills banks 0..n-1 as now.
- **I/O page.** `$FF00-$FFFF` (index `0x3F00-0x3FFF`) is served from the
  current bank's top 256 bytes. Entries that devices own (Becker status and
  data today) are written through one function that stores into every
  loaded bank, so a bank switch never changes what a device read returns.
- **Bank switch.** A write to `$FF40` arrives as an event; the core1 hook
  runs `set x, n` then `jmp top` in the read state machine (two register
  writes), so the precomputed pointer is rebuilt.
- **Limit, to be measured (section 9):** the hook has to land before the
  next fetch from the cart. That is about 560 ns at 0.89 MHz and 280 ns at
  1.79 MHz; the estimate for event-to-switch is about 330 ns. A banked pak
  that switches banks from code in the cart while in fast mode may fetch
  one byte from the old bank. The self-test reports the real figure; a PIO
  decode of `$FF40` is the later fix if a real pak needs it.

## 6. Event stream

### 6.1 State machine

Starts on the same trigger as the read path (Pico 2: `OE_BUS` low; Plus-W:
flag 1). While the cycle lasts it copies the pins (`in_base` GP0: D0-D7,
A0-A13, R/W in one word) and keeps the last sample taken while the cycle
was still in progress; when the cycle ends it pushes that sample. Reads and
writes alike: on a read the data bits are what the engine drove.

### 6.2 DMA and ring

- **C**: RX FIFO to `bus_events[]`, 32-bit, endless, write address wrapping
  on a power-of-two ring (2048 entries, 8 KB).
- core1 reads channel C's write address to find new entries. DMA registers
  are on AHB. core1 does not read a PIO register per event (rule from the
  sbelectronics review: an APB poll in the hot loop stalls when the radio's
  DMA bursts run).

### 6.3 core1 event loop

For each entry: index, R/W, data.

- **Write:** write hooks (`$FF40` bank switch), then the existing write
  queue to core0 (`bus_pop_write` consumers are unchanged), counters.
- **Read:** read hooks if the index is hooked (Becker status, Becker data),
  counters.
- Nothing else. The loop and every hook stay in SRAM and flash-free;
  `check_core1_flash_free.py` keeps guarding that with an updated entry
  list.

`bus_on_read_done` / `bus_on_write` become one `bus_event(idx, rw, data)`
that the host simulator calls too, so the host tests keep exercising the
same hook and queue code.

### 6.4 Hook deadline

A hook must finish before the CoCo's next access to that device. Event
latency (end of cycle to hook done) is estimated at under 100 clk
(0.7 us). Real code reaches the same register again no sooner than about
five bus cycles (an extended-address load is five), which is 2.8 us at
1.79 MHz.

**Known difference from the CPU loop:** two accesses to one device on
consecutive cycles (`LDD $FF41` reads status then data back to back) see
the table as it was before the first access's hook ran. DriveWire clients
poll status, branch, then read data, so this does not arise in practice;
it is documented, and the self-test includes the case so its behaviour is
known and stable.

### 6.5 Rate

Executing from the cart at 1.79 MHz produces 1.79 M events per second: 84
clk per event for core1. A ROM read event costs a compare and two counter
updates. The self-test must show core1 keeping up through a sustained
burst with Becker traffic mixed in (`event_lag_max` stays far below the
ring size). If it cannot, the fallback is for the event state machine to
record only writes and I/O-page reads unless tracing is on; that is a
change to one small program, decided by the measurement, not built
speculatively.

## 7. Trace, counters, guards

- **Trace.** The ring is the trace. `trace dump [n]` prints the last `n`
  events as `seq idx R|W data`. There is no per-event timestamp; `seq` is
  the event count. On a Becker underrun or DriveWire CRC error core1 copies
  the last 512 events to a frozen buffer, as today; `trace run` thaws.
  `tools/tracedump.py` reads the new line format.
- **Counters** in `status`: `bus cycles/reads/writes`, `write_overrun`,
  `whooks`, and new `engine_stall`, `event_lag_max`, `event_drop`.
  `addr_resample`, `late_precompute`, `oe_glitch` and the resample fields
  go: they described the CPU loop.
- **Stall guard.** If the read state machine sits at `pull` with DMA B idle
  across two checks 1 ms apart, the engine is restarted and `engine_stall`
  counts it. With outputs enabled only after the pull, a stall means "not
  answering", never "driving".

## 8. Plus-W

- Read and event state machines in PIO0 (GPIO base 0). Helper in PIO2,
  whose GPIO base is 16 (the cyw43 driver puts its own state machine in
  PIO2 sm0 and sets that base; the engine asserts the base rather than
  assuming it). PIO2's "next" block is PIO0, which is what the cross-block
  flag needs; the helper must stay in PIO2.
- DMA: the radio holds channels 0-1. The engine claims its channels through
  the SDK's allocator, never by number.
- `OE_FW` (GP31) is held high. **JP2 must be at 1-2** (hardware bus
  enable), the default.
- **Not available until the respin** (`docs/ADDITIONAL_ROADMAP.md` section
  9): A14 and A15 are not used (no 32K paks, no Extended BASIC at `$8000`);
  firmware-decoded addresses (`$FF60-$FF7F`, `bus_fw_enable`) are removed;
  `bus selftest net` reports that it is not supported by this engine until
  it is rebuilt on the new self-test.
- The bare-module self-test generates `OE_BUS` itself (there is no decode
  chip on a bare module). Its fake adds 5 clk against the real chip's ~1.5;
  the self-test prints both the raw figure and the corrected one.

## 9. Self-test (the regression gate)

`bus selftest fast`, bare module only, on each board. It refuses to run
when the pins show a live bus. It keeps what the spike built (4,096-cycle
back-to-back bursts at 1.79 and 0.89 MHz, the sample-point sweep, mixed
read/write, unselected gaps, late select on the Plus-W, drive off, engine
restarts, core0 stress, radio busy) and adds:

| Check | Pass condition |
|---|---|
| Write capture | Every written byte and address arrives in the event ring, in order |
| Becker at speed | A status-poll / data-read loop at the tightest real spacing delivers a known byte stream with no loss, duplicate or phantom read, at both speeds |
| Becker back to back | `LDD`-style status+data on consecutive cycles: behaviour recorded and stable |
| Bank switch | Write `$FF40`, fetch at the next cycle and at +1, +2 cycles: which fetch first sees the new bank, at both speeds. Reported, and must be the next cycle at 0.89 MHz |
| I/O page across banks | Becker entries read the same in every bank |
| Event rate | Sustained from-cart burst with Becker traffic: `event_drop 0`, `event_lag_max` reported |
| Stall guard | A forced stall is detected, the engine restarts, pins were not driven meanwhile |

The older functional `bus selftest` is folded into this one; anything it
checked that still applies moves over, the rest is deleted.

## 10. Bench acceptance

On the PCB (Pico 2 build), then on a Plus-W PCB when one is built:

- CoCo 2, 16K, no Extended BASIC: `carttest.rom` from the manager, 30
  minutes clean (TEST_PLAN I.5, J.1).
- CoCo 3: HDB-DOS at 0.89 and 1.79 MHz (J.3.1-2), the manager and its
  launches (J.3), NitrOS-9 from flash (H.3.1), double RESET.
- **Tetris displays correctly on the CoCo 3.** This is the bug that started
  the work.
- `status` after each: `engine_stall 0`, `event_drop 0`, DriveWire counters
  clean.

## 11. Limits that remain (documented, not solved here)

- A14/A15 on either board; firmware-decoded addresses: the respin.
- A bank switch from cart code in fast mode may be one fetch late (section
  5).
- Two accesses to one device on consecutive cycles (section 6.4).
- Devices whose read value must change within the same cycle as a write.
- JP2 2-3 on a Plus-W.

**As built (2026-10-02).** Where the code differs from the text above
(`docs/firmware-architecture.md` §3.3-3.4 and §8 describe what is built):

- Read program: `in x, 3` runs after the trigger (`rd:`), not before the
  start wait; `mov pindirs, ~null` drives; the end is `mov pins, null [1]`
  then `mov pindirs, null`; the Plus-W end waits are `wait 1 irq 2`.
- Bank switch (§5): one exec'd `set x, n` under the engine lock, no
  `jmp top`. It lands one fetch late at both speeds, not only in fast
  mode (hook 82-93 clk after the write); the 0.89 MHz gate is +0 or +1.
- Latency (§4.3): Pico 2 22 clk (146 ns), margin 14; Plus-W 29 clk raw,
  24 corrected, margin 12.
- Plus-W helper (§8): a third flag (2, cycle end); it never stops;
  `engine_start` waits for OE_BUS high and clears flags 0 and 2, capped at
  64 passes (`start_wait_cap`).
- Event ring (§6.2): core1 finds events by a sentinel (`0xFFFFFFFF`), not
  by DMA C's write address; the lag scan is capped at 256 and a lap
  resyncs (`event_lap`).
- `bus_event` (§6.3): a write runs its hooks first, then trace, counters
  and queue; a read runs trace, counters, then hooks.
- Trace (§7): a separate 512-word ring; a freeze stops it instead of
  copying 512 events.
- Stall guard (§7): also requires OE_BUS high and the read SM's RX FIFO
  empty.
- Self-test (§9): `bus selftest [stress] [radio] [restarts] [switches]`;
  there is no `fast` keyword. `bus selftest net` is not rebuilt.
- core1 is launched right after `bus_engine_init()`, before the config
  replay and the `becker net` hold.

## 12. Non-goals

- New devices (sound, SDC registers, the cart RAM port). The engine is
  built so they are table contents plus event hooks; none is added here.
- A PIO/DMA write path into the table (for RAM-like devices with zero
  latency). Noted as the natural next step, not built.
- Overclocking. The margins above are at 150 MHz.

## 13. Docs to update with the work

`docs/firmware-architecture.md` sections 3.2.2-3.4, 4 and 8 (rewritten to
describe this engine; the old PIO sketch and the CPU loop text go),
`CLAUDE.md` Firmware section (the core1 read-path bullets become history;
new rules: nothing between the start wait and `push` or between `pull` and
`out pindirs` without a self-test run; device entries in the I/O page go
through the all-banks writer; engine start/stop runs from SRAM),
`firmware/README.md` (status counters, trace format, self-test),
`firmware/TEST_PLAN.md` (new section for the engine's self-test and bench
rows), `docs/pcb-bringup.md` (pointer to the new timing numbers),
`docs/ADDITIONAL_ROADMAP.md` (item 6 read-path margin: closed by this).

## 14. Order of work

1. Table and banking in the read path (pointer with bank; `bus_mem`;
   all-banks I/O writer). Self-test: bursts still clean on both boards.
2. Event state machine, DMA C, ring; core1 event loop with counters and
   trace. Self-test: write capture, event rate.
3. Hooks on events: Becker, bank switch. Self-test: Becker at speed, bank
   switch, I/O page across banks.
4. Stall guard; capture-only mode; engine start/stop paths.
5. Fold the old self-test in; host simulator on `bus_event`; delete what
   the CPU loops left behind.
6. Docs.
7. Bench: Pico 2 PCB on the CoCo 2 and CoCo 3.
