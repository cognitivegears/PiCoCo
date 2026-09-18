# PiCoCo — Hardware Design

PiCoCo is a Tandy Color Computer (CoCo 1/2/3) cartridge that hosts a
Raspberry Pi Pico 2 (RP2350), or optionally a Waveshare RP2350B-Plus-W
soldered flat on the same carrier footprint, and presents two behaviors
to the CoCo:

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
         ├──► U10/U11/U12/U13 VCC, U15 VCC
         └──► Pico 3V3_EN pull-up

             CoCo 5V side          │ level shifters │  3V3 Pico side
             ─────────────         │ ─────────────  │  ──────────────
  D0..D7 ──── P1[10..17] ◄──► [U10 SN74LVC245A @3.3V, cart on B] ◄──► GP0..GP7
  A0..A7 ──── P1[19..26]  ──► [U11 SN74LVC245A]  ──► GP8..GP15
  A8..A13──── P1[27..31,37]──► [U12 SN74LVC245A]  ──► GP16..GP21
  /R/W   ──── P1[18]      ──► [U12 ch7]          ──► R12 33Ω ──► GP22, U10 DIR
  /CTS   ──── P1[32]      ──► [U12 ch8] → CTS_BUF ──► U15 gate 1
  /SCS   ──── P1[36]      ──► [U13 ch1] → SCS_BUF ──► U15 gate 1
  E      ──── P1[6]       ──► [U13 ch2] → E_BUF   ──► U15 gate 2, JP3 pad 1
  /RESET ──── P1[5]       ──► [U13 ch7] → RESET_BUF──► R9 100Ω ──► Pico RUN (R10 10kΩ pull-up to +3V3)
  Q, /SLENB, /HALT, /NMI, /CART ──► [U13 input-only; outputs no-connect]
  SND    ──── P1[35]      ◄── R21/R22 ◄── 2-pole RC ◄── AUDIO_PWM (JP3: hdr34 on Pico 2 / GP34 on Plus-W)

  Hardware /OE gate (U15 74LVC00, two gates wired NAND-NAND):
    CTS_BUF ─┐
    SCS_BUF ─┴─NAND──► SEL_N ─┐
                     E_BUF ────┴─NAND──► OE_BUS_RAW ──► R11 33Ω ──► OE_BUS ──► JP2 pad 1 (→ U10 /OE), Pico header pin 31, TP1

  Pico /HALT drive (hold-until-booted + DriveWire flow control):
    GP27 (HALT_GATE) ──► R8 100Ω ──► [Q2 2N7002] ──► /HALT_CART pin
                         R7 100kΩ to +3V3 pulls gate high during Pico boot
                         → Q2 ON → /HALT held low until firmware releases
