# Plus-W bus engine — design

**Status:** design approved in chat 2026-09-27, spec for review.
**Deliverable:** firmware changes under `firmware/` that let a Waveshare
RP2350B-Plus-W build decode cart addresses itself, plus core1 write hooks,
banked ROM, a firmware `/CART` pulse, an on-chip fake 6809 for self-test,
and a TCP Becker listener in the host build. No sound in this spec; it is
the prerequisite for `2026-09-27-sound-and-midi-design.md`.

## 1. Goals

1. A Plus-W build with JP2 in the 2-3 position responds to CoCo cycles at
   addresses outside `/CTS` and `/SCS`: initially `$FF6E/6F` (MIDI Pak)
   and `$FF7D/7E` (Speech/Sound Pak). The Pico 2 build keeps today's loop,
   untouched.
2. Core1 can run a small hook on a write to a chosen address, before the
   event reaches core0, so a write can change what the very next read
   returns (bank switch, busy flag).
3. ROM images larger than 16 KB load as 16 KB banks selected by a write to
   `$FF40`, up to 128 KB (Games Master Cartridge format, MAME `coco_gmc`).
4. A firmware-pulsed `/CART` on the Plus-W (GP33, Q4 stage) for autostart
   Program Paks. Closes `docs/ADDITIONAL_ROADMAP.md` §6 item 12.
5. Automated tests that do not need a CoCo: pure decode vectors on the
   host, an on-chip fake 6809 driving the real core1 loop at 6809 timing,
   and XRoar talking to the host build over its Becker TCP port.

Out of scope: any sound, MIDI or new device. Those are the next spec.

## 2. Decisions

| Decision | Choice | Why |
|---|---|---|
| Where decode runs | core1, board-conditional (`#if PICOCO_BOARD_PLUSW`) | Only the Plus-W has A14/A15, CTS/SCS/E/Q on the pad grid |
| When to sample the address | on Q rising, while E is low | Address is valid before E; decode finishes before the data window opens, so the read path gains margin over today's OE-triggered loop |
| Who drives U10 `/OE` | GP31 `OE_FW`, low while E is high and the cycle is selected | Mirrors what U15 does in hardware, plus the firmware-only address set |
| Write hooks | fixed table, `BUS_MAX_HOOKS` entries, called on core1 after the data byte is captured, before `bus_on_write` queues it | Same shape as the existing read hooks; passes the flash-free check the same way |
| Bank storage | SRAM, 16 KB per bank, up to 8 banks | core1 must never touch flash; 128 KB fits the RP2350's 520 KB with room for the sound spec |
| Bank switch mechanism | core1 read path indexes `rom_base[idx]` for `idx < 0x3F00`, else `bus_table[idx]`; the `$FF40` write hook swaps `rom_base` | One compare on the hot path; no 16 KB copy inside a bus cycle |
| Single-writer rule | kept. Where core1 and core0 both influence a readable byte, each owns its own variable and the read hook composes the byte | Existing rule (`becker.c`), extended rather than relaxed |
| Fake 6809 | PIO1 program driving address/control pins from a cycle list, core0 fed; data pins stay SIO | Exercises the real BUS_HOT loop at real timing with no extra hardware |
| Host Becker transport | TCP listener in `picoco_host`, XRoar's `-becker` protocol | Zero-hardware end-to-end test of every DriveWire feature |

## 3. Architecture

```
                 Plus-W core1 loop (new, board-conditional)
  ┌────────────────────────────────────────────────────────────────┐
  │ wait E low ─► wait Q high ─► sample A0-15, R/W, CTS, SCS        │
  │   ─► sel = !CTS || !SCS || fw_decode(addr)                      │
  │   ─► wait E high ─► if sel: OE_FW low                           │
  │        read : drive D0-7 from rom_base/bus_table, hooks_read    │
  │        write: sample D0-7 at E falling, hooks_write, bus_on_write│
  │   ─► E low: OE_FW high, release D0-7                            │
  └────────────────────────────────────────────────────────────────┘
  Pico 2 core1 loop: unchanged (OE_BUS-triggered, hardware-decoded).
```

`fw_decode(addr)` is a pure function over the full 16-bit address returning
true for addresses in a small enabled set. The set is a bitmap over
`$FF60-$FF7F` (32 bits) that core0 configures at boot from device
registrations; no other page is ever firmware-decoded. `$FF40-$FF5F` and
`$C000-$FEFF` remain hardware-selected by `/SCS` and `/CTS`.

## 4. Firmware

### 4.1 Board layer (`firmware/boards/plusw.h`)

Add pin macros for the pad-grid signals already listed there as capture-only:
`PIN_CTS 24`, `PIN_SCS 25`, `PIN_E 26`, `PIN_Q 27`, `PIN_A14 29`,
`PIN_A15 30`, `PIN_OE_FW 31`. `PIN_CART_DRV 33` exists. Define
`PICOCO_BOARD_PLUSW 1`. The Pico 2 header defines none of these; code that
needs them is inside `#if PICOCO_BOARD_PLUSW`.

