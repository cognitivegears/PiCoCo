# PiCoCo — Firmware Architecture

Raspberry Pi Pico 2 (RP2350) firmware that turns the PiCoCo board
into a combined **ROM emulator** and **DriveWire Becker‑port
peripheral**.

This document explains how the firmware is structured. For the
hardware it runs on, see [`hardware-design.md`](hardware-design.md).

---

## 1. Goals

1. Respond to every `/CTS` read cycle (the $C000–$FEFF window; `rom.c`
   serves an 8 KB or 16 KB image, or a banked one, out of idx
   `0x0000-0x3EFF` of it) in ≤ 1 bus cycle — data on D0–D7 before E falls.
2. Respond to Becker‑port reads on $FF41 (status) and $FF42 (data),
   and accept Becker‑port writes to $FF42 (data), implementing the
   DriveWire protocol.
3. Never drive the data bus when the cart is not selected — enforced
   by U15, a 74LVC00 wired NAND-NAND (`OE_BUS` low only while `/CTS` or
   `/SCS` is low and E is high), which enables U10. The engine never
   re-checks the select; it drives only inside an `OE_BUS`-low cycle.
4. Hold the CoCo's `/HALT` low during Pico boot until the PIO
   state machines are armed, so the CoCo's first HDB‑DOS `DOS`
   command can't race the Pico.
5. Keep the ARM cores free of per‑bus‑cycle work: PIO + DMA handle
   the bus, ARM handles DriveWire state and USB/UART diagnostics.

## 2. Toolchain

- **SDK**: Raspberry Pi Pico SDK ≥ 2.1.0 (Pico 2 support).
- **Compiler**: `arm-none-eabi-gcc` ≥ 13.
- **Build**: CMake + Ninja.
- **Flash**: UF2 over BOOTSEL (`bootsel` on the console), or
  `picotool load -x`. The board has no SWD header.
- **Language**: C11 throughout, the DriveWire protocol layer included.

Repo layout. `host/` and the platform-independent parts of `src/` are
covered by `ctest` (14 suites):

```
firmware/
├── CMakeLists.txt          # -DPICOCO_HOST=ON builds host lib + tests + picoco-host
├── pico_sdk_import.cmake
├── boards/                 # pico2_breadboard.h (Pico 2), plusw.h + picoco_plusw.h (Plus-W)
├── ld/                     # linker additions: bus_events + bus_mem region (section 5.1)
├── src/
│   ├── main.c              # Pico target: init, /HALT release, main loop
│   ├── plat.h              # platform functions, resolved at link time
│   ├── plat_pico.c         # RP2350 implementation of plat.h
│   ├── ring.h              # SPSC byte ring, shared by bus and becker
│   ├── log.h / log.c       # ring-buffered leveled logging
│   ├── bus/
│   │   ├── bus.h / bus.c   # response table, write ring, read hooks, trace ring, stats
│   │   ├── bus_engine.pio / bus_engine.c   # PIO + DMA bus engine (section 3.3)
│   │   ├── bus_core1.c     # core1 event loop (section 3.4)
│   │   └── fake6809.pio / fake6809.c       # `bus selftest` (section 3.2.5)
│   ├── dev/
│   │   ├── device.h / device.c   # device registry, write-event dispatch
│   │   ├── rom.h / rom.c         # ROM device
│   │   └── becker.h / becker.c   # Becker port device
│   ├── dw/
│   │   ├── dw_util.h / dw_util.c       # checksum, LSN pack/unpack
│   │   ├── dw_store.h                  # storage ops struct
│   │   ├── dw_store_posix.c            # host implementation (also used by picoco-host)
│   │   ├── dw_store_fatfs.c            # Pico implementation (flash FAT volume)
│   │   ├── dw_disk.h / dw_disk.c       # image format detection, LSN mapping
│   │   ├── dw_vser.h / dw_vser.c       # virtual-serial command channel (section 7.3)
│   │   └── dw.h / dw_server.c          # DriveWire protocol state machine
│   ├── console/
│   │   ├── console.h / console.c  # line parser + commands
│   │   └── mode.h / mode.c        # mode switch (diag/loop/bridge/native/net) + pump
│   ├── net/                # WiFi transport, Plus-W only; net_stub.c elsewhere (section 7.3.1)
│   ├── ui/                 # ROM manager screens; manager_rom.h is the built-in stub
│   ├── fs_flash.c          # FatFS diskio over the flash partition; USB MSC glue
│   ├── crash.c             # hard fault + panic record in noinit RAM
│   └── usb/                # usb_descriptors.c, tusb_config.h
├── host/
│   ├── plat_host.c         # POSIX implementation of plat.h
│   ├── picoco_host.c       # TCP 65504 server + stdin console around dw_server
│   └── sim_bus.h / sim_bus.c   # virtual CoCo for host-side tests
├── tests/
│   ├── test.h              # ~30-line TEST/ASSERT macros
│   ├── test_*.c            # one file per module
│   └── fixtures/           # synthetic images, captured byte streams, trace dumps
├── tools/
│   ├── dwtest.py           # DriveWire client exerciser (TCP)
│   ├── tracedump.py        # decodes `trace dump` output
│   └── ...                 # check_core1_flash_free.py, bench.py, pconsole.py, flash.sh,
│                           # becker_relay.py, eou_becker.py (see firmware/README.md)
└── roms/                   # user-supplied ROM images, gitignored
```

## 3. Runtime architecture

### 3.1 Core split

Reversed from the original proposal: the bus service is the
time-critical job, so it gets its own core with nothing else running
on it.

- **core1** — the bus event loop only (`src/bus/bus_core1.c`).
  Flash-free: never executes from or reads flash, runs with interrupts
  disabled from SRAM (`BUS_HOT` section attribute), so the build sets
  `PICO_FLASH_ASSUME_CORE1_SAFE` (`firmware/CMakeLists.txt`; not
  `PICOCO_FLASH_ASSUME_CORE1_SAFE` — that name never existed in the
  build). Reads are served by PIO and DMA with no CPU in the path
  (section 3.3); core1 walks the event ring after each cycle, runs the
  hooks and pushes write events into the write ring (section 3.4).
- **core0** — everything else: USB (2x CDC + MSC), the console, the
  DriveWire server, storage (flash/FatFS or the host POSIX
  equivalent), and device write dispatch (`device_dispatch_writes()`
  in `mode_pump()`). Core0 is free to touch flash; core1 never is.

Communication is the response table, write ring and read hooks in
`src/bus/bus.h` (SPSC, no locks) — not the Pico SDK `queue_t` mentioned
in the original proposal. The one lock is the engine's spin lock
(`elock`, `bus_engine_lock()`), which serialises bank switches, `rom
load`'s unbank/publish and the engine's start/stop.

### 3.2 Pin map (Pico 2 header numbering)

