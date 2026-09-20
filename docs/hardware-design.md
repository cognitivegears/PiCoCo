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
  /RESET ──── P1[5]       ──► [U13 ch7] → RESET_BUF──► R9 100Ω ──► Pico RUN (R10 10kΩ pull-up to +3V3, DNP)
  Q, /SLENB ──────────────► [U13 input-only; outputs no-connect]
  /HALT, /NMI, /CART      ──► NOT buffered through U13 at all — they go straight to the
                              Q2/Q3/Q4 N-FET drive stages and their own R1/R2 pull-ups (§4.4, §9)
  SND    ──── P1[35]      ◄── R21/R22 ◄── 2-pole RC ◄── AUDIO_PWM (JP3: hdr34 on Pico 2 / GP34 on Plus-W)

  Hardware /OE gate (U15 74LVC00, two gates wired NAND-NAND):
    CTS_BUF ─┐
    SCS_BUF ─┴─NAND──► SEL_N ─┐
                     E_BUF ────┴─NAND──► OE_BUS_RAW ──► R11 33Ω ──► OE_BUS ──► JP2 pad 1 (→ U10 /OE), Pico header pin 31, TP1

  Pico /HALT drive (hold-until-booted + DriveWire flow control):
    GP27 (HALT_GATE) ──► R8 100Ω ──► [Q2 2N7002] ──► /HALT_CART pin
                         R7 10kΩ to +3V3 pulls gate high during Pico boot
                         → Q2 ON → /HALT held low until firmware releases
