# PiCoCo — Bill of Materials (v2.3.1)

Source of truth: symbol properties (`LCSC`, `MPN`) in `tools/gen_schematic.py`,
looked up 2026-09-17 (revised 2026-09-18 and 2026-09-19, "v2.3.1" in code
comments — see spec §11) and reproduced here from spec §3.3/§11
(`docs/superpowers/specs/2026-09-17-main-board-v2.3-design.md`). If this table
and the generator ever disagree, the generator wins — regenerate this file's
numbers from it, don't hand-patch around a mismatch.

## Active components

| Ref | MPN | Package | LCSC | JLC library | Lifecycle | Notes |
|-----|-----|---------|------|-------------|-----------|-------|
| U1  | Raspberry Pi Pico 2, or Waveshare RP2350B-Plus-W | SMD+TH module | (not stocked — order from authorized resellers: adafruit.com / pishop.us / waveshare.com) | n/a | Active | Not placed by JLC assembly; solder it yourself last |
| U10–U13 | SN74LVC245ADWR (or Nexperia 74LVC245AD if cheaper at order) | SOIC-20W | C571201 | Extended | Active | Same part x4: U10 data bidi (A=Pico, B=cart), U11–U13 address/control in |
| U14 | AMS1117-3.3 | SOT-223 | C6186 | Basic | Active | +5V → +3.3V LDO |
| U15 | SN74LVC00ADR (TI; alt Nexperia 74LVC00AD) | SOIC-14 | C485072 | Extended | Active | NAND-NAND decode, 2 of 4 gates used |
| Q2, Q4 | 2N7002 | SOT-23 | C8545 | Basic | Active | Q2 (/HALT drive), Q4 (/CART drive, reachable from a Pico 2 via JP5 1-2) — both populated |
| Q3 | 2N7002 | SOT-23 | C8545 | Basic | Active | /NMI drive (JP5 2-3 on a Pico 2, or the Plus-W pad grid) — **DNP**, needs R15/R17 fitted too |
| D2  | SS14 | SMA | C2480 | Basic | Active | +5V → VSYS_PICO |

## Passives (all 0805 unless noted)

