# PiCoCo — Hardware Design

PiCoCo is a Tandy Color Computer (CoCo 1/2/3) cartridge that hosts a
Raspberry Pi Pico 2 (RP2350) and presents two behaviors to the CoCo:

1. **ROM emulation** over the `/CTS` window (`$C000–$FEFF`) — the Pico
   serves a 16 KB HDB‑DOS + DriveWire boot image (or any ROM of your
   choice) so the CoCo has a DriveWire client to run.
2. **Becker port** peripheral at `$FF41` / `$FF42` (inside the `/SCS`
   decode range) — the Pico implements the DriveWire server protocol
   directly in firmware, turning the "virtual" Becker port used by
   CoCo3FPGA / XRoar into a real hardware port.

This document describes the **electrical design**. For firmware, see
[`firmware-architecture.md`](firmware-architecture.md).

---

## 1. Block diagram

```
                +5V (cart pin 9)
                    │
         ┌──────────┼──────────┐
         │          │          │
    [U14 AMS1117]  [D2 Schottky]
     5V → 3.3V       │
         │           │
       +3V3       VSYS_PICO ──► Pico 2 VSYS
         │
         ├──► U10 Vcca, U11/U12/U13 Vcc, U15 Vcc
         └──► Pico 3V3_EN pull-up

             CoCo 5V side          │ level shifters │  3V3 Pico side
             ─────────────         │ ─────────────  │  ──────────────
  D0..D7 ──── P1[10..17] ◄──► [U10 SN74LVC8T245] ◄──► GP0..GP7
  A0..A7 ──── P1[19..26]  ──► [U11 SN74LVC245 ]  ──► GP8..GP15
  A8..A13──── P1[27..31,37]──► [U12 SN74LVC245 ]  ──► GP16..GP21
  /R/W   ──── P1[18]      ──► [U12 ch7]          ──► R12 33Ω ──► GP22, U10 DIR
  /CTS   ──── P1[32]      ──► [U12 ch8] → CTS_BUF ──► U15 IN_A
  /SCS   ──── P1[36]      ──► [U13 ch1] → SCS_BUF ──► U15 IN_B
  E      ──── P1[6]       ──► [U13 ch2] → E_B     ──► U15 IN_C, GP28
  /RESET ──── P1[5]       ──► [U13 ch7] → RESET_DBG──► R9 100Ω ──► Pico RUN (R10 10kΩ pull-up to +3V3)
  Q, /SLENB, /HALT, /NMI, /CART ──► [U13 input-only; outputs no-connect]

  Hardware /OE gate (U15 SN74LVC1G11, 3-input AND):
    CTS_BUF ─┐
    SCS_BUF ─┼──AND──► R11 33Ω ──► OE_BUS ──► U10 /OE, Pico GP26
    E_B     ─┘

  Pico /HALT drive (hold-until-booted + DriveWire flow control):
    GP27 (HALT_GATE) ──► R8 100Ω ──► [Q2 2N7002] ──► /HALT_CART pin
                         R7 100kΩ to +3V3 pulls gate high during Pico boot
                         → Q2 ON → /HALT held low until firmware releases
```

Direction of `U10` is driven by the **buffered /R/W** wire (same net
that reaches Pico GP22). `/OE` of `U10` is driven by the hardware
3-input AND gate `U15`, so `U10` is tri-stated unless **all** of:
(a) `/CTS` OR `/SCS` is asserted (i.e., cart selected) AND (b) E is
high (data-valid phase).

Firmware distinguishes ROM reads ($C000–$DFFF) from Becker accesses
($FF40–$FF5F) by inspecting **A13** when `OE_BUS` is asserted: A13=0
means /CTS range, A13=1 means /SCS range. This saves a GPIO compared
to routing both /CTS and /SCS separately.

## 2. Component list

