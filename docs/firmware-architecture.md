# PiCoCo — Firmware Architecture

Raspberry Pi Pico 2 (RP2350) firmware that turns the PiCoCo board
into a combined **ROM emulator** and **DriveWire Becker‑port
peripheral**.

This document explains how the firmware is structured. For the
hardware it runs on, see [`hardware-design.md`](hardware-design.md).

---

## 1. Goals

1. Respond to every `/CTS` read cycle ($C000–$DFFF, the HDB‑DOS
   16 KB slot) in ≤ 1 bus cycle — data on D0–D7 before E falls —
   using a stored 16 KB ROM image.
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

Repo layout (proposed):

```
firmware/
├── CMakeLists.txt
├── pico_sdk_import.cmake
├── src/
│   ├── main.c              # core0/core1 entry
│   ├── bus.c / bus.h       # PIO programs + setup for D/A/E
│   ├── bus.pio             # PIO assembly (bus watcher, data driver, write capture)
│   ├── rom.c / rom.h       # ROM image store, /CTS read handler
│   ├── becker.c / becker.h # $FF41/$FF42 register emulation + FIFO
│   ├── drivewire.c / .h    # DriveWire V3/V4 protocol state machine
│   ├── usb_cdc.c / .h      # USB CDC for DriveWire server traffic + diag
│   └── led.c / .h          # Heartbeat, status LEDs
├── roms/
│   └── hdbdos_dw.bin       # 16 KB HDB‑DOS + DriveWire build (user‑supplied)
└── tools/
    ├── build.sh            # one‑shot build
    └── flash.sh            # SWD flash via picoprobe
```

## 3. Runtime architecture

### 3.1 Core split

- **core0** — PIO/DMA orchestration, Becker port read/write service,
  USB CDC pump.
- **core1** — DriveWire protocol engine (parses commands from the
  host‑side DriveWire server, maintains virtual drive state).

Inter‑core comm via Pico SDK's `queue_t` (lock‑free MPSC).

### 3.2 Pin map (Pico 2 header numbering)

| GPIO | Header pin | Role |
|------|-----------|------|
| GP0–GP7   | 1,2,4,5,6,7,9,10 | D0–D7 (bidi via U10) |
| GP8–GP21  | 11,12,14–27      | A0–A13 (input from U11+U12) |
| GP22      | 29               | /R/W (input from U12; also drives U10 DIR) |
| GP26      | 31               | **OE_BUS** — cart‑selected (U15 output) |
| GP27      | 32               | **HALT_GATE** — output; drives Q2 → /HALT |
| GP28      | 34               | E (input from U13) |
| RUN       | 30               | CoCo /RESET (via R9 + R10 pull‑up) |
| VSYS      | 39               | +5V via D2 Schottky |

`OE_BUS` is asserted whenever the cart is selected — i.e., whenever
`/CTS` OR `/SCS` is low AND the E clock is high (E qualifies the
address‑valid window). Firmware disambiguates by **A13**:

- `OE_BUS` low with `A13=0` ⇒ ROM read in $C000–$DFFF window.
- `OE_BUS` low with `A13=1` ⇒ Becker access in $FF40–$FF5F window.

### 3.3 PIO block usage

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

## 6. Becker port protocol

The Becker port convention, per CoCo3FPGA / XRoar and used by
DriveWire client code:

| Addr | R/W | Meaning |
|------|-----|---------|
| `$FF41` | R | Status byte. Only bit 1 is defined: **1 = byte available to read**. |
| `$FF41` | W | Ignored (write‑through to a "any byte" to trigger something, but per spec, writing does nothing). |
| `$FF42` | R | Next byte from server. Always safe to read; if no byte available, implementation‑defined (we return 0xFF). |
| `$FF42` | W | Byte to server. Pushed into the outgoing queue. |

### 6.1 State

```c
struct becker_state {
    ring_u8 rx_from_server;  // bytes the CoCo will read via $FF42
    ring_u8 tx_to_server;    // bytes the CoCo has written to $FF42
};
```