```

Reserved: the top-right corner of the board (about 25 x 15 mm) is kept
clear of components and dense routing for a future HDMI-A receptacle
(see spec §10, `docs/superpowers/specs/2026-09-17-main-board-v2.3-design.md`).
**This corner is not wirable on the v2.3 board.** HSTX is on `GP12..GP19`,
which are `A4..A11` of the address bus on both modules, and none of
GP12-19 reaches the pad grid. The reserve is kept as a keepout for a
future variant that moves the address bus off those pins, not for an
HDMI build on this revision.

Direction of `U10` is driven by the **buffered /R/W** wire (same net
that reaches Pico GP22). `/OE` of `U10` is driven by `U15`'s NAND-NAND
decode through JP2 (default position 1-2), so `U10` is tri-stated
unless **all** of: (a) `/CTS` OR `/SCS` is asserted (i.e., cart
selected) AND (b) E is high (data-valid phase).

Firmware distinguishes ROM reads ($C000–$FEFF, the full `/CTS` window)
from Becker accesses ($FF40–$FF5F) by inspecting **A13** when `OE_BUS`
is asserted: A13=0 means /CTS range, A13=1 means /SCS range. This saves
a GPIO compared to routing both /CTS and /SCS separately.

## 2. Component list

| Ref | Part | Package | Role |
|---|---|---|---|
| U1  | Raspberry Pi Pico 2 (RP2350) or Waveshare RP2350B-Plus-W | SMD+TH module (`PiCoCo:Pico-Carrier`) | MCU |
| U10 | SN74LVC245ADWR | SOIC‑20W | D0–D7 buffer, A side (pins 2..9) = Pico, B side (18..11) = cart |
| U11 | SN74LVC245ADWR | SOIC‑20W | A0–A7 buffer (5V→3.3V) |
| U12 | SN74LVC245ADWR | SOIC‑20W | A8–A13, /R/W, /CTS buffer |
| U13 | SN74LVC245ADWR | SOIC‑20W | /SCS, E, Q, /SLENB, /RESET, A14, A15 buffer (NOT /HALT, /NMI or /CART — those bypass U13, see §3.1/§4.4a) |
| U14 | AMS1117‑3.3  | SOT‑223  | +5V → +3.3V LDO (supplies buffers only) |
| U15 | SN74LVC00AD  | SOIC‑14 | Quad NAND, two gates used NAND-NAND: `SEL_N = NAND(CTS_BUF,SCS_BUF)`, `OE_BUS_RAW = NAND(SEL_N,E_BUF)`; gates 3/4 grounded |
| JP2 | Solder jumper, 3-pad bridged 1-2 | SMD | U10 `/OE` source: 1=OE_BUS (default), 2=U10 /OE, 3=OE_FW (Plus-W firmware /OE) |
| JP3 | Solder jumper, 3-pad bridged 1-2 | SMD | Header pin 34: 1=AUDIO_PWM (default, v2.3.1), 2=PICO_P34, 3=E_BUF (cut 1-2/bridge 2-3 for E on Pico 2) |
| JP4 | Solder jumper, 2-pad, open | SMD | Q_CART <-> CART_CART autostart tie, open by default (v2.3.1) |
| JP5 | Solder jumper, 3-pad, open | SMD | Header pin 34 drive select: 1=`CART_DRV` (Q4, populated), 2=`PICO_P34`, 3=`NMI_DRV` (Q3, DNP). Open by default; mutually exclusive with JP3 (never bridge both — they share header pin 34, see §4.6/§9) |
| Q2  | 2N7002 | SOT‑23 | N‑FET: firmware‑controlled /HALT sink (hold at boot) |
| Q3  | 2N7002 (DNP) | SOT‑23 | N‑FET stage for a Plus-W-driven /NMI (pad-grid `NMI_DRV`); needs R15/R17 fitted too |
| Q4  | 2N7002 | SOT‑23 | N‑FET stage for a firmware-pulsed /CART, reachable from a Pico 2 via JP5 1-2 (pad-grid `CART_DRV`) |
| D2  | Schottky SS14 | SMA | +5 V → VSYS_PICO (replaces cascade via LDO) |
| R1  | 4.7 kΩ 0805 | — | /HALT pull‑up to +5 V (required; idles /HALT when Q2 off) |
| R2  | 4.7 kΩ 0805 (DNP) | — | /NMI pull‑up footprint; CoCo already pulls this up, so DNP by default |
| R3  | 4.7 kΩ 0805 | — | /RESET pull‑up — populated (v2.3.1: avoids a floating /RESET buffer input on the bench) |
| R4  | 10 kΩ 0805 | — | Pico 3V3_EN pull‑up to VSYS_PICO |
| R7  | 10 kΩ 0805 | — | Q2 gate pull‑up to +3V3 (default‑on during Pico boot) — 10 k (v2.3.1: was 100 k, which the RP2350's own reset pull-down beat, see §4.4) |
| R8  | 100 Ω 0805 | — | Q2 gate series from GP27 |
| R9  | 100 Ω 0805 | — | Pico RUN series from RESET_BUF |
| R10 | 10 kΩ 0805 (DNP) | — | Pico RUN pull‑up to +3V3 — DNP (v2.3.1: held RUN low when the board was USB-only, see §4.5) |
| R11 | 33 Ω 0805 | — | OE_BUS series termination (fan‑out damping) |
| R12 | 33 Ω 0805 | — | RW_BUF series termination |
| R15 | 100 Ω 0805 (DNP) | — | Q3 gate series from NMI_DRV |
| R16 | 100 Ω 0805 | — | Q4 gate series from CART_DRV |
| R17 | 100 kΩ 0805 (DNP) | — | Q3 gate pull‑down to GND (released when nothing drives it) |
| R18 | 100 kΩ 0805 | — | Q4 gate pull‑down to GND (released when nothing drives it) |
| R19, R20 | 470 Ω 0805 | — | Audio 2-pole RC filter stage (v2.3.1: was 1 kΩ, see §4.6) |
| R21 | 1 kΩ 0805 | — | Audio level divider, AUDIO_AC -> SND_CART (v2.3.1: was 2.2 kΩ, see §4.6) |
| R22 | 1 kΩ 0805 | — | Audio level divider to GND |
| R23 | 10 kΩ 0805 | — | SLENB_CART pull-up to +5V (v2.3.1: pin 40 is cart->CoCo, nothing else drives U13 A5, see §3.1) |
| R24 | 0 Ω 0805 | — | AUDIO_F2 -> AUDIO_AC bypass (v2.3.1: DC-coupled default; remove + fit C15 for AC coupling, see §4.6) |
| R25 | 10 kΩ 0805 | — | U10_OE pull-up to +3V3 (v2.3.1: guards against an unprogrammed module with JP2 in the firmware position driving the CoCo bus) |
| C1  | 10 µF 0805 X5R ≥10V | — | +5V bulk at edge connector |
| C2  | 10 µF 0805 X5R ≥10V | — | LDO input |
| C3  | 22 µF 0805 X5R ≥6.3V | — | LDO output (MLCC‑compatible vendor required) |
| C4, C6–C10 | 100 nF 0805 | — | Per‑IC decoupling (U10, U11–U13, U15, Pico local) |
| C12 | 1000 µF 6.3V SMD electrolytic, D8x10 (DNP) | — | VSYS_PICO bulk cap for Plus-W Wi-Fi transmit bursts |
| C13, C14 | 10 nF 0805 | — | Audio 2-pole RC filter stage |
| C15 | 1 µF 0805 (DNP) | — | AC-coupling series cap, AUDIO_F2 -> AUDIO_AC ahead of R21 (v2.3.1: fit + remove R24 for AC coupling — the old parallel-R21 C15 was a treble boost, not AC coupling, see §4.6) |
| C16 | 10 µF 0805 | — | Local +3V3 bulk at the buffer row, next to U11/U12 (v2.3.1: the only prior bulk was C3 at the LDO, 63-134 mm of 0.2 mm track away) |
| J1  | Conn_01x05, 2.54 mm header (DNP) | — | "EXP (Plus-W)": pins `EXP_GP35`/`GP43`/`GP44`/`GP45` + GND, only meaningful on a Plus-W — those pads exist only on the Plus-W grid (see §3.2) |
| P1  | COCO‑CART‑2.1X1.75 (custom footprint) | Edge fingers | Cartridge slot mate |
| TP1–TP8 | 1×1 mm SMD pads | — | OE_BUS, RW_BUF, CTS_BUF, SCS_BUF, E_BUF, +3V3, SND_CART, GND |
| FID1–FID3 | 1 mm fiducial, 2 mm mask | — | SMT assembly fiducials, no net, excluded from BOM |

J2 and J3 are DNP 1x3 2.54 mm pin headers on the same three nets as JP3 and
JP5 respectively, for builders who want a movable shunt instead of a solder
bridge. Fit at most one of each pair: cut the solder jumper before shunting the
header, or the two selections short together.

**Unique active parts: 7** (Pico 2 / Plus-W module, SN74LVC245A, AMS1117‑3.3,
SN74LVC00A, 2N7002, SS14 Schottky). All are in the JLCPCB Basic or
Extended Library. All passives are 0805; DNP refs (R2, R10, R15, R17, Q3,
C12, C15, J1) are populated footprints on the board but not fitted by
default. Q4/R16/R18 (the firmware-pulsed /CART stage reachable from JP5 on
a Pico 2) are populated. No `J_SWD` header and no `MTG1` mounting hole on
this board (see §7 and §9).

## 3. Signal map

### 3.1 Cartridge edge connector (P1, `COCO-CART-2.1X1.75`)

40 fingers, 0.1" pitch, 2.1" × 1.75". Pin numbering per CoCo Technical
Reference Manual.

| Pin | Signal  | Direction (cart‑side) | Destination on board |
|-----|---------|-----------------------|----------------------|
| 1   | -12 V   | unused                | NC |
| 2   | +12 V   | unused                | NC |
| 3   | /HALT   | bidi (open‑drain) on CoCo | **Not buffered through U13.** R1 4.7 kΩ pull‑up to +5 V + Q2 drain (Pico‑controlled pull‑down, §4.4) |
| 4   | /NMI    | bidi (open‑drain)     | **Not buffered through U13.** R2 4.7 kΩ pull‑up (DNP) + Q3 drain (DNP; driven from `NMI_DRV` via JP5 2-3 on a Pico 2, or the Plus-W pad grid — needs R15/R17 fitted too, §9) |
| 5   | /RESET  | bidi (open‑drain)     | U13 in + R3 4.7 kΩ pull‑up (populated, v2.3.1); buffered copy (RESET_BUF) drives Pico RUN via R9 (R10 pull‑up footprint is DNP) |
| 6   | E       | CoCo → cart           | U13 → E_BUF → U15 gate 2, JP3 pad 3 (v2.3.1: default bridges 1-2 = AUDIO_PWM to header pin 34; cut 1-2/bridge 2-3 to route E there instead). JP5 (open by default) shares this same header pin 34 for a firmware /CART or /NMI drive instead — never bridge both JP3 and JP5. |
| 7   | Q       | CoCo → cart           | U13 in only; output left unconnected (not routed to Pico — saves one GPIO). JP4 (open by default, v2.3.1) can tie Q_CART to CART_CART (pin 8) as the classic Program Pak autostart trick. **Bridging JP4 breaks HDB-DOS and any DK-signature DOS ROM** — it pulses /CART forever, so the CPU jumps to $C000 as code on every cycle, and a DOS ROM's first bytes ($44/$4B, the "DK" signature) are not a valid instruction. |
| 8   | /CART   | cart → CoCo (open collector) | **Not buffered through U13.** Q4 drain (populated) + JP4 pad 2 (Q_CART tie, open by default). No pull-up populated by default — HDB-DOS autostarts on the DK signature with /CART open. JP5 pad 1 (`CART_DRV`, open by default) lets a Pico 2 pulse this pin from firmware: assert for the first cycles after reset for autostart ROM images, release for DK-signature DOS ROMs — see §9. |
| 9   | +5 V    | power                 | C1 bulk, U14 in, D2 Schottky anode (→ VSYS_PICO), R1 pull‑up |
| 10–17 | D0–D7 | bidirectional         | U10 B‑side |
| 18  | /R/W    | CoCo → cart           | U12 in; U12 output → R12 33 Ω → U10 DIR + GP22 |
| 19–26 | A0–A7 | CoCo → cart           | U11 in → GP8–GP15 |
| 27–31 | A8–A12 | CoCo → cart          | U12 in → GP16–GP20 |
| 32  | /CTS    | CoCo → cart           | U12 in → CTS_BUF → U15 gate 1 (NOT routed to Pico — firmware uses OE_BUS + A13) |
| 33, 34 | GND  | ground                | GND plane |
| 35  | SND     | cart → CoCo (audio)   | R21 1 kΩ / R22 1 kΩ divider ← 2-pole RC ← AUDIO_PWM (see §4.6); DC-coupled, inert when nothing drives AUDIO_PWM |
| 36  | /SCS    | CoCo → cart           | U13 in → SCS_BUF → U15 gate 1 (NOT routed to Pico — firmware uses OE_BUS + A13) |
| 37  | A13     | CoCo → cart           | U12 in → GP21 (serves double duty as address MSB and ROM/Becker selector) |
| 38  | A14     | CoCo → cart           | P1 only; not routed to Pico or buffer |
| 39  | A15     | CoCo → cart           | P1 only; not routed to Pico or buffer |
| 40  | /SLENB  | cart → CoCo (v2.3.1: direction corrected — earlier drafts had this backwards) | U13 in + R23 10 kΩ pull‑up to +5 V (nothing on this board drives it, so it idles inactive); output left unconnected |

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
| GP28     | 34 | GP28/GP42 | `AUDIO_PWM` (default via JP3 1-2, v2.3.1) | Sound output stage (§4.6); firmware does not use E today. Cut JP3 1-2/bridge 2-3 to get E on this pin instead for `WAIT 1/0 PIN 20` (base 8) bus-phase sync, needed only by the v2 PIO engine (`docs/firmware-architecture.md` §3.3). This is also the header pin JP5 shares (pad 2, `PICO_P34`) for a firmware-driven /CART or /NMI — JP3 and JP5 both bridge onto the same pin, so bridge at most one of them. |
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
| GP26 | E_BUF | capture (also reaches the module's header pin 34 only if JP3 is cut to 2-3 — header pin 34 carries `AUDIO_PWM` by default as of v2.3.1) |
| GP27 | Q_BUF | capture |
| GP28 | SLENB_BUF | capture |
| GP29 | A14_BUF | capture |
| GP30 | A15_BUF | capture |
| GP31 | OE_FW | JP2 alternate (2-3): firmware-driven U10 /OE |
| GP32 | NMI_DRV | Q3 gate (DNP stage — needs R15/R17 fitted too); also reachable from a Pico 2 via JP5 2-3 |
| GP33 | CART_DRV | Q4 gate (populated); also reachable from a Pico 2 via JP5 1-2 |
| GP34 | AUDIO_PWM | sound output stage (§4.6) |
| GP35 | EXP_GP35 → J1 pin 1 | Only meaningful on a Plus-W; a flat-mounted Pico 2 lands its SWDIO pad here instead (see §7) |
| GP43 | EXP_GP43 → J1 pin 2 | Plus-W only |
| GP44 | EXP_GP44 → J1 pin 3 | Plus-W only |
| GP45 | EXP_GP45 → J1 pin 4 | Plus-W only |

J1 ("EXP (Plus-W)", DNP by default) is a 1x5 2.54 mm header breaking out
these four pads plus GND (pin 5). Hardware SPI does not reach all four:
RP2350B SPI1 gives TX/RX/CSn on GP43/44/45, but SCK would be GP42, which
is header pin 34 (`AUDIO_PWM`/`E`) — an SD card on J1 is a PIO-SPI job,
not a peripheral-SPI one.

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
  tri‑state when the cart is not selected. **R25 10 kΩ (v2.3.1)** pulls
  `U10_OE` up to +3V3: with JP2 cut to 2-3 (firmware /OE) and an
  unprogrammed or not-yet-booted module, an undriven GPIO could
  otherwise float `U10_OE` low and drive the CoCo bus; R25 keeps it
  tri-stated until firmware actively asserts it.

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
                                      ├── R7 10 kΩ ─── +3V3
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

**Decision 2026-09-18: R7 changed 100 kΩ → 10 kΩ.** A pre-order review
found the RP2350's own internal reset pull-down on GP27 was strong
enough to beat a 100 kΩ external pull-up, so the /HALT-held-at-boot
default was a lottery depending on exactly when the SDK configured the
pin. 10 kΩ wins that race reliably without materially changing R8's
gate-drive current budget.

### 4.4a /NMI and /CART drive stages — Q3, Q4, JP5

Two more 2N7002 stages, built the same way as Q2 (§4.4) but with their
gates fed from `NMI_DRV`/`CART_DRV` instead of a Pico GPIO directly:

```
NMI_DRV  ─── R15 100 Ω (DNP) ─── Q3 gate ─── R17 100 kΩ (DNP) ─── GND
                                    │
                             Q3 drain ── /NMI_CART (cart pin 4)

