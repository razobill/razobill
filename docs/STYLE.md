# Razobill documentation style guide

Razobill documentation turns log telemetry into exact, structured data with zero byte loss.
Read this guide if you are writing or updating documentation for Razobill.
Skip this guide if you are only querying the DuckDB extension or calling the Python API.

---

## 1. Essence

Razobill is an exact, invertible telemetry engine that decomposes log strings into structured templates, numbers, and words, turning unstructured text into queryable columnar data with zero byte loss.

Documentation should feel like a precision instrument: calm, mechanically exact, and directly verifiable against the codebase.

---

## 2. Voice and tone

Razobill speaks with three core voice traits.

### Trait 1: Definite and factual

State concrete data structures, mathematical invariants, and runtime behaviors directly.
Avoid hand-waving claims, defensive comparisons, and hype words.

- **Before**: "Razobill is a groundbreaking, revolutionary engine that leaves probabilistic tools like Drain in the dust while miraculously losing zero data."
- **After**: "Razobill splits a log line into a template string, an array of 64-bit integers, and an array of variable words, guaranteeing bit-exact reconstruction."
- **What it never sounds like**: "In today's fast-paced cloud landscape, log data is growing exponentially, but our world-class engine makes parsing effortless."

### Trait 2: Mechanically exact

Every command, function signature, and code block must execute cleanly and produce the exact output shown.
Include hardware context and corpus parameters when reporting benchmarks.

- **Before**: "Our algorithm runs in sub-microsecond time with basically zero overhead."
- **After**: "On Windows 11 with 12 logical cores, `RzSplit` averages 331,000 lines/s on SSH authentication logs without heap allocations on the hot path."
- **What it never sounds like**: "It's blisteringly fast and uses minimal resources."

### Trait 3: Unadorned and respectful of the reader's time

Use short sentences.
Dedicate one paragraph to one idea.
Show working code before explaining internal theory.

- **Before**: "Before we dive in, let us ponder the tragedy of stringification and why for twenty years observability treated logs as an unsupervised learning problem."
- **After**: "Call `rz.RzSplit(line)` to parse a message into a template and parameter arrays. Call `rz.RzJoin(*rec)` to restore the original string."
- **What it never sounds like**: "Let's dive right in! But first, a quick story about why logs matter."

---

## 3. Glossary

Use only the canonical term for each concept.
Banned synonyms are enforced by automated linting.

| Canonical Term | Banned Synonyms | One-Line Definition |
| :--- | :--- | :--- |
| **template** | form, formKey, skeleton, pattern, cluster | The constant text skeleton containing typed `{i}` and `{s}` placeholders. |
| **integers (ints)** | integer slots, digits, numerical values | 64-bit signed integers parsed from numeric tokens in the input. |
| **strings (strs)** | string slots, dynamic tokens, residuals | Variable substrings such as IP addresses, paths, UUIDs, or usernames. |
| **split** | decompose, extract, tokenize | The operation separating a raw log line into a template, numbers, and words. |
| **join** | rebuild, synthesize, format | The operation restoring the exact original log line from a template and values. |
| **semantic mode** | grouping mode, smart mode | Default mode grouping variable tokens (IPs, paths, hashes) into `{s}` slots. |
| **exact mode** | raw mode, strict mode, delimiter mode | Tokenizer mode that isolates only pure numeric tokens and literal words. |
| **lossless invertibility** | round-trip fidelity, reversibility | The guarantee that `join(split(line)) == line` for all valid inputs. |

---

## 4. Components

Use only these six named recurring blocks.
Each component serves one documented purpose and uses GitHub alert markdown syntax.

### Component 1: TL;DR

Provides an executive summary in two sentences or fewer at the start of complex guides.

```markdown
> [!NOTE]
> **TL;DR**: Razobill splits log lines into templates and parameter arrays, reducing storage footprint while guaranteeing byte-identical reconstruction.
```

### Component 2: Try it

Presents a verified, copy-pasteable snippet that produces immediate results.

```markdown
> [!TIP]
> **Try it**: Run `python -c "import pyrazobill as rz; print(rz.RzSplit('port 22 open'))"` in your terminal.
```

### Component 3: Gotcha

Flags non-obvious failure modes, lifetime gotchas, or boundary conditions.

```markdown
> [!WARNING]
> **Gotcha**: In the C ABI, `outRecord.strValues` contains pointers into the caller's message buffer; do not free the buffer before freeing the record.
```

### Component 4: Under the hood

Explains engine internals, SIMD dispatching, or memory layout for advanced readers.

