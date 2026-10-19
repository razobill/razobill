# Design notes

Why the Razobill site looks and behaves the way it does, what each decision rests on, and how
strong that evidence is. Written so a maintainer can overturn a decision knowing what it cost.

The short version: ink on paper, two colours that each name a column of the data model, square
corners taken from the logo, and one demonstration of the product in place of an illustration.

## Evidence, and how much weight it carries

Colour and form psychology is the weakest part of this literature. Elliot (2015, *Frontiers in
Psychology*) calls research on colour and psychological functioning "nascent" and stresses that
colour meanings depend on context. So no decision below relies on "blue means trust". Where a
claim is strong it is used; where it is contested it is named and not leaned on.

| Decision | Basis | Strength |
| --- | --- | --- |
| Light theme by default, dark theme on request and from the system setting | Nielsen Norman Group's review of dark mode: dark text on a light background performs better for people with normal vision; dark mode can help some low-vision users; offer both | Moderate to strong |
| Contrast of at least 4.5:1 for every text colour, most at 7:1 or higher | WCAG 2.2 success criterion 1.4.3; verified by `scripts/check-colors.py` and by axe-core on every page | Strong (a standard, and measured here) |
| Colour is never the only signal | WCAG 1.4.1. Numbers and words also carry `{i}` / `{s}` glyphs, an underline, and a position in a named lane | Strong |
| Blue and vermillion for numbers and words | Okabe and Ito (2008), *Color Universal Design*: the pair is separable under the common colour-vision deficiencies. Separation was re-measured for the adjusted values: CIEDE2000 ≥ 45 under protan, deutan and tritan simulation (below) | Strong for the palette; the hue-to-meaning mapping is a convention, not a finding |
| Hues assigned by function (data type), not mood | Colour meaning is context-dependent (Elliot, 2015). Palmer and Schloss (2010, *PNAS*) show blue is *liked* more on average, which is a statement about preference, not about trust or competence | Preference result is solid; any marketing claim built on it is not |
| Square corners everywhere | For: Jiang, Gorn, Galli and Chattopadhyay (2016, *Journal of Consumer Research*) found angular shapes lead people to infer hardness and durability, and the effect fades when copy contradicts the shape, so the copy says "exact" and "byte for byte". Against: Bar and Neta (2006, n = 14, 140 object pairs) found people like curved objects more, and the preference has been replicated since; Leder, Tinio and Bar (2011) found it holds for neutral and positive objects but not negative ones | Mixed. This is a real trade-off, made because the logo is built from flat cuts and the product's promise is exactness. Calm is bought back with whitespace, thin rules and slow easing |
| Typing animation runs once, ends in about 3.4 s, never loops; the caret is solid | WCAG 2.2.2: automatically moving content that lasts more than 5 s needs a pause control, and a caret that blinks indefinitely counts | Strong (a standard) |
| Everything else animates in 100 to 260 ms, and only in response to an action | Nielsen Norman Group on animation timing: roughly 100 ms for feedback, 200 to 300 ms for larger changes | Moderate |
| Reduced motion shows the finished demo, with no typing and no transitions | `prefers-reduced-motion`; WCAG 2.3.3 | Strong |
| Monospace for headings, code and data; a proportional face for sentences | Payne (1967, *Visible Language*): proportional spacing was read significantly faster than monospaced, with no loss of comprehension. Not confirmed elsewhere: a 1993 proofreading study found no significant difference between monospaced and well-spaced proportional text, and an eye-movement study ("Eye movements, the perceptual span, and reading speed") found perceptual span unaffected by fixed versus proportional width | Weak and mixed. The rule used is information design more than reading speed: short strings that carry data are monospaced, sustained prose is not |
| Prose measure of about 62 to 70 characters | Dyson and Kipping (1998): lines of about 100 characters were read faster than 25-character lines at equal comprehension, yet readers judged about 55 easiest; Dyson and Haselgrove (2001): 55 supported both rate and comprehension; Shaikh and Chaparro (2005): 95 was fastest and satisfaction was unaffected. The studies disagree, so this is a compromise inside the commonly recommended 40 to 75 range, not a finding | Mixed |
| A code-first hero, no illustration | Evil Martians' study of 100 developer-tool landing pages: for infrastructure and observability tools the product's own output is the usual hero visual; vague copy and decorative filler hurt | Practitioner evidence, not peer reviewed |
| The "Where it falls short" section and cells where Razobill loses | Two-sided messages raise credibility, most clearly for sceptical audiences (Crowley and Hoyer, 1994; Eisend, 2006 meta-analysis). The negative points here are real but not core to the claim, which is when the effect holds | Moderate to strong |
| No customer logos, testimonials or star-count badges | None exist to show, and inventing them would break the project's own style guide | Not a research question |
| Explaining speed with the product's own data layout | The strongest idea on clickhouse.com is animating rows versus columns; here the equivalent is one template plus columns of numbers and words | Design observation |

## Palette