CART_DRV ─── R16 100 Ω        ─── Q4 gate ─── R18 100 kΩ        ─── GND
                                    │
                             Q4 drain ── /CART_CART (cart pin 8)
```

`NMI_DRV` and `CART_DRV` both land on **JP5**, a 3-pad open solder
jumper: pad 1 = `CART_DRV`, pad 2 = `PICO_P34` (module header pin 34),
pad 3 = `NMI_DRV`. JP5 is open by default and **mutually exclusive with
JP3** — both jumpers bridge onto the same header pin 34, so bridging
both at once ties two drivers together. Silk says so; never bridge both.

- **JP5 1-2** on a Pico 2 gives a firmware-pulsed `/CART`: assert for
  the first cycles after reset so an autostart ROM image runs
  immediately, release once a DK-signature DOS ROM (HDB-DOS etc.) has
  had a chance to install its own hooks. This is the Q4 stage, and as
  of v2.3.1 **Q4/R16/R18 are populated** — it needs no rework.
- **JP5 2-3** on a Pico 2 gives a firmware-pulsed `/NMI` instead,
  trading away the `/CART` drive. This is the Q3 stage, and
  **Q3/R15/R17 stay DNP** — fit all three to use it.
- On a Plus-W, `CART_DRV` and `NMI_DRV` are also reachable directly
  from the pad grid (`GP33`/`GP32`, §3.2), independent of JP5/JP3.
- **JP4** (§4.2, cart pin 7 `Q_CART` tied to pin 8 `CART_CART`) is the
  separate, passive alternative for `/CART` autostart on a build that
  drives neither JP5 pad — see §9 for when to use which.

### 4.5 Reset path — Pico RUN from CoCo /RESET

```
/RESET_CART ─── U13 B5 ─── RESET_BUF ─── R9 100 Ω ─── Pico RUN (pin 30)
                                                          │
                                                       R10 10 kΩ ── +3V3  (DNP, v2.3.1)
