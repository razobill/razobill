# Changelog

All notable changes to **`librazobill`** and **`pyrazobill`** will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html).

---

## [1.1.6] - 2026-10-24

### Fixed

- **Round-2 Hardening Audit across C++ Core, C ABI, and Language Bindings**:
  - **W1 C++ Core Correctness & Memory Safety**: Overflow-checked size accumulation in `reconstructLegacy` and `reconstructNew`; manual-record sanity bounds (`templateLength <= 16MB`, `intCount/strCount <= templateLength/3 + 1`, `strLengths[i] <= 16MB`) in `RzJoin`; `InternalRecord` heap allocation reuse in `RzSplit`; DuckDB-style 1 MB scratch retention cap in `CAbiScratch::trim()`; pruned dead `out.skeleton` and `out.types` string building from `core::parse()` (+23% single-core throughput improvement); finite and range validation for `sharedKey` and `dither` in `Channel.cpp` with UB float-to-u64 cast prevention; fixed relational comparison with `nullptr` in `Scanner.hpp`; locale-independent `num()` via `std::to_chars` and ASCII-only `requireName`; `templateOf` raw 0x12/0x13 control byte passthrough reconstruct parity; `FormCatalog::registerForm` strong exception safety with rollback cleanup; capped `MemoryArena::allocate` alignment at 4096 bytes; serialized `AdbcBackend` cleanup under `connMutex_`.
  - **W2 C ABI Contract Honesty**: Pinned `RZ_ABI_VERSION == 1` with compile-time `static_assert`; documented zero-initialization requirement (`rec = {0}`), shallow-copy/double-free hazards, `intValues` 8-byte alignment, `*outLength`-on-error semantics, and manual record bounds in `RazobillC.h` and `docs/reference/c-abi.md`.
  - **W3 Language Bindings**:
    - **Python**: Retained `os.add_dll_directory` cookies in module-level list to prevent GC unregistration on Windows; wrapped `PyUnicode_AsEncodedString` in `py::reinterpret_steal<py::bytes>` for exception-safe cleanup; single-sourced version from `VERSION`; removed hardcoded MSVC `/arch:AVX2`; synchronized MinGW static-libgcc and winpthread link flags with CMake; added native vs fallback hash parity tests.
    - **DuckDB**: Rejected `UBIGINT >= 2^63` integers with descriptive error instead of silent narrowing; refreshed extension description and bumped version to `1.1.6` in `description.yml`; added tests for varargs `RzJoin` overloads.
    - **OTel**: Added fast-fail ABI version gate `C.RzAbiVersion() == 1` in `Start()`; aligned empty-body behavior to emit empty template; added per-resource context cancellation checks; ran `go mod tidy` for direct `cespare/xxhash/v2` dependency.
    - **Vector**: Set `crate-type = ["rlib"]`; deleted dead `RawRecord`; removed phantom `.hash` key from `VectorConfig.yaml`; removed unverified throughput claim; added tests for empty input and static lines.
  - **W4 Build & CI**: Added `-fvisibility=hidden` on `razobill_cabi` shared library; added opt-in `RZ_WERROR`; wired `markdownlint-cli2`, `VerifyMarkdownLinks.py`, `LintDocs.py`, and `ruff` as enforcing CI jobs; replaced fake SBOM stub with real `sha256sum * > SHA256SUMS`; pinned release actions to commit SHAs; added `.github/CODEOWNERS`; dynamically sourced `EXTENSION_VERSION` in `Makefile`; guarded DuckDB metadata padding against silent truncation.
  - **W5 & W6 Test Hardening & Verification**: Expanded property fuzzing alphabet to include whitespace escape characters (`\n`, `\r`, `\f`); verified 1M iterations in `FuzzDecomposeReconstruct`; verified 62 native unit tests and 15 DuckDB integration tests.

## [1.1.5] - 2026-10-24

### Fixed

- **F1 Round-trip Format Escaping & Robustness**:
  - Implemented `0x11` delimiter escaping in `src/Slots.cpp` so that literal round-trip delimiter bytes (`\x12`, `\x13`, `\x11`) in raw input messages escape losslessly without corrupting template reconstruction.
  - Hardened pure-Python `_fallback.py` to mirror native C++ `0x11` escape logic.
  - Preserved backward compatibility for unescaped legacy forms (`isLegacyFormKey`).