| Ref | Part | Package | Role |
|---|---|---|---|
| U1  | Raspberry Pi Pico 2 (RP2350) | SMD+TH module | MCU |
| U10 | SN74LVC8T245DW | SOIC‑24 | D0–D7 bidirectional shifter |
| U11 | SN74LVC245AD | SOIC‑20 | A0–A7 buffer (5V→3.3V) |
| U12 | SN74LVC245AD | SOIC‑20 | A8–A13, /R/W, /CTS buffer |
| U13 | SN74LVC245AD | SOIC‑20 | /SCS, E, Q, /SLENB, /HALT, /NMI, /RESET, /CART buffer |
| U14 | AMS1117‑3.3  | SOT‑223  | +5V → +3.3V LDO (supplies buffers only) |
| U15 | SN74LVC1G11  | SOT‑363 (SC‑70‑6) | 3‑input AND gate: `/OE = /CTS ∧ /SCS ∧ /E` |
| Q2  | 2N7002 | SOT‑23 | N‑FET: firmware‑controlled /HALT sink (hold at boot) |
| D2  | Schottky SS14 | SMA | +5 V → VSYS_PICO (replaces cascade via LDO) |
| R1  | 4.7 kΩ 0805 | — | /HALT pull‑up to +5 V (required; idles /HALT when Q2 off) |
| R2, R3 | 4.7 kΩ 0805 (DNP) | — | /NMI, /RESET pull‑up footprints; CoCo already pulls these up, so DNP by default |
| R4  | 10 kΩ 0805 | — | Pico 3V3_EN pull‑up to VSYS_PICO |
| R6  | 4.7 kΩ 0805 | — | /CART pull‑up to +5 V (in series with JP1) |
| R7  | 100 kΩ 0805 | — | Q2 gate pull‑up to +3V3 (default‑on during Pico boot) |
| R8  | 100 Ω 0805 | — | Q2 gate series from GP27 |
| R9  | 100 Ω 0805 | — | Pico RUN series from RESET_DBG |
| R10 | 10 kΩ 0805 | — | Pico RUN pull‑up to +3V3 |
| R11 | 33 Ω 0805 | — | OE_BUS series termination (fan‑out damping) |
| R12 | 33 Ω 0805 | — | RW_BUF series termination |
| R13 | 100 Ω 0805 | — | SWCLK series to J_SWD |
| R14 | 100 Ω 0805 | — | SWDIO series to J_SWD |
| C1  | 10 µF 0805 X5R ≥10V | — | +5V bulk at edge connector |
| C2  | 10 µF 0805 X5R ≥10V | — | LDO input |
| C3  | 22 µF 0805 X5R ≥6.3V | — | LDO output (MLCC‑compatible vendor required) |
| C4…C10 | 100 nF 0805 | — | Per‑IC decoupling (U10 Vcca, U10 Vccb, U11–U13, U15, Pico local) |
| C11 | 10 µF 0805 X5R ≥10V | — | Local +5 V bulk at U10 Vccb |
| P1  | COCO‑CART‑2.1X1.75 (custom footprint) | Edge fingers | Cartridge slot mate |
| JP1 | 1×2 0.1" header + shunt | TH | /CART pull‑up enable (install shunt to activate) |
| J_SWD | 1×4 header | 0.1" TH | Pico SWD + GND + 3V3 |
| TP1–TP6 | 1×1 mm SMD pads | — | OE_BUS, RW_BUF, CTS_BUF, SCS_BUF, E_B, +3V3 |

**Unique active parts: 7** (Pico 2, SN74LVC8T245, SN74LVC245A, AMS1117‑3.3,
SN74LVC1G11, 2N7002, SS14 Schottky). All are in the JLCPCB Basic or
Extended Library. All passives are 0805; footprints for R2 and R3 are
populated on the board but the BOM lists them as DNP.

## 3. Signal map

### 3.1 Cartridge edge connector (P1, `COCO-CART-2.1X1.75`)

40 fingers, 0.1" pitch, 2.1" × 1.75". Pin numbering per CoCo Technical
Reference Manual.