| Ref | Value | Package | LCSC | JLC library | Notes |
|-----|-------|---------|------|-------------|-------|
| R1  | 4.7 kΩ 1% | 0805 | C17673 | Basic | /HALT pull-up (required) |
| R2  | 4.7 kΩ 1% | 0805 | C17673 | Basic | /NMI pull-up — **DNP** |
| R3  | 4.7 kΩ 1% | 0805 | C17673 | Basic | /RESET pull-up — populated (no floating /RESET buffer input on the bench) |
| R4  | 10 kΩ 1% | 0805 | C17414 | Basic | 3V3_EN pull-up |
| R7  | 10 kΩ 1% | 0805 | C17414 | Basic | Q2 gate pull-up — 10 k (was 100 k; the RP2350's own reset pull-down beat 100 k) |
| R8, R9 | 100 Ω 1% | 0805 | C17408 | Basic | Q2 gate series, Pico RUN series |
| R10 | 10 kΩ 1% | 0805 | C17414 | Basic | Pico RUN pull-up — **DNP** (held RUN low when the board was USB-only) |
| R11, R12 | 33 Ω 1% | 0805 | C17634 | Basic | OE_BUS / RW_BUF series termination |
| R15 | 100 Ω 1% | 0805 | C17408 | Basic | Q3 gate series — **DNP** |
| R16 | 100 Ω 1% | 0805 | C17408 | Basic | Q4 gate series (populated, v2.3.1) |
| R17 | 100 kΩ 1% | 0805 | C149504 | Basic | Q3 gate pull-down — **DNP** |
| R18 | 100 kΩ 1% | 0805 | C149504 | Basic | Q4 gate pull-down (populated, v2.3.1) |
| R19, R20 | 470 Ω 1% | 0805 | C17710 | Basic | Audio stage (v2.3.1: was 1 kΩ, see hardware-design §4.6) |
| R21, R22 | 1 kΩ 1% | 0805 | C17513 | Basic | Audio level divider (v2.3.1: R21 was 2.2 kΩ, see hardware-design §4.6) |
| R23 | 10 kΩ 1% | 0805 | C17414 | Basic | SLENB_CART pull-up to +5V (pin 40 is cart->CoCo; nothing else drives U13 A5) |
| R24 | 0 Ω | 0805 | C17477 | Basic | AUDIO_F2 -> AUDIO_AC bypass (DC-coupled default; remove + fit C15 for AC coupling) |
| R25 | 10 kΩ 1% | 0805 | C17414 | Basic | U10_OE pull-up to +3V3 (guards an unprogrammed module with JP2 in the firmware position) |
| C1, C2 | 10 µF X5R ≥10V | 0805 | C15850 | Basic | +5V edge bulk / LDO input |
| C3 | 22 µF X5R ≥6.3V | 0805 | C45783 | Basic | LDO output — MLCC-compatible AMS1117 variant required |
| C4, C6–C10 | 100 nF X7R 50V | 0805 | C49678 | Basic | Per-IC decoupling (C5 intentionally unused) |
| C13, C14 | 10 nF X7R | 0805 | C1710 | Basic | Audio 2-pole RC filter |
| C16 | 10 µF X5R ≥10V | 0805 | C15850 | Basic | Local +3V3 bulk at the buffer row, next to U11/U12 (v2.3.1) |
| C12 | 1000 µF 6.3V SMD electrolytic | D8x10 mm | pick at order (C970711 had stock 2026-09-17) | n/a | VSYS_PICO Wi-Fi burst cap — **DNP** on a Pico 2 |
| C15 | 1 µF | 0805 | C28323 | Basic | AC-coupling series cap, AUDIO_F2 -> AUDIO_AC ahead of R21 (fit + remove R24 for AC coupling) — **DNP** |

## Connectors and mechanical

| Ref | Part | Footprint | Notes |
|-----|------|-----------|-------|
| P1  | (custom COCO-CART edge fingers) | `PiCoCo:COCO-CART-2.1X1.75` | PCB fingers only; no connector part |
| JP2 | Solder jumper, 3-pad bridged 1-2 | `Jumper:SolderJumper-3_P1.3mm_Bridged12_RoundedPad1.0x1.5mm` | U10 /OE source select |
| JP3 | Solder jumper, 3-pad bridged 1-2 | (same footprint) | Header pin 34: audio (default, v2.3.1) or E |
| JP4 | Solder jumper, 2-pad open | `Jumper:SolderJumper-2_P1.3mm_Open_RoundedPad1.0x1.5mm` | Q_CART <-> CART_CART autostart tie — open by default |
| JP5 | Solder jumper, 3-pad open | `Jumper:SolderJumper-3_P1.3mm_Open_RoundedPad1.0x1.5mm` | Header pin 34 drive select: 1=CART_DRV (Q4), 2=PICO_P34, 3=NMI_DRV (Q3, DNP) — open by default, mutually exclusive with JP3 (v2.3.1) |
| J1  | 1x5 pin header, 2.54 mm (DNP) | `Connector_PinHeader_2.54mm:PinHeader_1x05_P2.54mm_Vertical` | "EXP (Plus-W)": EXP_GP35/GP43/GP44/GP45 + GND — Plus-W only (v2.3.1) |
| TP1–TP8 | `TestPoint:TestPoint_Pad_1.0x1.0mm` | — | OE_BUS, RW_BUF, CTS_BUF, SCS_BUF, E_BUF, +3V3, SND_CART, GND |
| FID1–FID3 | `Fiducial:Fiducial_1mm_Mask2mm` | — | SMT assembly fiducials, no net, excluded from BOM |

There is no `J_SWD` header and no mounting hole (`MTG1`) on this board — both
were removed in v2.3 (see `CLAUDE.md` and `docs/hardware-design.md` §7, §9).

DNP by default: R2, R10, R15, R17, Q3, C12, C15, J1. Everything else,
including the JP5-reachable /CART stage (Q4/R16/R18), is populated.

## Hobbyist substitutions

- **D2 Schottky**: any 1 A Schottky in SMA/DO-214AC works (SS14, SS24,
  SS34, B150-E3, 1N5819HW). The ~0.3 V drop is what matters; higher-Vf
  parts leave less VSYS_PICO headroom.
- **AMS1117-3.3**: LM1117-3.3 and NCP1117-3.3 drop in on the same
  SOT-223 footprint. If C3 oscillation appears at bring-up, switch to
  TLV755P-33 (SOT-23-5, slightly different footprint) which is
  unconditionally stable with MLCC.
- **2N7002**: any small-signal N-FET with Vgs(th) ≤ 2 V and
  Id ≥ 100 mA in SOT-23: BSS138, ZVN3306A, PMV40UN2. Required: can be
  turned on by a 3.3 V gate drive.
- **SN74LVC245A**: any 3.3V-tolerant octal transceiver in the same
  SOIC-20W pinout (74HCT245 works electrically but is not 5V-tolerant
  on its 3.3V side at speed — stick to the LVC family).
- **SN74LVC00A**: any quad 2-input NAND in SOIC-14, e.g. 74HC00 for a
  3.3V-tolerant bench substitute (used during breadboard bring-up).

## Non-board parts

- **Assembled HDB-DOS ROM image (16 KB)**: for MVP firmware (Phase 1),
  users must supply their own binary. Sources: Cloud9 `HDB-DOS-DW`
  build (commercial), or extract from a physical Disk BASIC cart if
  owned.
- **USB-C to USB-A cable**: for DriveWire host connection (Phase 1
  bridge) or for flashing (BOOTSEL, or the module's own debug pads).
- **DriveWire host software**: `pyDriveWire` (open source) or
  DriveWire4 (Java, Cloud9). Runs on any PC/Mac/Linux with a USB port.

## Ordering

See `fab/main/READ-BEFORE-ORDERING.txt` for the exact JLCPCB order
settings (base material, gold fingers, bevel, DNP list, SMT assembly
files) and the budget variant for bare boards.

## Cost estimate (2026 pricing, 5 PCBs + 2 assembled)

- **Hard gold fingers + 30° bevel (recommended)**: about $90–$130
  shipped, PCB + SMT assembly included, module and hand-soldered
  passives (Q3/R15/R17/C12/C15/J1 DNP by default) not included.
- **ENIG, no gold fingers, no bevel (budget/bare-board variant)**:
  subtract roughly $30–$50 from the above — fine for a lightly used
  cart, but the fingers will wear faster in a well-used CoCo slot.

C45783 (C3, 22 µF ≥6.3 V) was re-verified on 2026-09-17 and is no longer on
the unverified list. Five LCSC numbers remain unverified: C17710 (R19/R20,
470 Ω, v2.3.1 — replaces the no-longer-used 2.2 kΩ C17520), C1710, C17513,
C17477 (R24, 0 Ω), and C28323 (C15, 1 µF) — confirm them in JLCPCB's parts
step. The 100 kΩ LCSC number changed from C17407 (discontinued 2026-09) to
C149504. Prices drift; check DigiKey/Mouser/LCSC and JLCPCB's
current quote before ordering. Substitute equivalents freely on the
generics (0805 passives, SOIC logic) — the part numbers above are one
known-good source each.