```

Reserved: the top-right corner of the board (about 25 x 15 mm) is kept
clear of components and dense routing for a future HDMI-A receptacle
(Plus-W only — see spec §10, `docs/superpowers/specs/2026-09-17-main-board-v2.3-design.md`).

Direction of `U10` is driven by the **buffered /R/W** wire (same net
that reaches Pico GP22). `/OE` of `U10` is driven by `U15`'s NAND-NAND
decode through JP2 (default position 1-2), so `U10` is tri-stated
unless **all** of: (a) `/CTS` OR `/SCS` is asserted (i.e., cart
selected) AND (b) E is high (data-valid phase).

Firmware distinguishes ROM reads ($C000–$DFFF) from Becker accesses
($FF40–$FF5F) by inspecting **A13** when `OE_BUS` is asserted: A13=0
means /CTS range, A13=1 means /SCS range. This saves a GPIO compared
to routing both /CTS and /SCS separately.

## 2. Component list

| Ref | Part | Package | Role |
|---|---|---|---|
| U1  | Raspberry Pi Pico 2 (RP2350) or Waveshare RP2350B-Plus-W | SMD+TH module (`PiCoCo:Pico-Carrier`) | MCU |
| U10 | SN74LVC245ADWR | SOIC‑20W | D0–D7 buffer, A side (pins 2..9) = Pico, B side (18..11) = cart |
| U11 | SN74LVC245ADWR | SOIC‑20W | A0–A7 buffer (5V→3.3V) |
| U12 | SN74LVC245ADWR | SOIC‑20W | A8–A13, /R/W, /CTS buffer |
| U13 | SN74LVC245ADWR | SOIC‑20W | /SCS, E, Q, /SLENB, /HALT, /NMI, /RESET, /CART buffer |
| U14 | AMS1117‑3.3  | SOT‑223  | +5V → +3.3V LDO (supplies buffers only) |
| U15 | SN74LVC00AD  | SOIC‑14 | Quad NAND, two gates used NAND-NAND: `SEL_N = NAND(CTS_BUF,SCS_BUF)`, `OE_BUS_RAW = NAND(SEL_N,E_BUF)`; gates 3/4 grounded |
| JP2 | Solder jumper, 3-pad bridged 1-2 | SMD | U10 `/OE` source: 1=OE_BUS (default), 2=U10 /OE, 3=OE_FW (Plus-W firmware /OE) |
| JP3 | Solder jumper, 3-pad bridged 1-2 | SMD | Header pin 34: 1=E_BUF (default), 2=PICO_P34, 3=AUDIO_PWM (Pico 2 audio option) |
| Q2  | 2N7002 | SOT‑23 | N‑FET: firmware‑controlled /HALT sink (hold at boot) |
| Q3, Q4 | 2N7002 (DNP) | SOT‑23 | N‑FET stages for Plus-W-driven /NMI, /CART (pad-grid `NMI_DRV`/`CART_DRV`) |
| D2  | Schottky SS14 | SMA | +5 V → VSYS_PICO (replaces cascade via LDO) |
| R1  | 4.7 kΩ 0805 | — | /HALT pull‑up to +5 V (required; idles /HALT when Q2 off) |
| R2, R3 | 4.7 kΩ 0805 (DNP) | — | /NMI, /RESET pull‑up footprints; CoCo already pulls these up, so DNP by default |
| R4  | 10 kΩ 0805 | — | Pico 3V3_EN pull‑up to VSYS_PICO |
| R7  | 100 kΩ 0805 | — | Q2 gate pull‑up to +3V3 (default‑on during Pico boot) |
| R8  | 100 Ω 0805 | — | Q2 gate series from GP27 |
| R9  | 100 Ω 0805 | — | Pico RUN series from RESET_BUF |
| R10 | 10 kΩ 0805 | — | Pico RUN pull‑up to +3V3 |
| R11 | 33 Ω 0805 | — | OE_BUS series termination (fan‑out damping) |
| R12 | 33 Ω 0805 | — | RW_BUF series termination |
| R15, R16 | 100 Ω 0805 (DNP) | — | Q3/Q4 gate series from NMI_DRV/CART_DRV |
| R17, R18 | 100 kΩ 0805 (DNP) | — | Q3/Q4 gate pull‑downs to GND (released when nothing drives them) |
| R19, R20 | 1 kΩ 0805 | — | Audio 2-pole RC filter stage |
| R21 | 2.2 kΩ 0805 | — | Audio level divider into SND_CART |
| R22 | 1 kΩ 0805 | — | Audio level divider to GND |
| C1  | 10 µF 0805 X5R ≥10V | — | +5V bulk at edge connector |
| C2  | 10 µF 0805 X5R ≥10V | — | LDO input |
| C3  | 22 µF 0805 X5R ≥6.3V | — | LDO output (MLCC‑compatible vendor required) |
| C4, C6–C10 | 100 nF 0805 | — | Per‑IC decoupling (U10, U11–U13, U15, Pico local) |
| C12 | 1000 µF 6.3V SMD electrolytic, D8x10 (DNP) | — | VSYS_PICO bulk cap for Plus-W Wi-Fi transmit bursts |
| C13, C14 | 10 nF 0805 | — | Audio 2-pole RC filter stage |
| C15 | 100 nF 0805 (DNP) | — | Optional AC-coupling across R21 |
| P1  | COCO‑CART‑2.1X1.75 (custom footprint) | Edge fingers | Cartridge slot mate |
| TP1–TP7 | 1×1 mm SMD pads | — | OE_BUS, RW_BUF, CTS_BUF, SCS_BUF, E_BUF, +3V3, SND_CART |
| FID1, FID2 | 1 mm fiducial, 2 mm mask | — | SMT assembly fiducials, no net, excluded from BOM |

**Unique active parts: 7** (Pico 2 / Plus-W module, SN74LVC245A, AMS1117‑3.3,
SN74LVC00A, 2N7002, SS14 Schottky). All are in the JLCPCB Basic or
Extended Library. All passives are 0805; DNP refs (C12, C15, Q3, Q4, R2, R3,
R15–R18) are populated footprints on the board but not fitted by default.
No `J_SWD` header and no `MTG1` mounting hole on this board (see §7 and §9).

## 3. Signal map

### 3.1 Cartridge edge connector (P1, `COCO-CART-2.1X1.75`)

40 fingers, 0.1" pitch, 2.1" × 1.75". Pin numbering per CoCo Technical
Reference Manual.

| Pin | Signal  | Direction (cart‑side) | Destination on board |
|-----|---------|-----------------------|----------------------|
| 1   | -12 V   | unused                | NC |
| 2   | +12 V   | unused                | NC |
| 3   | /HALT   | bidi (open‑drain) on CoCo | U13 in + R1 4.7 kΩ pull‑up to +5 V + Q2 drain (Pico‑controlled pull‑down) |
| 4   | /NMI    | bidi (open‑drain)     | U13 in + R2 4.7 kΩ pull‑up (DNP) + Q3 drain (DNP, Plus-W drive stage) |
| 5   | /RESET  | bidi (open‑drain)     | U13 in + R3 4.7 kΩ pull‑up (DNP); buffered copy (RESET_BUF) drives Pico RUN via R9+R10 |
| 6   | E       | CoCo → cart           | U13 → E_BUF → U15 gate 2, JP3 pad 1 (default: header pin 34) |
| 7   | Q       | CoCo → cart           | U13 in only; output left unconnected (not routed to Pico — saves one GPIO) |
| 8   | /CART   | cart → CoCo (open collector) | U13 in only + Q4 drain (DNP, Plus-W drive stage). No pull-up populated by default — HDB-DOS autostarts on the DK signature with /CART open. |
| 9   | +5 V    | power                 | C1 bulk, U14 in, D2 Schottky anode (→ VSYS_PICO), R1 pull‑up |
| 10–17 | D0–D7 | bidirectional         | U10 B‑side |
| 18  | /R/W    | CoCo → cart           | U12 in; U12 output → R12 33 Ω → U10 DIR + GP22 |
| 19–26 | A0–A7 | CoCo → cart           | U11 in → GP8–GP15 |
| 27–31 | A8–A12 | CoCo → cart          | U12 in → GP16–GP20 |
| 32  | /CTS    | CoCo → cart           | U12 in → CTS_BUF → U15 gate 1 (NOT routed to Pico — firmware uses OE_BUS + A13) |
| 33, 34 | GND  | ground                | GND plane |
| 35  | SND     | cart → CoCo (audio)   | R21 2.2 kΩ / R22 1 kΩ divider ← 2-pole RC ← AUDIO_PWM (see §4.6); DC-coupled, inert when nothing drives AUDIO_PWM |
| 36  | /SCS    | CoCo → cart           | U13 in → SCS_BUF → U15 gate 1 (NOT routed to Pico — firmware uses OE_BUS + A13) |
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
consumed in the current design. The `PiCoCo:Pico-Carrier` footprint/symbol
also has 15 hidden underside pads used only when a Waveshare
RP2350B-Plus-W is soldered flat instead (NC on a Pico 2 build) — see the
Plus-W column and the pad-grid table below.

| GPIO | Header pin | Plus-W header label | Signal | Notes |
|---|---|---|---|---|
| GP0–GP7  | 1,2,4,5,6,7,9,10 | same | D0–D7 | Contiguous for `OUT PINS, 8` / `IN PINS, 8` |
| GP8–GP15 | 11,12,14–20 | same | A0–A7 | — |
| GP16–GP21| 21,22,24–27 | same | A8–A13 | — (GP8..GP21 contiguous = 14‑bit address) |
| GP22     | 29 | same | /R/W   | 15th bit of the address IN word; `IN PINS, 15` at base 8 grabs A0–A13 + /R/W in one cycle |
| GP23–GP25| — | same | *internal* | SMPS PS / VBUS sense / onboard LED (not header‑accessible) |
| GP26     | 31 | GP26/GP40 | **OE_BUS** | Cart-selected signal from U15's NAND-NAND decode, through JP2 (default 1-2). `WAIT 0 PIN 18` (base 8) for cart-cycle gate. |
| GP27     | 32 | GP27/GP41 | **HALT_GATE** | Firmware output; drives Q2 gate via R8 to sink /HALT_CART. HIGH = hold /HALT, LOW = release. |
| GP28     | 34 | GP28/GP42 | E (default via JP3 1-2) | `WAIT 1/0 PIN 20` (base 8) for bus-phase sync. JP3 2-3 repurposes this pin as `AUDIO_PWM` on a Pico 2 (see §4.6); firmware does not use E today. |
| Pin 30   | 30 | same | RUN    | CoCo /RESET input via U13 + R9 series + R10 pull‑up |

Pico onboard LED (GP25) is used for heartbeat — no header GPIO spent.
`/CTS` and `/SCS` are **not** separately routed to the Pico header pins;
firmware uses `OE_BUS + A13` to distinguish ROM vs Becker cycles, which
freed GP27 for the /HALT driver.

**Plus-W pad grid (NC on a Pico 2 build).** 15 pads at 2.54 mm pitch,
1.5 mm square (1.8 mm carrier pads, except 1.4 mm at GP29/GP32/GP35 to
clear the Pico 2's own debug rings), measured per `docs/RP2350B_IDEAS.md`
§13.4. GP24..GP30 are contiguous so one `gpio_in` read on a Plus-W
captures all seven at once.

| Pad-grid pin | Net | Purpose (Plus-W only; NC on a Pico 2) |
|---|---|---|
| GP24 | CTS_BUF | capture |
| GP25 | SCS_BUF | capture |
| GP26 | E_BUF | capture (same net also reaches the module via header pin 34 by default) |
| GP27 | Q_BUF | capture |
| GP28 | SLENB_BUF | capture |
| GP29 | A14_BUF | capture |
| GP30 | A15_BUF | capture |
| GP31 | OE_FW | JP2 alternate (2-3): firmware-driven U10 /OE |
| GP32 | NMI_DRV | Q3 gate (DNP stage) |
| GP33 | CART_DRV | Q4 gate (DNP stage) |
| GP34 | AUDIO_PWM | sound output stage (§4.6) |
| GP35, GP43, GP44, GP45 | no-connect | spare |

This board carries no on-carrier SWD header (`J_SWD` was dropped — its
footprint at header pins 41-43 collides with this pad grid). Debug the
module through its own castellations/pads, or the test points in §7.

## 4. Level‑shifter topology

### 4.1 Data bus — U10 (SN74LVC245ADWR)

- `VCC` = +3.3 V (single supply — this is an LVC245A, not a dual-rail
  level shifter). A side (pins 2..9, A1..A8) = Pico `D0`..`D7`. B side
  (pins 18..11, B1..B8) = cart `D0_CART`..`D7_CART`. The LVC family's
  inputs are 5 V‑tolerant, so the B side survives the CoCo's 5 V logic
  without a second supply rail.
- `DIR` (pin 1) = `RW_BUF` directly. RW high (CoCo read) drives A→B,
  putting the Pico's byte onto the cart bus. RW low (CoCo write) drives
  B→A, delivering the cart's byte to the Pico. Since A is the Pico side
  and B is the cart side, RW's own sense picks the right direction
  without any Pico involvement.
- `/OE` (pin 19) = `U10_OE`, the JP2 centre pad — `OE_BUS` by default
  (JP2 1-2), or a Plus-W firmware GPIO (`OE_FW`, JP2 2-3). Forces
  tri‑state when the cart is not selected.

Because direction is hardwired to the buffered `/R/W`, the Pico never
has to drive a DIR pin. It only controls its own pindirs to decide when
to put bits on the 3.3 V side.

### 4.2 Address + control — U11, U12, U13 (SN74LVC245ADWR)

All three buffers are wired as one‑way CoCo → Pico, VCC = 3.3 V,
inputs 5 V‑tolerant per the LVC family. Same SOIC-20W part and pinout
as U10 (A1..A8 = pins 2..9, B1..B8 = pins 18..11, DIR = pin 1, /OE =
pin 19, VCC = pin 20, GND = pin 10).

- U11: A1..A8 = `A0_CART`..`A7_CART` (cart P1[19..26]); B1..B8 =
  `A0_BUF`..`A7_BUF` → GP8..GP15.
- U12: A1..A6 = `A8_CART`..`A13_CART` (cart P1[27..31, 37]), A7 =
  `RW_CART` (cart P1[18]), A8 = `CTS_CART` (cart P1[32]); B1..B6 =
  `A8_BUF`..`A13_BUF` → GP16..GP21, B7 = `RW_BUF_RAW` → R12 33 Ω →
  `RW_BUF` (fans to U10 DIR and Pico GP22), B8 = `CTS_BUF` → U15 gate 1
  only (not routed to the Pico header).
- U13: A1 = `SCS_CART`, A2 = `E_CART`, A3 = `Q_CART`, A4 =
  `SLENB_CART`, A5 = `RESET_CART`, A6 = `A14_CART`, A7 = `A15_CART`,
  A8 = GND; B1 = `SCS_BUF` → U15 gate 1 only, B2 = `E_BUF` → U15 gate 2
  and JP3 pad 1 (default: header pin 34), B3 = `Q_BUF`, B4 =
  `SLENB_BUF`, B5 = `RESET_BUF` → R9 100 Ω → Pico RUN (R10 pull‑up), B6
  = `A14_BUF`, B7 = `A15_BUF`, B8 no-connect. /HALT, /NMI and /CART are
  not buffered as inputs at all (see §5.1's cart-pin table). `Q_BUF`
  and `SLENB_BUF` are input-only from the Pico header's point of view —
  they only reach the Plus-W pad grid, not a Pico 2 header pin.

DIR of all three tied high (`+3V3`). `/OE` of all three tied low
(`GND`) so outputs are always live.

### 4.3 OE gate — U15 (SN74LVC00A, NAND-NAND decode)

```
CTS_BUF ─┐
SCS_BUF ─┴── NAND ── SEL_N ─┐
                    E_BUF ──┴── NAND ── OE_BUS_RAW ── R11 33 Ω ── OE_BUS ── JP2 pad 1 (→ U10 /OE), Pico header pin 31, TP1