```

When a CoCo user presses the RESET button (or the CPU asserts
/RESET), RESET_BUF goes LOW at 3.3 V logic level. R9 limits transient
current; R10, when fitted, holds RUN HIGH while /RESET is deasserted.
This resets the Pico in sync with the CoCo, clearing Becker FIFO state
instead of leaving it drifted relative to the CoCo's software.

**Decided 2026-09-17: populate R9, keep the tie.** A CoCo reset must
reach the Pico definitively, and the video design (RP2350B_IDEAS §4.5)
keys its shadow-state handling off the chip reset reason: RUN-pin reset
means "zero the shadow, the CoCo ROM repopulates it", watchdog or
software reset means "keep it". Doing that over RUN costs no GPIO, which
the Pico 2 does not have spare and the RP2350B budget does not either.
Costs accepted: every reset press reboots the Pico (about 1.2 s to
"halt released") and drops the USB console; the /HALT hold in §4.4 is
what makes that reboot safe for the CoCo.

**Decision 2026-09-18: R10 is DNP.** A pre-order review found R10 held
RUN low when the board was powered from USB only (no +5 V at the cart
edge, so +3V3 is dead and R10's own pull-up rail is gone). R9 alone is
kept; the R10 footprint stays on the board if a future build finds a
case that needs the pull-up back.

### 4.6 Sound stage — AUDIO_PWM to SND_CART

Cart pin 35 (`SND_CART`) is an analog input the CoCo mixes into its own
audio path when `AUDIO ON` selects the cart (the same input the
Orchestra-90 and Speech/Sound Pak use). This stage is populated by
default, all Basic 0805 parts, and inert when nothing drives
`AUDIO_PWM`:

```
AUDIO_PWM ── R19 470R ── AUDIO_F1 ── C13 10nF to GND
                            │
                        R20 470R ── AUDIO_F2 ── C14 10nF to GND
                                        │
                          R24 0R (default) or C15 1uF (DNP, AC coupling)
                                        │
                                    AUDIO_AC
                                        │
                                    R21 1k ── SND_CART ── R22 1k ── GND