| Pin | Signal  | Direction (cart‑side) | Destination on board |
|-----|---------|-----------------------|----------------------|
| 1   | -12 V   | unused                | NC |
| 2   | +12 V   | unused                | NC |
| 3   | /HALT   | bidi (open‑drain) on CoCo | U13 in + R1 4.7 kΩ pull‑up to +5 V + Q2 drain (Pico‑controlled pull‑down) |
| 4   | /NMI    | bidi (open‑drain)     | U13 in + R2 4.7 kΩ pull‑up (DNP) |
| 5   | /RESET  | bidi (open‑drain)     | U13 in + R3 4.7 kΩ pull‑up (DNP); buffered copy drives Pico RUN via R9+R10 |
| 6   | E       | CoCo → cart           | U13 → GP28 and U15 IN_C |
| 7   | Q       | CoCo → cart           | U13 in only; output left unconnected (not routed to Pico — saves one GPIO) |
| 8   | /CART   | cart → CoCo (open collector) | R6 4.7 kΩ + JP1 header, then P1[8]. Install JP1 shunt to enable pull‑up. |
| 9   | +5 V    | power                 | C1 bulk, U14 in, U10 Vccb, D2 Schottky anode (→ VSYS_PICO), R1/R6 pull‑ups |
| 10–17 | D0–D7 | bidirectional         | U10 B‑side |
| 18  | /R/W    | CoCo → cart           | U12 in; U12 output → R12 33 Ω → U10 DIR + GP22 |
| 19–26 | A0–A7 | CoCo → cart           | U11 in → GP8–GP15 |
| 27–31 | A8–A12 | CoCo → cart          | U12 in → GP16–GP20 |
| 32  | /CTS    | CoCo → cart           | U12 in → CTS_BUF → U15 IN_A (NOT routed to Pico — firmware uses OE_BUS + A13) |
| 33, 34 | GND  | ground                | GND plane |
| 35  | SND     | cart → CoCo (audio)   | NC on MVP |
| 36  | /SCS    | CoCo → cart           | U13 in → SCS_BUF → U15 IN_B (NOT routed to Pico — firmware uses OE_BUS + A13) |
| 37  | A13     | CoCo → cart           | U12 in → GP21 (serves double duty as address MSB and ROM/Becker selector) |
| 38  | A14     | CoCo → cart           | P1 only; not routed to Pico or buffer |
| 39  | A15     | CoCo → cart           | P1 only; not routed to Pico or buffer |
| 40  | /SLENB  | CoCo → cart           | U13 in only; output left unconnected |

A14 and A15 are not needed for the 16 KB `/CTS` window or the Becker
port; they terminate at the cart edge fingers only. The
`OE_BUS + A13` decode scheme is the key simplification: firmware
determines cart‑selection from OE_BUS (U15 output) and ROM‑vs‑Becker
from A13 (which is 0 in the /CTS window, 1 in the /SCS window).

### 3.2 Pico 2 GPIO map (PIO‑optimized)

Pico 2 module exposes **26 GPIO** (GP0–GP22 + GP26–GP28). All 26 are
consumed in the current design.

| GPIO | Signal | Notes |
|---|---|---|
| GP0–GP7  | D0–D7 | Contiguous for `OUT PINS, 8` / `IN PINS, 8` |
| GP8–GP15 | A0–A7 | — |
| GP16–GP21| A8–A13 | — (GP8..GP21 contiguous = 14‑bit address) |
| GP22     | /R/W   | 15th bit of the address IN word; `IN PINS, 15` at base 8 grabs A0–A13 + /R/W in one cycle |
| GP23–GP25| *internal* | SMPS PS / VBUS sense / onboard LED (not header‑accessible) |
| GP26     | **OE_BUS** | Cart-selected signal from U15 output. `WAIT 0 PIN 18` (base 8) for cart-cycle gate. |
| GP27     | **HALT_GATE** | Firmware output; drives Q2 gate via R8 to sink /HALT_CART. HIGH = hold /HALT, LOW = release. |
| GP28     | E      | `WAIT 1/0 PIN 20` (base 8) for bus-phase sync |
| Pin 30   | RUN    | CoCo /RESET input via U13 + R9 series + R10 pull‑up |