Brand colours were sampled from `Logo.jpeg`, `Logo-light.jpeg` and `icon.jpeg`: ink `#010101`, paper
`#F3F3F3`, dark `#0C0C0C`. The page uses `#0A0A0A` for text, and `#F3F3F3` / `#0C0C0C` as the two page colours.

| Token | Light | Dark | Meaning |
| --- | --- | --- | --- |
| `--ink` | `#0A0A0A` | `#ECECEC` | Text, rules, and the template |
| `--paper` | `#F3F3F3` | `#0C0C0C` | Page |
| `--sheet` | `#FFFFFF` | `#151515` | Raised surfaces: console, tables, code |
| `--num` | `#0072B2` | `#56B4E9` | Numbers (the `ints` column) |
| `--word` | `#B34A00` | `#FF9A55` | Words (the `strs` column) |

Output of `python scripts/check-colors.py`:

```text
light    ink 17.84:1   ink-2 7.99:1   ink-3 5.17:1   num 4.67:1   word 4.86:1      (on paper)
dark     ink 16.56:1   ink-2 8.52:1   ink-3 5.82:1   num 8.48:1   word 9.32:1      (on paper)

numbers vs words, CIEDE2000 under simulated colour-vision deficiency
         normal   protan   deutan   tritan
light     47.7     49.0     53.1     63.9
dark      47.8     45.7     50.7     56.0       (above 20 is easily distinguished)
```

`--word` in the light theme is `#B34A00`, a darkened Okabe-Ito vermillion, because the original
`#D55E00` reaches only 3.9:1 on white. Code highlighting uses the same two hues
(strings in `--word`, numbers in `--num`), so a snippet and the data model agree.

The two partner logos (DuckDB, ClickHouse) are shown unaltered in their light and dark variants.
They are the only other colour on the site, and they are third-party marks.

## What was left out on purpose

- The cartoon mascot and old illustrations. They belong to the previous brand and clash with the new mark.
- Scroll-triggered fade-and-slide on every section. Motion is spent on the demo and nowhere else.
- Cards, glass panels, gradients, glows, shadows and rounded anything.
- A star-count badge. It would need a build-time call to the GitHub API that can fail.
- A looping or autoplaying video. The clip is click-to-play.

## Where data comes from

- Demo output: `scripts/gen-examples.py` runs the real `pyrazobill` engine and writes `src/data/examples.json`.
  It exits non-zero if any line fails to join back, and the page build repeats that check.
  Run against pyrazobill 1.1.2, native engine.
- The SQL shown in the console and install tab was executed in DuckDB 1.5 with `RzSplit`, `RzJoin` and
  `RzTemplate` registered from the real engine, because the community extension could not be downloaded
  in the build sandbox. The returned structures and the `true` results match.
- Benchmark figures: `src/data/numbers.ts`. Every row is a range or a value from a table in
  `docs/explanation/benchmarks.md` or `comparisons.md`. They are the author's measurements and have not
  been independently reproduced.
- Docs: the repository's own `docs/**/*.md`, rendered unchanged. Links are rewritten to site routes at build
  time, and the breadcrumb line at the top of each page is replaced by the site's own.

## Open questions for the maintainer

These are inconsistencies in the project's own material that surfaced while the site was built from it.
The site avoids publishing any figure it could not reconcile.

1. **The old landing page and the docs disagreed on several figures.** Needle-search latency was
   0.18 to 0.52 ms on the old page and 2.7 to 8.8 ms in `comparisons.md`. Per-dataset compression
   percentages for CLP and Razobill also differed. The site uses the docs, which are newer (17 October)
   and carry per-dataset tables with source scripts.
2. **The "long run" row does not reconcile.** `README.md` and `benchmarks.md` say Razobill took 0.09 s for
   100,000 SSH lines against 26.5 s for Drain3; the old landing page said 0.30 s. 26.5 s is
   100,000 / 3,779 lines/s, so it is an extrapolation of the 25,000-line rate, not a separate measurement.
   And 0.09 s implies about 1.1 million lines/s, while the same document measures Razobill at 57,060
   lines/s in the Drain3 harness and `comparisons.md` at 331,761 lines/s in the CLP harness. The site drops
   this row. It is worth settling in the docs.
3. **The two harnesses report different Razobill speeds** (57k to 81k lines/s against Drain3, 245k to 332k
   against CLP). Both are real, so the site shows them as two separately labelled rows.
4. **Two of the four demo examples on the old page did not match the engine.** For
   `2026-10-07 14:02:11 ...`, the real split is `{s} {i}:{s}:{i}` with ints `[14, 11, 5432, 3]`. The old
   page showed the time components as words, and still claimed "real output from pyrazobill 1.1.2".
5. **`docs/STYLE.md` §5.2 specifies the old palette** (`#0284C7`, `#059669`, `#0F172A`). The new brand is black
   and white with the two data hues above. The diagrams in the docs are Mermaid and pick up the site palette
   automatically, but the style guide should be updated to match.
6. **The old page's glossary drift.** It used "ints", "strs", "rebuild" and "parse"; the docs glossary
   says numbers, words, join and split. The site follows the glossary in prose and keeps `ints` and `strs`
   only where they are actual column names.