```

- **JP3** (pad 1 = `AUDIO_PWM`, pad 2 = `PICO_P34` = module header pin
  34, pad 3 = `E_BUF`) is bridged 1-2 by default as of **v2.3.1**, so
  header pin 34 carries `AUDIO_PWM` out of the box — a Pico 2 build gets
  sound without cutting a jumper. Cut 1-2 and bridge 2-3 to put E on
  header pin 34 instead (needed only by the v2 PIO engine, a
  Plus-W-only path — `OE_BUS` is already E‑qualified in hardware, so
  firmware does not need E today). **On a Plus-W, JP3 1-2 ties pad-grid
  `GP34` to `GP42`** (the same header pin 34, wired to a different GPIO
  number on that module) — bridging it shorts two GPIOs of the same die
  together unless one of them is left as an input. Keep one of GP34/GP42
  an input on any Plus-W build; JP5 (§4.4a) shares this same header pin
  for a third option (a firmware /CART or /NMI drive) and is mutually
  exclusive with JP3.
- **Plus-W**: the pad-grid's own GP34 drives `AUDIO_PWM` directly; JP3
  should stay at its default (1-2). Do not set JP3 to 2-3 on a Plus-W
  with GP34 populated — that puts two outputs on one net. The
  silkscreen says so.
- The two-pole RC (R19/C13, R20/C14) rolls off around 18 kHz into the
  loaded R21/R22 divider. **v2.3.1: R19/R20 changed 1 kΩ → 470 Ω and
  R21 changed 2.2 kΩ → 1 kΩ.** The two filter resistors sit inside the
  divider and load each other, so they are not two independent poles —
  solving the actual loaded network with the old 1 k/1 k/2.2 k values
  gave only 0.63 V pk-pk from a 3.3 V square wave and a −3 dB point of
  9.1 kHz, both far short of the "about 1 V, ~16 kHz" this section used
  to claim. The new 470/470/1k/1k network gives about **1.1 V pk-pk**
  and **−3 dB near 18 kHz**, and a 31.25 kHz PWM carrier is attenuated
  about 12.5 dB. **R24 (0 Ω, v2.3.1)** bridges `AUDIO_F2` to `AUDIO_AC`
  by default, giving a DC-coupled divider to that ~1.1 V full scale, the
  same way the Orchestra-90 fed this input. **C15 (0805, 1 µF, DNP,
  v2.3.1)** sits in series in the same spot: fit C15 and remove R24 for
  AC coupling instead. (The v2.3 layout put C15 in parallel with R21,
  which turned out to act as a treble boost rather than AC-couple the
  signal — v2.3.1 corrects the topology.)
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

`+5V`, `+3V3` and `VSYS_PICO` are on the **Power netclass** (0.5 mm
track, see §6) as of v2.3.1; everything else stays on Default (0.2 mm).

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
- **C16 (10 µF 0805, v2.3.1)** adds local +3V3 bulk at the buffer row,
  next to U11/U12. Before C16 the only +3V3 bulk was C3 at the LDO,
  63–134 mm of 0.2 mm track away from the four LVC245s that are the
  board's entire dynamic load.
- No ferrite beads — not needed at sub‑2 MHz bus rates.

### 5.4 Decoupling loop discipline

Each bypass cap's GND pad gets its own stitching via straight to the
B.Cu GND plane — **never** share a via between multiple caps. The goal
is shortest possible current loop. As of v2.3.1 this is implemented as
a GND via at every decoupling cap's GND pad and every IC's GND pin, plus
an 8 mm-pitch stitching grid across the buffer-row corridor
(`tools/gnd_stitch.py`, run after `finish_route`) so the two copper
pours stay tied together instead of splitting into isolated islands
under the dense bus routing.

## 6. PCB layer stack & rules

**Board outline (v2.3.1):** 98.0 x 67.0 mm body + 10.16 mm finger
tongue = 77.16 mm overall (board y 32.187..109.347 in the KiCad
coordinate frame). Grown 12 mm away from the fingers from v2.3's
98 x 55 mm outline after a bench measurement of a CoCo 3's cartridge
opening (§9) showed the shorter board put the module's USB-C port and
the reserved HDMI corner (§5) on or past the case surface. Everything
at or above JP2's row (module U1, JP3, C10, TP1–6, the antenna keepout,
the HDMI reserve, top-edge silk) moved up with the outline; the buffer
row, power block and sound stage did not move.

| Parameter | Value |
|---|---|
| Layers | 2 (F.Cu, B.Cu) |
| Stack‑up | 1.6 mm FR4, 1 oz copper |
| Default netclass track | 0.20 mm |
| Default netclass via | 0.8 mm pad / 0.4 mm drill (a few 0.6/0.3 mm GND stitching vias) |
| Default netclass clearance | 0.15 mm |
| Design-rule floors | min track 0.127 mm, min clearance 0.127 mm, copper-to-edge 0.2 mm, hole clearance 0.25 mm |

**v2.3.1 adds a Power netclass** at 0.5 mm track for `+5V`, `+3V3` and
`VSYS_PICO`; a pre-order review found the cart's 300 mA budget was fine
electrically at 0.2 mm (roughly 0.5 A at a 10 °C rise for a 0.20 mm
trace) but there was no reason to leave the power trunks that thin, and
it removes an obvious "no power netclass" criticism. Net classes as
actually set in `PiCoCo.kicad_pro`:

| Class   | Track | Via (pad/drill) | Clearance | Nets |
|---------|-------|-----------------|-----------|------|
| Default | 0.20 mm | 0.8/0.4 mm     | 0.15 mm | everything else |
| Power   | 0.50 mm | 0.8/0.4 mm     | 0.15 mm | `+5V`, `+3V3`, `VSYS_PICO` |

### 6.1 Copper pour plan

- **B.Cu and F.Cu**: full GND pour on both layers, board outline
  inset 0.3 mm, cut 1 mm above the edge fingers so the pour never
  touches the gold-fingered area. Built by `tools/pour.py`.
- Stitching vias: a GND via at every decoupling cap's GND pad and every
  IC's GND pin, plus an 8 mm-pitch stitching grid across the buffer-row
  corridor (`tools/gnd_stitch.py`, v2.3.1 — see §5.4) so return current
  isn't forced around pour-island boundaries under the dense bus
  routing; one U11 pad (pad 10, GND) has zone connection "none" —
  it's tied in by track + via instead of a thermal spoke.
- See `docs/kicad-workflow.md` §"Routing" for the full autoroute →
  grid-route → pour → gnd-stitch → gnd-fix pipeline that produced the
  routed board.

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

- **TP1–TP8** SMD pads on the nets most likely to need scope access
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
| TP8 | GND | Ground reference (v2.3.1) |

(+5 V can be probed at P1 pin 9 or C1; GND is ubiquitous via edge
fingers and IC grounds.)

### Fiducials

FID1, FID2 and FID3 (`Fiducial:Fiducial_1mm_Mask2mm`) are SMT-assembly
fiducial marks in open, copper-free areas of the board — no net,
excluded from the BOM. FID3 was added in v2.3.1 for a third reference
point. They're for the assembler's placement-machine vision, not for
hand debug.

### LEDs

- No external LED on the board — a power/heartbeat indicator is
  invisible inside a cartridge case, and leaving it out saves a
  part and a few mA of idle current off +3.3 V. Heartbeat indication
  uses the Pico's onboard LED (GP25, internal), controlled by firmware.
  Bare‑board "is +3.3 V up?" checks use TP6 on a DMM.

## 8. Build notes

- Hand‑assembly order (helps self‑test): U14 + C2 + C3 + C16 → verify
  3.3 V with no other parts → D2 → verify VSYS_PICO ≈ 4.7 V with 5 V
  applied → U10–U13 + C4, C6–C10 → U15 → JP2/JP3/JP5 (leave JP2/JP3 at
  default 1-2; JP4 and JP5 left open) → Q2 + R7/R8 → Q4 + R16/R18 →
  passives → edge fingers → TP pads → module last.
  R2, R10, R15, R17, Q3, C12, C15, J1 are DNP by default (Plus-W-only
  or optional provisions) — skip them on a Pico 2 build. Q4/R16/R18
  (the JP5-reachable /CART drive) are populated regardless of module.
- JLCPCB fab order: 2‑layer, 1.6 mm, **ENIG** surface finish (all pads
  gold; fingers still get hard gold below, never HASL), **Gold Fingers
  enabled**, 30° bevel (both critical for the CoCo slot's bronze
  wipers). A budget variant (bare boards, no assembly) can drop gold
  fingers/bevel and take ENIG everywhere — fine for light hobbyist use.
  See `fab/main/READ-BEFORE-ORDERING.txt` for the full checklist.
- SMT assembly (optional): U10–U13 (SN74LVC245A) and U15 (SN74LVC00A)
  are JLC Extended; U14, Q2/Q4, D2 and all 0805 passives are Basic.
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
- Driving `/NMI`, `/OE` from firmware, and reading A14/A15 and `Q`,
  stay provisioned in hardware (Q3 DNP stage, JP2, and the full U13
  channel set) but reachable only from the Plus-W pad grid — a Pico 2
  build has no spare GPIO to use them. Firmware support for the pad
  grid is itself out of scope for this spin (spec §1,
  `docs/superpowers/specs/2026-09-17-main-board-v2.3-design.md`); see
  `docs/firmware-architecture.md`'s Plus-W pin plan. **`/CART` is the
  exception as of v2.3.1**: JP5 1-2 plus the now-populated Q4/R16/R18
  stage (§4.4a) lets a Pico 2 pulse `/CART` from firmware over header
  pin 34, no spare GPIO needed — asserted for autostart on plain ROM
  images, released for DK-signature DOS ROMs. **v2.3.1** also kept the
  passive-only alternative: JP4 (open by default) ties `Q_CART` to
  `CART_CART`, the classic Program Pak autostart trick, for a build
  that doesn't want to give up header pin 34 to JP5.
- **v2.3.1, outline growth.** A bench measurement of a real CoCo 3's
  cartridge opening (114.3 x 30.2 mm) found the connector face sits
  about 43 mm inside the case and the case surface about 52-54 mm from
  the finger tip. At v2.3's 98 x 55 mm outline the module's USB-C port
  landed almost exactly on that case surface and the reserved HDMI
  corner (§5) only 10 mm past it — too tight to use either reliably.
  The board grew 12 mm away from the fingers to fix this (§6); no
  mounting hole was added in the new strip, so the board's lack of a
  mounting hole (`CLAUDE.md`) is unchanged.

### 9.1 Deferred to v2.4

Hardware items from the 2026-09-19 quality reviews that need a re-place
or a bench MPI first, so they wait for the next spin rather than this
order:

- **Series termination on the data bus.** R11/R12 terminate `OE_BUS`
  and `RW_BUF` — the two quietest, lowest-fanout nets — while the eight
  lines that actually drive the CoCo backplane from a 24 mA LVC output
  have none. Add 8x 33 Ω on the B-side `D0..D7_CART`; no room without a
  re-place.
- **/SLENB drive for Multi-Pak writes.** CocoFLASH asserts `/SLENB` on
  every cart-space write because the MPI's data buffer otherwise won't
  pass CPU writes through; PiCoCo only pulls `/SLENB` up (R23, §3.1) and
  drives nothing. Wire U15's spare gate 3 as an inverter on `OE_BUS`
  into a 2N7002 + 100 Ω sinking `SLENB_CART`, behind a DNP jumper, open
  by default. Unverified — needs a bench MPI first.
- **C3 as a 10 V 1206.** The current 22 µF 6.3 V X5R 0805 typically
  delivers half its marked capacitance at 3.3 V DC bias; a 10 V part in
  1206 would be honest about the LDO's actual stability margin.
- **Real 2.4 GHz antenna clearance.** The keepout (§6.1) is a widened
  1 mm rule area (x 151.3..158.0, y 32.7..55.7 as of v2.3.1), not the
  ~5 mm clearance a Plus-W's antenna actually wants; `A12_BUF` and
  `AUDIO_PWM` still run within a fraction of a mm of its edge. Needs a
  re-place to push the address bundle further out.

## 10. Cross‑references

- Firmware architecture: [`firmware-architecture.md`](firmware-architecture.md)
- KiCad regen workflow: [`kicad-workflow.md`](kicad-workflow.md)
- v2.3 design spec: `docs/superpowers/specs/2026-09-17-main-board-v2.3-design.md`
- CoCo Technical Reference Manual (cart pinout): §3
- DriveWire specs: Cloud9 `DriveWire 3 Specification.pdf`; `github.com/boisy/DriveWire/wiki/DriveWire-Specification`
- Level‑shifter selection rationale: `bigmessowires.com/2023/08/22/a-tale-of-three-bidirectional-level-shifters/`
- JLCPCB fab checklist: `fab/main/READ-BEFORE-ORDERING.txt`

