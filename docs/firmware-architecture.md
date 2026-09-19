# PiCoCo — Firmware Architecture

Raspberry Pi Pico 2 (RP2350) firmware that turns the PiCoCo board
into a combined **ROM emulator** and **DriveWire Becker‑port
peripheral**.

This document explains how the firmware is structured. For the
hardware it runs on, see [`hardware-design.md`](hardware-design.md).

---

## 1. Goals

1. Respond to every `/CTS` read cycle (the $C000–$FEFF window; `rom.c`
   currently serves a 16 KB HDB‑DOS image out of idx `0x0000-0x3EFF` of
   it) in ≤ 1 bus cycle — data on D0–D7 before E falls.
2. Respond to Becker‑port reads on $FF41 (status) and $FF42 (data),
   and accept Becker‑port writes to $FF42 (data), implementing the
   DriveWire protocol.
3. Never drive the data bus when the cart is not selected — enforced
   by the hardware 3‑input AND gate (`U15`: `/OE = /CTS ∧ /SCS ∧ /E`)
   and asserted again in firmware for belt‑and‑suspenders.
4. Hold the CoCo's `/HALT` low during Pico boot until the PIO
   state machines are armed, so the CoCo's first HDB‑DOS `DOS`
   command can't race the Pico.
5. Keep the ARM cores free of per‑bus‑cycle work: PIO + DMA handle
   the bus, ARM handles DriveWire state and USB/UART diagnostics.

## 2. Toolchain

- **SDK**: Raspberry Pi Pico SDK ≥ 2.1.0 (Pico 2 support).
- **Compiler**: `arm-none-eabi-gcc` ≥ 13.
- **Build**: CMake + Ninja.
- **Flash**: picotool (USB MSC) or picoprobe + `openocd` (SWD).
- **Language**: C11 for performance‑critical paths; the DriveWire
  protocol layer can be C11 too, or ported from pyDriveWire piece by
  piece.

Repo layout, as built by the Plan A host-testable core
(`docs/superpowers/plans/2026-09-07-firmware-host-core.md`). Everything
under `host/` and most of `src/` is implemented and covered by `ctest`.
Entries marked **Plan B** do not exist yet; they land with the Pico
target (core1 bus loop, USB, flash storage):

```
firmware/
├── CMakeLists.txt          # -DPICOCO_HOST=ON builds host lib + tests + picoco-host
├── pico_sdk_import.cmake
├── boards/                 # Plan B: pico2_breadboard.h GPIO map
├── src/
│   ├── main.c              # Plan B target only today: Pico LED blink stub
│   ├── plat.h              # platform functions, resolved at link time
│   ├── plat_pico.c         # Plan B: RP2350 implementation of plat.h
│   ├── ring.h              # SPSC byte ring, shared by bus and becker
│   ├── log.h / log.c       # ring-buffered leveled logging
│   ├── bus/
│   │   ├── bus.h / bus.c   # response table, write ring, read hooks, trace ring, stats
│   │   └── bus_core1.c     # Plan B: the core1 loop; the only file that touches SIO GPIO
│   ├── dev/
│   │   ├── device.h / device.c   # device registry, write-event dispatch
│   │   ├── rom.h / rom.c         # ROM device
│   │   └── becker.h / becker.c   # Becker port device
│   ├── dw/
│   │   ├── dw_util.h / dw_util.c       # checksum, LSN pack/unpack
│   │   ├── dw_store.h                  # storage ops struct
│   │   ├── dw_store_posix.c            # host implementation (also used by picoco-host)
│   │   ├── dw_store_fatfs.c            # Plan B: Pico implementation
│   │   ├── dw_disk.h / dw_disk.c       # image format detection, LSN mapping
│   │   └── dw.h / dw_server.c          # DriveWire protocol state machine
│   ├── console/
│   │   ├── console.h / console.c  # line parser + commands
│   │   └── mode.h / mode.c        # mode switch (diag/loop/bridge/native) + pump
│   ├── fs_flash.c          # Plan B: FatFS diskio over the flash partition; USB MSC glue
│   ├── crash.c             # Plan B: hard fault + panic record in noinit RAM
│   └── usb_descriptors.c, tusb_config.h   # Plan B
├── host/
│   ├── plat_host.c         # POSIX implementation of plat.h
│   ├── picoco_host.c       # TCP 65504 server + stdin console around dw_server
│   └── sim_bus.h / sim_bus.c   # virtual CoCo for host-side tests
├── tests/
│   ├── test.h              # ~30-line TEST/ASSERT macros
│   ├── test_*.c            # one file per module, see section 10
│   └── fixtures/           # synthetic images, captured byte streams, trace dumps
├── tools/
│   ├── dwtest.py           # DriveWire client exerciser (TCP)
│   └── tracedump.py        # decodes `trace dump` output
└── roms/                   # user-supplied ROM images, gitignored
```