- **F2 C ABI Zero-Heap Allocation & Contract Fixes**:
  - Added zero-allocation pre-flight buffer sizing probes to `RzJoin` and `RzTemplate` when `outBuffer == nullptr`, returning `RZ_OK` with required capacity written to `*outLength`.
  - Added `RzStatusMessage` null-safety for unknown status codes, returning `"Unknown error"`.
  - Removed per-call `std::string` heap allocations in `RzTemplate` hot path using direct string-view buffer writes.
  - Eliminated undefined behavior in `Channel.cpp` `failureProbabilityBound` when handling negative inputs or NaN values.
- **F3 Core C++ Robustness & Hardening Sweep**:
  - Replaced C-style casts with `static_cast` throughout `include/librazobill/Hash.hpp`.
  - Guarded `ThreadPool` empty task queues and thread joins with exception safety.
  - Replaced manual loops in `FormCatalog` with `std::find_if`.
- **F4 & F5 SQL Control Plane: TransactionGuard & Dialect Parity**:
  - Activated DuckDB transaction management in `TransactionGuard`, ensuring transactional rollback and atomic commit.
  - Resolved MVCC table creation conflict in DuckDB compact mode by verifying table existence prior to table creation.
  - Normalized `DuckDialect::templateExpr` and `ClickHouseDialect::templateExpr` to produce identical `{i}` and `{s}` template placeholders.
  - Updated `ClickHouseDialect::formExpr` to modern `arrayStringConcat(arrayZipWith(...))`.
- **F6 BackendAdbc Driver Hardening**:
  - Added RAII `StreamGuard` releasing Arrow array streams upon exceptions or early return.
  - Added bounds checking for Arrow array offsets and null bitmaps across `isValid()`, `at()`, `decode()`, and `run()`.
  - Enforced compile-time little-endian assertion `std::endian::native == std::endian::little`.
  - Unified table accessor out-of-bounds error handling to consistently raise `BackendError`.
- **F7 Python Bindings Fidelity & GIL Release**:
  - Wrapped `splitOne`, `templateOne`, and `joinOne` with `py::gil_scoped_release` to enable true multi-threaded CPU concurrency in Python.
  - Scoped DLL loading to package directories on Windows, eliminating insecure system PATH traversal.
  - Cleaned `MANIFEST.in` stale references.
- **F8 Rust Vector VRL Crate Losslessness & C ABI Linking**:
  - Linked `razobill_cabi` shared library instead of DuckDB extension in `ext/vector/build.rs`.
  - Switched `RzStatus` to integer constants to prevent enum undefined behavior.
  - Supported raw byte slices losslessly in `parse_razobill(impl AsRef<[u8]>)`.
  - Bumped `razobill-vector` to `2.0.0`.
- **F9 OpenTelemetry Go Processor Robustness**:
  - Linked `razobill_cabi` and scoped `-lstdc++` to Linux in `ext/otel/RazobillProcessor.go`.
  - Supported both string and byte bodies (`pcommon.ValueTypeBytes`).
  - Added panic-safe cleanup with `defer C.free` and `defer C.RzFree`.
  - Enforced valid `semantic` mode in `Config.Validate()`.
- **F10 & F11 Build System & CI Pipelines**:
  - Single-sourced DuckDB C API ABI target version (`v1.5.6`) across CMake and metadata stamping tools.
  - Updated `librazobill.pc.in` to link `-lrazobill_cabi`.
  - Documented `LIBRAZOBILL_STATIC`/`LIBRAZOBILL_EXPORTS` macro visibility matrix.
  - Added Go and Rust test jobs to GitHub Actions CI and enforced native C++ engine with `RZB_REQUIRE_NATIVE=1`.
  - Added `gomod` and `cargo` to Dependabot configuration.
