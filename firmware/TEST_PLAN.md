# PiCoCo firmware bench test plan

Checklist for the manual tests that need hands on the hardware. Run it
with Claude: you do the wiring and meter readings, Claude drives the
console over USB and records results. Each step says what to do, what
Claude will run, and what "pass" looks like.

State (updated 2026-09-08 evening): firmware `0.3-pico` from `main`
78fb594 (includes the DriveWire conformance fixes) is on the Pico 2, saved config is `bus drive off`,
no ROM, `becker native`, drives unmounted. Console port is the second
`/dev/cu.usbmodem*` (was `/dev/cu.usbmodem103`).

## Already verified on this Pico 2 (no CoCo)

| Check | Result |
|---|---|
| USB enumerates: two serial ports + storage interface | pass (after allowing the accessory in macOS Privacy & Security) |
| `version`, `status`, `log dump`, `help` | pass |
| `smoke` (GP0..GP7 + LED), `halt on` / `halt off` accepted | pass (electrical check of GP27 still pending, step B) |
| `fs format`, `fs ls`, `save`, reboot, config replay | pass |
| `dw selftest` (create, mount, READ, WRITE, READEX, eject) | pass |
| `fs export` mounts as `PICOCO` on the Mac, copy a `.dsk`, eject, `fs import`, file persists across reboot | pass |
| Guards: export/format refused with a drive mounted or capture open; fs/dw commands refused while exporting; import refused before host eject | pass |
| `crash` and `crash panic` leave a record that `status` shows after reboot | pass |
| `reboot` reports `last reset reboot`, hard fault reports `hardfault pc=...` | pass |
| core1 running: `log dump` shows `core1 up, halt released`; GP26 to GND logs read cycles at index 0x3FFF (714 cycles) | pass |
| Address decode: GP8 low moves the index to 0x3FFE | pass |
| Pattern byte on the data pins | not yet (step A) |

## Pin reference (Pico 2 header, physical pin numbers)

| Signal | GPIO | Pin |
|---|---|---|
| D0 | GP0 | 1 |
| D1 | GP1 | 2 |
| A0 | GP8 | 11 |
| A13 | GP21 | 27 |
| OE_BUS | GP26 | 31 |
| HALT_GATE | GP27 | 32 |
| E | GP28 | 34 |
| GND | | 3, 8, 13, 18, 23, 28, 33, 38 |

Notes that matter for the jumper tests:
- core1 logs a cycle only on a new falling edge of GP26. Holding GP26
  on GND continuously logs one cycle and then drives that cycle's byte
  until you lift the jumper. To log a fresh cycle after changing other
  jumpers, lift GP26 and touch it down again.
- `rom pattern` fills only indices 0x0000..0x1FFF. With every address
  pin high the index is 0x3FFF, outside the pattern, and the table byte
  is 0xFF. To land inside the pattern, A13 (GP21) must be low.

## A. Pattern byte on the data pins (needs jumpers + meter)

1. You: nothing connected to GP0..GP7. Tell Claude "ready A".
2. Claude runs: `rom pattern`, `bus drive on`, `stats reset`.
3. You: jumper GP21 (pin 27) to GND and GP8 (pin 11) to GND. Then touch
   GP26 (pin 31) to GND and hold it.
4. Claude runs: `trace dump 2`. Pass: a line ending `1ffe R fe`.
5. You, while still holding GP26: meter GP0 (pin 1) to GND reads about
   0 V; GP1 (pin 2) reads about 3.3 V. Pass if both.
6. You: remove all three jumpers. Claude runs: `bus drive off`, `rom off`.

## B. HALT drive (needs meter)

1. You: meter between GP27 (pin 32) and GND. Tell Claude "probe on".
2. Claude runs: `halt on`. Pass: about 3.3 V.
3. Claude runs: `halt off`. Pass: about 0 V.
4. Optional: power-cycle the Pico with the meter still on GP27. Pass:
   it reads high for the first few hundred milliseconds after power-on
   and drops to 0 V once `core1 up, halt released` (Claude can confirm
   with `log dump`). This is the boot hold that keeps the CoCo halted
   until the bus loop is armed.

