# PiCoCo v2.3.1 PCB bring-up checklist

First assembled boards arrived 2026-09-30 (JLC assembly, module not fitted).
Do the phases in order; each one is cheap to undo and the next one is not.
Record results in the table at the end.

Bench power: +5 V into finger 9 (front side, 5th from the pin-1 end; silk
"2" is on the back), GND at TP8. Set the supply to 5.0 V, current limit
100 mA for phase 2, 500 mA once the module is on. No bench supply
(2026-09-30): a Mac USB port through a breakout works, or use the CoCo
itself as the supply. The module's USB does not feed +5V (D2 blocks it).
In the CoCo the top 28 mm of the board is outside the case, so TP6, TP8
and the module pads are reachable; finger-side checks (finger 3, 5) move
to the console phase.

## Phase 1: no power, DMM only (10 min)

1. Fingers: gold, not silver; bevel slopes under a fingernail. Test-fit in
   the CoCo slot with the CoCo unplugged, then pull it out.
2. DNP list matches the board: R2 R10 R15 R17 Q3 C12 C15 J1 J2 J3 empty,
   Q4/R16/R18 fitted.
3. No shorts (resistance mode, expect > 1 kΩ, capacitors may ramp):
   finger 9 to TP8, TP6 to TP8, module pad 39 (VSYS) to TP8.
4. D2 polarity, diode mode: red on finger 9, black on module pad 39 reads
   ~0.2-0.3 V; reversed reads open. Reversed D2 = the module never powers.
5. Jumpers: JP2 1-2 and JP3 1-2 read 0 Ω; JP4 open; all three JP5 pads open.

## Phase 2: bench power, no module (10 min)

Clip finger 6 (E) to GND for every bench test without a CoCo. E low forces
OE_BUS high (TP1), so U10 stays disabled while the other cart inputs float.

1. Apply 5.0 V. Current under ~30 mA and steady.
2. TP6 = 3.3 V (3.2-3.4). U14 stays cool after a minute. Reversed U14 would
   put +5 V on TP6: cut power at once.
3. Module pad 39 = ~4.7 V (5 V minus D2). Pad 36 (3V3_OUT) reads 0 V, it
   is NC by design.
4. TP1 (OE_BUS) high with E clipped low. Finger 3 (/HALT) low: R7 pulls Q2
   on with no module, that is the boot-hold default working.
5. Finger 5 (/RESET) ~5 V via R3; module RUN pad (pin 30) ~3.3 V behind
   U13. Short finger 5 to GND: RUN pad falls to 0 V.

## Phase 3: fit the module

Start with a Pico 2 on the first board: that firmware path is proven on
the breadboard, so a failure here is a board fault, not a new-code fault.
Put the Plus-W on the second board.

- Pico 2 flat mount: Kapton over the carrier grid pads under the Pico's
  three SWD/debug pads (GP29/GP32/GP35) first. See
  fab/main/READ-BEFORE-ORDERING.txt "MODULE".
- Plus-W: paste the 15 grid pads with fab/main/stencil-module, hand-solder
  the 40 castellations.
- Repeat phase 1 step 3 after soldering (a bridged castellation shows up
  as a rail short now).

## Phase 4: bench power + USB, no CoCo (20 min)

Keep the E-to-GND clip on. Bench 5 V on finger 9 plus USB to the Mac.
Do not run `bus selftest` on the PCB: U11-U13 outputs would fight PIO1.

1. USB only first: two serial ports enumerate, `version`, `status`.
   `log dump` shows `core1 up, halt released`.
2. Add bench 5 V: total edge current at idle (expect < 100 mA for a
   Pico 2). This is the number the 300 mA cart budget starts from.
3. /HALT electrical check (closes TEST_PLAN "step B"): finger 3 is high
   after boot (R1 to +5 V), `halt on` pulls it to 0 V, `halt off` releases.
4. RUN path: short finger 5 to GND for a second. The console drops and
   comes back, `status` shows a fresh boot. This is the "RUN-pin path is a
   PCB check" item from 2026-09-29.
5. Cold boot from the edge, no USB: power-cycle the bench 5 V with USB
   unplugged, watch finger 3 on a scope or DMM. It must be low from t=0 and
   go high once (firmware release, ~1.2 s). Never high before that.
6. Plus-W only: `net join` + `bus selftest net` while scoping module pad 39.
   Log the VSYS droop during the WiFi burst; that decides whether C12 gets
   fitted (TEST_PLAN G.2 "VSYS scope trace", done on the bench first).

## Phase 5: in the CoCo 3

Remove the E clip. CoCo off, insert the board, USB to the Mac for the
console, then power the CoCo on. Boot the saved config from the breadboard
(native, HDB-DOS).

1. HDB-DOS banner on the CoCo screen; `status` shows read cycles and
   `addr_resample`. Any garbage or no banner: `status` counters first,
   then TP1/TP5 on a scope (OE_BUS low only inside E high).
2. Breadboard gates on the PCB: DIR, LOADM+EXEC DINORUN, SAVE,
   `DRIVE 3:RUN"PICOCO"` manager.
3. CoCo RESET button: Pico reboots (console reconnects) and HDB-DOS comes
   back. Then a full CoCo power cycle with USB unplugged: the cold-boot
   race for real.