- **F12 & F13 Documentation Honesty & Benchmark Integrity**:
  - Rewrote `docs/how-to/cpp-developer.md` against real C++20 and C99 ABI signatures.
  - Corrected `Status::TooLarge` and `kMaxMessageBytes` references.
  - Removed fabricated 69.4M-lines/s fallback in `bench/s3/BenchDrain3VsRazobillS3.py`.
  - Updated `tools/BenchScale.py` to modern `split_many` / `join` worker-pool batching.
  - Aligned hardware memory receipts to 7.78 GB RAM and synchronized DuckDB storage receipts to `evidence/ResultsE7Default.txt`.

---

## [1.1.4] - 2026-10-22

### Fixed

- **DuckDB Community Extension Target Naming**:
  - Disambiguated CMake target outputs between `RazobillDuckdb` (DuckDB extension) and `razobill_cabi` (universal C FFI shared library).
  - Renamed `razobill_cabi` target output to `razobill_cabi` and guarded compilation with `if(NOT RAZOBILL_BUILD_EXT_ONLY)`.
  - Set `RazobillDuckdb` target output to `razobill` exporting `razobill_init_c_api` and `librazobill_init_c_api`, resolving the missing symbol loader error (`undefined symbol: razobill_init_c_api`) on community binaries.
- **DuckDB v2.0.0 Compatibility**:
  - Upgraded DuckDB extension C API headers (`duckdb.h`, `duckdb_extension.h`) in `thirdparty/duckdbcapi/` to official DuckDB v2.0.0.
  - Resolved table scan row group pruning in `RazobillEngine` by maintaining `TransactionGuard` dormant on mixed DDL/DML storage flows.
  - Configured DuckDB 2.0 single-arrow lambda syntax (`SET lambda_syntax = 'ENABLE_SINGLE_ARROW'`) in integration test suite.

### Changed

- **Version Bump**:
  - Bumped canonical project version to `1.1.4` across `VERSION`, `Version.hpp`, `RazobillC.h`, `setup.py`, `pyproject.toml`, and `pyrazobill/__init__.py`.
  - Updated community distribution manifest `extensions/razobill/description.yml` to v1.1.4 pointing to commit `c9320e8` for DuckDB v2.0.0 (`ref`) and v1.5.6 (`variegata`).

---

## [1.1.3] - 2026-10-18

### Added

- **Clang Lifetime Safety Analysis**:
  - Introduced compile-time lifetime and borrow annotations in `include/librazobill/Lifetime.hpp` (`[[clang::lifetimebound]]`, `[[clang::coro_lifetimebound]]`, `[[clang::coro_return_type]]`).
  - Applied lifetime safety annotations across `StringPool`, `Scanner`, `MemoryArena`, `RazobillEngine`, `DuckdbExtension`, `Support.hpp`, `Razobill`, and `BackendAdbc`.
  - Added dedicated lifetime unit test suite (`tests/UnitLifetime.cpp`) with 8 test cases verifying dangling string_view and pointer prevention.
  - Implemented automated Clang 18+ `-Wlifetime-safety` workflow (`.github/workflows/lifetime-safety.yml`) publishing Markdown job summaries.
  - Added comprehensive architectural documentation in `docs/explanation/lifetime-safety.md`.
- **Contributor and Agent Operations Governance**:
  - Added `AGENTS.md` defining repository invariants, verified build and test commands, documentation lint rules, and operational guidelines.
- **Supply Chain and Citation Standards**:
  - Added standard Citation File Format metadata (`CITATION.cff`) for both foundational research and software distribution.
  - Automated CycloneDX SPDX SBOM generation during release packaging.

### Changed

- **DuckDB Extension API Upgrade to v1.5.6**:
  - Upgraded DuckDB extension C API headers (`duckdb.h`, `duckdb_extension.h`) from v1.2.0 to official DuckDB v1.5.6.
  - Updated metadata footer stamping in `CMakeLists.txt` (`-dv v1.5.6`, `-ev v1.1.3`, `C_STRUCT` ABI).
  - Configured `Makefile` with `TARGET_DUCKDB_VERSION=v1.5.6` and `EXTENSION_VERSION=v1.1.3`.
  - Updated community distribution manifest `ext/duckdb/description.yml` to `1.1.3` (`v1.1.3`).
  - Bumped Python dependency baseline in `lib/python/pyproject.toml` and GitHub Actions workflows to `duckdb>=1.5.6`.
  - Validated DuckDB 1.5.6 compatibility: passed all 14 integration test cases (134,910 assertions) and 1,000,000 row scale test with 0 mismatches.
