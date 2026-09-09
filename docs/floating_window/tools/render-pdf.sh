#!/bin/bash
# Regenerates the PDF next to each Markdown doc in this directory.
#
# Markdown -> HTML (marked), with ```mermaid blocks left as <pre class="mermaid">
# and mermaid.min.js inlined; then this checkout's own Chromium prints it to PDF.
# Using the local build avoids needing puppeteer or a system Chrome.
#
# Usage:  docs/floating_window/tools/render-pdf.sh [out_dir_with_a_built_chrome]
set -euo pipefail

OUT_DIR="${1:-out/Linux}"
SRC_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
DOC_DIR="$SRC_ROOT/docs/floating_window"
CHROME="$SRC_ROOT/$OUT_DIR/chrome"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

[ -x "$CHROME" ] || { echo "no chrome at $CHROME; build it first" >&2; exit 1; }

# marked renders the Markdown; mermaid renders the diagrams in-page.
( cd "$WORK" && npm install --silent --no-audit --no-fund marked@12 mermaid@11 )

for md in "$DOC_DIR"/*.md; do
  name="$(basename "$md" .md)"
  node "$DOC_DIR/tools/md2html.mjs" "$md" "$WORK/$name.html" \
       "$WORK/node_modules/mermaid/dist/mermaid.min.js"
  # --virtual-time-budget lets mermaid finish laying out before the page is printed;
  # without it the PDF can capture the document with the diagrams still unrendered.
  "$CHROME" --headless --no-sandbox --disable-gpu \
    --virtual-time-budget=30000 --no-pdf-header-footer \
    --print-to-pdf="$DOC_DIR/$name.pdf" "file://$WORK/$name.html" 2>/dev/null
  echo "wrote $DOC_DIR/$name.pdf"
done
