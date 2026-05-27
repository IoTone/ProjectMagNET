#!/usr/bin/env bash
# render-diagrams.sh — extract every ```mermaid``` block from ../PROPOSAL.md
# and render each to SVG + PNG in this directory.
#
# Requires:
#   - mmdc (`brew install mermaid-cli`)
#   - Google Chrome at /Applications/Google Chrome.app  (puppeteer needs it;
#     mmdc 11.15 wants Chrome v148 which isn't in the puppeteer cache, so we
#     point at the system Chrome instead of letting mmdc download one)
#
# Naming convention: each diagram is named by the first non-keyword token
# its mermaid source contains, OR by a manual index — see DIAGRAM_NAMES
# below. Add a new entry there when you add a new mermaid block to the
# proposal, in document order.
#
# Usage: cd into this directory then `./render-diagrams.sh`.

set -euo pipefail

PROP="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/PROPOSAL.md"
OUTDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Document-order names for each mermaid block in PROPOSAL.md.
# When you add another mermaid block, append its name here.
DIAGRAM_NAMES=(
  dataspace-architecture
  join-code-interaction
)

# Puppeteer config — mmdc's bundled chrome-headless-shell wants a specific
# Chrome version not present in the cache; redirect to system Chrome.
PUPPETEER_CFG="$(mktemp -t mmdc-puppeteer-XXXX.json)"
cat > "$PUPPETEER_CFG" <<'EOF'
{
  "executablePath": "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
  "args": ["--no-sandbox", "--disable-setuid-sandbox"]
}
EOF
trap 'rm -f "$PUPPETEER_CFG"' EXIT

echo "Extracting mermaid blocks from: $PROP"
awk -v outdir="$OUTDIR" '
  /^```mermaid$/ { in_m=1; idx++; out=outdir "/_block-" idx ".mmd"; next }
  in_m && /^```$/ { in_m=0; close(out); next }
  in_m { print > out }
' "$PROP"

EXTRACTED_COUNT=$(ls "$OUTDIR"/_block-*.mmd 2>/dev/null | wc -l | tr -d ' ')
echo "Extracted $EXTRACTED_COUNT mermaid block(s)."

if [ "$EXTRACTED_COUNT" -ne "${#DIAGRAM_NAMES[@]}" ]; then
  echo "WARN: $EXTRACTED_COUNT block(s) extracted but DIAGRAM_NAMES has ${#DIAGRAM_NAMES[@]} entries." >&2
  echo "      Edit DIAGRAM_NAMES at the top of this script to match document order." >&2
fi

for i in "${!DIAGRAM_NAMES[@]}"; do
  idx=$((i + 1))
  name="${DIAGRAM_NAMES[$i]}"
  src="$OUTDIR/_block-${idx}.mmd"
  if [ ! -f "$src" ]; then
    echo "skip: $name (no block #$idx)"
    continue
  fi
  mv "$src" "$OUTDIR/$name.mmd"
  echo "=== rendering $name ==="
  mmdc -p "$PUPPETEER_CFG" -i "$OUTDIR/$name.mmd" -o "$OUTDIR/$name.svg" \
       -t default --backgroundColor white 2>&1 | tail -2
  mmdc -p "$PUPPETEER_CFG" -i "$OUTDIR/$name.mmd" -o "$OUTDIR/$name.png" \
       -t default --backgroundColor white -s 2 -w 2400 2>&1 | tail -2
done

# Remove any stragglers from a previous run with a different DIAGRAM_NAMES size.
rm -f "$OUTDIR"/_block-*.mmd

echo
echo "Outputs:"
ls -lh "$OUTDIR"/*.{mmd,svg,png} 2>/dev/null