Pico onboard LED (GP25) is used for heartbeat — no header GPIO spent.
`/CTS` and `/SCS` are **not** separately routed to the Pico; firmware
uses `OE_BUS + A13` to distinguish ROM vs Becker cycles, which freed
GP27 for the /HALT driver.

## 4. Level‑shifter topology

### 4.1 Data bus — U10 (SN74LVC8T245DW)

- `Vccb` = +5 V (cart side, pins A1..A8 face the cart).
- `Vcca` = +3.3 V (Pico side, pins B1..B8 face the Pico).
- `DIR` = buffered `/R/W`. With /R/W high (CoCo reading), the part
  drives A→B (CoCo→Pico). With /R/W low (CoCo writing), B→A (Pico→CoCo).
- `/OE` = output of `U15` (AND gate below). Forces tri‑state when the
  cart is not selected.

Because direction is hardwired to the bus, the Pico never has to drive
a DIR pin. It only controls its own pindirs to decide when to put bits
on the 3.3 V side.

### 4.2 Address + control — U11, U12, U13 (SN74LVC245AD)

All three buffers are wired as one‑way CoCo → Pico. Vcc = 3.3 V,
inputs are 5 V‑tolerant per the LVC family.

- U11 channels 1..8 ← A0..A7 (cart P1[19..26]) → GP8..GP15
- U12 channels 1..6 ← A8..A13 (cart P1[27..31, 37]) → GP16..GP21
- U12 channel 7    ← /R/W (cart P1[18])    — output RW_BUF_RAW → R12 33 Ω → RW_BUF fans to U10 DIR and Pico GP22
- U12 channel 8    ← /CTS (cart P1[32])    — output CTS_BUF → U15 IN_A only
- U13 channel 1    ← /SCS (cart P1[36])    — output SCS_BUF → U15 IN_B only
- U13 channel 2    ← E    (cart P1[6])     — output E_B → U15 IN_C and GP28
- U13 channel 7    ← /RESET (cart P1[5])   — output RESET_DBG drives Pico RUN via R9+R10
- U13 channels 3,4,5,6,8 ← Q, /SLENB, /HALT, /NMI, /CART — inputs only; outputs left unconnected. Add TP pads on these output pins if you later need scope access.

DIR of all three tied high (VCC). /OE of all three tied low (GND) so
outputs are always live.

> Note: channel numbering above is logical. Actual schematic pin
> numbers follow the SN74LVC245 datasheet (A1..A8 on pins 2..9, B1..B8
> on pins 18..11, DIR pin 1, /OE pin 19, VCC pin 20, GND pin 10).

### 4.3 OE gate — U15 (SN74LVC1G11, 3‑input AND)

```
CTS_BUF ─┐
SCS_BUF ─┼─── AND ─── R11 33 Ω ─── OE_BUS ─── U10 /OE, Pico GP26, J_LVC
E_B     ─┘
```

All three inputs are active‑low from the cart (CTS, SCS, E) through
the LVC245 buffers. The AND output (`OE_BUS`) is LOW whenever **all
three** are asserted — i.e., the cart is selected AND the CoCo's E
clock is in the data‑valid phase. This adds the important
qualification that U10 is only enabled during the E‑high window,
preventing the CoCo's pre‑E address‑setup phase from causing a brief
driver conflict on writes.

Propagation delay on the LVC1G11 is ~5 ns typ, negligible against the
~560 ns (CoCo 3 @ 1.79 MHz) or ~1120 ns (CoCo 1/2 @ 0.89 MHz) bus
cycle.

### 4.4 /HALT driver — Q2 (2N7002) + R7 + R8

The Pico can sink /HALT_CART through a 2N7002 N‑FET:

```
GP27 (HALT_GATE) ─── R8 100 Ω ─── Q2 gate
                                      │
                                      ├── R7 100 kΩ ─── +3V3
                                      │
Q2 source ── GND                      │
Q2 drain ── /HALT_CART (cart pin 3, pulled up by R1 4.7 kΩ to +5 V)
```

Polarity summary:
- Pico GP27 HIGH → Q2 ON → /HALT pulled LOW → CoCo CPU halted
- Pico GP27 LOW  → Q2 OFF → /HALT floats HIGH via R1 → CoCo runs
- Pico GP27 high‑Z (boot) → R7 pulls gate high → Q2 ON → /HALT held

Firmware defaults to "asserted" at boot (via R7) and explicitly
drives GP27 LOW after PIO programs are armed. This eliminates the
cold‑start race where a user could type `DOS` before firmware is
ready. After release, firmware can re‑assert by driving GP27 HIGH
for DriveWire flow control of long host operations.

**Decision 2026-09-17: Q2, R7 and R8 stay on the PCB.** The breadboard
bring-up ran with the Pico on USB, so the cold-boot race was never
exercised; the Pico reaches "core1 up, halt released" about 1.16 s after
its own boot, and the CoCo's reset-to-first-$C000-read time was not
measured (breadboard plan step 8, now optional). Keeping the hold costs
three parts on a pin that is already budgeted and doubles as DriveWire
flow control, so it is kept without the measurement.

### 4.5 Reset path — Pico RUN from CoCo /RESET

```
/RESET_CART ─── U13 ch7 ─── RESET_DBG ─── R9 100 Ω ─── Pico RUN (pin 30)
                                                          │
                                                       R10 10 kΩ ── +3V3
```

When a CoCo user presses the RESET button (or the CPU asserts
/RESET), RESET_DBG goes LOW at 3.3 V logic level. R9 limits transient
current; R10 holds RUN HIGH while /RESET is deasserted. This resets
the Pico in sync with the CoCo, clearing Becker FIFO state instead
of leaving it drifted relative to the CoCo's software.

**Open (2026-09-17):** whether to populate this tie at all. Every reset
press reboots the Pico, drops the USB console and re-runs the boot race
that §4.4 then has to win. With the /HALT hold kept, the tie is not
needed for correctness; decide before fab (R9 DNP keeps the option).

## 5. Power

### 5.1 Rails

- **+5 V** — sourced from P1 pin 9. Decoupled by C1 (10 µF) at the
  edge and C11 (10 µF) + C5 (100 nF) local to U10's Vccb pins.
- **+3.3 V** — regulated by U14 (AMS1117‑3.3) from +5 V. 10 µF on
  input (C2), 22 µF on output (C3). Drives:
  - U10 Vcca
  - U11, U12, U13 Vcc
  - U15 Vcc
  - Pico 3V3_EN pull‑up (R4)
  - Q2 gate pull‑up (R7)
  - Pico RUN pull‑up (R10)
- **VSYS_PICO** — +5 V through Schottky D2 (SS14, ~0.3 V drop ≈ 4.7 V),
  feeding only the Pico's VSYS input. The Pico's internal buck‑boost
  then generates its own 3V3_OUT internally (left NC externally).

### 5.2 Pico power wiring