```

U15 is a quad 2-input NAND (74LVC00A); only two of its four gates are
used. Gate 1 takes `CTS_BUF` and `SCS_BUF` and produces `SEL_N` — low
whenever either `/CTS` or `/SCS` is asserted (cart selected). Gate 2
takes `SEL_N` and `E_BUF` and produces `OE_BUS_RAW` — low only when the
cart is selected **and** E is high (data-valid phase), i.e.
`OE_BUS = NAND(NAND(CTS_BUF,SCS_BUF), E_BUF)`, the same truth table the
old 3-input AND implemented, built from two NAND stages instead of one
AND. Gates 3 and 4 are unused: their inputs are tied to `GND` and their
outputs left no-connect. This adds the important qualification that
U10 is only enabled during the E‑high window, preventing the CoCo's
pre‑E address‑setup phase from causing a brief driver conflict on
writes.

Propagation delay on the LVC00A is a few ns typ per gate, negligible
against the ~560 ns (CoCo 3 @ 1.79 MHz) or ~1120 ns (CoCo 1/2 @
0.89 MHz) bus cycle even through two cascaded gates.

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
/RESET_CART ─── U13 B5 ─── RESET_BUF ─── R9 100 Ω ─── Pico RUN (pin 30)
                                                          │
                                                       R10 10 kΩ ── +3V3
```

