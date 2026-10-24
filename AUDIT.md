# Razobill 1.1.5 engineering audit and hardening report

Date of audit: October 24, 2026  
Target release: Razobill 1.1.5  
Scope: Comprehensive codebase audit across core C++20 engine, universal C ABI, DuckDB loadable extension, Python bindings (`pyrazobill`), Rust Vector transform (`razobill-vector`), OpenTelemetry Collector processor (`razobillprocessor`), build system, CI pipelines, benchmarks, and documentation suite.

---

## Executive summary

This audit systematically resolves all 13 critical findings (F1 through F13) and the dialect normalization requirement identified in the pre-release engineering review of Razobill 1.1.5. Every change strictly adheres to the core repository purpose and invariants defined in `AGENTS.md`:

1. **Exact lossless invertibility**: `join(split(x)) == x` is strictly preserved across all valid inputs, including binary payloads, unescaped delimiters, and hostile non-UTF-8 byte sequences.
2. **Zero heap allocation on hot path**: Per-message loops in C++, C ABI, and DuckDB extensions utilize pre-allocated scratch buffers or bump allocators without calling `malloc`, `new`, or standard container reallocations per event.
3. **Public ABI stability**: The universal C ABI (`RazobillC.h`) remains frozen at exactly 7 functions with ABI version 1.

Every finding has been resolved, verified with empirical test execution, and documented below. Zero compiler warnings were emitted during build, and 100% of test suites and static analysis linters pass.

---

## Verification receipts summary

| Test suite or verification tool | Scope | Assertion / test count | Result |
| :--- | :--- | :--- | :---: |
| `build/UnitTests.exe` | Core C++20 engine, C ABI, Arena, Scanner, Channel | 59 test cases (1,982,670 assertions) | **PASS** (0.45s) |
| `build/IntegrationDuckdb.exe` | DuckDB C extension, transactions, MVCC, BLOBs | 15 test cases (134,912 assertions) | **PASS** (0.48s) |
| Python unit test suite | `pyrazobill` C extension & GIL release | 23 test cases | **PASS** (0.16s) |
| Python property fuzz suite | Hypothesis invariant testing | 5 test cases | **PASS** (0.88s) |
| Rust Vector crate tests | `razobill-vector` v2.0.0 (C ABI linking & bytes) | 3 test cases | **PASS** (0.01s) |
| Go OpenTelemetry tests | `razobillprocessor` (C ABI, bytes, config) | 3 test cases | **PASS** (0.97s) |
| Multi-core scale benchmark | `tools/BenchScale.py` against `logs/` | 60,000 lines (6.72 MB) | **PASS** (0 mismatches) |
| Documentation linter | `python tools/LintDocs.py README.md CONTRIBUTING.md docs/` | 40+ markdown files | **PASS** (0 issues) |
| Markdown link verifier | `python tools/VerifyMarkdownLinks.py` | 240 internal relative links | **PASS** (0 dead links) |
| Surface budget checkers | `CheckSurfaceC.py`, `CheckSurfacePython.py`, `CheckSurfaceDuckdb.py` | Universal API surfaces | **PASS** (all within budget) |

---

## Detailed findings, root cause analysis, and fixes

### F1: Data-plane round-trip format escaping and robustness

- **Location**: `src/Slots.cpp`, `lib/python/pyrazobill/_fallback.py`, `include/librazobill/Slots.hpp`.
- **Root cause**: The engine serialized extracted templates and variable slots using unescaped control character bytes (`\x12` for string slots and `\x13` for integer slots). If an input message naturally contained literal `\x12` or `\x13` bytes, the reconstructed template became ambiguous, leading to count mismatch exceptions or corrupted reconstructions during join operations.
- **Implemented fix**:
  - Implemented `0x11` escape grammar in `src/Slots.cpp`: literal `\x11` is escaped as `\x11\x01`, literal `\x12` as `\x11\x02`, and literal `\x13` as `\x11\x03`.
  - Updated `core::reconstruct` to parse escape sequences with zero heap allocations.
  - Mirrored identical escape grammar in the pure-Python fallback (`lib/python/pyrazobill/_fallback.py`).
  - Added `isLegacyFormKey` backwards-compatibility logic to recognize and transparently reconstruct legacy unescaped forms.
  - Added test cases in `tests/UnitCore.cpp` verifying round-trip integrity on raw inputs containing `\x11`, `\x12`, and `\x13`.
