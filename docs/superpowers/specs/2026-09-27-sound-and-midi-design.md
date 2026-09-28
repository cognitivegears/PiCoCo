# Sound and MIDI — design

**Status:** design approved in chat 2026-09-27, spec for review.
**Depends on:** `2026-09-27-plusw-bus-engine-design.md` (firmware address
decode, core1 write hooks, banked ROM, fake 6809).
**Deliverable:** firmware under `firmware/` that emulates the Tandy
Speech/Sound Cartridge (26-3144) and the Games Master Cartridge with audio
out of cart pin 35, and a USB MIDI device fed by an emulated Rulaford MIDI
Pak and by DriveWire virtual channel 14.

## 1. Goals

Let people run software written for two carts that are now hard to buy:

| Cart | Software that must work | Build |
|---|---|---|
| Speech/Sound Cartridge | SoundMaster 2.0, Gold Runner I/II, Pitfall II, F-16 Assault, Galagon, Gauntlet, Space Wrek, Wizard's Castle, Adventure in Mythology | Plus-W |
| Games Master Cartridge | Fahrfall, Dunjunz GME, Blockdown, Treasure Island Defence | Pico 2 and Plus-W |
| Rulaford MIDI Pak | Lyra, UltiMusE III, CoCo MIDI Pro, via USB MIDI to a PC | Plus-W |
| DriveWire MIDI | NitrOS-9 `/midi` (DW4 channel 14), via USB MIDI | both |

Fidelity for the SSC is the real TMS7040 firmware running in an
interpreter, as MAME does. Output is mono into cart pin 35 through the
populated RC stage; the CoCo mixes it into its own sound.

Out of scope, all of which reuse the pipeline later: CoCo PSG,
Orchestra-90, Symphony 12, X-SID, Stereo Composer, stereo output or a
jack, a MIDI DIN jack, on-Pico General MIDI synthesis, chip-to-MIDI
translation, DriveWire 4 synth profiles and instrument remapping.

## 2. Decisions

| Decision | Choice | Why |
|---|---|---|
| Output | PWM, 10-bit, one slice, DMA-fed at 32 kHz, 2 x 256-sample blocks | Carrier near 146 kHz at 150 MHz sits far above the 18 kHz filter; 8-16 ms latency; no new hardware |
| Where synthesis runs | core0, in the DMA block-complete interrupt | core1 stays a flash-free bus loop; core0's main loop has no tick and stalls |
| Register-write timing | applied at the next block boundary as core0 drains the write ring | ≤ 8 ms jitter is inaudible for PSG-class chips and the SSC's command stream |
| Flash-erase stalls | accepted; measured by a soak test; `ponytail:` note names deeper buffers as the upgrade | GMC games and the SSC don't write flash mid-play |
| SSC fidelity | TMS7000 interpreter running the 4 KB TMS7040 ROM; user-supplied ROMs | MAME-proven compatibility; no protocol reverse engineering |
| SSC busy flag | composed on core1 from a write counter (core1) and an ack counter plus shadows (core0) | busy asserts on the next cycle after a write regardless of core0 latency; every variable has one writer |
| GMC activation | implied by loading a ROM larger than 16 KB | nothing to configure or forget |
| MIDI transport | USB MIDI class added to the existing TinyUSB composite (2 x CDC + MSC) | zero hardware, works on the shipping board |
| DW MIDI | channel 14 bytes routed to the same USB MIDI sink | matches DW4 (`VSerial_MIDIPort` default 14) |
| Chip cores | SN76489 and AY-3-8913 from PicoGUS's PSG module or emu2149, SP0256 from MAME/jzintv, pending a licence check against CERN-OHL-S + MIT | small, proven; licence decided in planning, not assumed |

## 3. Architecture

```
 CoCo write  ─► core1 ─► write hook (SSC latch / GMC bank) ─► write ring ─► core0 main loop
                                                                              │ device_dispatch_writes
                                                          ┌───────────────────┼─────────────────┐
                                                          ▼                   ▼                 ▼
                                                     gmc: SN76489       ssc: host latch     midi: 6850 TX ──► USB MIDI
                                                          │             (TMS7000 + AY + SP0256)       ▲
                                                          └────────┬──────────┘        DW ch 14 ──────┘
                                                                   ▼
                                       audio block IRQ: mixer sums sources ─► PWM buffer ─► DMA ─► PWM slice ─► GP28/GP34
 CoCo read   ◄─ core1 read hook composes $FF7E status / pops 6850 RX ring ◄─ core0 shadows, USB MIDI IN
```

