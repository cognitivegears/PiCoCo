# PiCoCo — Fab Outputs

This directory holds the machine-generated outputs used to order PCBs
from a board house. All artifacts here are produced by
`tools/gen_fab.sh` from the KiCad project in `../PiCoCo/`.

## Regeneration

```bash
bash tools/gen_fab.sh
```

Main board outputs land in `main/` (breakout/cobbler boards keep their
own `breakout/` and `cobbler/` subdirectories). Produces, under `main/`:

- `PiCoCo-*.gbr`/`.gtl`/`.gbl`/etc. — one Gerber per copper/silk/mask layer.
- `PiCoCo-*.drl` — Excellon drill file.
- `PiCoCo-pos.csv` — full pick-and-place (position) file.
- `PiCoCo-gerbers.zip` — zipped Gerbers + drill, ready to upload.
- `PiCoCo-BOM-jlc.csv` / `PiCoCo-CPL-jlc.csv` — JLCPCB assembly BOM and
  CPL (DNP parts and non-BOM footprints excluded).
- `stencil-module/PiCoCo-stencil-module-F_Paste.gbr` — standalone paste
  stencil for hand-soldering the Pico 2 module onto U1's castellations.

DRC is **not** run by the script because `kicad-cli pcb drc` crashes
on the current macOS build with a Swift runtime error. Run DRC in the
KiCad GUI before ordering: `Inspect → Design Rule Checker`.

## Ordering

See `main/READ-BEFORE-ORDERING.txt` for the JLCPCB order-form checklist.
The hard-gold finger / 30° bevel / 1 µm plating settings are not
optional — ignoring them will damage the CoCo slot on the first
insertion.

## Zipping for upload

`tools/gen_fab.sh` zips the Gerbers + drill into `main/PiCoCo-gerbers.zip`
automatically. The BOM/CPL CSVs and the module stencil are uploaded
separately for assembly services.
