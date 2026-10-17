# Changelog

All notable changes to **`librazobill`** and **`pyrazobill`** will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html).

---

## [1.1.3] - 2026-10-18

### Changed

- **DuckDB Extension API Upgrade**:
  - Upgraded DuckDB extension C API headers and metadata stamping to DuckDB v1.5.6.
  - Aligned build configurations (`Makefile` with `TARGET_DUCKDB_VERSION=v1.5.6` and `CMakeLists.txt` with `-dv v1.5.6`).
  - Bumped Python test dependency baseline to `duckdb>=1.5.6`.
  - Updated community distribution manifest `ext/duckdb/description.yml` to `1.1.3` (`v1.1.3`).

---

## [1.1.2] - 2026-10-16

### Fixed

- **Infinite loop on quoted tokens with backslashes**:
  - Fixed infinite loop in `RzSplit` (affecting native scanner and pure-Python `_fallback.py`) when parsing inputs containing quoted tokens ending with a backslash before the quote delimiter (such as `opened "C:\Users\bob\" ok` or `"\"`).
  - Added regression test suites across C++ unit tests (`tests/UnitCore.cpp`) and Python unit tests (`lib/python/tests/TestPyRazobill.py`).
- **Citation notice**:
  - Replaced overreaching legal interpretation of Apache 2.0 Section 4(d) in `NOTICE`, `lib/python/NOTICE`, and documentation with standard academic citation language ("please cite").
- **Absolute URLs in package documentation**:
  - Converted relative Markdown links and logo paths in `lib/python/README.md` and `ext/duckdb/README.md` to absolute URLs to prevent 404s on PyPI and DuckDB community documentation indexers.

### Added

- **Standard Citation File Format (`CITATION.cff`)**:
  - Added root CFF 1.2.0 metadata separating the foundational theory paper citation (`atta2026latitude`, DOI 10.5281/zenodo.23287286) from the software implementation citation (`razobill2026`).
- **Python `RzHash`**:
  - Added `RzHash` (and `rz.hash` alias) to the Python API (`pyrazobill`), exposing bit-exact xxHash64 log template hashing matching DuckDB SQL `RzHash` and chDB/ClickHouse `xxHash64`.
- **Architectural comparison with CLP**:
  - Added dedicated comparison in `README.md` clarifying structural differences between Razobill (in-database relational decomposition into standard SQL columns) and YScope CLP (custom compressed archive format and IR).
- **Technical postmortem**:
  - Added in-depth architectural explanation and reproducer dissection for the quote-backslash edge-case hang.

### Changed

- **Calibrated documentation and quickstart**:
  - Synchronized `README.md` quickstart to canonical 1.1.x API (`RzSplit`, `RzJoin`, `RzTemplate`, `RzHash`, `RzSplitMany`). Removed references to retired or non-exported symbols.
  - Unified benchmark numbers across the repository and site to a single calibrated LogHub run (5 datasets, 2.5×–15.1× / average 5.6× vs Drain3) with links to raw evidence scripts (`tools/BenchDrain.py`).
  - Clarified wheel binary platform availability (Linux x86-64, Windows x86-64, macOS; Linux ARM via source distribution).
  - Clarified DuckDB extension installation options: community repository (`INSTALL razobill FROM community;`) and local binary fallback (`LOAD 'build/razobill.duckdb_extension';`), noting ongoing compatibility verification for DuckDB 2.0.0.

---

## [1.1.1] - 2026-10-14

### Changed

- **Unified Public Surface Across Ecosystem**:
  - Streamlined DuckDB SQL surface to exactly 4 canonical functions with zero aliases: `RzSplit`, `RzJoin`, `RzTemplate`, and `RzHash`.
  - Streamlined Python package (`pyrazobill`) to canonical `RzSplit`, `RzJoin`, `RzTemplate`, `RzSplitMany`, `RzRecord`, and `RzError`.
  - Streamlined Universal C ABI (`RazobillC.h`) to 6 functions (`RzSplit`, `RzJoin`, `RzTemplate`, `RzFree`, `RzStatusMessage`, `RzVersion`) and 2 types (`RzRecord`, `RzStatus`).
  - Standardized template placeholders on `{i}` (integers) and `{s}` (strings), with `{{` and `}}` for literal braces.
- **Modern Diataxis Documentation System**:
  - Overhauled documentation into four Diataxis quadrants (`tutorials/`, `how-to/`, `reference/`, `explanation/`).
  - Added centralized navigation index (`docs/README.md`), controlled glossary (`docs/glossary.md`), and error-string troubleshooting guide (`docs/troubleshooting.md`).
  - Added automated documentation style linter (`tools/LintDocs.py`).

---

## [1.0.1] - 2026-10-11

### Added

- **Resilient Pure-Python Fallback (`pyrazobill._fallback`)**:
  - Full API parity for `decompose`, `reconstruct`, `formKey`, `templateOf`, `hash64`, `entropy`, and `computeLedger` in pure Python.
  - Guarantees seamless installation and operation across all environments, even when C++ development headers or MSVC runtimes are absent.
- **Windows SDK / UCRT Auto-Discovery**:
  - Automatically identifies Windows 10/11 SDK header directories (`ucrt`, `shared`, `um`) in `setup.py`, resolving missing `<io.h>` compilation errors on Windows with MSVC.
- **Pre-Compiled Multi-Platform Binary Wheels**:
  - Automated building of native wheels across Python 3.9, 3.10, 3.11, 3.12, and 3.13 on Windows (`win_amd64`), Linux (`manylinux_2_28_x86_64`), and macOS (`arm64` and `x86_64`).

### Fixed

