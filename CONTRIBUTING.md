# Contributing to Razobill

Thank you for your interest in contributing to Razobill. Razobill splits log lines into templates, integers, and strings, and reconstructs them byte for byte.

This guide provides a verified walk-through to take you from a fresh git clone to passing tests in minutes.

---

## Code of conduct

This project adheres to the [Contributor Covenant](CODE_OF_CONDUCT.md). By participating, you agree to honor its terms. Report issues or violations to the project maintainers.

---

## Repository layout

| Directory | Purpose | Key files |
| --- | --- | --- |
| `include/librazobill/` | Public C++20 headers and C ABI declarations | `Razobill.hpp`, `Slots.hpp`, `RazobillC.h`, `SimdScanner.hpp` |
| `src/` | Core C++20 engine and C ABI implementation | `Slots.cpp`, `RazobillC.cpp`, `BackendAdbc.cpp`, `Channel.cpp` |
| `lib/python/` | Python C-extension wrapper (`pyrazobill`) and test suite | `PyRazobill.cpp`, `tests/TestPyRazobill.py`, `tests/TestProperties.py` |
| `ext/duckdb/` | DuckDB C extension adapter | `DuckdbExtension.cpp` |
| `ext/otel/` | OpenTelemetry Collector processor component | `RazobillProcessor.go`, `Factory.go` |
| `ext/vector/` | Vector VRL transform crate | `RazobillVrl.rs` |
| `tests/` | Native C++ unit tests and database integration tests | `UnitCore.cpp`, `UnitArena.cpp`, `IntegrationDuckdb.cpp` |
| `bench/` & `tools/` | Performance microbenchmarks and validation scripts | `BenchCore.cpp`, `BenchDrain.py`, `BenchCLP.py` |
| `docs/` | Diataxis documentation suite | `tutorials/`, `how-to/`, `reference/`, `explanation/` |

---

## Engineering invariants

Every pull request must preserve three architectural invariants:

1. **Exact lossless invertibility**:
   The engine must guarantee bit-exact round-trip reconstruction across all byte sequences:
   $$\text{join}(\text{split}(x)) = x$$

2. **Zero heap allocation on the hot path**:
   The parsing loop must route memory through monotonic arenas (`MemoryArena`) or thread-local scratchpads. Do not invoke `malloc`, `new`, or trigger container heap reallocations during per-message processing.

3. **Public ABI stability**:
   The C ABI (`RazobillC.h`) and DuckDB function signatures are stable contracts. Do not alter struct member order or existing enum values without following the [Deprecation policy](docs/reference/deprecation-policy.md).

---

## From clone to green test (10-step guide)

Measured total setup and verification time: approximately 3 minutes on a multi-core workstation.

### Step 1: Clone the repository

```bash
git clone https://github.com/razobill/razobill.git
cd razobill
```

### Step 2: Install build prerequisites

- **C++20 compiler**: GCC 12+, Clang 15+, or MSVC 2022 (v19.30+)
- **CMake**: Version 3.20 or newer
- **Ninja**: Fast build tool (recommended)
- **Python**: Version 3.9+ with `pip` and virtual environment support

### Step 3: Configure the CMake build tree

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

### Step 4: Build the C++ core and test executables

```bash
cmake --build build --config Release
```

### Step 5: Run the native C++ unit tests

Executes 68 test cases and verifies 1,982,750 assertions in under 1 second:

```bash
# Linux / macOS
./build/UnitTests

# Windows
.\build\UnitTests.exe
```

Expected output:

```text
[doctest] Status: SUCCESS! - 68 test cases passed! (1982750 assertions)
```

### Step 6: Run the DuckDB extension integration tests

Verifies 15 test cases and 134,917 assertions:

```bash
# Linux / macOS
./build/IntegrationDuckdb

# Windows
.\build\IntegrationDuckdb.exe
```

Expected output:

```text
[doctest] Status: SUCCESS! - 15 test cases passed! (134917 assertions)
```

### Step 7: Create a virtual environment and build pyrazobill

```bash
python -m venv .venv

# Linux / macOS
source .venv/bin/activate

# Windows (PowerShell)
.venv\Scripts\Activate.ps1

pip install -e lib/python
```

### Step 8: Run the Python unit and property test suites

```bash
# Standard unittest suite (24 tests)
python -m unittest discover -s lib/python/tests

# Hypothesis property-based fuzz tests (5 tests)
pip install pytest hypothesis
python -m pytest lib/python/tests/TestProperties.py
```

Expected output:

```text
Ran 15 tests in 0.15s
OK
5 passed in 0.90s
```

### Step 9: Verify foreign extensions (optional)

If modifying the OpenTelemetry or Vector components:

```bash
# Vector Rust crate
cd ext/vector && cargo test && cd ../..

# OpenTelemetry Go module
cd ext/otel && go test ./... && cd ../..
```

### Step 10: Run documentation and link linters

Before committing changes, verify that markdown formatting, style invariants, and internal links pass:

```bash
# Markdownlint static analysis
npx markdownlint-cli2 "**/*.md"

# Style and banned-word linter
python tools/LintDocs.py README.md CONTRIBUTING.md docs/

# Internal documentation relative link verifier
python tools/VerifyMarkdownLinks.py
```

Expected output:

```text
Summary: 0 issues in 0 files
[SUCCESS] All documentation files comply with style rules.
[SUCCESS] All relative documentation links are 100% valid and verified on disk!
```

---

## Submitting pull requests

1. Create a descriptive branch: `git checkout -b feature/my-enhancement` or `git checkout -b fix/issue-description`.
2. Keep commits focused and atomic with conventional commit prefixes: `feat:`, `fix:`, `docs:`, `perf:`, `test:`, `refactor:`.
3. Ensure all tests and linters pass before opening a pull request.
4. Fill in the checklist in [.github/PULL_REQUEST_TEMPLATE.md](.github/PULL_REQUEST_TEMPLATE.md).