- **VSYS (Pico pin 39)** ← **VSYS_PICO** (+5 V via D2 Schottky). The
  Pico 2's buck‑boost accepts 1.8–5.5 V on VSYS. Feeding VSYS from +5V
  (instead of the former cascade through AMS1117's 3.3 V output)
  avoids inefficient cascade regulation and the brown‑out latch that
  happens when 3V3_EN is tied to the rail the buck is generating.
- **3V3_EN (Pico pin 37)** ← 10 kΩ (R4) to **VSYS_PICO**, pulling it
  high so the Pico's internal regulator enables. Tying EN to VSYS
  (not to +3V3) breaks the latch loop.
- **3V3_OUT (Pico pin 36)** ← **NC**. Do not back‑feed.
- **VBUS (Pico pin 40)** ← NC (no USB host; USB only used for flashing
  firmware, during which the Pico is typically removed from the cart
  or powered via SWD/CDC only).
- **RUN (Pico pin 30)** ← CoCo /RESET via U13 + R9 + R10 (see §4.5).

### 5.3 Decoupling policy

- One 100 nF 0805 at each IC's Vcc pin, within 5 mm, with its own via
  to the GND plane (C4–C10).
- Bulk 10 µF at the edge connector (C1, +5 V), 10 µF local to U10
  Vccb (C11, +5 V), 10 µF at LDO input (C2, +5 V), and 22 µF at the
  LDO output (C3, +3.3 V). C3 must be a vendor rated MLCC‑compatible
  with AMS1117‑3.3 (e.g., AMS1117CD‑3.3 silicon).
- No ferrite beads — not needed at sub‑2 MHz bus rates.

### 5.4 Decoupling loop discipline

Each bypass cap's GND pad gets its own stitching via straight to the
B.Cu GND plane — **never** share a via between multiple caps. The goal
is shortest possible current loop.

## 6. PCB layer stack & rules

| Parameter | Value |
|---|---|
| Layers | 2 (F.Cu, B.Cu) |
| Stack‑up | 1.6 mm FR4, 1 oz copper |
| Min track | 0.2 mm (signal) |
| Power net class track | 0.5 mm (+5V, +3V3, GND traces before the plane takes over) |
| Min via | 0.3 mm drill / 0.6 mm pad |
| Min clearance | 0.15 mm |
| Edge clearance | 0.3 mm (fingers: 0.5 mm from silk) |

Net classes (in `PiCoCo.kicad_pro`):

| Class   | Track | Via (drill/pad) | Nets |
|---------|-------|-----------------|------|
| Default | 0.2 mm | 0.3/0.6 mm     | signals |
| Power   | 0.5 mm | 0.4/0.8 mm     | +5V, +3V3 |

### 6.1 Copper pour plan

- **B.Cu**: full GND pour, assigned to the `GND` net.
- **F.Cu**: components + signals, remaining copper poured to GND.
- Stitching vias: every ~5 mm around the edge connector, under every
  IC, and in the board interior along power traces.

### 6.2 Cartridge edge fingers

- Keep the existing `COCO‑CART‑2.1X1.75` footprint geometry.
- **Fab notes:**
  - **Hard gold** plating on fingers (JLCPCB: "Gold Fingers" option,
    30 µin min).
  - **30° bevel** on leading edge.
  - No silkscreen or soldermask on finger pads (they're on F.Cu and
    B.Cu copper directly, mask pulled back).

## 7. Debug / probe access

### Debug strategy (no big breakout headers)

The v2.2 board intentionally omits J_CART / J_LVC 2×20 debug
breakouts — at 50.8 mm long each, they dominated a 98×55 mm board.
Bring‑up debug happens via:

- **TP1–TP6** SMD pads on the nets most likely to need scope access
  (see the test‑point table below).
- **J_SWD** for SWD flashing / debug of the Pico.
- **Pico USB CDC** — firmware streams bus snapshots and diag logs
  over USB serial during bring‑up (see firmware §10).
- **Cart‑edge fingers** — clip directly onto the edge when a signal
  isn't on a test pad.

If a particular U13‑buffered signal (/HALT_DBG, /NMI_DBG, Q_DBG,
/SLENB_DBG, /CART_DBG) ever needs scope access, add a TP pad on the
corresponding U13 output pin in `tools/gen_schematic.py`; the
buffers still run even though their outputs are otherwise
unconnected.

### J_SWD (Pico SWD)

4‑pin 0.1" header: SWCLK, SWDIO, GND, 3V3. Mates with Raspberry Pi
Debug Probe ribbon cable or picoprobe jumpers.

### Test points

1×1 mm SMD pads, silk‑labeled, for oscilloscope and logic‑analyzer
access during bring‑up:

| Ref | Net | Purpose |
|---|---|---|
| TP1 | OE_BUS | Cart‑selected signal (U15 output post‑termination) |
| TP2 | RW_BUF | Buffered /R/W at U12 output post‑termination |
| TP3 | CTS_BUF | /CTS after U12 (before U15) |
| TP4 | SCS_BUF | /SCS after U13 (before U15) |
| TP5 | E_B | Buffered E clock (same as Pico GP28) |
| TP6 | +3V3 | 3.3 V rail |

(+5 V can be probed at P1 pin 9 or C1; GND is ubiquitous via edge
fingers and IC grounds.)

### LEDs

- No external LED on the board — a power/heartbeat indicator is
  invisible inside a cartridge case, and leaving it out saves a
  part and a few mA of idle current off +3.3 V. Heartbeat indication
  uses the Pico's onboard LED (GP25, internal), controlled by firmware.
  Bare‑board "is +3.3 V up?" checks use TP6 on a DMM.

## 8. Build notes

- Hand‑assembly order (helps self‑test): U14 + C2 + C3 → verify 3.3 V
  with no other parts → D2 → verify VSYS_PICO ≈ 4.7 V with 5 V
  applied → U10–U13 + C4–C11 → U15 → Q2 + R7/R8 → passives → edge
  headers → JP1 + TP pads → Pico last.
- JLCPCB fab order: 2‑layer, 1.6 mm, HASL on SMD pads, **Gold Fingers
  enabled**, 30° bevel, **Hard Gold, 1 µm minimum** (ENIG is NOT
  acceptable for CoCo slot fingers — it wears quickly on the slot's
  bronze wipers). See `fab/READ-BEFORE-ORDERING.txt` for the full
  JLCPCB order checklist.
- SMT assembly (optional): all ICs and 0805 passives are JLC Basic
  Library; Q2 (2N7002) and D2 (SS14) are Extended. Verify JLC's
  rotation CSV matches the KiCad project.

## 9. Known limitations / v2 ideas

- Pico cannot drive `/NMI` or `/RESET` *into* the CoCo on MVP. /HALT
  drive is now covered by Q2 + GP27. v2: add an SN74LVC07A open‑drain
  buffer next to U13 for /NMI and /RESET drive (consumes 2 of the
  debug channels and requires a GPIO freed by an IO expander or CPLD).
- `/SLENB` is not in the U15 /OE gate, so CoCo 3 RAM/ROM toggle could
  fight the cart. v2: upgrade U15 to a 4‑input combinational gate
  (74LVC1G332 mux) or cascade two 1G11s to include /SLENB.
- `Q` clock is not routed to the Pico — all 26 GPIOs consumed. v2:
  replace U11 (A0–A7 buffer, 74LVC245A) with a 74LVC573 transparent
  D‑latch clocked by Q for pre‑settled addresses on CoCo 3 1.79 MHz.
  Not pin‑compatible; requires layout rework.
- Pico cannot drive `/CART` FIRQ back to the CoCo. v2: add another
  N‑FET controlled by a freed Pico GPIO for async Becker‑has‑data
  interrupts.
- No fuse / TVS / reverse‑polarity protection on the +5 V cart input.
  Matches CoCo convention (original Tandy carts have no such
  protection; users know to power off before inserting carts).

## 10. Cross‑references

- Firmware architecture: [`firmware-architecture.md`](firmware-architecture.md)
- KiCad regen workflow: [`kicad-workflow.md`](kicad-workflow.md)
- CoCo Technical Reference Manual (cart pinout): §3
- DriveWire specs: Cloud9 `DriveWire 3 Specification.pdf`; `github.com/boisy/DriveWire/wiki/DriveWire-Specification`
- Level‑shifter selection rationale: `bigmessowires.com/2023/08/22/a-tale-of-three-bidirectional-level-shifters/`
- JLCPCB fab checklist: `fab/READ-BEFORE-ORDERING.txt`

