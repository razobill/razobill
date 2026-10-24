# AGENTS.md

Instructions for automated coding assistants and LLM agents operating on Razobill.

## Core purpose and canonical one-liner

Razobill splits log lines into templates, integers, and strings, and reconstructs them byte for byte.

## Three non-negotiable invariants

1. **Exact lossless invertibility**: `join(split(x)) == x` must hold for all valid inputs.
2. **Zero heap allocation on hot path**: Per-message loops must use bump allocators or pre-allocated scratch buffers. Never call `malloc`, `new`, or resize standard containers per event.
3. **Public ABI stability**: Preserve C ABI (`RazobillC.h`) and DuckDB function signatures. Follow `docs/reference/deprecation-policy.md`.

## Repository layout

- `include/librazobill/`: C++20 engine headers and `RazobillC.h` universal C ABI.
- `src/`: C++20 engine implementation (`Slots.cpp`, `RazobillC.cpp`, `BackendAdbc.cpp`).
- `lib/python/`: Python package (`pyrazobill`) wrapping the C ABI with GIL release.
- `ext/duckdb/`: DuckDB loadable C extension (`DuckdbExtension.cpp`).
- `ext/otel/`: OpenTelemetry Collector component (`razobillprocessor`).
- `ext/vector/`: Vector VRL transform crate (`razobill-vector`).
- `tests/`: Native C++ unit tests (`UnitCore.cpp`) and integration tests (`IntegrationDuckdb.cpp`).
- `bench/` & `tools/`: Performance microbenchmarks and documentation linters.
- `docs/`: Diataxis documentation suite (`tutorials/`, `how-to/`, `reference/`, `explanation/`).

## Verified build commands

```bash
# C++ core and test executables
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

# Python package (in active virtual environment)
pip install -e .
```

## Verified test commands

```bash
# Native C++ unit tests (68 test cases, 1,982,750 assertions, ~0.5s)
./build/UnitTests          # Windows: .\build\UnitTests.exe

# DuckDB extension integration tests (15 test cases, 134,917 assertions, ~0.5s)
./build/IntegrationDuckdb  # Windows: .\build\IntegrationDuckdb.exe

# Python unit test suite (26 tests, ~0.9s)
python -m unittest discover -s lib/python/tests

# Python property-based fuzz tests (5 tests, ~0.9s)
python -m pytest lib/python/tests/TestProperties.py
```

## Documentation and linting rules

Before committing documentation or modifying `*.md` files:

```bash
# 1. Markdownlint static analysis (must report 0 issues)
npx markdownlint-cli2 "**/*.md"

# 2. Style and banned-words linter
python tools/LintDocs.py README.md CONTRIBUTING.md docs/

# 3. Internal relative documentation link verifier
python tools/VerifyMarkdownLinks.py
```

## Operational rules for agents

1. **Untouchable paths**: Never modify `LICENSE`, `NOTICE`, `thirdparty/`, `extension-ci-tools/`, `logs/`, or raw files in `evidence/`.
2. **Relative links**: Inside markdown files, all cross-document links must be relative (e.g. `../reference/c-abi.md`).
3. **Markdownlint rules**: Ensure a blank line precedes and follows every code block (MD031) and list (MD032). Ensure spaces around pipes in tables.
4. **Honesty and receipts**: Never invent versions, metrics, benchmarks, or APIs. Quote verified numbers only.