- Resolved `fatal error C1083: Cannot open include file: 'io.h'` during Windows `pip install`.
- Modernized `pyproject.toml` license configuration to direct SPDX string (`license = "Apache-2.0"`), resolving setuptools deprecation notices.

---

## [1.0.0] - 2026-10-07

### Added

- **`librazobill` Core C++20 Modular Monolith**:
  - Zero-underscore, publication-grade C++20 engine under [`include/librazobill/`](include/librazobill/).
  - High-performance, zero-allocation token scanning with `ScanMode::Exact` and `ScanMode::Semantic`.
  - Contiguous Structure-of-Arrays (SoA) layout (`SlotRecord`) for cache locality and vectorization.
  - Generational compacting `StringPool` with mark-and-sweep `gc()`, monotonic compaction, and heterogeneous `std::string_view` lookup.
  - Monotonic 64-byte cache-line aligned `MemoryArena` bump allocator with block freelist recycling chunks across resets.
  - Cache-aware work-stealing `ThreadPool` supporting embarrassingly parallel multi-core log auditing.
  - Latitude Channel module (`Channel.hpp`) for channel capacity calculation (Theorem 4c) and slot-based payload embedding/decoding.
- **AVX-512, AVX2 & ARM NEON SIMD Delimiter Scanners**:
  - 64-byte AVX-512BW, 32-byte AVX2, and 16-byte ARM NEON vectorization in `SimdScanner.hpp` utilizing branchless hardware trailing-zero count (`_tzcnt_u64` / `__builtin_ctzll`).
  - Dynamic CPUID multiversion dispatcher (`findWhitespaceDynamic`).
- **Universal C ABI (`RazobillC.h`)**:
  - Zero-overhead `extern "C"` functions (`RzParse`, `RzDecompose`, `RzReconstruct`, `RzReconstructSlots`, `RzTemplateOf`, `RzHash64`, `RzFreeRecord`).
  - Statistical analysis exports (`RzComputeLedger`, `RzMillerMadow`, `RzComputeCertifiedInterval`).
  - Runtime version query functions (`RzVersion`, `RzVersionMajor`, `RzVersionMinor`, `RzVersionPatch`, `RzAbiVersion`).
  - Binary compatibility assertions guaranteeing struct alignment and cross-language FFI safety.
- **Python Client Library (`pyrazobill`)**:
  - Minimalist zero-underscore Python API under `lib/python/pyrazobill`.
  - High-speed direct decomposition tuple API (`decompose`, `decomposeBatch`, `reconstruct`).
  - Mathematical information ledger calculation (`computeLedger`), consequence decomposition, and certified confidence intervals.
  - Latitude Channel capacity profiling, payload embedding, and extraction (`channelAnalyzeCapacity`, `channelEmbed`, `channelDecode`).
  - Memory-safe `PySlotRecord` with heap-pinned string buffer lifetime management.
  - High-throughput multi-threaded parsing releasing GIL.
  - Native version inspection: `pyrazobill.__version__`, `__version_info__`, `version()`, `abiVersion()`.
- **Database Engine Integrations**:
  - Native loadable DuckDB extension (`RazobillDuckdb`) exporting single-pass composite struct function `rbDecompose` returning `STRUCT(form VARCHAR, ints BIGINT[], strs VARCHAR[])` with 66% lower parsing overhead.
  - Individual scalar UDFs: `rbForm`, `rbInts`, `rbStrs`, `rbReconstruct`, `rbDecomposeSemantic`, `rbEntropy`, `rbTemplate`, `rbHash`.
  - Cross-engine identity matching ClickHouse `xxHash64` and ADBC port support.
- **Information Theory Grounding (*Latitude and Doubt*, M. M. Atta 2026)**:
  - Exact information ledger assertions: $E = I + \Lambda$ and $C = I + \Delta$ verified to $10^{-9}$.
  - Adequacy and leakage auditing ($L_Y(W) = I(X;Y) - I(W;Y)$) with Jensen-Shannon divergence gating ($\varepsilon = 0.05$ bits/event).
  - Stratified Miller-Madow finite-sample bias correction and Proposition 10 certified distribution-free confidence intervals.
  - Vocabulary Law (Corollary 2) bounds and variation of information metric distance (Proposition 3).
- **Empirical Validation**:
  - Multi-core stress test across all 4,861 real-world log files (69.6M events, 6.82 GB) at 1.11M msgs/sec with **0 mismatches (100% byte-for-byte exact)**.
  - Head-to-head empirical benchmark against Drain3 demonstrating up to 283.7× throughput speedup, zero entropy doubt ($\Delta = 0.0000$), and 100% lossless reconstruction.
  - DuckDB scale test parsing 1,000,000 rows at 3.7M msg/s with composite struct `rbDecompose` and rebuilding at 10.37M msg/s with 0 errors.
  - 34/34 core unit tests passed (976,444 assertions).
  - 12/12 DuckDB extension integration tests passed (134,890 assertions).
  - 14/14 Python test cases passed.

### Fixed

- Eliminated all dangling string views in Python batch processing via buffer pinning.
- Fixed DuckDB Windows extension loading compatibility requiring `allow_unsigned_extensions`.
- Eliminated dictionary memory bloat via generational mark-and-sweep compaction.
- Mitigated token over-splitting on non-numeric identifiers (usernames, hostnames) via semantic scanning mode.

### Changed

- Promoted project status from experimental prototype (`0.1.0`) to production release (`1.0.0`) under strict SemVer 2.0.0 governance.
- Renamed all public C++, C, Python, and CMake interfaces to adhere to an absolute zero-underscore naming policy.
- Restructured repository layout placing multi-language libraries under `lib/` (`lib/python/`).

---

[1.0.0]: https://github.com/razobill/razobill/releases/tag/v1.0.0