4. Audio: JP3 1-2 default puts AUDIO_PWM on the CoCo speaker; play a
   sound from the console or a game and check TP7.
5. Plus-W: TEST_PLAN G.2 rows (WiFi boot hold, DW4/FujiNet-PC over WiFi,
   fallback, manager WiFi screen).
6. Case: fit check against case/ once the board works electrically. (passed 2026-10-01)

## 1.79 MHz fault found on the PCB (2026-09-30)

Symptom: with hdbdw3bc3.rom (CoCo 3, 1.79 MHz during transfers) a DIR or
SAVE intermittently returned ?IO ERROR and every DriveWire op after it
failed its checksum until the Pico rebooted. Counters: one Becker
underrun, pushes = reads + 1, so the CoCo had taken one phantom byte
while the server was still fetching the sector and the stream was shifted
by one from then on. The trace (now frozen at the first underrun or CRC
error) showed the request bytes, a few not-ready polls, then a $FF42 read
the Pico never announced.

Cause, two parts:
1. The Pico 2 core1 read path drove the data pins ~245 ns after E rose
   (sample, 70 ns resample, lookup). At 1.79 MHz E is high for 279 ns, so
   the drive sat in the 6809E's latch window on timing tails. Adding 33 ns
   (+5 nops) made the very first poll fail, which confirmed the margin.
2. D0-D7 idled with pull-ups, so a late or missed $FF41 poll showed the
   CoCo 0xFF through the hardware-enabled U10, and 0xFF has the Becker
   "ready" bit set. That turned a rare late poll into a phantom byte.

Fix (firmware, uncommitted at the time of writing): core1 precomputes the
response while OE_BUS is high and drives within ~100 ns of it falling
(mismatches redrive and count as addr_resample; back-to-back cycles with
no idle sample count as late_precompute, 4097 per boot from the CoCo 3
ROM copy, benign); D0-D7 idle pulled down so a late poll reads "not
ready" and is retried. The breadboard never showed this because it ran
firmware 1.2 with the lookup inline and with the same pull-ups; the PCB
exposed it on firmware 1.3.

## Results

| Check | Result | Date |
|---|---|---|
| 1 fingers/bevel/slot fit | pass, fits the CoCo 3 slot, bevel good | 2026-09-30 |
| 1 DNP list | pass | 2026-09-30 |
| 1 rail shorts | pass, all open | 2026-09-30 |
| 1 D2 polarity | pass, 0.240 V red finger 9 / black pad 39 | 2026-09-30 |
| 1 jumper defaults | pass, JP2/JP3 1-2, JP4/JP5 open | 2026-09-30 |
| 2 TP6, pad 39, pad 36 (powered from the CoCo, no module) | pass: TP6 3.3 V, pad 39 ~4.7 V, pad 36 0.2-0.4 mV (floating, NC); current not measured, no bench supply | 2026-09-30 |
| 2 /HALT low with no module | | |
| 2 RUN follows /RESET | | |
| 4 USB enumerate + core1 up | pass: Pico 2 flat-mounted, firmware 1.3 flashed via BOOTSEL, two CDC ports, `core1 up, halt released`; files + config loaded (hdbdw3bck.rom, DINORUN.DSK drive 0, PICOCO.DSK drive 3, native) | 2026-09-30 |
| 4 edge current with module | | |
| 4 halt on/off at finger 3 | pass functionally: boot hold works on cart power (row below); no DMM reading taken | 2026-09-30 |
| 4 RUN reboot from finger 5 | pass by inference: Pico uptime restarted when the CoCo powered on with USB already attached (CoCo /RESET pulsed RUN through U13/R9) | 2026-09-30 |
| 4 cold-boot hold from edge power | pass: CoCo power-up with USB unplugged boots straight into HDB-DOS (R7 10k hold wins) | 2026-09-30 |
| 4 Plus-W VSYS droop | | |
| 5 HDB-DOS boots in the CoCo | pass, first try: banner + DIR of DINORUN.DSK. status: 11869 cycles, addr_resample 0, becker 1799 reads underrun 0 overrun 0, dw 7 reads crc_err 0, halt released 2.6 s after boot | 2026-09-30 |
| 5 1.79 MHz ROM (hdbdw3bc3) | FAILED on firmware 1.3 as merged, then fixed (see below): SAVE gave ?IO ERROR, then every sector failed its checksum (crc_err 5, underrun 1, one byte left in the ring = permanent one-byte desync). 0.89 MHz ROM and the write path were clean throughout. After the fix: 4 LOADMs + SAVE at 1.79 MHz, crc_err 0 underrun 0 | 2026-09-30 |
| 5 DIR/LOADM/SAVE/manager | pass: DIR, LOADM+EXEC DINORUN, `DRIVE 3:RUN"PICOCO"` manager (exit is BREAK). SAVE not yet tried. After: 46450 cycles, addr_resample 0, 72 dw reads crc_err 0, becker 18752 reads underrun/overrun 0 | 2026-09-30 |
| 5 RESET button + cold power cycle | pass: RESET reboots the Pico (uptime restarts) and the CoCo warm-starts back into DINORUN, which hooks the reset vector (CoCo behaviour, not the board); cold power cycle boots HDB-DOS | 2026-09-30 |
| 5 audio at TP7 / speaker | open: nothing drives AUDIO_PWM until the sound plan lands | |
| 5 G.2 WiFi rows | | |