| GPIO | Header pin | Role |
|------|-----------|------|
| GP0–GP7   | 1,2,4,5,6,7,9,10 | D0–D7 (bidi via U10) |
| GP8–GP21  | 11,12,14–27      | A0–A13 (input from U11+U12) |
| GP22      | 29               | /R/W (input from U12; also drives U10 DIR) |
| GP26      | 31               | **OE_BUS** — cart‑selected (U15 output) |
| GP27      | 32               | **HALT_GATE** — output; drives Q2 → /HALT |
| GP28      | 34               | AUDIO_PWM by default (JP3 1-2, v2.3.1); E (input from U13) if JP3 is cut to 2-3. Firmware does not initialise this pin (no sound firmware yet, `docs/ADDITIONAL_ROADMAP.md` §5 item 4) — neither driven nor pulled. |
| RUN       | 30               | CoCo /RESET (via R9 series; R10 pull‑up footprint is DNP by default) |
| VSYS      | 39               | +5V via D2 Schottky |

`OE_BUS` is asserted whenever the cart is selected — i.e., whenever
`/CTS` OR `/SCS` is low AND the E clock is high (E qualifies the
address‑valid window). Firmware disambiguates by **A13**:

- `OE_BUS` low with `A13=0` ⇒ ROM read in the full $C000–$FEFF `/CTS`
  window (`rom.c` covers idx `0x0000-0x3EFF`).
- `OE_BUS` low with `A13=1` ⇒ Becker access in $FF40–$FF5F window.

### 3.2.1 Plus-W pin plan (board v2.3, implemented)

The v2.3 board (`docs/superpowers/specs/2026-09-17-main-board-v2.3-design.md`
§3.2) adds 15 hidden underside pads to the `PiCoCo:Pico-Carrier`
footprint, wired only when a Waveshare RP2350B-Plus-W is soldered flat
instead of a Pico 2 — NC on a Pico 2 build. `firmware/boards/plusw.h`
(select with `-DPICOCO_BOARD=plusw`) implements this table. OE_BUS is
GP40 on this board, outside PIO0's GPIO window, so the bus engine watches
it from a helper state machine in PIO2 (section 3.2.2), and `main.c`'s
`gpio_setup()` inits exactly the pins below plus D0-7/A0-13//R/W/OE_BUS,
from a board-header pin mask. Of GP24-GP30 the engine reads only GP26
(E), the Plus-W event state machine's end-of-cycle pin.