## 3. Runtime architecture

### 3.1 Core split

Reversed from the original proposal above: the bus service is the
time-critical job, so it gets its own core with nothing else running
on it.

- **core1** — the bus loop only (`src/bus/bus_core1.c`, Plan B).
  Flash-free: never executes from or reads flash, runs with interrupts
  disabled from SRAM (`BUS_HOT` section attribute), so the build sets
  `PICO_FLASH_ASSUME_CORE1_SAFE` (`firmware/CMakeLists.txt`; not
  `PICOCO_FLASH_ASSUME_CORE1_SAFE` — that name never existed in the
  build). It serves `bus_table[]` on reads,
  pushes write events into the write ring, and runs read hooks
  in-place (see section 5).
- **core0** — everything else: USB (2x CDC + MSC), the console, the
  DriveWire server, storage (flash/FatFS or the host POSIX
  equivalent), and device write dispatch (`device_dispatch_writes()`
  in `mode_pump()`). Core0 is free to touch flash; core1 never is.

Communication is the response table, write ring and read hooks in
`src/bus/bus.h` (SPSC, no locks) — not the Pico SDK `queue_t` mentioned
in the original proposal.

### 3.2 Pin map (Pico 2 header numbering)

| GPIO | Header pin | Role |
|------|-----------|------|
| GP0–GP7   | 1,2,4,5,6,7,9,10 | D0–D7 (bidi via U10) |
| GP8–GP21  | 11,12,14–27      | A0–A13 (input from U11+U12) |
| GP22      | 29               | /R/W (input from U12; also drives U10 DIR) |
| GP26      | 31               | **OE_BUS** — cart‑selected (U15 output) |
| GP27      | 32               | **HALT_GATE** — output; drives Q2 → /HALT |
| GP28      | 34               | AUDIO_PWM by default (JP3 1-2, v2.3.1); E (input from U13) if JP3 is cut to 2-3 |
| RUN       | 30               | CoCo /RESET (via R9 series; R10 pull‑up footprint is DNP by default) |
| VSYS      | 39               | +5V via D2 Schottky |

`OE_BUS` is asserted whenever the cart is selected — i.e., whenever
`/CTS` OR `/SCS` is low AND the E clock is high (E qualifies the
address‑valid window). Firmware disambiguates by **A13**:

- `OE_BUS` low with `A13=0` ⇒ ROM read in the full $C000–$FEFF `/CTS`
  window (`rom.c` covers idx `0x0000-0x3EFF`) — not $C000–$DFFF, a
  stale range this section used to claim.
- `OE_BUS` low with `A13=1` ⇒ Becker access in $FF40–$FF5F window.

### 3.2.1 Plus-W pin plan (board v2.3, not implemented)