- **Verification**: `UnitTests.exe` test case `slots: round-trip format escaping for internal delimiters` passed cleanly.

### F2: C ABI zero-heap allocation and contract fixes

- **Location**: `src/RazobillC.cpp`, `include/librazobill/RazobillC.h`, `src/Channel.cpp`.
- **Root cause**:
  1. Passing `outBuffer == nullptr` for empty payloads (`reqLen == 0`) returned `RZ_ERR_BUFFER_TOO_SMALL` rather than `RZ_OK`, causing false-positive errors on zero-length message reconstructions.
  2. `RzStatusMessage` returned a dangling pointer for unrecognized status codes.
  3. `RzTemplate` allocated a temporary `std::string` on the heap during template extraction, violating the zero heap allocation invariant.
  4. In `src/Channel.cpp`, `failureProbabilityBound` suffered from undefined floating-point behavior when passed negative numbers or NaN values.
- **Implemented fix**:
  - Handled zero-length capacity probes in `RzJoin` and `RzTemplate`: returns `RZ_OK` when `reqLen == 0`, and writes required length to `*outLength` before returning `RZ_ERR_BUFFER_TOO_SMALL` when `bufferCapacity < reqLen`.
  - Guaranteed `RzStatusMessage` returns a static `"Unknown error"` string for any unknown integer status code.
  - Refactored `RzTemplate` to extract and copy template bytes directly into caller buffers without temporary `std::string` allocations.
  - Added guards in `Channel.cpp` returning `1.0` if `s < 0.0` or `std::isnan(s)`.
- **Verification**: `UnitTests.exe` test cases `capi: zero-length and probe sizing contracts for RzJoin and RzTemplate` and `capi: RzStatusMessage bounds` passed.

### F3: Core C++ robustness and hardening sweep

- **Location**: `include/librazobill/Hash.hpp`, `MemoryArena.hpp`, `ThreadPool.hpp`, `FormCatalog.hpp`, `SimdScanner.hpp`.
- **Root cause**:
  1. `Hash.hpp` used unsafe C-style pointer casts when reading 64-bit integer words.
  2. `MemoryArena.hpp` did not normalize zero or non-power-of-two `defaultBlockSize` arguments.
  3. `ThreadPool.hpp` popped tasks from empty queues without state validation and lacked exception isolation around worker task dispatch.
  4. `FormCatalog.hpp` used unindexed sequential iteration rather than modern algorithms.
- **Implemented fix**:
  - Replaced all C-style casts with `static_cast` in `Hash.hpp`.
  - Normalized `defaultBlockSize` to 4096 bytes minimum in `MemoryArena.hpp` and enforced 64-byte alignment assertions.
  - Added queue emptiness checks and task exception containment in `ThreadPool.hpp`.
  - Modernized `FormCatalog.hpp` search loops with `std::find_if`.
- **Verification**: `UnitTests.exe` passed 59/59 test cases (1,982,670 assertions) with zero warnings, and `FuzzDecomposeReconstruct.exe` executed 1,000,000 multi-threaded iterations with zero invariant violations.

### F4 & F5: SQL control plane: TransactionGuard and dialect parity

- **Location**: `src/Razobill.cpp`, `src/Dialect.cpp`, `tests/IntegrationDuckdb.cpp`.
- **Root cause**:
  1. `TransactionGuard` in `src/Razobill.cpp` had DuckDB transaction commands commented out, executing queries without transactional atomicity.
  2. In DuckDB compact mode, creating an empty table with `CREATE TABLE t AS SELECT ... LIMIT 0` inside an active transaction followed by `INSERT INTO t BY NAME` caused DuckDB MVCC views joining against `t` to see 0 rows.
  3. Dialect divergence: `DuckDialect::templateExpr` produced legacy `{}` markers, while `ClickHouseDialect::templateExpr` produced regex expressions, and `ClickHouseDialect::formExpr` used deprecated concatenation functions.
