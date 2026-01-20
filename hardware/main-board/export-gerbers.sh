#!/bin/bash
# Export Gerber files for JLCPCB
# Usage: ./export-gerbers.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_NAME="neopico-hd"
PCB_FILE="$SCRIPT_DIR/$PROJECT_NAME.kicad_pcb"
OUTPUT_DIR="$SCRIPT_DIR/gerbers"
ZIP_FILE="$SCRIPT_DIR/$PROJECT_NAME-gerbers-jlcpcb.zip"

# Find kicad-cli
if command -v kicad-cli &> /dev/null; then
    KICAD_CLI="kicad-cli"
elif [ -f "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli" ]; then
    KICAD_CLI="/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"
else
    echo "Error: kicad-cli not found"
    exit 1
fi

echo "Using: $KICAD_CLI"
echo "PCB: $PCB_FILE"
echo ""

# Create output directory
mkdir -p "$OUTPUT_DIR"

# Export Gerbers
echo "Exporting Gerber files..."
"$KICAD_CLI" pcb export gerbers \
    --output "$OUTPUT_DIR/" \
    "$PCB_FILE"

# Export Drill files
echo "Exporting Drill files..."
"$KICAD_CLI" pcb export drill \
    --output "$OUTPUT_DIR/" \
    --format excellon \
    --excellon-units mm \
    "$PCB_FILE"

# Create ZIP for JLCPCB (only essential files)
echo "Creating ZIP for JLCPCB..."
rm -f "$ZIP_FILE"

cd "$OUTPUT_DIR"
zip -j "$ZIP_FILE" \
    "$PROJECT_NAME-F_Cu.gtl" \
    "$PROJECT_NAME-B_Cu.gbl" \
    "$PROJECT_NAME-F_Silkscreen.gto" \
    "$PROJECT_NAME-B_Silkscreen.gbo" \
    "$PROJECT_NAME-F_Mask.gts" \
    "$PROJECT_NAME-B_Mask.gbs" \
    "$PROJECT_NAME-F_Paste.gtp" \
    "$PROJECT_NAME-B_Paste.gbp" \
    "$PROJECT_NAME-Edge_Cuts.gm1" \
    "$PROJECT_NAME.drl"

echo ""
echo "Done! Output: $ZIP_FILE"
echo ""
ls -lh "$ZIP_FILE"

# Open Finder and highlight the file (macOS)
if [[ "$OSTYPE" == "darwin"* ]]; then
    open -R "$ZIP_FILE"
fi