Pure-C sources (chip cores, TMS7000, mixer, 6850 model, SSC glue) live in
`CORE_SRC` and build on the host. Hardware (PWM, DMA, IRQ, TinyUSB MIDI) is
Pico-only in `firmware/src/audio/audio_pico.c` and `firmware/src/usb/`.

## 4. Firmware

### 4.1 Audio pipeline (`firmware/src/audio/`)

- `audio.h`: `audio_init()`, `audio_set_enabled(bool)`, `audio_set_volume(shift)`,
  `audio_add_source(render_fn)`, `audio_render_block(int16_t *out, n)`
  (pure, host-testable), `audio_stats` {underruns, blocks, max_block_us}.
- `audio_pico.c`: PWM slice on `PIN_AUDIO` (GP28 Pico 2, GP34 Plus-W;
  both board headers gain the macro), wrap 1023, DMA channel paced by a DMA
  timer to 32 000 Hz writing the slice's CC register from two 256-sample
  `uint16_t` blocks. Block-complete IRQ on core0 calls
  `audio_render_block` into the freed block. A late refill (IRQ found the
  next block already started) counts an underrun. On the Plus-W,
  `PIN_AUDIO_SHADOW` (GP42, tied to GP34 by JP3 1-2) is forced to input at
  init and never touched again.
- Mixer: sums each active source's `int16` block, applies the volume shift,
  clips, and biases to unsigned 10-bit for the PWM. A built-in test-tone
  source (`audio test <hz>`) exists for bring-up and self-test.
- Chip cores run at their native clocks (SN76489 4 MHz, AY per the SSC's
  derived clock, SP0256 3.12 MHz) and decimate to 32 kHz internally, as
  MAME's cores do. Only the PWM divider changes if the system clock moves to
  200 MHz.

### 4.2 Games Master Cartridge (`firmware/src/dev/gmc.c`)

- Bank select at `$FF40` and banked ROM: entirely in the bus-engine spec
  (`rom.c` write hook). `gmc.c` registers a device covering `0x3F41` writes
  only; `device_dispatch_writes` calls `sn76489_write(data)`.
- Becker's registration shrinks from `0x3F40-0x3F5F` to `0x3F42` for
  writes (its reads at `$FF41/42` are core1 read hooks and unaffected).
  Reads at `$FF41` remain Becker status; GMC software never reads it.
- Enabled automatically when `rom_load_file` loads an image larger than
  16 KB; disabled on `rom off` or a plain load. No saved state beyond the
  `rom` line `save` already writes.
- Autostart: the Plus-W firmware `/CART` pulse (bus-engine spec) covers
  GMC titles. On a Pico 2, JP5 1-2 gives `/CART` but takes header pin 34
  from audio, so a Pico 2 GMC user chooses autostart or sound, or types
  `EXEC &HC000`. README states this.

### 4.3 MIDI (`firmware/src/dev/midi_pak.c`, `firmware/src/usb/`, `dw_server.c`)

- **USB:** `CFG_TUD_MIDI 1`, one cable, descriptors updated in
  `usb_descriptors.c`. `midi_usb_out(const uint8_t *p, n)` wraps
  `tud_midi_stream_write`, which packetises a raw byte stream, so running
  status and SysEx pass through. The device may need re-approval in the
  macOS USB accessory gate after the descriptor change.
- **MIDI Pak (Plus-W):** 6850 at `$FF6E` (control/status) and `$FF6F`
  (data), registered into the firmware-decode mask. Becker pattern: core1
  owns `bus_table[0x3F6E]` and `[0x3F6F]`; a read hook on `0x3F6F` pops the
  RX ring (1024 bytes, `ring.h`) into `[0x3F6F]` and updates the
  receive-full bit of `[0x3F6E]`; core0 only pushes USB MIDI IN bytes into
  that ring. Status: bit 0 RDRF, bit 1 TDRE = 1 always, `/DCD` and `/CTS`
  bits 0 (asserted), others 0. Writes to `$FF6F` go through the write ring
  to `midi_usb_out`; a control write with the master-reset pattern
  (`0x03` in bits 1:0) clears the RX ring, other control writes are
  ignored. Interrupt line: checked against MAME `coco_midi.cpp` during
  planning; if the real cart drove `/CART`, it is added on the Plus-W's
  `CART_DRV` behind the 6850's receive-interrupt-enable bit. Polled
  operation ships first.