When a CoCo user presses the RESET button (or the CPU asserts
/RESET), RESET_BUF goes LOW at 3.3 V logic level. R9 limits transient
current; R10 holds RUN HIGH while /RESET is deasserted. This resets
the Pico in sync with the CoCo, clearing Becker FIFO state instead
of leaving it drifted relative to the CoCo's software.

**Decided 2026-09-17: populate R9, keep the tie.** A CoCo reset must
reach the Pico definitively, and the video design (RP2350B_IDEAS §4.5)
keys its shadow-state handling off the chip reset reason: RUN-pin reset
means "zero the shadow, the CoCo ROM repopulates it", watchdog or
software reset means "keep it". Doing that over RUN costs no GPIO, which
the Pico 2 does not have spare and the RP2350B budget does not either.
Costs accepted: every reset press reboots the Pico (about 1.2 s to
"halt released") and drops the USB console; the /HALT hold in §4.4 is
what makes that reboot safe for the CoCo.

### 4.6 Sound stage — AUDIO_PWM to SND_CART

Cart pin 35 (`SND_CART`) is an analog input the CoCo mixes into its own
audio path when `AUDIO ON` selects the cart (the same input the
Orchestra-90 and Speech/Sound Pak use). This stage is populated by
default, all Basic 0805 parts, and inert when nothing drives
`AUDIO_PWM`:

```
AUDIO_PWM ── R19 1k ── AUDIO_F1 ── C13 10nF to GND
                            │
                        R20 1k ── AUDIO_F2 ── C14 10nF to GND
                                        │
                                    R21 2.2k ── SND_CART ── R22 1k ── GND
```

- **JP3** (pad 1 = `E_BUF`, pad 2 = `PICO_P34` = module header pin 34,
  pad 3 = `AUDIO_PWM`) is bridged 1-2 by default, so header pin 34
  carries E as it always has. On a **Pico 2** build, where no GPIO is
  spare, cut 1-2 and bridge 2-3 to turn GP28 into the PWM audio output
  instead — firmware does not use E today (`OE_BUS` is already
  E‑qualified in hardware; E was reserved for the v2 PIO engine, a
  Plus-W-only path).
- **Plus-W**: the pad-grid's own GP34 drives `AUDIO_PWM` directly; JP3
  stays at its default (1-2). Do not set JP3 to 2-3 on a Plus-W with
  GP34 populated — that puts two outputs on one net. The silkscreen
  says so.
- The two-pole RC (R19/C13, R20/C14) rolls off around 16 kHz. R21/R22
  form a DC-coupled divider to about 1 V full scale, the same way the
  Orchestra-90 fed this input — this is the level knob: tune the exact
  values against a real CoCo on the first board and record them here.
  C15 (0805, DNP) sits in parallel with R21 for AC coupling if that
  turns out to work better.
