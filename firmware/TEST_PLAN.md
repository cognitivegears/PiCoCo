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

## F. PCB bring-up (v2.3.1): two boards, real CoCo hardware

One board built with a Pico 2, one with a Waveshare RP2350B-Plus-W
(`-DPICOCO_BOARD=plusw`). Test both on CoCo 1, CoCo 2, CoCo 3, bare and
through a Multi-Pak Interface (MPI).

### F.1 Pre-power checks (both boards)

- JP2 at 1-2 (hardware /OE), JP3 at 1-2 (audio on header pin 34), JP4 open
  (no autostart tie), JP5 open (no firmware /CART or /NMI drive yet).
- R7 = 10 kΩ (the v2.3.1 value; 100 kΩ loses to the RP2350's own reset
  pull-down and never releases /HALT).
- Plus-W builds only, before first power: confirm on the physical module
  (Waveshare schematic or a continuity check) that its radio uses GP36-GP39,
  not pad-grid GP24/GP25/GP29 (CTS_BUF/SCS_BUF/A14_BUF): Zephyr's board port
  and arduino-pico issue #3297 both say GP36-39. Firmware blinks LED2 on
  GP23 (same sources); after flashing, check the LED blinks. Also confirm 16 MB flash on the module (no
  runtime JEDEC ID check exists; `PICO_FLASH_SIZE_BYTES` in
  `firmware/boards/picoco_plusw.h` is a build-time assumption, not verified
  against the part actually on the board).

### F.2 Per-build bring-up steps

1. Build and flash:
   - Pico 2: `cmake -B build-pico -G Ninja -DPICO_SDK_PATH=... firmware &&
     ninja -C build-pico`, then `flash.sh build-pico/picoco.uf2`.
   - Plus-W: same with `-DPICOCO_BOARD=plusw` into `build-pico-plusw`.
2. Power up with the cart seated; /HALT should hold (CoCo dead) until
   `log dump` shows `core1 up, halt released`.
3. `status`: check cycle counts are moving and `bus addr_resample` is 0 (or
   small and not growing) once past the CoCo's own boot-time ROM copy.
4. Becker loop, no ROM needed: `becker loop`, `bus drive on`, then on the
   CoCo `POKE &HFF42,65: PRINT PEEK(&HFF41), PEEK(&HFF41), PEEK(&HFF42)` →
   `0 2 65`.
5. `rom load <rom>` (see F.3 for which file), `save`, power-cycle: HDB-DOS
   boots, `DIR` lists the test disk, `LOADM"DINORUN":EXEC` runs it.
6. `SAVE` a program from BASIC, power-cycle, `DIR` still lists it.
7. `dw stats`: `crc_err 0` throughout.

### F.3 ROM notes

- CoCo 1 and CoCo 2: `hdbdw3bck.rom` (`rom load hdbdw3bck.rom`, then `save`).
  `hdbdw3bc3.rom` (bc3, 1.79 MHz during transfers) is CoCo 3 only.
- CoCo 1 needs Extended Color BASIC (not plain Color BASIC) for HDB-DOS to
  hook in.
- On CoCo 1/2, BASIC `PEEK(&HC000)` reaches the cart directly (unlike a
  CoCo 3, which runs BASIC from RAM) — they execute the DOS ROM as code from
  the cart, so an address-decode error crashes immediately instead of
  showing up as a stray PEEK. Watch `bus addr_resample` closely on these two.
- Confirm the test disk image is readable from CoCo 1/2 DOS before blaming
  the hardware for a failed `DIR`.

### F.4 Through an MPI

- Set both the /CTS and /SCS slot-select switches to PiCoCo's slot.
- An unmodified Tandy 26-3024 MPI needs the CoCo 3 upgrade to work with a
  CoCo 3; a 26-3124 works as shipped.
- The MPI's own buffers add delay, so the CoCo 3 fast-mode (bc3) row through
  the MPI is the timing-margin test for roadmap item 6 (read-path margin).
- A working MPI row is also the first data point for the unverified /SLENB
  item in `docs/hardware-design.md` §9 (deferred to v2.4).
- Also try a second cart in another MPI slot, and switch PiCoCo between
  slots, to rule out a slot-specific fault.

### F.5 Test matrix

Rows: CoCo model / mode. Columns: board. Cells: pass/fail + date, or blank
until run.