- **DriveWire channel 14 (both builds):** in `dw_server.c` (or the
  coco-manager branch's `dw_vser.c` once merged) SERWRITE, SERWRITEM and
  FASTWRITE `$8E` on channel 14 call `midi_usb_out`. Today that channel is
  consumed and dropped. No MIDI input on this path, matching DW4.
- `midi status`: bytes out (Pak, DW), bytes in, RX ring depth. Nothing to
  save.

### 4.4 Speech/Sound Cartridge (`firmware/src/dev/ssc.c`, `firmware/src/emu/tms7000.c`, `sp0256.c`, `ay8910.c`)

- **Registers (Plus-W):** `$FF7D` write = reset the TMS7040; `$FF7E` write
  = command byte into the host latch; `$FF7E` read = status: bit 7 busy,
  bit 6 SP0256 standby, bit 5 sound-activity detector, bits 4:0 = `11111`
  (MAME `coco_ssc.cpp`). Both in the firmware-decode mask.
- **Handshake across cores:** the `0x3F7E` write hook (core1) stores the
  byte in `ssc_latch` and increments `ssc_wr_count`. Core0 increments
  `ssc_ack_count` when the emulated MCU reads the latch. Core0 maintains
  `ssc_shadow` (bit 7 = MCU-driven busy, bit 6 standby, bit 5 activity).
  The `0x3F7E` read hook (core1) composes
  `(wr != ack ? 0x80 : 0) | ssc_shadow | 0x1F` into `bus_table[0x3F7E]`.
  One writer per variable. Whether the real busy is a hardware latch or an
  MCU pin is a planning check in the service manual; the composition is a
  correct superset either way. The `0x3F7D` write hook sets
  `ssc_reset_req`; core0 resets the interpreter when it sees it.
- **MCU emulation:** `tms7000.c` is a new C interpreter for the TMS7000
  instruction set (about a thousand lines), with the TMS7040's on-chip
  RAM and ports. Peripherals wired per `coco_ssc.cpp`: port A host
  interface, port B addressing the 2 KB RAM, port D as the shared data
  bus to the RAM, AY-3-8913 and SP0256, plus the control lines. Exact port
  bit assignments are copied from MAME at planning time, not reconstructed.
  The interpreter runs inside `audio_render_block`: each 8 ms block executes
  8 ms of MCU cycles at the clock `coco_ssc.cpp` declares, so MCU time is
  locked to the sample clock. `audio_stats.max_block_us` shows the cost;
  the 200 MHz system clock on the roadmap is the escape hatch.
- **Speech and sound:** `sp0256.c` renders at its native rate and resamples
  into the 32 kHz mixer; `ay8910.c` is the same AY core the mixer already
  has. The sound-activity bit is an envelope follower over the AY's
  rendered block, thresholded.
- **ROMs and activation:** `ssc on` loads `pic-7040-510.bin` (4096 bytes)
  and `sp0256-al2.bin` (2048 bytes) from the FAT volume root into SRAM,
  refuses with a named reason if either is missing or the wrong size or
  the build is a Pico 2, then registers the device. `ssc off` unregisters.
  `save` persists `ssc on`. The ROMs are copyrighted and never ship with
  the firmware; README says where MAME users keep them.

### 4.5 Console

`audio on|off|vol <0-7>|test <hz>|test off|status|selftest`, `ssc on|off`,
`midi status`. `save` writes `audio on`, `audio vol N`, `ssc on`. All are
lines in the existing `dispatch()` if-chain; `help` lists them.
`picoco.cfg` replay runs before core1 launches, so `ssc on` has its ROMs
and hooks in place before the bus is live.

## 5. Error handling

Refusals with a reason, never silent fallbacks: missing or wrong-size SSC
ROMs; `ssc on`, MIDI Pak or `audio selftest` on a Pico 2 build; audio
self-test while real bus cycles are being seen. Per-cycle conditions are
counters: `bus_stats.write_overrun`, `audio_stats.underruns`,
`audio_stats.max_block_us`, MIDI byte counts. The audio hardware is
initialised at boot on every build and is silent until a source is active,
so a build with no sound configured behaves as today.

## 6. Testing

### 6.1 Host (`ctest`)

- `test_audio`: mixer sums, clips and biases; test tone at 440 Hz has the
  expected zero-crossing count over one second of blocks.
- `test_sn76489`, `test_ay8910`, `test_sp0256`: each core rendered for a
  fixed register sequence and compared loosely (tone frequency within 1 %,
  envelope timing within one block) against a WAV MAME produced for the
  same sequence with `-wavwrite`; WAV fixtures are checked in, small.
- `test_tms7000`: differential test. MAME's debugger traces the TMS7040 in
  `coco_ssc` (`trace` on that CPU) for a scripted command stream; our
  interpreter runs the same ROM and stream and the PC/register sequence is
  diffed step by step. The ROM is not in the repo; the test skips when
  `firmware/tests/fixtures/roms/` lacks it and CI treats the skip as a
  warning.
- `test_ssc`: handshake composition (busy on the cycle after a write,
  cleared after ack), reset request, ROM size refusals.
- `test_midi_pak`: 6850 status/data sequences; `test_dw_server` gains
  channel-14 routing cases.
- Trace replay: a MAME Lua script (`firmware/tools/mame_trace.lua`) taps
  writes to `$FF40-$FF7F` while a game runs and writes address, data,
  timestamp. `sim_bus` replays it through the whole stack on the host;
  `test_replay` gains GMC and SSC traces as fixtures.

### 6.2 On-device (`firmware/tools/bench.py` over the USB console)

- **Fake 6809 replay** (bus-engine spec) of the same GMC and SSC traces;
  asserts the write ring never overran and the SSC status sequence.
- **Underrun soak:** `audio test 440`, then a loop of `save` (flash
  erases) and console traffic for 60 s; reads `audio status`; reports
  underruns. Establishes the flash-stall glitch rate on both boards.
- **ADC loopback (Plus-W):** one jumper from TP7 to J1 GP44 (ADC on the
  RP2350B); `audio selftest` renders the test tone, captures 1024 ADC
  samples, checks frequency and amplitude against the §4.6 hardware
  numbers. A Pico 2 has no free ADC pin and skips it.
- **MIDI loopback:** `bench.py` sends notes into the Pico's USB MIDI IN and
  reads `midi status`; fake 6809 writes to `$FF6F` must arrive at the host
  as MIDI (python-rtmidi or `receivemidi`).

### 6.3 XRoar

XRoar on the host build's TCP Becker listener (bus-engine spec) boots
NitrOS-9; `/midi` output reaches a host MIDI monitor through channel 14.
XRoar's own GMC emulation is the behaviour reference when Fahrfall sounds
wrong on the bench.

### 6.4 Bench (CoCo 3, hand-run)

Test tone audible with the CoCo's cart audio selected; Fahrfall and Dunjunz
GME with sound; Lyra playing into a DAW over USB MIDI; SoundMaster, Gold
Runner and Pitfall II on the SSC.

## 7. Order of work

1. Pipeline, test tone, `audio` commands, underrun soak. Both boards.
2. GMC: SN76489 core, device, Becker range shrink, trace replay, bench.
3. MIDI: USB MIDI class, DW channel 14, MIDI Pak, loopback tests, XRoar.
4. SSC: TMS7000 interpreter with the differential test, SP0256 and AY
   cores, handshake, `ssc` command, trace replay, bench.

## 8. Docs to update

`docs/firmware-architecture.md` (pipeline, devices, cross-core handshake),
`docs/hardware-design.md` §4.6 (pointer to the firmware, the GP42 rule),
README (ROM files, autostart-versus-audio on a Pico 2, MIDI setup),
`ADDITIONAL_ROADMAP.md` item 11 closed, both board headers, `CLAUDE.md`
hard rules (GP42 never driven on a Plus-W; the SSC/MIDI single-writer
variables; no synthesis on core1).

## 9. References

- MAME `src/devices/bus/coco/coco_ssc.cpp` (registers, TMS7040 ports,
  clocks, ROM `pic-7040-510.bin`), `coco_gmc.cpp` (`$FF41`, 4 MHz),
  `coco_midi.cpp` (6850 at `$FF6E/6F`).
- DriveWire 4 `virtualserial/DWVSerialPorts.java` (`VSerial_MIDIPort`
  default 14), `DWVSerialPort.java` (MIDI byte handling).
- PicoGUS (`sw/`) for the PSG cores and the single-image, runtime-mode
  shape; rp2040-doom sound notes for OPL cost figures (not used here).
- emu2149 (MIT), MAME `sp0256.cpp` (BSD-3), jzintv `sp0256.c` (GPL):
  licence decision in planning.
- Tandy Speech/Sound Cartridge service manual (colorcomputerarchive.com)
  for the busy-latch question.
- `docs/RP2350B_IDEAS.md` §5, §10, §11, §12.