```markdown
> [!IMPORTANT]
> **Under the hood**: Delimiter scanning uses 32-byte AVX2 vector registers with branchless hardware trailing-zero counts.
```

### Component 5: Migration note

Explains migrations from legacy v1.x symbols or third-party tools.

```markdown
> [!CAUTION]
> **Migration note**: Legacy `rbDecompose` and `\x12`/`\x13` delimiters were replaced in v1.1.1 by `RzSplit` and `{i}`/`{s}` placeholders.
```

### Component 6: Real-world impact

Quantifies disk, bandwidth, or latency impact with verifiable production metrics.

```markdown
> [!NOTE]
> **Real-world impact**: Replaces 10 TB of raw text logs with 1.2 TB of Parquet ZSTD columnar stripes.
```

---

## 5. Visuals and diagrams

1. **Wordmark / Logo**: Embed the logo via `<picture>` with light and dark mode variants using `assets/Logo.jpeg` and `assets/Logo-light.jpeg`.

   ```html
   <picture>
     <source media="(prefers-color-scheme: dark)" srcset="assets/Logo.jpeg">
     <img src="assets/Logo-light.jpeg" alt="Razobill Logo" width="320">
   </picture>
   ```

2. **Palette**: Constrain diagram and UI styling to four primary colors:
   - Primary Accent: `#0284C7` (Teal Blue)
   - Invertibility / Success: `#059669` (Emerald Green)
   - Dark Base: `#0F172A` (Slate Navy)
   - Secondary / Border: `#64748B` (Muted Slate)
3. **Diagrams**: All architecture and dataflow diagrams must use Mermaid syntax. No raster screenshots or ASCII art.
4. **Badges**: Maximum four live badges at the top of the README. Never hard-code pass counts or test totals.

---

## 6. Humor and tone policy

1. Personality lives in the clarity of the voice, not decorative humor.
2. At most one light, wry observation per page.
3. Zero humor in reference specifications, security advisories, error messages, or migration guides.
4. Zero decorative emojis. Use functional GitHub alert icons only.

---

## 7. Writing mechanics

- **Sentence-case headings**: Use `## Quick installation`, not `## Quick Installation`.
- **One sentence per line**: Put every sentence on a new line for clean git diffs.
- **Average sentence length**: Maximum 20 words per sentence.
- **Stable anchors**: Do not change heading anchors without leaving backward-compatible stubs.
- **Tagged code fences**: Every code block must declare its language tag (e.g. ````python````, ````sql````, ````bash````, ````cpp````).
- **Descriptive links**: Write `[Python developer guide](how-to/python.md)`, never `[click here](...)`.

---

## 8. Diataxis page templates

Every page belongs to one of four Diataxis quadrants.

### Template A: tutorial (learning-oriented)

```markdown
# [Action-oriented title: e.g. Getting started in 5 minutes]

One-sentence summary of what the reader will build or achieve.
Read this if you are using Razobill for the first time.
Skip this if you already have the package installed and configured.

## What you will build
Brief summary of the outcome.

## Step 1: Install the library
Concrete command with expected terminal output.

## Step 2: Split your first log line
Runnable snippet with real output.

## Step 3: Reconstruct the original bytes
Proof of exact invertibility.

## Next steps
Links to relevant how-to guides and API reference.
```

### Template B: how-to guide (task-oriented)

```markdown
# [Goal-oriented title: e.g. Compacting logs in DuckDB]

One-sentence summary of the specific task solved by this guide.
Read this if you need to solve this specific integration task.
Skip this if you are looking for general concepts or API signatures.

## Problem
Context and technical challenge.

## Solution
Step-by-step instructions with verified code blocks.

## Verification
Query or test confirming success.
```

### Template C: reference (information-oriented)

```markdown
# [Symbol or module name: e.g. DuckDB SQL functions]

One-sentence technical summary of the API surface.
Read this if you need exact signatures, types, or return codes.
Skip this if you need an introductory guide.

## Overview
Summary table of all functions in scope.

## Function reference
### `FunctionName(param: Type) -> ReturnType`
- **Parameters**: Exact types and constraints.
- **Returns**: Structure and nullability.
- **Errors**: Conditions triggering failures.
- **Example**: Verifiable snippet.
```

### Template D: explanation (understanding-oriented)

```markdown
# [Concept title: e.g. Lossless invertibility]

One-sentence explanation of the architectural principle.
Read this if you want to understand the design decisions and trade-offs.
Skip this if you only need the API syntax.

## Background
Why this design exists and what problem it solves.

## Mechanics
How the engine achieves the invariant.

## Trade-offs
Alternative approaches and measured trade-offs.
```