| CoCo | Pico 2, bare | Pico 2, MPI | Plus-W, bare | Plus-W, MPI |
|---|---|---|---|---|
| CoCo 1 (`hdbdw3bck`) | | | | |
| CoCo 2 (`hdbdw3bck`) | | | | |
| CoCo 3, 0.89 MHz (`hdbdw3bck`) | | | | |
| CoCo 3, 1.79 MHz (`hdbdw3bc3`) | | | | |

## G. WiFi (Plus-W)

Firmware 1.3, Plus-W build. Server: `picoco-host` on the Mac unless noted.

### G.1 Bare module over USB (no CoCo)

1. `net scan` lists networks.
2. `net join <ssid>`, `net psk <psk>`, `net server <mac-ip> 65504`, `becker net`: `net status` reaches `net state up`.
3. `bus selftest net` passes (worst round trip under 200 ms).
4. Error reasons: wrong SSID, closed port, bad DNS name.
5. Server down at boot with `becker net` saved: fallback after 10 s; `becker net` re-arms once the server is back.
6. Cold-boot SNTP seeding (power cycle, then `time` shows the clock set).

### G.2 On the PCB with a CoCo

- Boot hold with a real CoCo: HDB-DOS boots over WiFi with /HALT released at link-up.
- DIR and LOADM from DW4 over WiFi.
- DIR and LOADM from FujiNet-PC over WiFi (BoIP).
- Fallback with the server stopped: CoCo boots native after 10 s.
- Manager WiFi screen end to end (scan, PSK, server, mode, save).
- VSYS scope trace during a LOADM over WiFi, against the 300 mA cart budget.

### G.3 Results

| Check | Result | Date |
|---|---|---|
| G.1 join + DHCP + connect at boot | 5.3 s (`net up in 5327 ms`) | 2026-09-30 |
| G.1 OP_TIME round trip | 8-13 ms typical, worst 23-30 ms over 20 | 2026-09-30 |
| G.1 `bus selftest net` | pass | 2026-09-30 |
| G.1 bad SSID / closed port / bad DNS name | `no such network` / `refused` / `dns failed`, ~10 retries in 22 s each | 2026-09-30 |
| G.1 server down at boot | `net failed (refused), native fallback` after 10 s; `becker net` re-arms | 2026-09-30 |
| G.1 `net scan` | 21 networks | 2026-09-30 |
| G.1 cold-boot SNTP seeding | pass 2026-09-30: after a USB power cycle the boot log shows `net: sntp seeded 1790778946`, `time` reports it with `clock kept`, `net up in 8329 ms` | |
| G.1 `net tz` (firmware 1.3, 25517de) | pass 2026-09-30: `net tz -240` + `save` + reboot logs `net: sntp set 1790765675 (utc 1790780075 tz -240)`, exactly Mac UTC minus 14400; `net tz 0` then `net tz -240` move `time` at once, no hourly wait | |
| G.1 reconnect after a Pico reboot with picoco-host (af12f4d) | pass 2026-09-30: the server replaces the stale client on the new accept; `bus selftest net` passes right after the reboot (dwinit 5 ms, worst 19 ms) without restarting the server | |
| G.2 boot hold with CoCo | | |
| G.2 DIR/LOADM from DW4 | | |
| G.2 DIR/LOADM from FujiNet-PC | | |
| G.2 fallback, server stopped | | |
| G.2 manager WiFi screen | | |
| G.2 VSYS scope trace | | |

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

### 2026-09-30, PCB v2.3.1 #1, Pico 2, CoCo 3 (firmware 1.3 + core1 timing fix)
First assembled PCB (docs/pcb-bringup.md has the phase-by-phase log). HDB-DOS
boots on cart power alone, CoCo RESET reboots the Pico through U13/R9, DIR,
LOADM, SAVE and the manager all pass, `addr_resample 0` (the breadboard
needed the resample on every boot). hdbdw3bc3 (1.79 MHz) failed
intermittently with a one-byte Becker desync; root cause and fix in
docs/pcb-bringup.md "1.79 MHz fault". After the fix: 4 LOADMs + SAVE at
1.79 MHz, `dw reads 106 writes 4 crc_err 0`, `becker underrun 0`. Trace now
freezes at the first underrun/CRC error (`trace run` thaws).