The v2.3 board (`docs/superpowers/specs/2026-09-17-main-board-v2.3-design.md`
§3.2) adds 15 hidden underside pads to the `PiCoCo:Pico-Carrier`
footprint, wired only when a Waveshare RP2350B-Plus-W is soldered flat
instead of a Pico 2 — NC on a Pico 2 build. Firmware support for these
pins does not exist yet; this table records the wiring so a future
`boards/` header has something to build against.

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
| GP31 | OE_FW | JP2 alternate (2-3): firmware-driven U10 /OE |
| GP32 | NMI_DRV | Q3 gate (DNP stage — R15/R17 also DNP); also reachable from a Pico 2 via JP5 2-3 (shares header pin 34, see below) |
| GP33 | CART_DRV | Q4 gate (populated, v2.3.1); also reachable from a Pico 2 via JP5 1-2 |
| GP34 | AUDIO_PWM | sound output stage |
| GP35 | EXP_GP35 → J1 pin 1 | Plus-W only; on a Pico 2 this is where the module's own SWDIO pad lands instead (see the debug-pad note in `hardware-design.md` §7) |
| GP43, GP44, GP45 | EXP_GP43/44/45 → J1 pins 2-4 | Plus-W only |

GP24..GP30 are contiguous so one `gpio_in` read on a Plus-W captures
all seven at once, same trick as the header's A0..A13+/R/W word.

**JP5** (`docs/hardware-design.md` §4.4a) is the header-pin-34 solder
jumper for the Q3/Q4 drive stages above: pad 1 = `CART_DRV`, pad 2 =
`PICO_P34` (module header pin 34), pad 3 = `NMI_DRV`. Open by default,
and mutually exclusive with JP3 (both bridge onto header pin 34) — a
Pico 2 build picks at most one of audio (JP3), a firmware `/CART` pulse
(JP5 1-2), or a firmware `/NMI` drive (JP5 2-3, needs R15/R17 fitted).
No firmware for either JP5 position exists yet (item 12 in the roadmap
backlog, `docs/ADDITIONAL_ROADMAP.md` §6).

**Plus-W board header trap:** `PICO_DEFAULT_LED_PIN` is `GP25`. On a
Plus-W, pad-grid `GP25` is `SCS_BUF` — a U13 **output**, not an LED. A
future Plus-W `boards/` header that inherits the Pico 2 LED pin
unmodified would have firmware driving a buffer output as if it were an
LED; give the Plus-W header its own `PICO_DEFAULT_LED_PIN` (the Pico's
own onboard LED, used on a Pico 2 build) before writing that header.

### 3.3 PIO block usage (v2 bus engine)