- **Diataxis Documentation Suite Overhaul**:
  - Complete restructuring and rewrite across all four Diataxis quadrants (`tutorials/`, `how-to/`, `reference/`, `explanation/`).
  - Standardized all codebase comments and file banners across C++, Python, and workflows to English wrapped to 80 columns.
  - Verified 100% relative documentation links (240 links) with zero dead links.
- **Version Bump**:
  - Bumped canonical project version to `1.1.3` across `VERSION`, `Version.hpp`, `RazobillC.h`, `setup.py`, and `pyrazobill/__init__.py`.

### Fixed

- **CodeQL Alert Remediations**:
  - Resolved Alert #124 (`cpp/long-switch`): refactored `case Kind::IntList:` and `case Kind::StrList:` in `ext/duckdb/DuckdbExtension.cpp` into dedicated inline helper functions (`appendIntListValues`, `appendStrListValues`), reducing case length from 42 lines to 4 lines.
  - Resolved Alert #97 (`cpp/stack-address-escape` in `thirdparty/doctest.h`): dismissed as non-production test framework code while strictly preserving `thirdparty/` immutability.
- **DuckDB Multi-Width List Type Confusion**:
  - Fixed integer list vector decoding in `DuckdbExtension.cpp` to respect physical type strides for `BIGINT`, `INTEGER`, `SMALLINT`, `TINYINT`, `UBIGINT`, `UINTEGER`, `USMALLINT`, and `UTINYINT`, eliminating potential out-of-bounds heap reading.
- **Python Concurrency & GIL Ephemeral Bytes Lifetime**:
  - Fixed dangling reference vulnerability in `lib/python/PyRazobill.cpp` `RzSplitMany` by pinning `py::object pyBytes` in `InputItem` during multi-threaded worker execution with released GIL.
  - Added POSIX fork safety handler in `PyRazobill.cpp` (`pthread_atfork`) to prevent multi-threaded deadlock upon process fork.
- **ADBC Connector Bounds & Offset Validation**:
  - Hardened Arrow C data interface bounds checking in `src/BackendAdbc.cpp` (`isValid`, decimal decoding null buffer checks, negative offset validation).
- **Compiler & Linter Warnings**:
  - Removed reserved identifier `_CRT_SECURE_NO_WARNINGS` in source headers to comply with strict `clang-tidy`.
  - Handled invalid record count arguments gracefully in `tools/BenchDuckdb.py`.

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

## [1.1.0] - 2026-10-12

### Added

- **Unified Placeholder Syntax**:
  - Introduced standard `{i}` (integer) and `{s}` (string) placeholders with `{{` and `}}` literal brace escaping across C++, Python, and DuckDB extensions.
  - Added `ScanMode::Semantic` as default tokenizer mode grouping contextual strings (IPs, paths, hashes) alongside standalone numbers.
- **Foreign Language Adapter Scaffolding**:
  - Added OpenTelemetry Collector processor module (`ext/otel/`) and Vector Remap Language native transform crate (`ext/vector/`).

### Changed

- **Deprecation of Legacy Delimiters**:
  - Deprecated unescaped `0x12` and `0x13` control character delimiters in favor of canonical template strings and structured records.

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

[1.1.5]: https://github.com/razobill/razobill/releases/tag/v1.1.5
[1.1.4]: https://github.com/razobill/razobill/releases/tag/v1.1.4
[1.1.3]: https://github.com/razobill/razobill/releases/tag/v1.1.3
[1.1.2]: https://github.com/razobill/razobill/releases/tag/v1.1.2
[1.1.1]: https://github.com/razobill/razobill/releases/tag/v1.1.1
[1.1.0]: https://github.com/razobill/razobill/releases/tag/v1.1.0
[1.0.1]: https://github.com/razobill/razobill/releases/tag/v1.0.1
[1.0.0]: https://github.com/razobill/razobill/releases/tag/v1.0.0