### 6.2 Read service (hot path)

For Becker reads (`A13 = 1` when `OE_BUS` asserts), core0's IRQ
handler sees the bus_watcher snapshot and dispatches:

```c
#define SNAP_RW   (1u << 14)
#define SNAP_A13  (1u << 13)
#define BECKER_MASK 0x001F   // A0..A4 within the /SCS window

void on_bus_snapshot(uint16_t snap) {
    if (!(snap & SNAP_A13)) return;       // ROM cycle, handled by DMA chain
    if (!(snap & SNAP_RW)) return;        // write, handled by SM2
    switch (snap & BECKER_MASK) {
    case 0x01:  // $FF41 status
        pio_sm_put(pio0, SM_DATA,
                   ring_empty(&rx_from_server) ? 0x00 : 0x02);
        break;
    case 0x02:  // $FF42 data
        pio_sm_put(pio0, SM_DATA,
                   ring_empty(&rx_from_server) ? 0xFF
                                               : ring_pop(&rx_from_server));
        break;
    default:
        // $FF40, $FF43..$FF5F — not ours; don't drive data bus.
        // U10 /OE is hardware-gated so no contention either way; we
        // just skip the pio_sm_put.
        break;
    }
}
```

Address filter matches exactly the 32‑byte /SCS window (A0..A4), so
PiCoCo never aliases $FF41 onto $FF43/$FF45/etc. the way legacy
Becker carts sometimes did.

A future optimization pre‑computes a 32‑entry lookup table of
response bytes indexed by A0..A4 and lets a second DMA chain serve
Becker reads without core0 IRQ, matching the ROM path's pattern.

### 6.3 Write service

`write_capture` (§4.3) pushes a 16‑bit word per cart write cycle:
`(A4..A0) << 8 | D7..D0`. ARM pops and filters:

```c
void on_write_capture(uint16_t word) {
    uint8_t addr_lo = (word >> 8) & 0x1F;
    uint8_t data = word & 0xFF;
    if (addr_lo == 0x02) {          // $FF42 only
        ring_push(&tx_to_server, data);
    }
    // Other addresses in /SCS window are not ours; ignore.
}
```

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

### 7.2 Protocol state machine (core1)

DriveWire V3 and V4 are byte‑oriented request/response protocols.
core1 implements the standard op‑code handlers:

| Op | Meaning | Handler |
|----|---------|---------|
| `OP_NOP` (`0x00`)        | No‑op                       | ack |
| `OP_TIME` (`0x23`)       | Get current time            | respond with 6‑byte time |
| `OP_INIT` (`0x49`, `0x5a`) | Initialize                  | ack |
| `OP_READ` (`0x52`)       | Read 256‑byte sector        | pull sector from VFS |
| `OP_READEX` (`0xd2`)     | Read with checksum          | pull sector + CRC |
| `OP_WRITE` (`0x57`)      | Write 256‑byte sector       | push sector to VFS |
| `OP_REWRITE` (`0xd7`)    | Write with retry handshake  | push + ack |
| `OP_GETSTAT` / `OP_SETSTAT` | Status queries           | stubs |
| `OP_RESET1/2/3`          | Protocol reset              | re‑sync |

In MVP the VFS just proxies every op to the host over CDC and
forwards the response — so all file handling lives in the
host‑side pyDriveWire. In v2 the Pico can host the VFS itself.

### 7.3 pyDriveWire port path

pyDriveWire is pure Python, modular by protocol version. A realistic
port path:

1. **Phase 1** — Pico is a dumb Becker↔CDC bridge. Host runs
   pyDriveWire unchanged; just point it at the Pico's CDC port.
2. **Phase 2** — port pyDriveWire's `pyDwProtocolHandler` to C,
   running on core1. Transport is still CDC (host provides files).
3. **Phase 3** — add SPI SD card + FatFS, move VFS to Pico, remove
   host dependency.

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
gate is at pin 1 and is pulled up to +3V3 through R7 (100 kΩ). The
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

Each milestone is a firmware git tag.

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
