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
#
# The empty package.json is not a formality: without one, npm walks up from
# $WORK looking for a project root, and any stray package.json left in $TMPDIR
# by something else makes it install there -- or report "up to date" and
# install nothing at all -- leaving $WORK/node_modules missing.
echo '{"private":true}' > "$WORK/package.json"
( cd "$WORK" && npm install --silent --no-audit --no-fund marked@12 mermaid@11 )

# md2html.mjs is run from $WORK, not from the tree. Node resolves a bare ESM
# import like `from 'marked'` by walking up from the *importing module's* own
# directory -- not from the cwd, and NODE_PATH is ignored for ESM -- so running
# it in place would search docs/floating_window/tools/node_modules upwards and
# never find the packages that were just installed into $WORK.
cp "$DOC_DIR/tools/md2html.mjs" "$WORK/md2html.mjs"

for md in "$DOC_DIR"/*.md; do
  name="$(basename "$md" .md)"
  node "$WORK/md2html.mjs" "$md" "$WORK/$name.html" \
       "$WORK/node_modules/mermaid/dist/mermaid.min.js"
  # --virtual-time-budget lets mermaid finish laying out before the page is printed;
  # without it the PDF can capture the document with the diagrams still unrendered.
  "$CHROME" --headless --no-sandbox --disable-gpu \
    --virtual-time-budget=30000 --no-pdf-header-footer \
    --print-to-pdf="$DOC_DIR/$name.pdf" "file://$WORK/$name.html" 2>/dev/null
  echo "wrote $DOC_DIR/$name.pdf"
done
