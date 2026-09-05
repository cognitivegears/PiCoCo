# PiCoCo — Bill of Materials (v2.2)

Intended for a single-board hand build or a JLCPCB PCBA order. Prices
are 2026 ballparks from DigiKey/Mouser/LCSC and will drift; check
before ordering. Substitute equivalents freely on the generics (0805
passives, SOIC-20 logic) — the part numbers listed are one known good
source each.

## Active components

| Ref | Value | Package | DigiKey | Mouser | LCSC |
|-----|-------|---------|---------|--------|------|
| U1  | Raspberry Pi Pico 2 | SMD+TH module | SC1631-ND | 358-SC1631 | (not stocked — order from authorized resellers: adafruit.com / pishop.us) |
| U10 | SN74LVC8T245DW | SOIC-24W | 296-51395-1-ND | 595-SN74LVC8T245DW | C9963 |
| U11 | SN74LVC245ADWR | SOIC-20W | 296-8513-1-ND | 595-SN74LVC245ADWR | C35952 |
| U12 | SN74LVC245ADWR | SOIC-20W | (same) | (same) | C35952 |
| U13 | SN74LVC245ADWR | SOIC-20W | (same) | (same) | C35952 |
| U14 | AMS1117-3.3 | SOT-223-3 | LM1117IMPX-3.3/NOPBCT-ND | 863-LM1117IMPX-3.3 | C6186 |
| U15 | SN74LVC1G11DCK | SC-70-6 | 296-15856-1-ND | 595-SN74LVC1G11DCK | C130103 |
| Q2  | 2N7002 | SOT-23 | 2N7002-TPMSCT-ND | 863-2N7002 | C8545 |
| D2  | SS14 | SMA (DO-214AC) | SS14FSCT-ND | 625-SS14-E3/61T | C2480 |

## Passives (all 0805)

| Ref | Value | Tolerance | DigiKey series | Notes |
|-----|-------|-----------|-----------------|-------|
| R1  | 4.7 kΩ | 1% | RC0805FR-074K7L | /HALT pull-up (required) |
| R2  | 4.7 kΩ | 1% | (same) | /NMI pull-up — **DNP** |
| R3  | 4.7 kΩ | 1% | (same) | /RESET pull-up — **DNP** |
| R4  | 10 kΩ | 1% | RC0805FR-0710KL | 3V3_EN pull-up to VSYS_PICO |
| R6  | 4.7 kΩ | 1% | (same as R1) | /CART pull-up (in series with JP1) |
| R7  | 100 kΩ | 1% | RC0805FR-07100KL | Q2 gate pull-up to +3V3 |
| R8  | 100 Ω | 1% | RC0805FR-07100RL | Q2 gate series |
| R9  | 100 Ω | 1% | (same as R8) | Pico RUN series |
| R10 | 10 kΩ | 1% | (same as R4) | Pico RUN pull-up |
| R11 | 33 Ω  | 1% | RC0805FR-0733RL | OE_BUS series termination |
| R12 | 33 Ω  | 1% | (same as R11) | RW_BUF series termination |
| R13 | 100 Ω | 1% | (same as R8) | SWCLK series |
| R14 | 100 Ω | 1% | (same as R8) | SWDIO series |

| Ref | Value | Voltage rating | Dielectric | Notes |
|-----|-------|----------------|------------|-------|
| C1  | 10 µF | ≥10 V | X5R | +5 V edge bulk |
| C2  | 10 µF | ≥10 V | X5R | LDO input |
| C3  | 22 µF | ≥6.3 V | X5R | LDO output — **MLCC-compatible AMS1117 variant required** |
| C4–C10 | 100 nF | ≥10 V | X7R | Per-IC decoupling |
| C11 | 10 µF | ≥10 V | X5R | Local bulk at U10 Vccb |

## Connectors

| Ref | Part | Footprint | Notes |
|-----|------|-----------|-------|
| P1  | (custom COCO-CART edge fingers) | `PiCoCo:COCO-CART-2.1X1.75` | PCB fingers only; no connector part |
| JP1 | 1×2 0.1" pin header + shunt | `PinHeader_1x02_P2.54mm_Vertical` | /CART pull-up enable |
| J_SWD | 1×4 0.1" pin header | `PinHeader_1x04_P2.54mm_Vertical` | Pico SWD access |

## Test points

| Ref | Footprint | Net |
|-----|-----------|-----|
| TP1 | `TestPoint:TestPoint_Pad_1.0x1.0mm` | OE_BUS |
| TP2 | (same) | RW_BUF |
| TP3 | (same) | CTS_BUF |
| TP4 | (same) | SCS_BUF |
| TP5 | (same) | E_B |
| TP6 | (same) | +3V3 |

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
- **SN74LVC1G11**: three-input positive-AND in SC-70-6. NC7SZ11 is
  pin-compatible.

## Non-board parts

- **Assembled HDB-DOS ROM image (16 KB)**: for MVP firmware (Phase 1),
  users must supply their own binary. Sources: Cloud9 `HDB-DOS-DW`
  build (commercial), or extract from a physical Disk BASIC cart if
  owned.
- **USB-C to USB-A cable**: for DriveWire host connection (Phase 1
  bridge) or for flashing (SWD alternative path via BOOTSEL).
- **DriveWire host software**: `pyDriveWire` (open source) or
  DriveWire4 (Java, Cloud9). Runs on any PC/Mac/Linux with a USB port.

## Cost estimate (1 board, 2026 pricing)

- PCB fab (5 boards minimum, JLCPCB with hard gold fingers): ~$35–50
  shipped (one-time setup dominates)
- Active ICs: ~$3
- Pico 2 module: ~$5
- Passives: ~$1.50
- Connectors: ~$2
- **Single-board cost: ~$15–20 plus amortized fab NRE.**

For 10 boards with JLCPCB PCBA (SMT only, THT by hand):
approximately **$150–200 total, ~$15–20 per assembled board**.