- TP7 is on `SND_CART` for scope access.
- What this enables on **both** builds: any sound the Pico synthesizes
  itself, driven by firmware using PiCoCo's own registers in the /SCS
  window (a PSG at unused `$FF5x` addresses, DriveWire-side sound
  commands, streamed audio). Emulating an *existing* sound cart by
  capturing its writes (Orchestra-90 `$FF7A`/`$FF7B`, Speech/Sound Pak
  `$FF7D`/`$FF7E`) is **Plus-W only** — those addresses fall outside
  `/CTS` and `/SCS`, so it needs JP2 in the firmware-/OE position plus
  A14/A15 from the pad grid.

## 5. Power

### 5.1 Rails

- **+5 V** — sourced from P1 pin 9. Decoupled by C1 (10 µF) at the edge.
- **+3.3 V** — regulated by U14 (AMS1117‑3.3) from +5 V. 10 µF on
  input (C2), 22 µF on output (C3). U10–U13 are single-supply LVC245A
  parts at 3.3 V (no separate 5 V rail on the buffers). Drives:
  - U10, U11, U12, U13 VCC
  - U15 VCC
  - Pico 3V3_EN pull‑up (R4)
  - Q2 gate pull‑up (R7)
  - Pico RUN pull‑up (R10)
- **VSYS_PICO** — +5 V through Schottky D2 (SS14, ~0.3 V drop ≈ 4.7 V),
  feeding only the Pico's VSYS input. The Pico's internal buck‑boost
  then generates its own 3V3_OUT internally (left NC externally). C12
  (1000 µF 6.3 V SMD electrolytic, DNP) is a footprint on `VSYS_PICO`
  for a Plus-W build's Wi-Fi transmit-burst droop; DNP on a Pico 2.

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
  or debugged via its own debug pads / USB CDC only — there is no
  on-carrier SWD header, see §7).
- **RUN (Pico pin 30)** ← CoCo /RESET via U13 + R9 + R10 (see §4.5).

### 5.3 Decoupling policy

- One 100 nF 0805 at each IC's VCC pin, within 5 mm, with its own via
  to the GND plane (C4, C6–C10 — six ICs; C5 is deliberately skipped,
  not reused).
