// Renders ```mermaid fences to inline SVG at build time (no client JS).
// Each diagram is rendered twice, with the light and dark palettes; CSS shows the one for the active theme.
// Needs Chromium (playwright). If it cannot launch, or RZ_SKIP_MERMAID=1, the source is kept as a code block.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { createRequire } from 'node:module';
import { visit } from 'unist-util-visit';
import { toString } from 'hast-util-to-string';
import { fromHtml } from 'hast-util-from-html';

const require = createRequire(import.meta.url);
const FONT = 'Hanken Grotesk Variable';

const THEMES = {
  light: { primaryColor: '#FFFFFF', primaryBorderColor: '#0A0A0A', primaryTextColor: '#0A0A0A', lineColor: '#0A0A0A', textColor: '#0A0A0A',
           secondaryColor: '#F3F3F3', tertiaryColor: '#F3F3F3', clusterBkg: '#F3F3F3', clusterBorder: '#8A8A8A', edgeLabelBackground: '#F3F3F3', noteBkgColor: '#F3F3F3', noteTextColor: '#0A0A0A', noteBorderColor: '#8A8A8A' },
  dark:  { primaryColor: '#151515', primaryBorderColor: '#ECECEC', primaryTextColor: '#ECECEC', lineColor: '#ECECEC', textColor: '#ECECEC',
           secondaryColor: '#0C0C0C', tertiaryColor: '#0C0C0C', clusterBkg: '#101010', clusterBorder: '#6A6A6A', edgeLabelBackground: '#0C0C0C', noteBkgColor: '#101010', noteTextColor: '#ECECEC', noteBorderColor: '#6A6A6A' },
};

let rendererPromise;
let disabled = !!process.env.RZ_SKIP_MERMAID;
let cssUrl;
let counter = 0;

async function getRenderer() {
  if (!rendererPromise) {
    rendererPromise = (async () => {
      const { createMermaidRenderer } = await import('mermaid-isomorphic');
      // font-face for the headless page so node sizes are measured with the same font the site uses
      const pkg = path.dirname(require.resolve('@fontsource-variable/hanken-grotesk/package.json'));
      const woff = path.join(pkg, 'files', 'hanken-grotesk-latin-wght-normal.woff2');
      const cssFile = path.join(os.tmpdir(), 'razobill-mermaid-font.css');
      fs.writeFileSync(cssFile, `@font-face{font-family:"${FONT}";src:url("${pathToFileURL(woff).href}") format("woff2");font-weight:100 900;}`);
      cssUrl = pathToFileURL(cssFile);
      return createMermaidRenderer();
    })();
  }
  return rendererPromise;
}

const cfg = (theme) => ({
  theme: 'base', fontFamily: `"${FONT}", system-ui, sans-serif`, fontSize: 15,
  themeVariables: { ...THEMES[theme], fontFamily: `"${FONT}", system-ui, sans-serif`, fontSize: '15px' },
  flowchart: { curve: 'linear', htmlLabels: true, padding: 12, nodeSpacing: 28, rankSpacing: 36, wrappingWidth: 170, useMaxWidth: false },
  sequence: { useMaxWidth: false }, state: { useMaxWidth: false }, class: { useMaxWidth: false }, er: { useMaxWidth: false }, gantt: { useMaxWidth: false },
});

const svgNodes = (svg) => fromHtml(svg, { fragment: true, space: 'svg' }).children;

export default function rehypeMermaid() {
  return async (tree) => {
    const found = [];
    visit(tree, 'element', (node, index, parent) => {
      if (node.tagName !== 'pre' || !parent) return;
      const code = node.children?.find((c) => c.type === 'element' && c.tagName === 'code');
      const cls = code?.properties?.className ?? [];
      if (code && cls.includes('language-mermaid')) found.push({ node, index, parent, source: toString(code) });
    });
    if (!found.length) return;

    const keep = (f) => { f.node.properties = { ...f.node.properties, className: ['mermaid-source'] }; };
    if (disabled) return found.forEach(keep);

    try {
      const render = await getRenderer();
      const sources = found.map((f) => f.source);
      const run = async (theme) => render(sources, { css: cssUrl, prefix: `rz-${theme[0]}${counter++}`, mermaidConfig: cfg(theme) });
      const [light, dark] = [await run('light'), await run('dark')];
      found.forEach((f, i) => {
        if (light[i].status !== 'fulfilled' || dark[i].status !== 'fulfilled') {
          console.warn('[mermaid] diagram failed to render, keeping source:', light[i].reason ?? dark[i].reason);
          return keep(f);
        }
        const wrap = (cls, svg) => ({ type: 'element', tagName: 'div', properties: { className: [cls] }, children: svgNodes(svg) });
        f.parent.children[f.index] = {
          type: 'element', tagName: 'figure',
          properties: { className: ['diagram'], ariaLabel: 'Diagram' },
          children: [wrap('diagram-light', light[i].value.svg), wrap('diagram-dark', dark[i].value.svg)],
        };
      });
    } catch (err) {
      disabled = true;
      console.warn('[mermaid] disabled (could not launch Chromium). Run `npx playwright install chromium`.', err.message);
      found.forEach(keep);
    }
  };
}
