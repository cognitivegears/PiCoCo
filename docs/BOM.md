# PiCoCo — Bill of Materials (v2.3)

Source of truth: symbol properties (`LCSC`, `MPN`) in `tools/gen_schematic.py`,
looked up 2026-09-17 and reproduced here from spec §3.3
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
| Q2, Q3, Q4 | 2N7002 | SOT-23 | C8545 | Basic | Active | Q2 populated (/HALT drive); Q3, Q4 DNP (Plus-W /NMI, /CART drive) |
| D2  | SS14 | SMA | C2480 | Basic | Active | +5V → VSYS_PICO |

## Passives (all 0805 unless noted)

| Ref | Value | Package | LCSC | JLC library | Notes |
|-----|-------|---------|------|-------------|-------|
| R1  | 4.7 kΩ 1% | 0805 | C17673 | Basic | /HALT pull-up (required) |
| R2, R3 | 4.7 kΩ 1% | 0805 | C17673 | Basic | /NMI, /RESET pull-up — **DNP** |
| R4, R10 | 10 kΩ 1% | 0805 | C17414 | Basic | 3V3_EN and Pico RUN pull-ups |
| R7 | 100 kΩ 1% | 0805 | C17407 | Basic | Q2 gate pull-up |
| R8, R9 | 100 Ω 1% | 0805 | C17408 | Basic | Q2 gate series, Pico RUN series |
| R11, R12 | 33 Ω 1% | 0805 | C17634 | Basic | OE_BUS / RW_BUF series termination |
| R15, R16 | 100 Ω 1% | 0805 | C17408 | Basic | Q3/Q4 gate series — **DNP** |
| R17, R18 | 100 kΩ 1% | 0805 | C17407 | Basic | Q3/Q4 gate pull-downs — **DNP** |
| R19, R20, R22 | 1 kΩ 1% | 0805 | C17513 | Basic | Audio stage |
| R21 | 2.2 kΩ 1% | 0805 | C17520 | Basic | Audio level divider |
| C1, C2 | 10 µF X5R ≥10V | 0805 | C15850 | Basic | +5V edge bulk / LDO input |
| C3 | 22 µF X5R ≥6.3V | 0805 | C45783 | Basic | LDO output — MLCC-compatible AMS1117 variant required |
| C4, C6–C10 | 100 nF X7R 50V | 0805 | C49678 | Basic | Per-IC decoupling (C5 intentionally unused) |
| C13, C14 | 10 nF X7R | 0805 | C1710 | Basic | Audio 2-pole RC filter |
| C12 | 1000 µF 6.3V SMD electrolytic | D8x10 mm | pick at order (C970711 had stock 2026-09-17) | n/a | VSYS_PICO Wi-Fi burst cap — **DNP** on a Pico 2 |
| C15 | 100 nF | 0805 | C49678 | Basic | AC-coupling option across R21 — **DNP** |

## Connectors and mechanical

| Ref | Part | Footprint | Notes |
|-----|------|-----------|-------|
| P1  | (custom COCO-CART edge fingers) | `PiCoCo:COCO-CART-2.1X1.75` | PCB fingers only; no connector part |
| JP2 | Solder jumper, 3-pad bridged 1-2 | `Jumper:SolderJumper-3_P1.3mm_Bridged12_RoundedPad1.0x1.5mm` | U10 /OE source select |
| JP3 | Solder jumper, 3-pad bridged 1-2 | (same footprint) | Header pin 34: E (default) or audio |
| TP1–TP7 | `TestPoint:TestPoint_Pad_1.0x1.0mm` | — | OE_BUS, RW_BUF, CTS_BUF, SCS_BUF, E_BUF, +3V3, SND_CART |
| FID1, FID2 | `Fiducial:Fiducial_1mm_Mask2mm` | — | SMT assembly fiducials, no net, excluded from BOM |

There is no `J_SWD` header and no mounting hole (`MTG1`) on this board — both
were removed in v2.3 (see `CLAUDE.md` and `docs/hardware-design.md` §7, §9).

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
  passives (Q3/Q4/R15–R18/C12/C15 DNP by default) not included.
- **ENIG, no gold fingers, no bevel (budget/bare-board variant)**:
  subtract roughly $30–$50 from the above — fine for a lightly used
  cart, but the fingers will wear faster in a well-used CoCo slot.

Four LCSC numbers were not re-verified on 2026-09-17: C45783 (C3, must be
22 µF at ≥6.3 V), C1710, C17513, C17520 — confirm them in JLCPCB's parts
step. Prices drift; check DigiKey/Mouser/LCSC and JLCPCB's current quote
before ordering. Substitute equivalents freely on the generics (0805
passives, SOIC logic) — the part numbers above are one known-good
source each.