### 2026-09-29, breadboard: bridge mode, latency, clock, CoCo manager (firmware 1.2)
Firmware 1.2 (branch coco-manager) on the Pico 2 breadboard rig, CoCo 3.
- **Bridge mode (breadboard step 10) passes** against two servers: `picoco-host`
  over `firmware/tools/becker_relay.py` (CDC0 to TCP), and DriveWire 4.3.6p
  (Maven build, `DriveWireServer --noui`, serial device on CDC0, `SerialDTR`
  true, `HDBDOSMode` true). DIR, LOADM+EXEC of DINORUN, SAVE, DIR, all
  correct; `becker underrun 0 overrun 0` over 76 k Becker reads. Timings with
  no added delay: DIR 1 KB in 0.10 s, LOADM 23 KB (90 sectors) in 2.16 s.
- **Latency limit (WiFi stand-in, relay `--delay/--jitter`):** 25 ms one-way
  is fine (DIR 0.59 s, LOADM 14.3 s: two round trips per sector, purely
  latency-bound). 100 ms one-way is fine (boot's 3 sectors 1.17 s, DIR 1.64 s).
  150 ms one-way hangs the CoCo at the HDB-DOS banner: the server's
  `DW_PAYLOAD_TIMEOUT_MS` (250 ms) expires before the CoCo's READEX checksum
  bytes arrive (~305 ms after the data went out), the op is dropped, no status
  byte is sent, and the CoCo's Becker loop waits forever (`timeouts=1` in
  picoco-host stats). DW4 has the same class of limit (`ReadByteWait`, 200 ms).
  So a remote server needs a round trip under ~200 ms; the Pico is not the
  limit.
- **Clock (plan Task 8):** `pico_aon_timer` keeps time across a watchdog
  `reboot` and a BOOTSEL reflash (`clock kept`). A CoCo reset or power-cycle
  does not reboot the Pico on the breadboard (no RUN tie); the RUN-pin path
  is a PCB check.
- **CoCo manager (plan Task 15):** `DRIVE 3:RUN"PICOCO"` runs; list, jump,
  mount, already-mounted message, eject, new image, settings (ROM pick,
  HDB-DOS toggle, clock set, bad date), save, DIR after exit, boot picker
  and HELLO.BAS all pass; `reply_overflow 0`, `becker overrun 0`;
  `dw mount 1 picoco.cfg` and `dw disk insert 1 picoco.cfg` refused.
  Found: the SHIFT+letter chords never arrived as lowercase on the real
  CoCo 3 (XRoar's `-type` injects ASCII, so it never exercised a real
  SHIFT). Rebound to plain letters with `G` for goto. Also: mounting an
  image already open read-write in another drive fails on FatFS
  (`FF_FS_LOCK`); the firmware now reports `already mounted`.

### 2026-09-16, HDB-DOS boot + native DriveWire on the CoCo 3
Root cause of the 09-15 HDB-DOS garbage: core1 sampled the address ~20 ns
after OE_BUS fell and A8/A10 (sometimes A0/A2) read high on ~3% of cart
reads (trace: single-sample spikes of 0x100/0x400, steady rate, not a
loose wire). The CoCo 3 copies the cart into RAM at boot, so HDB-DOS ran
corrupted. Fix: `bus_core1.c` reads the GPIOs twice ~70 ns apart and uses
the second; `status` prints `bus addr_resample N bits X` (first-sample
disagreements). After the fix: 0 spikes in a 4096-read sweep, HDB-DOS 1.5
BECKER (`hdbdw3bck.rom`) boots, `DIR` of DINORUN.DSK on drive 0 (`dw
hdbdos on`) lists correctly, `LOADM"DINORUN":EXEC` loads and runs: `dw
reads 97 crc_err 0 timeouts 0`, `becker reads 24929 writes 679 underrun 0`.
Boot still logs addr_resample hits on all bits during the ROM copy (fast
loop); revisit before the `bc3` 1.79 MHz ROM. `hdbdw3bc3.rom` (CoCo 3 build, 1.79 MHz during transfers) also passes: boot,
DIR, LOADM+EXEC of DINORUN: `dw reads 97 crc_err 0`, Becker byte period
18 us (vs 37 us with the bck build), `addr_resample 97 bits 3ffe` for the
whole boot+load (vs 2629 with bck), so the 70 ns settle has margin at speed.
Saved config now serves bc3.
Step 11 write half also passed: `SAVE"HELLO"`, CoCo power-cycle, `DIR` still
lists it: `dw writes 4 write_err 0 crc_err 0`. Caveat: `POKE &HFFDE,0` ROM mode on the CoCo 3 with a cart
present hides Super ECB behind the cart window; BASIC then executes 0xFF
from the Pico (?NF ERROR, runaway). Use only for a lone PEEK, or not at all.

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