Header pins 31/32/34 carry the same nets on both modules, but the
GPIO number differs: on a Pico 2 those pins are `GP26`/`GP27`/`GP28`;
on a Plus-W the same physical header pins are `GP40`/`GP41`/`GP42`.
`E_BUF` also arrives separately on the Plus-W's own pad-grid `GP26` pin
(a different, lower-numbered GPIO than the header's GP40) — see the
"same net also reaches..." note in the table below.

| Pad-grid pin | Net | Purpose (Plus-W only) |
|---|---|---|
| GP24 | CTS_BUF | capture |
| GP25 | SCS_BUF | capture |
| GP26 | E_BUF | capture (also reaches the module's header pin 34 only if JP3 is cut to 2-3 — header pin 34 carries AUDIO_PWM by default as of v2.3.1; GP40/41/42 mapping: header 31/32/34 = GP40/41/42) |
| GP27 | Q_BUF | capture |
| GP28 | SLENB_BUF | capture |
| GP29 | A14_BUF | capture |
| GP30 | A15_BUF | capture |
| GP31 | OE_FW | Firmware U10 `/OE`, reached only with JP2 at 2-3. The PIO engine holds it high, so **JP2 must stay at 1-2** (hardware `/OE`) until the respin (`docs/ADDITIONAL_ROADMAP.md` §8) |
| GP32 | NMI_DRV | Q3 gate (DNP stage — R15/R17 also DNP); also reachable from a Pico 2 via JP5 2-3 (shares header pin 34, see below); no firmware drives this pin yet |
| GP33 | CART_DRV | Firmware-driven `/CART` pulse (bus-engine spec, §4.5-equivalent in `main.c`): toggled ~500 Hz for 500 ms after `/HALT` release when `rom_cart_wanted()` is true; also reachable from a Pico 2 via JP5 1-2, but no Pico 2 board header defines `PIN_CART_DRV` today, so the pulse is Plus-W only |
| GP34 | AUDIO_PWM | sound output stage |
| GP35 | EXP_GP35 → J1 pin 1 | Plus-W only; on a Pico 2 this is where the module's own SWDIO pad lands instead (see the debug-pad note in `hardware-design.md` §7) |
| GP43, GP44, GP45 | EXP_GP43/44/45 → J1 pins 2-4 | Plus-W only |

GP24..GP30 are contiguous, but A14/A15 (GP29/GP30) are not next to
A13 (GP21), so one PIO `in pins` cannot take A0-A15. The engine uses
A0-A13 on both boards: no 32K paks and no `$8000` window until the
respin.

**JP5** (`docs/hardware-design.md` §4.4a) is the header-pin-34 solder
jumper for the Q3/Q4 drive stages above: pad 1 = `CART_DRV`, pad 2 =
`PICO_P34` (module header pin 34), pad 3 = `NMI_DRV`. Open by default,
and mutually exclusive with JP3 (both bridge onto header pin 34) — a
Pico 2 build picks at most one of audio (JP3), a firmware `/CART` pulse
(JP5 1-2), or a firmware `/NMI` drive (JP5 2-3, needs R15/R17 fitted).
No firmware for either JP5 position exists yet (item 5 of the firmware
backlog, `docs/ADDITIONAL_ROADMAP.md` §5).

**Plus-W board header trap, avoided:** `PICO_DEFAULT_LED_PIN` is `GP25` on
a Pico 2. On a Plus-W, pad-grid `GP25` is `SCS_BUF` — a U13 **output**,
not an LED. The Plus-W has two user LEDs: LED1 on the radio's `WL_GPIO0`
(needs the CYW43 driver) and LED2 on `GP23`, which the module does not bring
out. `firmware/boards/plusw.h` uses LED2 (`PIN_LED 23`). The radio itself
uses GP36-GP39 (REG_ON, DATA/IRQ, CS, CLK), clear of the pad grid,
verified 2026-09-29 from the Waveshare schematic and a live scan
(`firmware/README.md` "WiFi").

### 3.2.2 Plus-W differences

Both state machines sit in PIO0 (GPIO base 0), as on a Pico 2. The read
program is the same, with its three waits patched at load (§4.1); the
event state machine runs its own program, `bus_event_pw` (§4.2), which
starts on helper flag 1 and ends when E falls. OE_BUS (GP40) is outside PIO0's window, so a helper, `bus_sel_pw`
(section 4.3), runs in PIO2 with GPIO base 16. The cyw43 driver puts its
own state machine in PIO2 sm0 and sets that base; `bus_engine_init()`
runs after `net_init()` and sets the base itself only if the driver has
not. On OE_BUS low the helper raises PIO0 flag 0 (read SM: cycle start)
and flag 1 (event SM) with `irq next set`; on OE_BUS high, flag 2 (read
SM: cycle end). PIO2's "next" block is PIO0, which is why the helper must
stay in PIO2. It adds about 3 clk to the read path.

The helper runs from `bus_engine_init()` on and is never stopped, by
`bus drive off` or by a restart, so the event SM never misses a cycle.
While the read SM is stopped, flags 0 and 2 pile up unread. `engine_start`
clears them only between cycles: it waits for OE_BUS (read through SIO)
to be high for 16 clk, clears flags 0 and 2, reads the IRQ register back
so the clear has landed, and checks OE_BUS is still high, else it goes
round again. A start flag cleared while its end flag is still to come
would leave that end flag pending and release every later read at once.
The wait is capped at 64 passes (about 3 µs). At the cap it counts
`start_wait_cap` and leaves the read SM stopped; `bus_engine_tick`
retries the start on its next millisecond while `bus drive` is on.

The radio holds DMA channels 0-1; the engine claims its channels and state
machines through the SDK allocators, never by number (`bus engine` prints
who holds what). Until the respin (`docs/ADDITIONAL_ROADMAP.md` §8) a
Plus-W has the Pico 2's limits: `OE_FW` (GP31) is held high and JP2 must
be at 1-2, A14/A15 are not used, and there are no firmware-decoded
addresses (`$FF60-$FF7F` went with the CPU loop, tag `fw-1.4-cpu-loop`).

### 3.2.3 Write hooks

```c
int bus_add_write_hook(uint16_t idx, void (*fn)(uint8_t data));  /* 0 ok, -1 full */
```

Same shape as the existing read-hook table (`bus_add_read_hook`):
`BUS_MAX_HOOKS` (4) fixed entries, matched by `idx` with a linear scan —
cheap at four entries, no map needed. core1 calls the matching hook from
`bus_event()` when the write's event arrives, after the cycle, as the
first thing it does with it: before the trace store, the counters and
the core0-facing write ring. The `$FF40` bank hook races the next
cycle's `in x, 3` (section 3.2.4), so nothing goes in front of it, and
core0 sees a hook's effect no later than the write itself. core0 still
sees every write; the hook is an early look, not a replacement for the
queued event. `bus whooks` in `status` counts hook invocations. Hooks
are `BUS_HOT` — SRAM-resident, no libc, no flash — the same rule core1's
own loop follows.
`firmware/tools/check_core1_flash_free.py` discovers hook functions by
grepping for `bus_add_(read|write)_hook(...)` call sites (one regex
covers both), so a write hook that branches into flash fails the Pico
build the same way a read hook or the core1 loop itself would.

### 3.2.4 Table and banked ROM

The engine serves `bus_mem[8][16384]` (`bus.h`): eight 16 KB banks in one
128 KB-aligned block, in its own linker region at `0x20060000`
(`firmware/ld/`, section 5.1). Bank 0 is `bus_table`. The read SM builds
the address of the byte as `Y << 17 | X << 14 | A13..A0`, `Y` being
`&bus_mem >> 17` and `X` the bank (section 3.3), so a bank switch is one
register update. The I/O page (`$FF00-$FFFF`, idx `0x3F00-0x3FFF`) comes
from the current bank too. The entries devices own, `0x3F40-0x3F5F`, are
written only through `bus_io_set()`, which stores into all eight banks, so
a device read never changes with the bank; ROM loaders leave them alone.
`bus_peek(idx)` returns `bus_mem[bus_bank][idx]`.

`rom.c` loads three shapes:

- **8192 or 16384 bytes**: unbanked, into bank 0 (the 16 KB case skips
  `0x3F40-0x3F5F`, per the single-writer rule in §6.3).
- **32768, 65536 or 131072 bytes** (2/4/8 x 16 KB, the largest Games
  Master Cartridge size MAME's `coco_gmc` supports): banked, straight into
  banks 0..n-1 of `bus_mem`. `unbank()` (count 0, bank 0) runs first, so
  the `$FF40` hook is inert while banks fill; `rom_publish_banks()` then
  sets bank 0, the mask and the count in one step. Both take the engine
  lock that the hook takes, so a switch racing the loader sees "unbanked"
  or "fully loaded", never a half-filled table.
- Any other size is refused with `-2` and the previous ROM stays loaded;
  the console prints `rom load: size must be 8K, 16K, or banked 32K/64K/128K`.

`rom_init()` registers a `$FF40` write hook (`rom_bank_hook`) once, for
the life of the firmware; it is a no-op while no banks are loaded. On a
write it takes the engine lock and calls `bus_engine_set_bank_locked(data
& mask)`, which stores `bus_bank` and execs `set x, n` into the read SM.
No pause, no PC read, no jump: an exec'd instruction runs at an
instruction boundary, a state machine stalled in a `wait` or `pull` runs
it and stays stalled, a running one loses 1 clk. The read program takes
the bank (`in x, 3`) right after its start trigger, so a switch takes
effect for every cycle whose `in x, 3` has not yet run.

The hook lands 82-93 clk after the write's OE_BUS rises, which is after
the next cycle's `in x, 3` at both speeds. So the first cart fetch after
a `STA $FF40` comes from the old bank and every later one from the new:
`+1` on 512 of 512 switches at both speeds in the self-test. On a Plus-W
at 0.89 MHz the hook sits on the boundary and 1-95 of 512 switches land
at `+0`. No read is ever served from a wrong address. The later fix, if a
real pak needs it, is a PIO decode of `$FF40`
(`docs/ADDITIONAL_ROADMAP.md` §5 item 7).

`rom_is_dos()` (loaded and the first two bytes are `DK`) feeds
`rom_cart_wanted()`'s AUTO mode (the `/CART` pulse in `main.c`).

### 3.2.5 Self-test: fake 6809

`bus selftest` (`firmware/src/bus/fake6809.c`, PIO program in
`fake6809.pio`) runs the real engine, event loop and hooks against
synthetic 6809 bus cycles generated by a PIO1 state machine, clk-exact
(84 clk per cycle at 1.79 MHz, 168 at 0.89 MHz), so the bus path can be
exercised with no CoCo attached. It is the engine's regression gate.

**Unplug the board from the CoCo before running this — the guard below is
a backstop, not a licence to leave it plugged in.** Before touching a
single pin, it samples `bus_stats.cycles`, sleeps 100 ms, and re-checks
it: if the count moved, a CoCo is actually driving the bus, and the test
refuses immediately (`-2`, "bus is live (CoCo attached), refused"). That
check alone misses an idle CoCo: one running from RAM produces no cart
cycles at all, so the count never moves even with the board live. A
second guard covers that case: for 10 ms, sampled every 50 µs, the
address and `R/W` pins (plus `OE_BUS` on a Pico 2, or `E`/`Q` on a
Plus-W) must not move at all — those pins come through always-enabled
buffers and toggle with the CPU clock whenever anything is running them,
where an unplugged board just sits on its pull-ups. Only after both
checks pass does it hand the address/control pins to PIO1 (and back to
SIO when it is done). Write data comes from a test-only state machine in
PIO0, since D0-D7 belong to the engine. `firmware/tools/bench.py`
(below) is registered as the `bench` ctest target only when configured
with `-DPICOCO_BENCH=ON` — it is opt-in, not part of the default `ctest
--test-dir build-host` run, because it drives real bus pins.

On a Pico 2 the fake drives A0-A13 and R/W with `OE_BUS` as a side-set.
A bare Plus-W has no decode chip, so the fake drives E and the selects
and a fake decode generates `OE_BUS`; its own delay (5 clk, against about
1.5 for the real 74LVC00) is measured and taken out (`corrected`).

What it checks, in one run of about 2 s: 4,096-cycle back-to-back bursts
at the realistic sample point (36 clk after OE_BUS falls at 1.79 MHz,
72 at 0.89 MHz), mixed reads and writes, unselected gaps, write capture
with data, event content and rate, engine restarts under a burst, a lap
of the event ring, a forced stall, bank switches at random phases,
capture-only (`bus drive off`), all eight banks, the I/O page across
banks, a Becker byte stream at both speeds, Becker status and data on
consecutive cycles, where a `$FF40` switch first shows, sample-point
sweeps (`response_clk`), the release (`release_clk`), and on a Plus-W
late `/CTS` and `/SCS`. Options: `stress` (core0 memcpy load), `radio`
(Plus-W: scans and cyw43 polling), `restarts` (40 restart bursts, then
stop), `switches` (40 switch bursts with a phase histogram, then stop).
It prints `selftest fast pass` only if every gated line passed; the
expected lines per board are in `firmware/README.md` and
`firmware/TEST_PLAN.md` section K.

By design it should be safe on a PCB powered from USB alone: with no cart
+5 V present the board's +3.3 V rail (an LDO off +5 V) is off, so
U10/U11/U12/U13/U15's outputs sit in their unpowered high-impedance state
(LVC "partial-power-down" `Ioff`) and cannot drive against the PIO. That
has never been run; until someone checks it on a scope, run the self-test
on a bare module only, and never with a CoCo attached (the engine would
drive into a live bus). On success (or failure) it calls `rom_off()` and
prints `selftest rom cleared; reload with rom load` — the synthetic image
does not survive a run either way, which is why `firmware/tools/bench.py`
sends `reboot` at the end of its own run, to replay a saved `rom load`
from `picoco.cfg`. With +5 V on the board (a bench supply or a CoCo),
U11-U13 are powered and their outputs would fight PIO1; the measured
figures in section 8 all come from bare modules.

### 3.3 Bus engine: the read path

Reads are served by PIO and DMA alone (`bus_engine.pio`, `bus_engine.c`):
no CPU sits between OE_BUS falling and the byte on D0-D7, on either
board. The CPU loop of firmware 1.4 is tagged `fw-1.4-cpu-loop`; there is
no build switch back to it. Design spec:
`docs/superpowers/specs/2026-10-02-pio-bus-engine-design.md`.

| Resource | Pico 2 | Plus-W | Role |
|---|---|---|---|
| PIO0 sm0 | read | read | serves the byte (section 4.1) |
| PIO0 sm1 | event | event | one word per selected cycle (section 4.2) |
| PIO2 sm1 | — | helper | watches OE_BUS on GP40 (section 4.3) |
| DMA A | ch 0 | ch 2 | read SM RX (the pointer) → DMA B `READ_ADDR_TRIG`; 32-bit, endless |
| DMA B | ch 1 | ch 3 | that one byte → read SM TX; 8-bit, count 1, re-armed by every A write |
| DMA C | ch 2 | ch 4 | event SM RX → `bus_events`; 32-bit, endless, 8 KB write ring |

The numbers are what `bus engine` printed on the bench modules; every
state machine and channel comes from the SDK allocators. All three DMA
channels are high priority and DMA has bus priority (reads and writes).
Input synchronisers are bypassed on A0-A13 and R/W only; OE_BUS (an
asynchronous strobe) and D0-D7 keep theirs.

**Per read.** On the start trigger the read SM tests R/W (`jmp pin`); a
write is never driven. For a read it shifts the bank (`in x, 3`) and
A0-A13 (`in pins, 14`) into an ISR that already holds `Y`, so the ISR is
the address of the byte, `Y << 17 | X << 14 | A13..A0` (section 3.2.4).
`push noblock` hands it to DMA A, which writes it into DMA B's trigger
register; B copies the byte into the TX FIFO. An 8-bit DMA write reaches
the FIFO replicated across the word (measured), so `out pins, 8` takes the
right byte. The SM sets the output latch and only then enables the
outputs (`mov pindirs, ~null`): a stalled DMA leaves the pins released,
never driving. At the end of the cycle it drives D0-D7 low for 2 clk and
then releases them (RP2350-E9: a pad left high on the pull-down alone
stays high).

Every instruction between the start wait and `push`, or between `pull`
and `mov pindirs`, costs 1 clk of read latency; `in x, 3` is the one
added beyond the minimum (it costs every read 1 clk and saves a bank
switch from touching the program counter). Change either stretch only
with a self-test run on both boards.

**Start and stop.** `engine_stop` and `engine_start` run from SRAM: from
flash, a cache miss once left the pins driven for two cycles. `engine_stop`
runs whole with interrupts off, under the engine's spin lock;
`engine_start` takes the lock only from the `X` load on. Its drain, FIFO
clear, restart and `Y` load run with interrupts on and no lock, which is
harmless because the read SM is stopped. Stop disables the read SM and
execs `mov pins, null` then `mov pindirs, null`. Start waits until the
read SM's RX FIFO is empty and DMA B is idle (a pointer still in flight
would land after the FIFO clear and serve every later read one cycle
late), clears the FIFOs, restarts the SM, loads `Y`, jumps to `top`, sets
`X` to `bus_bank` under the lock, runs the Plus-W flag sequence (section
3.2.2) and enables.

**Capture-only (`bus drive off`).** Only the read SM stops, its pins
discharged and released. The event SM, DMA C and the Plus-W helper keep
running, so writes, the trace and every event still arrive. `bus drive
on` stops and restarts the read path.

**Stall guard.** `bus_engine_tick` runs in core0's main loop once a
millisecond and checks five things: the read SM is enabled, its PC is at
`pull`, DMA B is idle, OE_BUS is high, and the read SM's RX FIFO is
empty. Seen on two ticks in a row, it runs `engine_stop` and
`engine_start` and counts `engine_stall`. A served read also waits at
`pull` with B idle (12-20 % of samples under a reads burst), so without
the OE_BUS term a busy bus restarted the engine on a few per cent of tick
pairs. Detection takes 1-2 ms on an idle bus and about 6 ticks with
OE_BUS cycling, bounded in practice but not hard-bounded. Because the
outputs are enabled only after the pull, a stall means "not answering",
never "driving". The guard covers a lost pointer; a dead DMA A is not
covered.

### 3.4 Event stream and the core1 loop

The event SM samples D0-D7, A0-A13 and R/W (23 pins from GP0) from the
same trigger as the read path (Pico 2: OE_BUS low; Plus-W: flag 1) until
the cycle ends, keeps the last sample taken while the cycle was still on,
and pushes that one word: `[7:0]` data (on a read, what the engine
drove), `[21:8]` index, `[22]` R/W (1 = read), `[31:23]` always 0. Its RX
FIFO is joined (8 deep) and the push is `noblock`: a push into a full
FIFO drops the event and sets `FDEBUG.RXSTALL`, which core0 counts as
`event_drop` on each 1 ms tick. DMA C drains it into `bus_events[2048]`,
whose write address wraps. The event SM and DMA C start once in
`engine_init` and never stop, so DMA C's position and core1's index stay
in step.

core1 (`bus_core1_main`, SRAM, interrupts off) walks the ring by a
sentinel: every empty slot holds `BUS_EV_NONE` (`0xFFFFFFFF`), which no
selected cycle can produce (bits 23-31 read 0, and it would be index
`0x3FFF`, never selected). It reads its next slot until it is not the
sentinel, writes the sentinel back and calls `bus_event(w)`. It never
reads a DMA register per event: a core reading DMA registers back to back
delays DMA A by 1 clk now and then.

`bus_event()` is also what the host simulator calls, so the host tests run
the same hook and queue code. A write runs its hooks first, then the
trace store, the counters and the write ring; a read runs the trace
store, the counters, then its read hook (Becker status and data). The
read path is kept separate and short on purpose: at 1.79 MHz with every
cycle from the cart there are 84 clk per event, and estimates from the
disassembly put a ROM fetch event at about 45-50 clk and a `$FF41`/`$FF42`
read at about 110-120 clk, so the self-test's six-cycle Becker client
pattern costs about 72 of 84 clk. That leaves about 10-15 clk per event;
a shared write/read path once cost enough to lap the ring. Any change to
the read event path or the hooks must re-run the self-test's Becker
event-rate line.

**Lag and laps.** Every 256 events core1 counts the unread slots ahead,
up to 256 (`event_lag_max`, which saturates there). At the cap it checks
for a lap: DMA C's write position, read twice and unchanged, points at a
slot that still holds an unread event. On
a lap it empties every slot forward from the writer, then the ones DMA C
wrote meanwhile, jumps to DMA C's position and counts `event_lap`. A real
lap costs about 12k clk, about 146 events dropped with no hooks run for
them. Before a check finds the lap, up to 255 stale slots (256 more for
each check that leaves a moving writer to the next) are served out of
order. The resync relies on DMA C's `WRITE_ADDR` advancing on write
completion (datasheet wording), so the slot it clears last has landed.

**Hook deadline.** A hook must finish before the CoCo next touches that
device. Two accesses to one device on consecutive cycles see the table as
it was before the first one's hook ran. Measured on both boards: status
then data on consecutive cycles gives `02,5c` with a byte ready and
`00,ff` when empty, stable; a `$FF41` poll on the cycle straight after the
`$FF42` that took the last byte still sees `02` (a phantom `0xFF` read
would follow). HDB-DOS and DW4 leave at least five cycles between the two
reads (inferred from instruction timings, not yet seen on a CoCo).

**Trace.** core1 also stores every event in a separate 512-word trace
ring. `trace dump [n]` prints `seq idx R|W data`, `seq` being the event
count since boot (no timestamp). A Becker underrun freezes it from the
data hook, with that read as the last entry; a DriveWire CRC error
freezes it from core0. `trace run` thaws it.

**Launch order** (`main.c`). `bus_engine_init()` runs after `net_init()`
(the cyw43 driver claims PIO2 first) with the read SM stopped, since
`bus drive` is off until the config replays `bus drive on`. core1 is
launched straight after, before the config replay and the `becker net`
hold, so no event waits unread (more than 2048 would lap the ring). Every
hook is registered before the launch.

## 4. PIO programs

All in `firmware/src/bus/bus_engine.pio`, clock divider 1 (150 MHz).
The listings below drop the source comments; the file has them.

### 4.1 `bus_read` (PIO0)

`in_base` GP8 (A0), `out_base` GP0 (D0, 8 pins), `jmp_pin` GP22 (R/W), ISR
shifting left with no autopush. `Y` and `X` are loaded by `engine_start`
while the SM is stopped; `X` again by every bank switch.

```
.wrap_target
top:   mov isr, y              ; y = &bus_mem >> 17
trig:  wait 0 gpio 26          ; OE_BUS low.   Plus-W: wait 1 irq 0
       jmp pin rd              ; R/W high = CoCo read
wend:  wait 1 gpio 26          ; a write: never driven.   Plus-W: wait 1 irq 2
       jmp top
rd:    in x, 3                 ; bank, as it is now
       in pins, 14             ; isr = &bus_mem[x][A13..A0]
       push noblock            ; -> DMA A -> DMA B
wbyte: pull block              ; the byte from DMA B (stall guard: stuck here)
       out pins, 8
       mov pindirs, ~null      ; drive only once the byte is in the latch
rend:  wait 1 gpio 26          ; end of cycle.   Plus-W: wait 1 irq 2
       mov pins, null [1]      ; RP2350-E9: drive low two clk...
       mov pindirs, null       ; ...then release
.wrap
```

On a Plus-W `engine_init` rewrites the three waits at `trig`, `wend` and
`rend` in instruction memory to the helper's flags (section 4.3).

### 4.2 `bus_event_p2` / `bus_event_pw` (PIO0)

`in_base` GP0 with an input count of 23, so `mov isr, pins` is D0-D7,
A0-A13 and R/W and bits 23-31 read 0. `jmp_pin` is OE_BUS (GP26) on a
Pico 2 and E (GP26) on a Plus-W.

```
.program bus_event_p2               .program bus_event_pw
.wrap_target                        .wrap_target
      wait 1 gpio 26                      wait 1 irq 1     ; helper: cycle start
      wait 0 gpio 26                smp:  mov isr, pins
smp:  mov isr, pins                       jmp pin keep     ; E still high
      jmp pin done  ; OE_BUS high         jmp done
      mov x, isr                    keep: mov x, isr
      jmp smp                             jmp smp
done: mov isr, x                    done: mov isr, x
      push noblock                        push noblock
.wrap                               .wrap
```

A sample that finds the cycle over may be from after its end, so it is
dropped and the one before it (`x`) is pushed.

### 4.3 `bus_sel_pw` (PIO2, Plus-W only)

GPIO base 16, so `gpio 40` is reachable; `irq next` lands in PIO0.

```
.wrap_target
    wait 1 gpio 40          ; a start while OE_BUS is low skips that cycle
    wait 0 gpio 40          ; OE_BUS low: selected and E high
    irq next set 0          ; read SM: cycle start
    irq next set 1          ; event SM
    wait 1 gpio 40          ; OE_BUS high: cycle over
    irq next set 2          ; read SM: release
.wrap
```

## 5. Memory map

### 5.1 RAM layout (relevant sections)

| Address range | Size | Use |
|---------------|------|-----|
| `0x20000000-0x2005DFFF` | 376 KB | code in SRAM (`BUS_HOT`, engine start/stop), `.data`, `.bss`, heap, stacks |
| `0x2005E000-0x2005FFFF` | 8 KB | `bus_events[2048]` — DMA C's write ring (section 3.4) |
| `0x20060000-0x2007FFFF` | 128 KB | `bus_mem[8][16384]` — the table the engine serves (section 3.2.4) |

The last two are a linker region of their own (`firmware/ld/`,
NOLOAD), so the 128 KB alignment does not pad `.bss`; the linker script
asserts both addresses. SRAM left free below them: 212.6 KB on a Pico 2
and 164.4 KB on a Plus-W (from the ELF when the event stream went in,
2026-10-02).

### 5.2 Flash layout

- Bootloader at the reset vector (Pico SDK stage2).
- Application code + constant ROM images (the manager stub) in flash (XIP).
- The FAT filesystem from `PICOCO_FS_OFFSET` (`0x180000`) to the end of
  flash, and one sector just below it for the double-RESET marker
  (`firmware/README.md` "Filesystem").

**Config:** `picoco.cfg` is a plain list of console commands, replayed
line by line at boot by `console_run_config()`; `save` writes the current
state back out as that command list. Same information, no separate config
parser. On the host it lives at `<dir>/picoco.cfg` next to the disk
images; on the Pico at the root of the flash FAT volume.

## 6. Becker port protocol

The Becker port convention, per CoCo3FPGA / XRoar and used by
DriveWire client code:

| Addr | R/W | Meaning |
|------|-----|---------|
| `$FF41` | R | Status byte. Only bit 1 is defined: **1 = byte available to read**. |
| `$FF41` | W | Ignored (write‑through to a "any byte" to trigger something, but per spec, writing does nothing). |
| `$FF42` | R | Next byte from server. Always safe to read; if no byte available, implementation‑defined (we return 0xFF). |
| `$FF42` | W | Byte to server. Pushed into the outgoing queue. |

As built, the filter is not the 5-bit `/SCS` window (A0..A4) described
in earlier drafts of this section — it is the full 14-bit `bus_table`
index, since every device shares one 16384-entry table indexed by
A0..A13. `$FF41` is index `BUS_IDX_BECKER_STATUS` (0x3F41) and `$FF42`
is `BUS_IDX_BECKER_DATA` (0x3F42); the becker device claims the whole
`0x3F40..0x3F5F` range for write dispatch but only reacts to 0x3F42.

### 6.1 Response-table model

There is no per-device state struct or bus snapshot callback. Instead
(spec section 5, `firmware/src/bus/bus.h` and `bus/bus.c`):

- `bus_mem` (section 3.2.4) holds the byte the engine drives for any
  cart-selected read, indexed by bank and A0..A13. Devices call
  `bus_set_read(idx, byte)` (or `bus_io_set` for their I/O entries) to
  publish a value; nothing computes a response inside the cycle.
- `read_hooks[]` run on core1 when the event of a read at a registered
  index arrives, after the cycle (section 3.4). The becker device registers hooks on
  **both** 0x3F41 and 0x3F42 (not just 0x3F42) — see the single-writer
  rule below.
- A single-producer/single-consumer write-event ring carries
  `(idx, data)` pairs from core1 to core0; core0 drains it in its main
  loop (`device_dispatch_writes()`) and calls the owning device's
  `on_write`.

The move from the CPU loop to the PIO + DMA engine changed no device
code beyond the all-banks writer.

### 6.2 Becker device (`firmware/src/dev/becker.c`)

Two byte queues, `to_coco` (256 bytes: CoCo-bound) and `from_coco`
(1024 bytes: CoCo-written), implemented as plain SPSC rings
(`src/ring.h`). The status and data table entries are always derived
from `to_coco`'s state: `0x3F41` is 0x00 when empty else 0x02; `0x3F42`
is 0xFF when empty else the head byte. Reading `0x3F42` pops one byte
first, then both entries are refreshed; reading `0x3F41` refreshes
without popping. Writing `0x3F42` pushes onto `from_coco` (counted as
`overrun` if full); writes to any other address in the claimed range,
including `0x3F41`, are ignored. Core0-facing stream API:
`becker_read`, `becker_write`, `becker_rx_avail`, `becker_tx_free`, plus
a loopback pump (`becker_loopback_pump`) that copies `from_coco` back
into `to_coco` for the bring-up "loop" mode. Stats: `reads`, `writes`,
`underrun` (data read with nothing queued), `overrun` (write queue
full).

### 6.3 Single writer rule

Ruled during implementation (spec section 5, Task 7 of the plan): only
core1 writes the two Becker table entries, from the two read hooks,
through `bus_io_set` (all eight banks). Core0 (`becker_write` and the
loopback pump) only pushes into `to_coco` and never touches the
`0x3F41`/`0x3F42` entries; ROM loads skip `0x3F40-0x3F5F`. When
publishing, the hooks store data before status; when emptying, status
before data, so a status read never says "ready" over a stale data
byte. A byte core0 pushes becomes visible to the CoCo on its next
`$FF41` poll, about one extra poll of latency (roughly 10 µs). Two
writers touching those entries independently had a race where the
status byte could read "data ready" while the data byte still held
0xFF; registering the refresh on both indices and keeping core0 out of
it removes that race. See `firmware/src/dev/becker.c` for the
implementation and its `ponytail:` comment documenting this ceiling.

The DriveWire server's replies do not go straight to `becker_write`,
either: because a single `dw_feed` call can produce up to 259 bytes for
one request while `to_coco` only holds 256, `mode_pump()`
(`firmware/src/console/mode.c`) buffers pending reply bytes and drains
them into `becker_write` as space frees up across calls, one
`becker_tx_free()`-sized slice at a time.

## 7. DriveWire integration

### 7.1 Transport

Three Becker modes carry DriveWire (`becker native|bridge|net`):

- **native**: the DriveWire server (section 7.2) runs on the Pico, with
  disk images on the flash FAT volume. No host needed.
- **bridge**: the Pico relays Becker bytes over USB CDC0 to a host
  DriveWire server (DriveWire 4, `picoco-host` through
  `tools/becker_relay.py`, FujiNet-PC) that thinks it's talking to a real
  CoCo over a serial cable.
- **net** (Plus-W): the same relay over one TCP connection (section 7.3.1).

An SD card backing store is not built.

### 7.2 Protocol state machine

The DriveWire server (`firmware/src/dw/dw.h`, `dw_server.c`) is a
push-style, transport-agnostic state machine — not a core1 handler and
not a CDC proxy. Bytes are pushed in with `dw_feed(s, buf, n, now_ms)`
from whatever transport is in use (Becker stream on the Pico, a TCP
socket on the host); replies go out through a `dw_send_fn` callback
(`becker_write` on the Pico via `mode_pump`, `write(2)` to the socket
on the host); `dw_tick(s, now_ms)` handles the 250 ms payload timeout.
The same C code runs unchanged on both platforms, which is what makes
the host build a faithful test of the wire protocol.

Opcode coverage, byte-for-byte against pyDriveWire (`dwconstants.py`,
`dwserver.py`):

- **Implemented with full request/response semantics:** READ, REREAD,
  READEX, REREADEX (checksum handshake with a `DW_READEX_CKSUM` state),
  WRITE, REWRITE, TIME, SETTIME, TIMER, INIT, DWINIT, NOP, RESET1/2/3,
  TERM, and the virtual-serial set of section 7.3 (SERINIT, SERTERM,
  SERREAD, SERREADM, SERWRITE, SERWRITEM, SERSETSTAT with the 26-byte
  COMST extension, FASTWRITE).
- **Consumed as stubs** (payload read and discarded, correct reply
  shape sent where pyDriveWire sends one, no real behaviour):
  SERGETSTAT, GETSTAT, SETSTAT, PRINT, PRINTFLUSH,
  NAMEOBJ_MOUNT/CREATE/TYPE (reply 0). These exist so a real CoCo client
  doesn't desync waiting for bytes that never come.
- Unknown opcodes are counted (`stats.unknown_op`) and otherwise
  ignored; a stalled payload wait past 250 ms resets to `DW_IDLE` and
  counts `stats.timeouts`, matching pyDriveWire.

HDB-DOS mode (`dw->hdbdos`, on by default) maps `drive = lsn / 630`,
`lsn %= 630` for READ/READEX/WRITE and ignores the drive byte, per
pyDriveWire. `dw hdbdos off` uses the drive byte directly for OS-9.

Disk image formats (`dw_disk.c`): VDK, JVC, OS9 and raw are
autodetected in that order from the file header; all read/write goes
through `dw_store_ops` (below), so LSN-to-byte-offset math is the same
regardless of what's backing the file.

Storage (`dw_store.h`) is a small vtable — `open/read/write/size/
sync/close` — with a POSIX implementation (`dw_store_posix.c`, used by
both `ctest` and `picoco-host`) and a FatFS implementation
(`dw_store_fatfs.c`) for the Pico's flash partition.

### 7.3 Virtual-serial command channel (v1.2)

`dw_vser.c` implements DW4 virtual-serial command mode (channels 1-13,
one session at a time) on top of the same `dw_server` opcode parser
(SERINIT/SERTERM/SERSETSTAT/SERWRITE/SERWRITEM/SERREAD/SERREADM/FASTWRITE,
previously stubs) — it lives under `dw/`, not `console/`: it takes an
`exec` callback at init (`console_exec_remote`) rather than calling into
console code directly, so `dw_vser` stays host-testable with a fake exec.
It runs entirely on core0, inside the DW server's normal `dw_feed`/poll
path (no new thread, no core1 involvement); `console_exec_remote` must
never re-enter `dw_feed` (a remote command triggering another DriveWire
transaction would reenter the state machine mid-request). It swaps the
console's output function to a capturing buffer, checks a deny-by-default
allowlist, dispatches through the same `dispatch()` USB CDC1 uses, and
restores the output function, so USB console behaviour is unchanged. See
`firmware/README.md` for the allowlist contents and wire framing.

### 7.3.1 WiFi transport

Plus-W only (`PICOCO_HAVE_NET`). `firmware/src/net/net.c` runs lwIP in
polled mode (`pico_cyw43_arch_lwip_poll`): no background thread, no locks,
and the whole stack lives on core0. `net_poll` is called from the main loop
beside `tud_task` and `mode_pump`, and from the boot wait.

State machine: off, joining, connecting, up, failed. One TCP client, TCP_NODELAY,
1 KB rings each way, power save off after join, retry every 2 s, 15 s timeout
for join+DHCP and for connect. `MODE_NET` is the bridge case with the
platform read/write swapped for `net_read`/`net_write` (writes return n when
not up, the bridge rule). Core1 is untouched: it still only sees the
Becker ring, so it stays flash-free. With `becker net` saved, `main.c` holds
/HALT until the socket is up or 10 s, then falls back to native. SNTP starts
once per boot on link-up and seeds the clock only when it is stopped. Host
builds link `net_stub.c`. A remote server must answer within about 200 ms.
See `firmware/README.md` and `docs/superpowers/specs/2026-09-29-wifi-transport-design.md`.

### 7.4 Host build and test tools

The host build (`cmake -B build-host -DPICOCO_HOST=ON firmware`)
compiles the entire stack above plus:

- `picoco-host` (`firmware/host/picoco_host.c`): a standalone
  DriveWire server listening on TCP port 65504 (the standard Becker
  port), backed by `dw_store_posix`, with a stdin console
  (`console_feed`) and a `--replay FILE` mode that feeds a captured
  session back through `dw_feed` with no socket at all.
- `firmware/tools/dwtest.py`: a stdlib-only Python client that mounts
  images, compares full-image READEX against the file on disk, checks
  READ checksums, forces bad-checksum READEX/WRITE (expects `E_CRC`),
  checks an unmounted-drive read (`E_NOTRDY`), round-trips a
  scratch-drive WRITE/READEX, and checks TIME.
- A documented manual check: XRoar (`-cart becker`) pointed at
  `picoco-host`, booting a real HDB-DOS DriveWire ROM and running
  `DIR`/`LOADM` against a mounted image.

See `firmware/README.md` for exact commands. This is the "host first"
principle from spec section 9: everything except GPIO access is
verified on the Mac before it ever touches hardware.

The same build carries the full debuggability set from the design
spec (`docs/superpowers/specs/2026-09-07-firmware-design.md` section
9), all exercised by the host tests before they run on hardware: the
always-on bus trace ring (section 6.1's `bus.c`, frozen
for a consistent `trace dump`); per-module stats structs (`bus_stats`,
`becker_stats`, `dw_stats`) printed by the console's `status`/`dw
stats`; `dw capture on <file>` / `off`, which appends one
length-prefixed chunk per `dw_feed`/send call — `dir` (0 rx, 1 tx),
`len_lo`, `len_hi`, then that many bytes — to a file via `dw_store`;
`picoco-host --replay FILE` and the `firmware/tests/fixtures/*.cap`
regression captures, which feed only the dir-0 chunks back through
`dw_feed`; `firmware/tools/tracedump.py`, which decodes `trace dump`
output into symbolic addresses and groups consecutive `$FF41`/`$FF42`
cycles into decoded DriveWire transactions; and the ring-buffered
leveled logger (`firmware/src/log.c`) that is never called from the
core1-equivalent hot paths.

## 8. Timing analysis

CoCo 3 at double speed (1.79 MHz, 560 ns cycle):

| Event | t |
|-------|---|
| Q rising (address valid) | 0 |
| E rising | +140 ns |
| Data must be valid | +420 ns (before E fall) |
| E falling | +420 ns |
| Q falling | +280 ns |
| Bus cycle | 560 ns |

CoCo 1/2 at 0.89 MHz (1120 ns cycle): all numbers double. The setup
window from E‑rising to E‑falling is 560 ns instead of 280 ns, so
ROM‑serve timing is actually easier on the older machines.

The byte has to be on D0-D7 before the 6809E latches it. The self-test's
realistic sample point is 36 clk (240 ns) after OE_BUS falls at
1.79 MHz and 72 clk (480 ns) at 0.89 MHz. The engine's latency in clk is
the same at both speeds, so 0.89 MHz has twice the window (on a Pico 2
the margin goes from 14 to 50 clk). Q is not
routed to the Pico; OE_BUS (U15, qualified by E) is the trigger.

Measured on bare modules, 2026-10-02, `bus selftest` at 150 MHz (the
expected lines are in `firmware/TEST_PLAN.md` section K):

| | Pico 2 | Plus-W |
|---|---|---|
| OE_BUS falls → byte on D0-D7 (`response_clk`), both speeds | 22 clk (146 ns) | 29 clk (193 ns) after E rose; 24 after the fake OE_BUS fell |
| Margin to 36 clk at 1.79 MHz | 14 clk | 12 clk |
| Late `/CTS` or `/SCS` (select 4 clk after E rose) | — | 33 clk (220 ns) after E rose; 24 after the fake OE_BUS fell |
| D0-D7 back low after the cycle (`release_clk`) | 6 clk after OE_BUS rose | 11 clk after E fell |
| 4,096 back-to-back reads at 1.79 and 0.89 MHz | 0 mismatches | 0 mismatches |
| `$FF40` hook: write's OE_BUS rise → `bus_bank` stored | 82-93 clk | 82-92 clk |
| First fetch from the new bank, 0.89 MHz | +1 (512 of 512) | +1; +0 on 1-95 of 512 (on the boundary) |
| First fetch from the new bank, 1.79 MHz | +1 | +1 |
| Events, every cycle selected, 1.79 MHz | 20480 of 20480, lag max 0, drop 0, lap 0 | same |
| Events with the Becker stream, 1.79 MHz | 12293 of 12293, lag max 1-2, drop 0 | same |
| Becker stream, 2 KB at 1.79 and 0.89 MHz | 0 lost, duplicated or phantom; underrun 0, overrun 0 | same |
| Engine restarts at random phases (`restarts`) | 1840, events missing 0 | 1800, events missing 0 |
| Bank switches at random phases (`switches`) | 1820, bad 0, wrong bank 0 | 1800, bad 0, wrong bank 0 |
| On a PCB in a CoCo | not measured | not measured |

The Plus-W raw figures include the fake decode's 5 clk; the real 74LVC00
is about 1.5. The CPU loop's timing work (2026-09-30 and 2026-10-01) is
kept as history in `docs/pcb-bringup.md`.

## 8.1 /HALT hold‑until‑booted sequence

The Pico drives CoCo `/HALT` through the 2N7002 N‑FET (Q2), whose
gate is at pin 1 and is pulled up to +3V3 through R7 (10 kΩ, v2.3.1 —
was 100 kΩ, which the RP2350's own reset pull-down on GP27 beat). The
Pico's GP27 is connected to the gate through R8 (100 Ω series).

Power‑on sequence:

1. CoCo +5V rail stabilizes; D2 feeds VSYS_PICO ≈ 4.7 V.
2. Pico 2 boot ROM starts executing; GP27 is tri‑state until SDK runtime
   configures it. R7 holds the gate at ≈ 3.3 V → Q2 ON → **/HALT = low**.
   The CoCo's 6809E is held in HALT before it can fetch its first vector.
3. `main.c`'s `gpio_setup()`, the first thing `main()` runs, latches
   `PIN_HALT` high and makes it an output, so /HALT never glitches
   released.
4. `bus_engine_init()` arms the PIO state machines and DMA channels,
   core1 is launched, `picoco.cfg` is replayed (its `rom load` and `bus
   drive on`), and with `becker net` saved the board waits up to 10 s for
   the server socket.
5. `main()` then calls `gpio_put(PIN_HALT, 0)` and logs `core1 up, halt
   released`. Q2 turns OFF, R1 pulls /HALT high on the CoCo side, and
   the CPU runs.

After boot, `halt on` / `halt off` on the console assert and release
/HALT by hand; nothing in the firmware uses it for flow control yet
(`docs/ADDITIONAL_ROADMAP.md` §5 item 2 bounds such holds to a few
hundred µs).

Boundary: if firmware crashes before step 5, the CoCo will hang at
reset instead of running with a broken cart. That's the safer failure
mode.

## 9. Bring‑up (firmware side)

Bring-up is driven from the console commands listed in spec section 8
(`help`, `smoke`, `halt on/off`, `trace dump`, `rom pattern/load`,
`becker loop/bridge/native`, `dw mount`, etc.), not from bespoke test
firmware. The host-side stack each of these commands exercises — bus
tables, devices, DriveWire server, console — is verified by `ctest`'s
`test_stack`, `test_replay`, and `firmware/tools/dwtest.py` before any of
it runs on a Pico. The engine's own gate is `bus selftest` (§3.2.5); the
bench checks on a board are `firmware/TEST_PLAN.md` sections F-K. The
milestone tags `fw-0.1-blink` to `fw-1.0-native` were made on the CPU
loop; the current engine is `fw-1.5-pio-engine`.

## 9.1 v2 roadmap

These are not MVP blockers but tracked as next‑board items:

- **Q‑clock routing**: replacing U11 (74LVC245A, A0–A7 buffer) with a
  74LVC573 transparent D‑latch clocked by Q would give the Pico
  pre‑settled address bits ~140 ns earlier, relaxing PIO timing on
  CoCo 3 1.79 MHz operation. Pinout is NOT drop‑in; requires layout
  rework.
- **/SLENB in U15 decode**: currently U15 does not consider /SLENB,
  which is what the CoCo 3 GIME uses to tell the cart "I'm taking
  this address back for RAM." Replace U15 with a 4‑input combinatorial
  gate (74LVC1G332 or two LVC1G11s) to let GIME RAM/ROM toggle work
  without cart contention.
- **Pico → CoCo /CART FIRQ**: enables async events (e.g., "data
  ready" interrupts for Becker). The drive stage exists (Q4, GP33 on a
  Plus-W, JP5 1-2 on a Pico 2, `docs/hardware-design.md` §4.4a); the
  firmware uses it only for the autostart pulse after `/HALT` release.
- **Hardware‑latched ROM response table**: a 2 KB SRAM dual‑ported
  to both Pico SPI and a CoCo‑facing PIO could serve ROM with zero
  Pico CPU or DMA involvement, freeing all three cores for the
  DriveWire engine at higher speeds.

## 10. Development workflow

- Keep the Pico in the cart for normal dev; reflash over USB with
  `bootsel` on the console and a UF2 copy, so you don't have to unplug.
- USB exposes two CDC interfaces (`src/usb/usb_descriptors.c`): CDC0
  carries raw DriveWire bytes in bridge mode (the host's DriveWire
  server talks to CDC0 as if it were a serial cable); CDC1 is the
  console (`console_feed`/`console_exec`, section 9's commands and the
  boot-time `picoco.cfg` replay). No in-band escape sequences share a
  channel between the two.
- Bus activity is read on demand from the console (`trace dump`,
  `status`, `log dump`); nothing streams on its own.
- Add a `#define BUS_TRACE 1` gate around the diag ring so it can be
  compiled out for production.
- Run `cmake -DCMAKE_BUILD_TYPE=Release` for final ROM builds — the
  PIO programs are already assembled; only the ARM side benefits
  from `-O2`.

## 11. References

- Raspberry Pi Pico 2 datasheet (`datasheets.raspberrypi.com`): RP2350 GPIO, PIO instructions, DMA.
- Pico SDK: https://github.com/raspberrypi/pico-sdk
- pyDriveWire: https://github.com/n6il/pyDriveWire
- DriveWire 3 spec: http://www.cloud9tech.com/Cloud-9/Support/DriveWire%203%20Specification.pdf
- DriveWire 4 wiki: https://github.com/boisy/DriveWire/wiki/DriveWire-Specification
- Becker port description: CoCo3FPGA documentation
- HDB‑DOS + DriveWire build: http://www.cloud9tech.com