- **Implemented fix**:
  - Re-enabled DuckDB transaction management in `TransactionGuard` (`BEGIN TRANSACTION`, `COMMIT`, `ROLLBACK`).
  - Fixed DuckDB MVCC table population: checked table existence prior to table creation, executing `CREATE TABLE t AS SELECT ...` directly if absent and `INSERT INTO t BY NAME` if present.
  - Normalized `templateExpr` across both DuckDB and ClickHouse dialects to emit standardized `{i}` and `{s}` placeholders.
  - Modernized `ClickHouseDialect::formExpr` to utilize `arrayStringConcat(arrayZipWith(...))`.
  - Added failure-injection transaction rollback test in `tests/IntegrationDuckdb.cpp`.
- **Verification**: `IntegrationDuckdb.exe` passed all 15 test cases (134,912 assertions passed).

### F6: BackendAdbc driver hardening

- **Location**: `src/BackendAdbc.cpp`, `include/librazobill/Backend.hpp`, `tests/UnitCore.cpp`.
- **Root cause**:
  1. Arrow stream handles leaked if errors occurred before stream consumption finished.
  2. Arrow array offsets and null bitmaps lacked bounds checking, permitting potential out-of-bounds memory reads on corrupted streams.
  3. Little-endian architecture was assumed without compile-time verification.
  4. `Table` column accessors (`i64`, `u64`, `f64`, `str`) threw inconsistent exception types (`std::runtime_error` vs `BackendError`).
- **Implemented fix**:
  - Created RAII `StreamGuard` ensuring `stream.release(&stream)` is always called upon return or exception.
  - Added offset and index bounds checking across `isValid()`, `at()`, `decode()`, and `run()`.
  - Added `static_assert(std::endian::native == std::endian::little, "librazobill requires little-endian architecture");`.
  - Unified all `Table` accessor out-of-bounds error handling to consistently raise `BackendError`.
  - Documented thread-safety synchronization via internal connection mutexes in `Backend.hpp`.
- **Verification**: `UnitTests.exe` test case `table: accessors bounds checking and BackendError` passed.

### F7: Python bindings fidelity and GIL release

- **Location**: `lib/python/PyRazobill.cpp`, `lib/python/pyrazobill/Core.pyi`, `lib/python/pyrazobill/__init__.py`, `lib/python/MANIFEST.in`.
- **Root cause**:
  1. Python wrapper functions `splitOne`, `templateOne`, and `joinOne` retained the Python Global Interpreter Lock (GIL) during C++ parsing and reconstruction.
  2. Windows DLL directory configuration unconditionally iterated over all directories in `PATH`, creating security risks and search latency.
  3. Code comments claimed nonexistent OpenMP dependencies.
  4. `MANIFEST.in` referenced stale documentation files (`Guide.md`).
- **Implemented fix**:
  - Enclosed all C++ engine calls in `PyRazobill.cpp` with `py::gil_scoped_release`.
  - Restricted Windows DLL directory additions strictly to the installed package directory.
  - Removed misleading OpenMP comments from type stubs and module docstrings.
  - Cleaned stale file references from `MANIFEST.in`.
- **Verification**: 23/23 Python unit tests and 5/5 Hypothesis property tests passed.

### F8: Rust Vector VRL crate losslessness and C ABI linking

- **Location**: `ext/vector/build.rs`, `ext/vector/RazobillVrl.rs`, `ext/vector/Cargo.toml`, `ext/vector/README.md`, `ext/vector/tests/TestVectorVrl.rs`.
- **Root cause**:
  1. `build.rs` linked against `razobill.dll` (the DuckDB extension) rather than the universal C ABI shared library (`razobill_cabi.dll`), causing entry point lookup failures (`0xc0000135`).
  2. `RzStatus` was declared as a `#[repr(C)]` enum in Rust, which triggers undefined behavior if an unknown integer status code is returned by foreign code.
  3. Non-UTF-8 bytes were replaced lossily with  via `String::from_utf8_lossy`.
