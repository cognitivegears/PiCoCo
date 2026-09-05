# PiCoCo — Fab Outputs

This directory holds the machine-generated outputs used to order PCBs
from a board house. All artifacts here are produced by
`tools/gen_fab.sh` from the KiCad project in `../PiCoCo/`.

## Regeneration

```bash
bash tools/gen_fab.sh
```

Produces, under this directory:

- `PiCoCo-*.gbr` — one Gerber per copper/silk/mask layer.
- `PiCoCo-*.drl` — Excellon drill file.
- `PiCoCo-pos.csv` — pick-and-place (position) file.
- `PiCoCo-BOM.csv` — bill of materials in JLCPCB CSV format.

DRC is **not** run by the script because `kicad-cli pcb drc` crashes
on the current macOS build with a Swift runtime error. Run DRC in the
KiCad GUI before ordering: `Inspect → Design Rule Checker`.

## Ordering

See `READ-BEFORE-ORDERING.txt` for the JLCPCB order-form checklist.
The hard-gold finger / 30° bevel / 1 µm plating settings are not
optional — ignoring them will damage the CoCo slot on the first
insertion.

## Zipping for upload

Board houses accept a zip of the gerbers + drill. Position CSV and
BOM are uploaded separately for assembly services. A quick zip:

```bash
cd fab
zip PiCoCo-gerbers.zip PiCoCo-*.gbr PiCoCo-*.drl
```

The resulting `PiCoCo-gerbers.zip` is what you upload to
JLCPCB/PCBWay.