Init on the Plus-W: `OE_FW` output, driven high (buffer disabled) before
anything else; `CART_DRV` output low. All pad-grid inputs remain pulled
up as today.

### 4.2 core1 loop (`firmware/src/bus/bus_core1.c`)

Two loops selected at compile time:

- **Pico 2:** the existing loop, byte for byte, except the read path uses
  `rom_base` (4.4) and the write path calls the write-hook table (4.3).
- **Plus-W:** the loop in §3. Everything it calls is `BUS_HOT` or
  `static inline`. Interrupts stay disabled on core1. `fw_decode` is a
  bitmap lookup: `(addr & 0xFFE0) == 0xFF60 && (fw_mask >> (addr & 0x1F)) & 1`.
  `fw_mask` is a `uint32_t` written by core0 only. A 32-bit store is
  atomic, so a device enabled from the console after core1 is running
  takes effect on the next cycle with no handshake.

The Plus-W loop also honours `bus_drive` exactly as today: when false it
never drives D0-7 nor enables U10 outward; writes still enable U10 inward so
capture-only bring-up records write data.

Timing budget: with the address decoded during E low, the work between E
rising and data valid is one branch, one table load and one `gpio_put`
group, well inside the ~280 ns CoCo 3 window the docs measure against.
The self-test in 4.6 measures it.

### 4.3 Write hooks (`firmware/src/bus/bus.c`, `bus.h`)

```c
int bus_add_write_hook(uint16_t idx, void (*fn)(uint8_t data));  /* 0 ok, -1 full */
```

Same fixed table shape as `bus_add_read_hook`, `BUS_MAX_HOOKS` entries,
matched by `idx` with a linear scan (four entries; a scan is cheaper than a
map). Core1 calls the matching hook right after capturing the data byte
and before `bus_on_write` queues the event, so core0 still sees every
write. Hooks are `BUS_HOT`. `firmware/tools/check_core1_flash_free.py`
learns the new registration function name so hooks are followed by the
flash-free check the way read hooks are.

### 4.4 Banked ROM (`firmware/src/dev/rom.c`, `bus_core1.c`)

- `uint8_t *volatile rom_base` in SRAM, defaulting to `bus_table`. The read
  path takes `idx < 0x3F00 ? rom_base[idx] : bus_table[idx]`.
- `rom_load_file` reads the image size. 16 KB or smaller: today's
  behaviour (copy into `bus_table`, `rom_base = bus_table`). Larger: must
  be a multiple of 16 KB and at most 128 KB, else refuse with a message.
  Banks are copied into a static SRAM array of 8 x 16 KB (128 KB,
  allocated only in the Pico build's `.bss`; the host build uses a smaller
  fixture), `rom_base = bank[0]`, and a write hook on `0x3F40` does
  `rom_base = bank[data & (nbanks - 1)]` (bank counts are powers of two
  in every known GMC image; a non-power-of-two count is refused at load).
- Registering the `$FF40` hook is done by the ROM loader, not by a device,
  so a banked image works with no other configuration. `rom off` and a
  plain load remove it (hook table entry cleared; `bus_add_write_hook`
  gains a matching remove, or the hook checks `nbanks > 1`, whichever is
  shorter in code).
- The current loader's `$C000-$FEFF` coverage (idx `0x0000-0x3EFF`) is
  unchanged; bank images are 16 KB but the top 256 bytes are I/O on the
  CoCo and never read as ROM.

### 4.5 Firmware `/CART` pulse (Plus-W only)

As built: right after the existing `/HALT` release, core0 toggles
`CART_DRV` at about 500 Hz for 500 ms when a ROM is loaded and is *not*
a DOS ROM (no `DK` signature at its first two bytes). When the ROM has
the `DK` signature or no ROM is loaded, `CART_DRV` stays low. A console
`cart on|off|auto` override (default `auto`) forces the pulse on or off
regardless of the loaded ROM's signature, and is saved by `save`. A
Pico 2 has no pin for this; `cart on`/`cart off` are refused there and
the README points at JP5.

This pulses after the hold, not during it, unlike the original plan:
Color BASIC clears the PIA's cartridge-interrupt flag during its own
init, which runs after `/HALT` releases, so a pulse asserted only during
the hold would be cleared before BASIC ever samples it.

### 4.6 On-chip fake 6809 (`firmware/src/bus/fake6809.c`, `fake6809.pio`)

Purpose: run the real core1 loop against real 6809 timing with no CoCo.

- A PIO1 state machine owns, during a self-test only, the address pins and
  the control pins the loop watches (Pico 2: `OE_BUS`; Plus-W: `E`, `Q`,
  `CTS`, `SCS`, `A14`, `A15`). Their function select is switched to PIO for
  the test and back to SIO afterwards. D0-7 stay SIO.
- Each cycle is one 32-bit word from core0: address, R/W, select flags.
  The PIO emits the 6809 sequence at 1.117 µs per cycle: address valid,
  Q rise, E rise, (write: data must be valid), E fall. For write cycles
  core0 puts the data on D0-7 as SIO outputs before pushing the word; for
  read cycles D0-7 are inputs and the PIO `IN`s them at E falling and
  pushes the byte back to core0 through its RX FIFO.