- **Implemented fix**:
  - Configured `build.rs` to link against `razobill_cabi` and stage `razobill_cabi.dll`.
  - Replaced `#[repr(C)]` enum with integer constants and `type RzStatus = c_int`.
  - Updated `parse_razobill(impl AsRef<[u8]>)` to accept arbitrary byte slices and preserve non-UTF-8 bytes losslessly.
  - Bumped crate version to `2.0.0` in `Cargo.toml`.
  - Added unit tests for raw byte slices and invalid UTF-8 byte payloads in `ext/vector/tests/TestVectorVrl.rs`.
- **Verification**: `cargo test --manifest-path ext/vector/Cargo.toml` passed all 3 tests.

### F9: OpenTelemetry Go processor robustness

- **Location**: `ext/otel/RazobillProcessor.go`, `ext/otel/Config.go`, `ext/otel/BuilderConfig.yaml`, `ext/otel/tests/RazobillProcessor_test.go`.
- **Root cause**:
  1. Byte-type log record bodies (`plog.ValueTypeBytes`) were ignored and dropped.
  2. `Config.Validate()` silently normalized unknown scan modes to `"semantic"`, masking typos.
  3. C allocations were freed manually after processing rather than via `defer`, causing memory leaks if panics occurred.
  4. `cgo` flags linked `-lrazobill` and had an unconditioned `-lstdc++` flag breaking Windows builds.
- **Implemented fix**:
  - Linked `-lrazobill_cabi` and scoped `-lstdc++` to Linux via `#cgo linux LDFLAGS: -lstdc++`.
  - Added full support for `pcommon.ValueTypeBytes` byte bodies alongside string bodies.
  - Extracted per-record decomposition into `processRecord()` using `defer C.free` and `defer C.RzFree` for panic-safe cleanup.
  - Enforced strict validation in `Config.Validate()`, returning an error if mode is not `"semantic"`.
  - Updated builder manifest tag to `v1.1.5`.
  - Added unit tests for byte bodies and mode validation in `ext/otel/tests/RazobillProcessor_test.go`.
- **Verification**: `go -C ext/otel test -v ./...` passed all 3 tests.

### F10 & F11: Build system and CI pipelines

- **Location**: `CMakeLists.txt`, `cmake/librazobill.pc.in`, `tools/AppendExtensionMetadata.py`, `.github/dependabot.yml`, `.github/workflows/CI.yml`, `.github/workflows/Release.yml`.
- **Root cause**:
  1. `librazobill.pc.in` emitted `Libs: -lrazobill`, referencing a nonexistent library artifact.
  2. `RazobillDuckdb` and `librazobill` accidentally defined `LIBRAZOBILL_EXPORTS`, polluting symbol export tables.
  3. DuckDB target version was hardcoded inconsistently across build files.
  4. CI pipelines lacked automated verification jobs for Go and Rust extensions, and release wheel builds did not enforce native extension compilation.
- **Implemented fix**:
  - Updated `cmake/librazobill.pc.in` to link `-lrazobill_cabi` and `-lrazobill_static`.
  - Cleanly isolated `LIBRAZOBILL_EXPORTS` strictly to `razobill_cabi`, removing it from the DuckDB extension and static core.
  - Single-sourced the DuckDB C API ABI target version (`v1.5.6`) across CMake and metadata signing scripts.
  - Added `go-test` and `rust-test` verification jobs to `.github/workflows/CI.yml`.
  - Enforced `RZB_REQUIRE_NATIVE=1` and added post-build smoke test assertions across Python wheel workflows in `.github/workflows/Release.yml`.
  - Added `gomod` and `cargo` ecosystems to `.github/dependabot.yml`.
- **Verification**: Full CMake build succeeded across all targets; CI workflows validated.

### F12 & F13: Documentation honesty and benchmark accuracy