## C. Write capture (needs jumpers)

Shows that a CoCo write cycle is captured with its data byte.

1. Claude runs: `bus drive off`, `stats reset`.
2. You: jumper GP22 (R/W, pin 29) to GND (write cycle), GP0 (pin 1) to
   GND (data bit 0 low). Touch GP26 to GND briefly and release.
3. Claude runs: `status` and `trace dump 2`. Pass: `writes` is at
   least 1 and a trace line ends `3fff W fe`.
4. You: remove the jumpers.

## D. Becker loopback through the real table (needs jumpers)

Exercises the single-writer rule end to end without a CoCo. Address
0x3F42 needs A0 high, A1..A5 = 1,0,0,0,0... this is 14 address pins, so
it is impractical with jumpers. Skip on the bench; it is covered by
`test_stack` on the host and by the CoCo test (runbook row 9).

## E. When the breakout boards arrive

Follow `firmware/README.md` section "Bring-up" (also
`docs/breadboard-plan.md` section 6), rows 3 to 11, in order. Each row
names the console commands, the expected console output, the expected
CoCo behaviour and the git tag to make when it passes. Before the first
CoCo test:

- `bus drive off` and `rom off` saved in config, so the Pico only
  listens on the first power-up in the cartridge slot.
- Have the logic analyzer on E, /CTS, /SCS, OE_BUS for row 5.
- Have the HDB-DOS DriveWire 8 KB ROM ready to copy via `fs export`.

## How to resume with Claude

Plug the Pico in, then say "resume the bench test plan at step A" (or
B, C, E). Claude will check the console port, print `status`, and walk
the steps.

Step 0 on resume (done 2026-09-08 for 78fb594; repeat whenever `main`
moves): reflash from current `main`: `bootsel` on the console, wait for `/Volumes/RP2350`, then
`cp -X build-pico/picoco.uf2 /Volumes/RP2350/` (rebuild with
`ninja -C build-pico` if the UF2 is older than the last commit). Then
`version` still reads `0.3-pico`; `status` now shows `dw hdbdos`. Results get appended to this file under a dated "Results"
heading.

## Results

### 2026-09-15, breadboard gates 9 and 10 on a CoCo 3
Address buffers + 74HC00 decode (bench guide gate 9): passed after U15 pin 14
was found on GND. A CoCo 3 runs BASIC from RAM, so `PEEK(&HC123)` never hits
the cart; the boot-time `0000 R`/`0001 R` and /SCS PEEKs ($FF41/44/48/50)
proved A0-A4, A6-A13. Data buffer U10 + Becker loop (gate 10, plan step 9):
`POKE &HFF42,65: PRINT PEEK(&HFF41), PEEK(&HFF41), PEEK(&HFF42)` -> `0 2 65`,
trace `3f42 W 41`, `W a5` echoed exactly, `becker reads 3 writes 3 underrun
0`. First attempt traced `W e1` with the screen still reading 65: GP2-GP7 were
one Pico pin low (pin 3 GND not skipped). A5 still unproven.

### 2026-09-08 evening, reflash
`main` 78fb594 flashed via BOOTSEL; `version 0.3-pico`, config replayed
(`mode native`, `bus drive off`, `dw hdbdos off`), `dw selftest` ok.

### 2026-09-08, host validation against a real DW4 disk image
`picoco-host` with `dw/drivewire4/disks/run-dino-run/DINORUN.dsk` (161280
bytes, RAW) on drive 0 and a scratch copy on drive 1; `tools/dwtest.py`
in both `--hdbdos off` and `--hdbdos on`: all 7 checks ok (630/630
sectors match, READ checksum, READEX corrupt → E_CRC, unmounted →
E_NOTRDY, scratch write/readback/corrupt-write, TIME).