The bus engine actually being built (Plan B) is a plain C loop on
core1 polling GPIO in a tight `for(;;)` (spec section 4.3), not PIO+DMA.
It is simpler to debug. `bus_core1.c` inserts ten `nop`s (67 ns at the
default 150 MHz — nothing in `firmware/src` calls
`set_sys_clock_khz`) between detecting `OE_BUS` low and reading the
address, then drives data; measured on the breadboard loop, total
OE-to-data is roughly **180-200 ns**, not the "about 70 ns" this
section used to claim, once the poll granularity, index computation
and GPIO writes are counted (see `docs/ADDITIONAL_ROADMAP.md` §6 items
5-6 for the follow-up: read `bus addr_resample` on the first real PCB
and drop the nops if it's zero, then raise the clock). Margin against
a 280 ns CoCo 3 window is real but thinner than the number below
suggests. The PIO/DMA design below is kept as the v2 engine, to be adopted only
if the hardware logic analyzer shows jitter in the C loop once it's
running on real hardware. Everything in 3.3, 3.4 and section 4 (PIO
programs) describes that fallback, not the current implementation. It
also assumes E reaches header pin 34 (GP28), which as of v2.3.1 requires
cutting JP3 to 2-3 — the default (1-2) routes `AUDIO_PWM` there instead
(§3.2).

PIO0 hosts three state machines:

| SM | Name | Role |
|----|------|------|
| SM0 | `bus_watcher` | Snapshots A0–A13 + /R/W once per E‑rising edge; used for bus trace and DMA dispatch. |
| SM1 | `data_driver` | Drives D0–D7 on cart‑selected read cycles (ROM or Becker). |
| SM2 | `write_capture` | Captures D0–D7 on cart‑selected write cycles (Becker writes to $FF42). |

SM3 is reserved. PIO1 and PIO2 are entirely free.

### 3.4 DMA channels

| Channel | Source → Dest | Purpose |
|---------|---------------|---------|
| DMA0 | PIO0 RX FIFO (SM0) → `bus_snapshot_ring[]` | Diagnostic trace (write‑only, no reconfigure). |
| DMA1 | `rom_byte` → PIO0 TX FIFO (SM1) | Push one ROM byte per cycle. Reloaded by DMA2. |
| DMA2 | `dma1_cfg_template[]` → DMA1 read_addr register (alias) | Reconfigure channel: rewrites DMA1's source pointer. Chains DMA2 back to DMA1 so the pair forms a self‑reloading pump. |
| DMA3 | PIO0 RX FIFO (SM2) → `becker_rx_ring[]` | Capture Becker write bytes. |

**ROM‑serve chain (DMA1 ↔ DMA2)**: ARM pre‑computes a 16 K‑entry
table `rom_ptrs[i] = &rom_image[i]` (actually a single 16 KB buffer;
the table entries are just `rom_image + i` for `i = 0..16383`). When
SM0 captures an address with `A13=0` and `/R/W=1`, it also triggers
DMA2, which copies `rom_ptrs[address_low13]` into DMA1.READ_ADDR,
then DMA1 fires once, pulling that byte through to SM1's TX FIFO.
Both DMA channels have `chain_to` set to keep the loop alive.

End‑to‑end latency from E rising to D0–D7 valid, measured in PIO
cycles at 150 MHz (≈ 6.67 ns/cycle):

| Stage | Cycles | ns |
|-------|-------:|---:|
| PIO SM0 capture (`IN PINS, 15 [3]` with settling) | 4 | 27 |
| DMA0 + DMA2 reconfigure (RP2350 DMA is IRQ‑free chain) | 6 | 40 |
| DMA1 fetch from SRAM → TX FIFO | 4 | 27 |
| SM1 `MOV PINS, OSR` | 1 | 7 |
| **Total** | **15** | **~100** |

Within the 300 ns CoCo 3 1.79 MHz data window — 3× margin. On CoCo
1/2 at 0.89 MHz the window is ~600 ns, so 6× margin.

If DMA latency proves unacceptable, fall back to **core0 IRQ** on
`pio_sm_get_blocking(SM0)` and dispatch from C. Cortex‑M33 IRQ
latency is ~60 ns; add table lookup and PIO push: ~200 ns total,
still inside the window.

## 4. PIO programs

The three SM programs live in `src/bus.pio`. Pseudocode / sketches:

### 4.1 `bus_watcher` (SM0) — gated on OE_BUS + read cycle

```
.program bus_watcher

    wait 0 pin 20            ; alignment: ensure we start with E low
                             ; (GP28 = pin index 20 from in_base 8)
.wrap_target
    wait 0 pin 18            ; WAIT for OE_BUS low (GP26 = index 18)
    wait 1 pin 20 [3]         ; E rising + 3 cycles settling (~20 ns)
    in pins, 15              ; base 8: A0..A13 + /R/W → ISR
                             ; autopush at 15 bits → RX FIFO
    wait 1 pin 18            ; wait for OE_BUS to release
.wrap
```

- `in_base` = 8, so `pin 20` refers to GP28 (E) and `pin 18` to GP26
  (OE_BUS).
- Output: 15‑bit word `(rw << 14) | (A13 << 13) | A12..A0` in RX FIFO
  exactly once per cart‑selected cycle. A13 is the ROM/Becker selector;
  bits A12..A0 index into the 16 KB ROM buffer when A13=0.
- The leading `wait 0 pin 20` outside `.wrap_target` runs once and
  guarantees that we align on a known E‑low phase before entering the
  loop. Without it, an SM started mid‑cycle could capture a garbled
  address before E has even risen.

### 4.2 `data_driver` (SM1) — tri‑state BEFORE E falls

```
.program data_driver

.wrap_target
    pull block               ; wait for byte from DMA1 (or core0 IRQ)
    out pindirs, 8           ; set D0..D7 as outputs
    mov pins, osr            ; drive D0..D7 with the low 8 bits
    wait 1 pin 18            ; wait for OE_BUS to release (cycle ends)
    mov osr, null
    out pindirs, 8           ; tri-state D0..D7 before next cycle
.wrap
```

- `out_base` = 0, `out_count` = 8; `in_base` = 8 (so `pin 18` = GP26).
- **Critical change from v1**: wait on `OE_BUS` deassertion rather
  than E falling. OE_BUS is already gated by E (hardware AND), so
  OE‑deassert is never earlier than E‑fall, and always earlier than
  the next cycle's address presentation. This closes a write‑cycle
  race where the old code (waiting on E falling) could leave D0–D7
  driven into an immediately‑following CoCo write.
- U10's `/OE` is managed by the hardware 3‑input AND (U15). The PIO
  drive window is only open when the CoCo selected us; otherwise the
  transceiver is tri‑state regardless of what the PIO does.

### 4.3 `write_capture` (SM2) — Becker writes only

```
.program write_capture

.wrap_target
    wait 0 pin 18            ; OE_BUS low = cart cycle starts
    jmp pin 13 enter         ; GP21 = A13; jmp if A13=1 (Becker range)
    wait 1 pin 18            ; else (ROM range): skip this cycle
    jmp .wrap_target
enter:
    wait 1 pin 20            ; E rising (address valid)
    in pins, 8               ; capture D0..D7 + A0..A4 into ISR
    wait 1 pin 18            ; wait for OE_BUS release
.wrap
```

- `in_base` = 0 for D0–D7 capture.
- The JMP PIN check on A13 skips ROM cycles (where writes shouldn't
  happen anyway; if they do, we ignore them). A full address filter
  for $FF41/$FF42 vs other /SCS aliases is done in core0 after pop.
- **Per‑cycle load protection**: core0 must drain the FIFO fast enough
  during heavy disk I/O at $FF48–$FF4F in MPI configs. At 1.79 MHz
  worst case we see one /SCS cycle every ~3 µs; core0 has no trouble
  popping at that rate.

## 5. Memory map

### 5.1 RAM layout (relevant sections)

| Address range | Size | Use |
|---------------|------|-----|
| `0x20000000 +  0x0000` | 16 KB | `rom_image[]` — current ROM content (DMA1 reads from here) |
| `0x20000000 +  0x4000` | 4 KB  | `becker_rx_ring[]` — from SM2 via DMA2 |
| `0x20000000 +  0x5000` | 4 KB  | `becker_tx_ring[]` — ARM → SM1 staging |
| `0x20000000 +  0x6000` | 4 KB  | `bus_snapshot_ring[]` — SM0 capture, diag only |
| rest | — | heap + stacks |

RP2350 has 512 KB of on‑chip SRAM; the above uses ≤ 32 KB. The flash
image holds multiple ROMs selectable at build time (e.g., HDB‑DOS,
NitrOS‑9 boot, Cloud9 ROMs).

### 5.2 Flash layout

- Bootloader at the reset vector (Pico SDK stage2).
- Application code + constant ROM images in flash (XIP).
- Optional: a small settings page at the end of flash for config
  (selected ROM slot, DriveWire server URL, etc.).

**Config, as built:** not a settings page of key/value fields.
`picoco.cfg` is a plain list of console commands (the same commands
from section 9's bring-up table), replayed line by line at boot by
`console_run_config()`; `save` writes the current state back out as
that command list. Same information, no separate config parser. On
the host it lives at `<dir>/picoco.cfg` next to the disk images; on
the Pico it's Plan B (a small file on the flash-backed filesystem in
section 7).

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

- `bus_table[16384]` holds the byte core1 drives for any cart-selected
  read, indexed by A0..A13. Devices call `bus_set_read(idx, byte)` to
  publish a value; core1 never computes anything, it just serves the
  table.
- `read_hooks[]` run on core1 immediately after a read cycle at a
  registered index completes. The becker device registers hooks on
  **both** 0x3F41 and 0x3F42 (not just 0x3F42) — see the single-writer
  rule below.
- A single-producer/single-consumer write-event ring carries
  `(idx, data)` pairs from core1 to core0; core0 drains it in its main
  loop (`device_dispatch_writes()`) and calls the owning device's
  `on_write`.

This lets the bus engine become the PIO+DMA design in section 3.3
later without any device code changing.

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
core1 writes the two Becker table entries, from the two read hooks.
Core0 (`becker_write` and the loopback pump) only pushes into
`to_coco` and never touches `bus_table[0x3F41]`/`[0x3F42]` directly.
A 16 KB `rom_load_mem` overwrites those two entries with ROM bytes; the
next `$FF41` poll restores them, so a ROM load while the CoCo is
running is briefly visible, which is acceptable since the ROM contents
change under it anyway. A byte core0 pushes becomes visible to the CoCo on its next
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

MVP: **USB CDC** — the Pico appears as a virtual serial port over
USB‑C. A host machine runs a standard DriveWire server (pyDriveWire,
DriveWire4 Java) that thinks it's talking to a real CoCo over a
serial cable. PiCoCo relays bytes between the CoCo (via Becker port)
and the host (via CDC).

Later: run the DriveWire server *on the Pico itself* (no host
needed) and back virtual disks with FatFS on an SD card over SPI, or
with files served from the Pico flash.

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
  WRITE, REWRITE, TIME, INIT, DWINIT, NOP, RESET1/2/3, TERM.
- **Consumed as stubs** (payload read and discarded, correct reply
  shape sent where pyDriveWire sends one, no real behaviour): SERREAD,
  SERGETSTAT, SERINIT, SERTERM, SERWRITE, SERWRITEM, SERREADM,
  SERSETSTAT (including the 26-byte COMST extension), FASTWRITE,
  PRINT, PRINTFLUSH, NAMEOBJ_MOUNT/CREATE. These exist so a real CoCo
  client doesn't desync waiting for bytes that never come; they carry
  no virtual-serial or named-object behaviour yet (`// ponytail:` in
  `dw_server.c` names that ceiling — real replies wait on networking).
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
both `ctest` and `picoco-host`) today and a FatFS implementation
(`dw_store_fatfs.c`) as Plan B for the Pico's flash partition.

