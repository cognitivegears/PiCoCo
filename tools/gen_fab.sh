#!/usr/bin/env bash
# Generate PCB fabrication outputs for PiCoCo using kicad-cli.
#
# Produces Gerbers, drill files, pick-and-place, and BOM in ../fab/.
# DRC is intentionally NOT run: kicad-cli pcb drc crashes on current
# macOS with a Swift runtime error. Run DRC in the KiCad GUI before
# ordering boards.

set -euo pipefail

KICAD_CLI="${KICAD_CLI:-/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli}"
if ! [ -x "$KICAD_CLI" ]; then
    if command -v kicad-cli >/dev/null 2>&1; then
        KICAD_CLI="$(command -v kicad-cli)"
    else
        echo "error: kicad-cli not found. Set KICAD_CLI env var." >&2
        exit 1
    fi
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
# Usage: gen_fab.sh [path/to/board.kicad_pcb] [output-dir]
PCB="${1:-$PROJECT_ROOT/PiCoCo/PiCoCo.kicad_pcb}"
SCH="${PCB%.kicad_pcb}.kicad_sch"
FAB_DIR="${2:-$PROJECT_ROOT/fab}"
BASE="$(basename "$PCB" .kicad_pcb)"

if [ ! -f "$PCB" ]; then
    echo "error: PCB not found: $PCB" >&2
    exit 1
fi

mkdir -p "$FAB_DIR"

echo "=> Gerbers"
"$KICAD_CLI" pcb export gerbers \
    --output "$FAB_DIR/" \
    --layers "F.Cu,B.Cu,F.Paste,B.Paste,F.Silkscreen,B.Silkscreen,F.Mask,B.Mask,Edge.Cuts" \
    --subtract-soldermask \
    --no-x2 \
    --use-drill-file-origin \
    "$PCB"

echo "=> Drill"
"$KICAD_CLI" pcb export drill \
    --output "$FAB_DIR/" \
    --format excellon \
    --drill-origin plot \
    --excellon-zeros-format decimal \
    --excellon-units mm \
    --generate-map \
    --map-format gerberx2 \
    "$PCB"

echo "=> Position (pick-and-place)"
"$KICAD_CLI" pcb export pos \
    --output "$FAB_DIR/$BASE-pos.csv" \
    --format csv \
    --units mm \
    --use-drill-file-origin \
    "$PCB"

echo "=> BOM (best-effort; edit BOM.md by hand for final ordering)"
"$KICAD_CLI" sch export bom \
    --output "$FAB_DIR/$BASE-BOM.csv" \
    --preset "Grouped By Value" \
    --format-preset CSV \
    "$SCH" 2>/dev/null || \
    echo "   (skipped: 'sch export bom' not available in this kicad-cli; see BOM.md)"

echo "=> Zip for upload"
(cd "$FAB_DIR" && rm -f "$BASE-gerbers.zip" && zip -q "$BASE-gerbers.zip" *.g* *.drl)

echo
echo "=> Done. Outputs in $FAB_DIR/"
echo "   * Review fab/READ-BEFORE-ORDERING.txt before submitting."
echo "   * Run DRC before ordering: kicad-cli pcb drc --schematic-parity (outside the Claude sandbox) or the KiCad GUI."