- Bulk 10 µF at the edge connector (C1, +5 V), 10 µF at LDO input (C2,
  +5 V), and 22 µF at the LDO output (C3, +3.3 V). C3 must be a vendor
  rated MLCC‑compatible with AMS1117‑3.3 (e.g., AMS1117CD‑3.3 silicon).
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
| Default netclass track | 0.20 mm |
| Default netclass via | 0.8 mm pad / 0.4 mm drill (a few 0.6/0.3 mm GND stitching vias) |
| Default netclass clearance | 0.15 mm |
| Design-rule floors | min track 0.127 mm, min clearance 0.127 mm, copper-to-edge 0.2 mm, hole clearance 0.25 mm |

There is **no separate power netclass**. `+5V` and `VSYS_PICO` run at
the same 0.20 mm as everything else — fine for the cartridge's 300 mA
budget at 1 oz copper (roughly 0.5 A at a 10 °C rise for a 0.20 mm
trace). Net classes as actually set in `PiCoCo.kicad_pro`:

| Class   | Track | Via (pad/drill) | Nets |
|---------|-------|-----------------|------|
| Default | 0.20 mm | 0.8/0.4 mm     | everything, including power |

### 6.1 Copper pour plan

- **B.Cu and F.Cu**: full GND pour on both layers, board outline
  inset 0.3 mm, cut 1 mm above the edge fingers so the pour never
  touches the gold-fingered area. Built by `tools/pour.py`.
- Stitching vias: near each buffer's GND pin and along the board
  interior; one U11 pad (pad 10, GND) has zone connection "none" —
  it's tied in by track + via instead of a thermal spoke.
- See `docs/kicad-workflow.md` §"Routing" for the full autoroute →
  grid-route → pour → gnd-fix pipeline that produced the routed board.

### 6.2 Cartridge edge fingers

- Keep the existing `COCO‑CART‑2.1X1.75` footprint geometry.
- **Fab notes:**
  - **Hard gold** plating on fingers (JLCPCB: "Gold Fingers" option,
    30 µin min).
  - **30° bevel** on leading edge.
  - No silkscreen or soldermask on finger pads (they're on F.Cu and
    B.Cu copper directly, mask pulled back).

## 7. Debug / probe access

### Debug strategy (no big breakout headers, no SWD header)

The v2.2 board omitted J_CART / J_LVC 2×20 debug breakouts — at
50.8 mm long each, they dominated a 98×55 mm board. v2.3 also drops
the 1×4 `J_SWD` header: its pads at header pins 41-43 collide with the
Plus-W pad grid, and a Pico 2's own SWD castellations can't be exposed
on this footprint either way. Bring‑up debug happens via:

- **TP1–TP7** SMD pads on the nets most likely to need scope access
  (see the test‑point table below).
- **The module's own debug pads/castellations**, or its USB CDC, for
  SWD-style debug of the Pico itself. A flat-mounted Pico 2 rests its
  three underside debug pads (SWCLK/GND/SWDIO, 1.7 mm) on the carrier's
  GP29/GP32/GP35 grid pads (1.4 mm, ~0.05 mm overlap): tape those three
  grid pads before soldering a Pico 2 flat, or use headers. The Plus-W
  uses the grid and has no such pads.
- **Pico USB CDC** — firmware streams bus snapshots and diag logs
  over USB serial during bring‑up (see firmware §10).
- **Cart‑edge fingers** — clip directly onto the edge when a signal
  isn't on a test pad.

If a particular U13‑buffered signal (`Q_BUF`, `SLENB_BUF`) ever needs
scope access on a Pico 2 build, add a TP pad on the corresponding U13
output pin in `tools/gen_schematic.py`; the buffers still run even
though their outputs are otherwise unconnected on a Pico 2 header
(they do reach the Plus-W pad grid already).

### Test points

1×1 mm SMD pads, silk‑labeled, for oscilloscope and logic‑analyzer
access during bring‑up:

| Ref | Net | Purpose |
|---|---|---|
| TP1 | OE_BUS | Cart‑selected signal, post-termination (U15's decode through R11) |
| TP2 | RW_BUF | Buffered /R/W at U12 output |
| TP3 | CTS_BUF | /CTS after U12 (before U15) |
| TP4 | SCS_BUF | /SCS after U13 (before U15) |
| TP5 | E_BUF | Buffered E clock (JP3 default: same as header pin 34) |
| TP6 | +3V3 | 3.3 V rail |
| TP7 | SND_CART | Sound stage output to the cart (§4.6) |

(+5 V can be probed at P1 pin 9 or C1; GND is ubiquitous via edge
fingers and IC grounds.)

### Fiducials

FID1 and FID2 (`Fiducial:Fiducial_1mm_Mask2mm`) are SMT-assembly
fiducial marks in two open, copper-free areas of the board — no net,
excluded from the BOM. They're for the assembler's placement-machine
vision, not for hand debug.

### LEDs

- No external LED on the board — a power/heartbeat indicator is
  invisible inside a cartridge case, and leaving it out saves a
  part and a few mA of idle current off +3.3 V. Heartbeat indication
  uses the Pico's onboard LED (GP25, internal), controlled by firmware.
  Bare‑board "is +3.3 V up?" checks use TP6 on a DMM.

## 8. Build notes

- Hand‑assembly order (helps self‑test): U14 + C2 + C3 → verify 3.3 V
  with no other parts → D2 → verify VSYS_PICO ≈ 4.7 V with 5 V
  applied → U10–U13 + C4, C6–C10 → U15 → JP2/JP3 (leave at default
  1-2) → Q2 + R7/R8 → passives → edge fingers → TP pads → module last.
  Q3, Q4, R2, R3, R15–R18, C12, C15 are DNP by default (Plus-W-only or
  optional provisions) — skip them on a Pico 2 build.
- JLCPCB fab order: 2‑layer, 1.6 mm, **ENIG** surface finish (all pads
  gold; fingers still get hard gold below, never HASL), **Gold Fingers
  enabled**, 30° bevel (both critical for the CoCo slot's bronze
  wipers). A budget variant (bare boards, no assembly) can drop gold
  fingers/bevel and take ENIG everywhere — fine for light hobbyist use.
  See `fab/main/READ-BEFORE-ORDERING.txt` for the full checklist.
- SMT assembly (optional): U10–U13 (SN74LVC245A) and U15 (SN74LVC00A)
  are JLC Extended; U14, Q2/Q3/Q4, D2 and all 0805 passives are Basic.
  Files: `fab/main/PiCoCo-BOM-jlc.csv`, `fab/main/PiCoCo-CPL-jlc.csv`.
  Check JLC's placement preview against the rotation table in
  `tools/jlc_post.py` before ordering (unverified as of this writing —
  see `docs/kicad-workflow.md` §"JLCPCB files").

## 9. Known limitations / v2 ideas

- `/SLENB` is not in the U15 decode, so CoCo 3 RAM/ROM toggle could
  fight the cart. v2: upgrade U15 to a 4‑input combinational gate
  (74LVC1G332 mux) or cascade more NAND stages to include `/SLENB`.
- No fuse / TVS / reverse‑polarity protection on the +5 V cart input.
  Matches CoCo convention (original Tandy carts have no such
  protection; users know to power off before inserting carts).
- Driving `/NMI`, `/CART` and `/OE` from firmware, and reading A14/A15
  and `Q`, are now provisioned in hardware (Q3/Q4 DNP stages, JP2, and
  the full U13 channel set) but only reachable from the Plus-W pad
  grid — a Pico 2 build has no spare GPIO to use them. Firmware support
  for the pad grid is itself out of scope for this spin (spec §1,
  `docs/superpowers/specs/2026-09-17-main-board-v2.3-design.md`); see
  `docs/firmware-architecture.md`'s Plus-W pin plan.

## 10. Cross‑references

- Firmware architecture: [`firmware-architecture.md`](firmware-architecture.md)
- KiCad regen workflow: [`kicad-workflow.md`](kicad-workflow.md)
- v2.3 design spec: `docs/superpowers/specs/2026-09-17-main-board-v2.3-design.md`
- CoCo Technical Reference Manual (cart pinout): §3
- DriveWire specs: Cloud9 `DriveWire 3 Specification.pdf`; `github.com/boisy/DriveWire/wiki/DriveWire-Specification`
- Level‑shifter selection rationale: `bigmessowires.com/2023/08/22/a-tale-of-three-bidirectional-level-shifters/`
- JLCPCB fab checklist: `fab/main/READ-BEFORE-ORDERING.txt`