### 7.3 Host build and test tools

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
9), all exercised by the host tests before Plan B puts them on real
hardware: the always-on bus trace ring (section 6.1's `bus.c`, frozen
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

See §3.4 for a per‑stage cycle accounting of the DMA chain. With
total latency ≈ 100 ns and a 300 ns CoCo 3 data window, there is
3× slack. Q clock is **not** routed to the Pico — doing so would
require another GPIO we don't have. Instead, E rising plus 3 PIO
settling cycles gives us a solid address sample.

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
3. Pico boots (~30–50 ms typical), PIO programs load, DMA channels arm.
4. `rom_armed()` is called at the end of bus setup. It explicitly
   configures GP27 as an output and drives it LOW. Q2 turns OFF,
   R1 pulls /HALT high on the CoCo side, and the CPU runs.

```c
void halt_init(void) {
    gpio_init(27);
    gpio_set_dir(27, GPIO_OUT);
    gpio_put(27, 1);              // keep /HALT asserted
}

void halt_release(void) {
    gpio_put(27, 0);              // release /HALT
}

void halt_assert(void) {
    gpio_put(27, 1);              // reassert /HALT (flow control)
}
```

Call `halt_init()` in the earliest reachable C code (before SDK
runtime init if practical), then `halt_release()` once `bus_watcher`
is running and DMA chains are set up. After that, firmware can use
`halt_assert()` / `halt_release()` as a DriveWire flow‑control tool —
e.g., to pause the CoCo during a long SD‑card sector fetch in
Phase 3.

Boundary: if firmware crashes with `halt_release()` not yet called,
the CoCo will hang at reset instead of running with a broken cart.
That's the safer failure mode.

## 9. Bring‑up plan (firmware side)

Milestones `fw-0.1` onward are driven from the console commands listed
in spec section 8 (`help`, `smoke`, `halt on/off`, `trace dump`, `rom
pattern/load`, `becker loop/bridge/native`, `dw mount`, etc.), not from
bespoke test firmware per milestone. The host-side stack each of these
commands exercises — bus tables, devices, DriveWire server, console —
is already verified by `ctest`'s `test_stack`, `test_replay`, and
`firmware/tools/dwtest.py` before any of it runs on a Pico. Each
milestone is a firmware git tag.

| Tag | Description |
|-----|-------------|
| `fw-0.1-blink` | Toggle Pico LED. Verify SDK + flash path. |
| `fw-0.2-gpio-smoke` | Toggle all 26 GPIO at 10 Hz. Verify shifter directions and power budget on the board before plugging into a CoCo. |
| `fw-0.3-halt-ctrl` | Implement `halt_init()` + `halt_release()`. Verify on a CoCo that /HALT is held low during Pico boot (oscilloscope TP + CoCo doesn't run until released). |
| `fw-0.4-bus-capture` | SM0 only. Capture bus snapshots to `bus_snapshot_ring` gated on OE_BUS and dump to USB CDC. Plug into a CoCo, verify cart cycles fire on $C000/$FF4x reads. |
| `fw-0.5-rom-static` | Add SM1 + DMA1/DMA2 ROM chain. Serve a 16 KB repeating pattern over /CTS. `PEEK` from BASIC returns the pattern. |
| `fw-0.6-rom-hdbdos` | Flash a real HDB‑DOS+DW ROM as `rom_image[]`. Power‑cycle → `DOS` from BASIC should enter HDB‑DOS. |
| `fw-0.7-becker-loop` | Add SM2 + Becker register. $FF41 always returns `$02`; $FF42 echoes back. Verify from BASIC. |
| `fw-0.8-bridge` | CDC ↔ Becker bridge. Run pyDriveWire on host; `DW DIR` from HDB‑DOS shows virtual drive. |
| `fw-0.9-boot` | `BOOT` from HDB‑DOS starts loading a DECB / NitrOS‑9 image over DriveWire. |
| `fw-1.0-native`  | Optional: DriveWire server runs on Pico, SD card backing store. |

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
  ready" interrupts for Becker). Needs another freed GPIO — only
  feasible by adding an I/O expander or a small CPLD for address
  decoding.
- **Hardware‑latched ROM response table**: a 2 KB SRAM dual‑ported
  to both Pico SPI and a CoCo‑facing PIO could serve ROM with zero
  Pico CPU or DMA involvement, freeing all three cores for the
  DriveWire engine at higher speeds.

## 10. Development workflow

- Keep the Pico in the cart for normal dev; flash over SWD with
  picoprobe so you don't have to unplug.
- USB exposes two CDC interfaces (Plan B, `usb_descriptors.c`): CDC0
  carries raw DriveWire bytes in bridge mode (the host's DriveWire
  server talks to CDC0 as if it were a serial cable); CDC1 is the
  console (`console_feed`/`console_exec`, section 9's commands and the
  boot-time `picoco.cfg` replay). No in-band escape sequences share a
  channel between the two.
- `core0` keeps a stream of bus‑snapshot lines open on USB CDC for
  `picocom /dev/ttyACM1` debugging.
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
