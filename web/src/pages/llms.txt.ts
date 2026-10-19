import type { APIRoute } from 'astro';
import { loadDocs } from '../lib/docs';
import { SITE, REPO, PYPI, PAPER, version } from '../data/site';

export const GET: APIRoute = async () => {
  const { nav } = await loadDocs();
  const link = (href: string) => new URL(href.replace(/^\/razobill\//, ''), SITE).href;
  const body = `# Razobill

> Razobill is an open-source (Apache-2.0) log parser written in C++20. It splits each log line into a template, numbers and words, stores them as plain columns, and joins them back into the exact original line. It ships as a Python package (pyrazobill), a DuckDB community extension and a C API. Current version: ${version}.

## Key facts

- Lossless: 0 mismatches across 69.6 million lines (4,861 files, 6.82 GB) in the project's own audit.
- Install: \`pip install pyrazobill\`, or in DuckDB \`INSTALL razobill FROM community; LOAD razobill;\`.
- Functions: RzSplit(line) returns (template, ints, strs); RzJoin(template, ints, strs) returns the original line; RzTemplate(line) returns the template only.
- Against Drain3 on one thread: 2.5x to 15.1x faster. Against CLP IR: needle search 31x to 83x faster, join 1.4x to 1.8x faster; CLP ingests faster.
- Limits: not a clustering tool (median grouping accuracy 0.808 vs 0.871 for Drain3); benchmarks are author-run on one machine; one line is capped at 16 MiB.
- Source: ${REPO}  PyPI: ${PYPI}  Paper: ${PAPER}

## Documentation
${nav.filter((g) => g.items.length).map((g) => `\n### ${g.title}\n` + g.items.map((i) => `- [${i.title}](${link(i.href)}): ${i.summary}`).join('\n')).join('\n')}

## Optional

- [Full documentation as one text file](${new URL('llms-full.txt', SITE).href})
`;
  return new Response(body, { headers: { 'Content-Type': 'text/plain; charset=utf-8' } });
};
