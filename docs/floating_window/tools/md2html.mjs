import fs from 'node:fs';
import path from 'node:path';
import { marked } from 'marked';

const [,, inFile, outFile, mermaidPath] = process.argv;
const md = fs.readFileSync(inFile, 'utf8');
const mermaidJs = fs.readFileSync(mermaidPath, 'utf8');

const esc = (t) => t.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

// Keep ```mermaid blocks as <pre class="mermaid"> with the source HTML-escaped, so
// that textContent hands mermaid back the literal <br/> and <small> markup its
// label parser expects.
marked.use({
  renderer: {
    code(codeOrTok, infostring) {
      const isTok = typeof codeOrTok === 'object' && codeOrTok !== null;
      const text = isTok ? codeOrTok.text : codeOrTok;
      const lang = (isTok ? codeOrTok.lang : infostring) || '';
      if (lang.trim() === 'mermaid') {
        return `<pre class="mermaid">${esc(text)}</pre>\n`;
      }
      return `<pre><code>${esc(text)}</code></pre>\n`;
    },
  },
});

const body = marked.parse(md, { gfm: true });
const title = (md.match(/^#\s+(.+)$/m) || [null, path.basename(inFile)])[1]
  .replace(/`/g, '');

const html = `<!doctype html>
<html><head><meta charset="utf-8"><title>${esc(title)}</title>
<style>
  @page { size: A4; margin: 18mm 16mm; }
  body { font: 10.5pt/1.55 -apple-system, "Segoe UI", Roboto, "Helvetica Neue", sans-serif;
         color: #1a1a1a; max-width: none; margin: 0; }
  h1 { font-size: 21pt; margin: 0 0 .3em; border-bottom: 2px solid #333; padding-bottom: .25em; }
  h2 { font-size: 15pt; margin: 1.6em 0 .5em; border-bottom: 1px solid #ccc;
       padding-bottom: .18em; break-after: avoid; }
  h3 { font-size: 12pt; margin: 1.2em 0 .4em; break-after: avoid; }
  p, li { orphans: 3; widows: 3; }
  code { font-family: "SF Mono", Menlo, Consolas, monospace; font-size: 9pt;
         background: #f2f3f5; padding: .1em .32em; border-radius: 3px; }
  pre { background: #f7f8fa; border: 1px solid #e2e4e8; border-radius: 5px;
        padding: .7em .9em; overflow-x: auto; break-inside: avoid; }
  pre code { background: none; padding: 0; font-size: 8.6pt; line-height: 1.45; }
  blockquote { border-left: 3px solid #b9c0c9; margin: 1em 0; padding: .1em 1em;
               color: #444; background: #fafbfc; break-inside: avoid; }
  table { border-collapse: collapse; margin: 1em 0; font-size: 9.3pt; width: 100%;
          break-inside: avoid; }
  th, td { border: 1px solid #d5d8dd; padding: .38em .6em; text-align: left;
           vertical-align: top; }
  th { background: #eef0f3; }
  pre.mermaid { background: none; border: none; text-align: center; padding: .4em 0;
                break-inside: avoid; margin: 1.2em 0; }
  pre.mermaid svg { max-width: 100%; height: auto; }
  pre.mermaid small { font-size: .82em; opacity: .88; }
  pre.mermaid .edgeLabel small { font-size: .85em; }
  hr { border: none; border-top: 1px solid #ddd; margin: 1.8em 0; }
  a { color: #12508f; text-decoration: none; }
</style></head><body>
${body}
<script>${mermaidJs}</script>
<script>
  mermaid.initialize({
    startOnLoad: false, theme: 'neutral', securityLevel: 'loose',
    flowchart: { htmlLabels: true, curve: 'basis' },
    sequence: { useMaxWidth: true, wrap: false },
  });
  mermaid.run({ querySelector: 'pre.mermaid' })
    .then(() => { document.title = ${JSON.stringify(title)}; window.__mermaidDone = true; })
    .catch(e => { console.error('MERMAID FAIL', e); window.__mermaidDone = 'error'; });
</script>
</body></html>`;

fs.writeFileSync(outFile, html);
console.log('wrote', outFile, (html.length / 1024).toFixed(0) + 'KB',
            'mermaid blocks:', (body.match(/pre class="mermaid"/g) || []).length);
