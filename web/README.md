# Razobill web

The landing page and documentation site, built with [Astro](https://astro.build) and published to
<https://razobill.github.io/razobill/> by `.github/workflows/pages.yml`.

The documentation is **the repository's own `docs/` folder**, rendered unchanged. Edit a markdown
file under `docs/` and the site updates. There is no second copy to keep in sync.

Why it looks the way it does, and what each choice rests on, is in [DESIGN.md](DESIGN.md).

## Run it

Needs Node 22.22 or newer (Astro 7's requirement).

```bash
cd web
npm install
npx playwright install chromium   # once: renders the Mermaid diagrams at build time
npm run dev                       # http://localhost:4321/razobill/
npm run build                     # static site in dist/, plus the search index
npm run preview                   # serves the production build under /razobill/
```

Search (Ctrl K or ⌘K) works only after `npm run build`, because the index is created from the built pages.
Set `RZ_SKIP_MERMAID=1` to build without Chromium; diagrams then show their source instead.

## Layout

```text
web/
  astro.config.mjs        site URL and base path, markdown pipeline
  src/
    pages/                index.astro (landing), docs/, 404, sitemap.xml, llms.txt, llms-full.txt
    layouts/ components/  Base, Docs, header, footer, search, theme toggle
    styles/               global.css (tokens + landing), docs.css
    scripts/              console.ts (hero demo), ui.ts (tabs, copy, diagrams, scroll-spy)
    plugins/              remark-docs (links, breadcrumbs), rehype-mermaid, rehype-tables
    lib/                  console renderer shared by build and browser, docs helpers
    data/                 numbers.ts (benchmarks, FAQ, limits), docs-nav.ts, examples.json, site.ts
  public/                 logos, favicons, og.png, video/, robots.txt
```

## Everyday tasks

**Edit or add a documentation page.** Change or add a `.md` file under `docs/`. Use the H1 as the title
and put a one-sentence summary directly under it, because the docs index and search use both.
New pages appear automatically under "More". To place one in a group, add its id (the path without `.md`,
for example `how-to/my-guide`) to `src/data/docs-nav.ts`. Relative links such as `../reference/c-abi.md`
are rewritten to site routes, and links to files outside `docs/` become GitHub links.

**Change a figure on the landing page.** Edit `src/data/numbers.ts`. Each row says which docs table it came from.
Keep it that way: a number on the landing page should match the docs.

**Change colours.** Edit the tokens at the top of `src/styles/global.css`.

## Deployment

In the repository settings, set **Pages > Source** to **GitHub Actions**. After that, every push to `main`
that touches `web/`, `docs/` or `VERSION` builds and deploys. Pull requests build without deploying,
so broken links or diagrams fail the check.

On a custom domain, set `site` and `base` in `astro.config.mjs` (`base: '/'`) and add a `CNAME` file to `public/`.
Also update `SITE` in `src/data/site.ts` and the `Sitemap:` line in `public/robots.txt`.
Note that crawlers only read `robots.txt` at a domain root, so on a project page it has no effect.

## Checks that were run

- `axe-core` at WCAG 2.2 AA plus best-practice rules on six page types, two themes, two viewports: 0 violations.
- The hero demo finishes in about 3.4 s (under the 5 s limit in WCAG 2.2.2), and shows its finished state with
  JavaScript off or with reduced motion.
- All nine docs pages with Mermaid diagrams render them; no relative `.md` links survive the build.

## Third-party names and licences

DuckDB and ClickHouse names and logos are used unaltered, only to identify compatible software. Check each
project's trademark guidelines before changing how they appear. Fonts are Martian Mono and Hanken Grotesk
(SIL Open Font License, via Fontsource).

## Known notes

`npm audit` reports three low-severity advisories in KaTeX, pulled in by Mermaid. They apply to rendering
untrusted input; here Mermaid runs only at build time on the repository's own diagrams, and nothing from it
is shipped to visitors.