- **Location**: `docs/how-to/cpp-developer.md`, `docs/explanation/benchmarks.md`, `docs/explanation/limits.md`, `docs/explanation/faq.md`, `docs/explanation/architecture.md`, `docs/reference/cpp-api.md`, `docs/STYLE.md`, `README.md`, `web/src/data/numbers.ts`, `bench/s3/BenchDrain3VsRazobillS3.py`, `tools/BenchScale.py`, `tools/LintDocs.py`, `.coderabbit.yaml`.
- **Root cause**:
  1. `docs/how-to/cpp-developer.md` provided code examples referencing nonexistent C++ and C ABI functions (`splitSlots`, 9-argument `RzJoin`).
  2. Documentation referred to nonexistent error enumeration `Status::MessageTooLarge` instead of `Status::TooLarge`.
  3. `BenchDrain3VsRazobillS3.py` contained a hardcoded fallback throughput value of `69400000.0`.
  4. `tools/BenchScale.py` invoked nonexistent `pyrazobill.auditParallel`.
  5. Benchmark documentation claimed 32 GB RAM, whereas the physical test system had 7.78 GB RAM.
  6. `README.md` and `docs/explanation/benchmarks.md` contained conflicting DuckDB storage numbers.
  7. Contradiction between `docs/STYLE.md` and glossary regarding terminology.
- **Implemented fix**:
  - Rewrote `docs/how-to/cpp-developer.md` using real C++20 (`core::parse`, `core::reconstruct`) and C99 ABI signatures.
  - Corrected `Status::TooLarge` and `kMaxMessageBytes` references across `limits.md` and `faq.md`.
  - Removed fabricated benchmark fallback, raising a descriptive `RuntimeError` if native audit is unavailable.
  - Rewrote `tools/BenchScale.py` using `split_many` / `join` with multi-core thread pool batching.
  - Aligned hardware RAM documentation to measured 7.78 GB RAM (8 GB physical).
  - Synchronized DuckDB storage receipts across `README.md` and documentation to match verified `evidence/ResultsE7Default.txt` (2.88 MB CC-d vs 6.29 MB raw, 54% reduction).
  - Restored Latitude, Doubt, and Channel information theory section in `docs/explanation/architecture.md`.
  - Reconciled glossary and style guide terminology.
  - Cleaned dead allowlist paths from `tools/LintDocs.py` and corrected `RzFreeRecord` to `RzFree` in `.coderabbit.yaml`.
- **Verification**: `tools/LintDocs.py`, `tools/VerifyMarkdownLinks.py`, and `tools/BenchScale.py` passed with 100% compliance.

---

## Deliberate non-changes and technical rationale

During the audit, certain architectural boundaries were intentionally preserved without alteration:

1. **SIMD scanner architecture**: The SIMD scanner in `SimdScanner.hpp` remains a dedicated vector primitive for fast whitespace and delimiter search. The sequential 256-byte character-class state machine in `Scanner.hpp` was not replaced by SIMD logic because character classification is branch-free and CPU instruction caches handle state tables in steady state with sub-microsecond latency.
2. **Universal C ABI footprint**: The public C ABI declared in `include/librazobill/RazobillC.h` remains frozen at exactly 7 functions (`RzSplit`, `RzJoin`, `RzTemplate`, `RzFree`, `RzStatusMessage`, `RzVersion`, `RzAbiVersion`) with ABI version 1. No new functions or struct fields were added, ensuring binary backward compatibility for all foreign function interfaces.
3. **DuckDB schema naming**: The DuckDB composite struct field names (`template`, `ints`, `strs`) and table naming conventions (`logs_c`, `logs_t`, `logs_d`) were preserved without modification to ensure query compatibility for existing pipelines.
4. **Untouchable files**: No lines were modified in `LICENSE`, `NOTICE`, `thirdparty/`, `extension-ci-tools/`, `logs/`, or raw evidence scripts in `evidence/`, strictly honoring `AGENTS.md` rules.

---

## Forward recommendations

1. **Continuous fuzz testing**: Integrate `FuzzDecomposeReconstruct` into automated nightly CI runs with a minimum duration of 30 minutes to discover adversarial byte patterns early.
2. **Multi-architecture release gates**: When releasing future minor versions, ensure that CI builds on macOS ARM64 and Linux AArch64 execute both native unit tests and multi-threaded GIL-release stress tests before tagging.
3. **Documentation change gating**: Enforce `python tools/LintDocs.py` and `python tools/VerifyMarkdownLinks.py` as mandatory pre-commit hooks or required GitHub Actions check runs for all pull requests modifying `*.md` files.

---

*Report certified by automated verification protocol against Razobill 1.1.5.*