- On the Pico 2 the PIO drives `OE_BUS` low from E rise to E fall for
  selected cycles, reproducing U15. On the Plus-W it drives `E`, `Q`, and
  `CTS`/`SCS` low for hardware-selected cycles, leaving `OE_FW` to core1.
- `bus selftest` runs a built-in cycle list (reads across the ROM window,
  writes to `$FF42`, a bank switch followed by reads, firmware-decoded
  reads and writes on the Plus-W) and asserts the data core1 drove and the
  events it queued. `bus selftest trace <file>` replays a cycle list from
  the FAT volume, the same format the MAME trace scripts in the sound spec
  emit. Output: cycles run, mismatches, ring overruns, and the measured
  E-rise-to-data-valid time (PIO samples D0-7 at fixed offsets after E
  rise and reports the first offset at which the byte was correct).
- Refused when core1 has seen real bus cycles in the last second, so it
  cannot run inside a CoCo. Safe unplugged on the PCB: the +3.3 V rail is
  off without cart +5 V and the LVC245A/LVC00 outputs are high-impedance
  unpowered (Ioff partial-power-down).

### 4.7 Host build: TCP Becker listener (already exists)

`firmware/host/picoco_host.c` already wraps the DriveWire server in a
Becker-style TCP listener on port 65504, and `firmware/README.md` §"Manual
check: XRoar" documents booting XRoar from it. Nothing to build here; the
sound spec's DriveWire MIDI test uses it as is.

## 5. Error handling

- A banked image with a size that is not a multiple of 16 KB, more than
  128 KB, or a non-power-of-two bank count: `rom load` refuses, names the
  rule, leaves the previous ROM in place.
- Write-hook table full: the registration returns -1 and the caller logs
  it; nothing is silently dropped.
- `bus selftest` while live cycles are being seen: refused.
- `cart on` on a Pico 2: refused with "needs Plus-W (JP5 on a Pico 2)".
- Per-cycle conditions stay counters in `bus_stats` (add `fw_selected`,
  `hw_selected`, `hooks_run`), shown by `bus status`.

## 6. Testing

Host (`ctest`, new suites in `firmware/tests/`):

- `test_decode`: `fw_decode` over address vectors for several masks; the
  Pico 2 build has no decode so the vector set asserts the mask is
  ignored there.
- `test_rom_banked`: load fixtures of 16, 32, 64, 128 KB and a bad 24 KB;
  `sim_write(0xFF40, n)` then `sim_read(0xC000)` sees bank n; hook is gone
  after `rom off`.
- `test_hooks`: write hook fires before the event is queued; table-full
  returns -1.
- `test_becker_tcp`: connect to the listener, run the DriveWire
  `dwtest.py` sequence through it.

On-device (`bus selftest`, run by `firmware/tools/bench.py` over the USB
console; a ctest target that skips when no Pico is attached):

- Read data correct across the whole window; bank switch visible on the
  next cycle; write ring receives every write; zero overruns; measured
  response time printed and compared against a threshold (initially the
  documented 200 ns, tightened once measured).

XRoar: boot HDB-DOS from the host over TCP, `DIR`, `LOADM` a file. A
banked GMC image cannot be tested this way (XRoar's GMC is its own
emulation), the fake 6809 covers it.

Bench (CoCo 3, hand-run): a Plus-W board with JP2 2-3 boots HDB-DOS
(hardware-selected cycles still work through the firmware `/OE`); `bus
status` shows firmware-selected cycles when a small ML routine touches
`$FF7E`; a GMC image switches banks (BASIC `PEEK` at `$C000` does not reach
the cart on a CoCo 3, use the game itself or an ML probe).

## 7. Order of work

1. Write hooks + `rom_base` on the Pico 2 loop, host tests, flash-free
   check update.
2. Banked ROM loader, host tests.
3. Fake 6809 on the Pico 2 (OE_BUS driven), `bus selftest`, `bench.py`.
4. Plus-W loop, `fw_decode`, board macros, fake 6809 Plus-W variant.
5. `/CART` pulse.
6. Docs; confirm the existing XRoar-over-TCP check still passes.

## 8. Docs to update

`docs/firmware-architecture.md` (Plus-W loop, hooks, banked ROM,
self-test), `docs/hardware-design.md` §4.4a/§4.5 pointers, `CLAUDE.md`
hard rules (write hooks are BUS_HOT; `fw_mask` written before core1
launch only; `rom_base` swapped only from the `$FF40` hook), README
(banked ROMs, `cart` command, self-test), `ADDITIONAL_ROADMAP.md` item 12
closed.

## 9. References

- MAME `src/devices/bus/coco/coco_gmc.cpp`, `coco_pak.cpp` (bank size
  0x4000, region 0x20000).
- `docs/RP2350B_IDEAS.md` §1 (firmware `/OE`), §12.1 (address map).
- `firmware/src/bus/bus.h` (hook API shape), `firmware/src/dev/becker.c`
  (single-writer pattern).
- XRoar Becker port: `xroar -becker -becker-ip 127.0.0.1 -becker-port 65504`.
