# Razobill Round-2 Hardening & Correctness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Execute the complete Round-2 hardening audit sweep (W1 through W7) on Razobill 1.1.5, fixing all ~60 confirmed correctness, memory safety, ABI contract, binding fidelity, CI/supply chain, and documentation drift findings while preserving the three non-negotiable invariants, and publish the certified `AUDIT.md` Round 2 report bumping version to 1.1.6.

**Architecture:**
- Harden the C++20 core engine (`src/Slots.cpp`, `src/Channel.cpp`, `src/Razobill.cpp`, `include/librazobill/Scanner.hpp`) with overflow-checked size accumulation, manual-record sanity gates, finite/range parameter validation, ASCII locale-independence, and zero relational comparisons with `nullptr`.
- Double core parse throughput by dropping dead legacy `skeleton`/`types` string generation in `parse()`, keeping fields deprecated on `SlotRecord` for struct ABI compatibility.
- Ensure 100% C ABI contract honesty in `RazobillC.h` and `RazobillC.cpp` via reuse of existing `InternalRecord` heap allocations, `CAbiScratch::trim()`, explicit zero-init documentation, 8-byte integer alignment, and ABI version assertions.
- Eliminate binding warts: keep Python Windows DLL directory cookies alive across module lifetime, ensure exception-safe PyObject cleanup, eliminate hardcoded MSVC `/arch:AVX2`, reject DuckDB $\ge 2^{63}$ integers before sign overflow, add DuckDB varargs tests, gate Go OTEL processor on `RzAbiVersion()`, and remove dead `cdylib` and phantom `.hash` keys in Rust.
- Single-source the version across all build scripts and bindings from the canonical `VERSION` file (1.1.6).

**Tech Stack:** C++20 (GCC 12+, Clang 15+, MSVC 2022+), C99 ABI, DuckDB C API v1.5.6, Python 3.10–3.13 (pybind11), Rust (Vector VRL), Go 1.22+ (OpenTelemetry Collector), CMake 3.25+, Ninja.

**Spec:** `C:\FM\rzd\razobill\.zcode\plans\plan-session_18f67f6b9a887c80709b70d54f3fec20.md`

## Global Constraints

1. **Exact lossless invertibility**: `join(split(x)) == x` must hold for all inputs, byte for byte.
2. **Zero heap allocation on hot path**: Per-message loops must use bump allocators, scratch buffers, or capacity reuse; no `new`/`malloc` per event.
3. **Public ABI stability**: Preserve C ABI (`RazobillC.h`, 7 functions, version 1) and DuckDB function signatures (`RzSplit`, `RzJoin`, `RzTemplate`, `RzHash`).
4. **Untouchable paths**: Never modify `LICENSE`, `NOTICE`, `thirdparty/`, `extension-ci-tools/`, `logs/`, or raw files in `evidence/`.
5. **Honesty and receipts**: Never invent versions, metrics, benchmarks, or APIs. Quote verified numbers only.

## Review Focus

1. **Integer overflow in `reconstruct` / `reconstructNew`**: Corrupt or maliciously crafted input with huge string lengths must not wrap around `std::size_t` or bypass `kMaxMessageBytes`.
2. **Relational comparison with `nullptr` in `Scanner.hpp:128`**: `p < unclosedDoubleQuoteEnd` when `unclosedDoubleQuoteEnd == nullptr` is UB in C++. Must guard pointer validity first.
3. **Python `os.add_dll_directory` garbage collection**: On Windows, the return value of `os.add_dll_directory()` must be saved in a module-level list to prevent immediate unregistration upon garbage collection.
4. **DuckDB $\ge 2^{63}$ integer decoding**: Unsigned 64-bit values $\ge 2^{63}$ must not be cast silently into negative signed integers; must be rejected with a descriptive error.
5. **Channel floating-point parameter bounds**: Non-finite (`NaN`, `Inf`) or out-of-range (`[0, 1)`) keys and dithers must fail gracefully without UB float-to-uint64 casts.

---

## Work Breakdown Structure

### Task 1: W1.1 & W1.4 — C++ Core Reconstruct & C ABI Hardening
**Files:**
- Modify: `src/Slots.cpp:128-135,170-225`
- Modify: `src/RazobillC.cpp:45-90,130-170`
- Test: `tests/UnitCAPI.cpp`, `tests/UnitCore.cpp`

**Implementation Details:**
1. In `src/Slots.cpp`:
   - In `reconstructLegacy`: add overflow-checked size accumulation when adding `digits10(ints[i])` and `strs[i].size()`. If `kMaxMessageBytes - size < addLen`, return `Status::TooLarge`.
   - In `reconstructNew`: add overflow checks when computing `estSize`. If `kMaxMessageBytes - estSize < added`, return `Status::TooLarge`.
2. In `src/RazobillC.cpp`:
   - In `RzJoin`: validate incoming `RzRecord` before parsing:
     - If `record->templateLength > razobill::core::kMaxMessageBytes`, return `RZ_ERR_MESSAGE_TOO_LARGE`.
     - An `{i}` or `{s}` placeholder requires at least 3 bytes. If `record->intCount > record->templateLength / 3 + 1` or `record->strCount > record->templateLength / 3 + 1`, return `RZ_ERR_COUNT_MISMATCH`.
     - If any `record->strLengths[i] > razobill::core::kMaxMessageBytes`, return `RZ_ERR_MESSAGE_TOO_LARGE`.
   - In `RzSplit`: if `outRecord->internalHandle` is already non-null, reuse its allocated memory and clear capacities rather than `delete` followed by `new`:
     ```cpp
     InternalRecord* internal = static_cast<InternalRecord*>(outRecord->internalHandle);
     if (internal) {
         internal->slots.clear();
         internal->strPointers.clear();
         internal->strLengths.clear();
     } else {
         internal = new InternalRecord();
     }
     ```
   - In `CAbiScratch`: add `trim()` method that resets `result`, `strViews`, and `slots.templateStr` if capacities exceed `1 << 20` (1 MB) to prevent permanent memory retention after extreme message spikes.
3. Tests in `tests/UnitCAPI.cpp`:
   - Test corrupt manual records (huge templateLength, disproportionate slot counts, oversized string length) rejecting with `RZ_ERR_MESSAGE_TOO_LARGE` or `RZ_ERR_COUNT_MISMATCH`.
   - Test record reuse in `RzSplit` proving capacity is retained without memory leaks.

---

### Task 2: W1.4 — Hot-Path Allocation Pruning: Drop Dead `skeleton`/`types`
**Files:**
- Modify: `include/librazobill/Slots.hpp:28-40`
- Modify: `src/Slots.cpp:300-350`
- Modify: `ext/duckdb/DuckdbExtension.cpp:115-130`
- Test: `tests/UnitCore.cpp`, `bench/BenchCore.cpp`

**Implementation Details:**
1. In `include/librazobill/Slots.hpp`:
   - Mark `skeleton` and `types` in `SlotRecord` as `[[deprecated("legacy wire format fields no longer populated in parse")]]`.
2. In `src/Slots.cpp`:
   - In `parse()`: remove all writes to `out.skeleton` and `out.types`.
   - Remove `out.skeleton.reserve()`, `out.skeleton.append(gap)`, `out.skeleton.push_back(kPlaceholder)`, `out.skeleton.append(result.tail)`, and `out.types.push_back()`.
   - Only populate `out.templateStr`, `out.ints`, and `out.strs`.
3. In `ext/duckdb/DuckdbExtension.cpp`:
   - Remove `record.skeleton` from `Scratch::trim()` as it is never populated.
4. Verification:
   - Run `.\build\UnitTests.exe` to verify 100% test pass.
   - Run `.\build\BenchCore.exe` to benchmark throughput improvement.

---

### Task 3: W1.2 — Channel.cpp Finite & Range Validation + Documentation
**Files:**
- Modify: `src/Channel.cpp:168-195,245-260,305-335`
- Modify: `include/librazobill/Channel.hpp:85-115`
- Test: `tests/UnitChannel.cpp`

**Implementation Details:**
1. In `src/Channel.cpp`:
   - In `embed()`: validate `sharedKey` and `dither`:
     ```cpp
     if (!std::isfinite(sharedKey) || sharedKey < 0.0 || sharedKey >= 1.0 ||
         !std::isfinite(dither) || dither < 0.0 || dither >= 1.0) {
         res.success = false;
         return res;
     }
     ```
   - In `decode()`: validate `sharedKey`:
     ```cpp
     if (!std::isfinite(sharedKey) || sharedKey < 0.0 || sharedKey >= 1.0) {
         res.success = false;
         return res;
     }
     ```
   - In `decode()`: ensure `leftWrapped * numCells` and `rightWrapped * numCells` are finite and bounded before `static_cast<std::uint64_t>`.
2. In `include/librazobill/Channel.hpp`:
   - Add explicit documentation note: `exactFailureProbability` computes the theoretical bound under uniform dither distribution $V \sim U[0, 1)$, whereas `embed` defaults to deterministic fixed dither when not specified.
3. Tests in `tests/UnitChannel.cpp`:
   - Test `embed` and `decode` rejecting `NaN`, `+Inf`, `-Inf`, `-0.01`, `1.0`, and `1.5`.
   - Verify embed-decode roundtrip property.

---

### Task 4: W1.3 & W1.5 & W1.6 — Locale Independence, Scanner UB, `templateOf` Parity & Control Plane Hardening
**Files:**
- Modify: `include/librazobill/Scanner.hpp:50-60,126-135,190-201`
- Modify: `src/Slots.cpp:325-335,420-435`
- Modify: `src/Razobill.cpp:124-140,395-430,880-920`
- Modify: `include/librazobill/FormCatalog.hpp:130-145`
- Modify: `include/librazobill/MemoryArena.hpp:78-95`
- Modify: `src/BackendAdbc.cpp:300-305,370-395`
- Modify: `include/librazobill/Hash.hpp:20-25`
- Test: `tests/UnitCore.cpp`, `tests/UnitArena.cpp`, `tests/IntegrationDuckdb.cpp`

**Implementation Details:**
1. In `include/librazobill/Scanner.hpp`:
   - Line 128: fix relational pointer comparison UB by guarding `unclosedDoubleQuoteEnd != nullptr && p < unclosedDoubleQuoteEnd` and `unclosedSingleQuoteEnd != nullptr && p < unclosedSingleQuoteEnd`.
   - Line 190: remove dead `lastDelimChar == '@'` check because `@` is classified as non-delimiter.
2. In `src/Slots.cpp`:
   - In `parse()`: defensively handle `std::from_chars` result (`if (ec == std::errc()) ... else ... push to strs`).
   - In `templateOf()`: treat unescaped `0x12` and `0x13` as literal byte passthrough in the fallback branch, achieving exact parity with `reconstructNew`.
3. In `src/Razobill.cpp`:
   - In `requireName()`: use explicit ASCII range checks `(c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'` instead of locale-dependent `std::isalnum`.
   - In `num(double d)`: replace `std::ostringstream` with `std::to_chars` to guarantee locale-independent `.` decimal formatting.
   - Scope `rb_layout` per table family (`name + "_layout"` or include table family qualifier).
   - In `meaningOf()`: match both `id` and `template` to prevent 64-bit hash collision aliasing.
4. In `include/librazobill/FormCatalog.hpp`:
   - Guarantee strong exception safety: if `formsByKey_.emplace` or `formsById_.push_back` throws, clean up inserted keys from `hashToKeys_`.
5. In `include/librazobill/MemoryArena.hpp`:
   - Validate and cap alignment: power-of-two check and cap at 4096 bytes.
6. In `src/BackendAdbc.cpp`:
   - In `cleanup()`: lock `connMutex_` to serialize destruction with any in-flight execution.
   - Mark unused `rows` as `(void)rows;`.
7. In `include/librazobill/Hash.hpp`:
   - Remove unused `#include <cstdlib>`.

---

### Task 5: W2 — C ABI Contract Honesty
**Files:**
- Modify: `include/librazobill/RazobillC.h:40-60,200-220`
- Modify: `docs/reference/c-abi.md`
- Test: `tests/UnitCAPI.cpp`

**Implementation Details:**
1. In `include/librazobill/RazobillC.h`:
   - Update stale `1.1.4` version examples to `1.1.6`.
   - Document zero-initialization requirement for `RzRecord`: `RzRecord rec = {0};` must be passed to `RzSplit`.
   - Document shallow-copy hazard: `RzRecord` owns heap memory via `internalHandle`; copying by value causes double-free on `RzFree`.
   - Document `intValues` 8-byte alignment requirement (`alignof(int64_t)`).
   - Document `*outLength`-on-error semantics: `*outLength` receives the required buffer size whenever `RZ_ERR_BUFFER_TOO_SMALL` is returned.
   - Document manual-record bounds enforced by `RzJoin` (`templateLength <= 16MB`, slot counts $\le$ `templateLength / 3 + 1`, string lengths $\le 16MB$).
   - Add static assertion for ABI version:
     ```c
     #if defined(__cplusplus)
     static_assert(RZ_ABI_VERSION == 1, "Universal C ABI version mismatch");
     #endif
     ```
2. In `docs/reference/c-abi.md`:
   - Synchronize all documented contracts, error codes, and record constraints with `RazobillC.h`.
3. In `tests/UnitCAPI.cpp`:
   - Verify zero-initialized and pre-populated record contracts.

---

### Task 6: W3.1 — Python Bindings Hardening & Single-Sourcing
**Files:**
- Modify: `lib/python/pyrazobill/__init__.py:35-55`
- Modify: `lib/python/PyRazobill.cpp:170-225`
- Modify: `lib/python/setup.py:20-45,140-182`
- Modify: `lib/python/pyrazobill/_fallback.py:345-355`
- Modify: `lib/python/Publishing.md`
- Modify: `lib/python/tests/TestPyRazobill.py`
- Test: `python -m unittest discover -s lib/python/tests`, `python -m pytest lib/python/tests`

**Implementation Details:**
1. In `lib/python/pyrazobill/__init__.py`:
   - Fix `os.add_dll_directory` garbage collection bug: store directory handles in a module-level list `_dll_directories = []` to prevent Python from unregistering the paths on GC.
2. In `lib/python/PyRazobill.cpp`:
   - Ensure exception-safe `encoded` cleanup in `splitOne` for `py::str`: wrap `PyObject* encoded` in RAII helper or `py::reinterpret_steal<py::bytes>` so that exceptions during list creation or decoding do not leak memory.
3. In `lib/python/setup.py`:
   - Dynamically read `VERSION` from `repoRoot / "VERSION"` (with local fallback) instead of hardcoded `"1.1.4"`.
   - Remove hardcoded `/arch:AVX2` on MSVC: compile with `/std:c++20 /EHsc /bigobj` and rely on Razobill's runtime CPUID SIMD dispatch.
   - For MinGW builds on Windows, synchronize extra link flags with CMake: `-static-libgcc -static-libstdc++ -Wl,-Bstatic -l:libwinpthread.a -Wl,-Bdynamic`.
   - Sync staged `srcCore/` files during build so `sdist` builds include all MinGW TLS fixes.
4. In `lib/python/pyrazobill/_fallback.py`:
   - Dynamically load version or update to `1.1.6`.
5. In `lib/python/tests/TestPyRazobill.py`:
   - Add native vs pure-Python fallback hash parity test across diverse test strings.

---

### Task 7: W3.2 — DuckDB Extension Hardening & Varargs Tests
**Files:**
- Modify: `ext/duckdb/DuckdbExtension.cpp:330-345`
- Modify: `ext/duckdb/description.yml`
- Modify: `tests/IntegrationDuckdb.cpp`
- Test: `.\build\IntegrationDuckdb.exe`

**Implementation Details:**
1. In `ext/duckdb/DuckdbExtension.cpp`:
   - In `appendIntListValues()`: guard `DUCKDB_TYPE_UBIGINT`:
     ```cpp
     case DUCKDB_TYPE_UBIGINT: {
         const auto u = static_cast<const std::uint64_t*>(a.childData)[childIdx];
         if (u >= (std::uint64_t{1} << 63)) {
             return false; // integer exceeds signed int64 range
         }
         val = static_cast<std::int64_t>(u);
         break;
     }
     ```
   - In `RzJoinImpl`: if `appendIntListValues` returns `false`, report a clear error via `duckdb_scalar_function_set_error`: `"RzJoin: integer value exceeds signed 64-bit range (>= 2^63) or is null"`.
2. In `ext/duckdb/description.yml`:
   - Refresh extension description and update version to `1.1.6`.
3. In `tests/IntegrationDuckdb.cpp`:
   - Add integration tests for varargs `RzJoin` scalar overloads:
     `SELECT RzJoin('User {s} logged in with port {i}', 'alice', 443)`
   - Add test verifying rejection of `UBIGINT >= 2^63`.

---

### Task 8: W3.3 & W3.4 — OTel Go Processor & Rust Vector Crate Hardening
**Files:**
- Modify: `ext/otel/RazobillProcessor.go:45-80`
- Modify: `ext/otel/go.mod`
- Modify: `ext/otel/tests/RazobillProcessor_test.go`
- Modify: `ext/vector/Cargo.toml:15-20`
- Modify: `ext/vector/RazobillVrl.rs:50-60`
- Modify: `ext/vector/VectorConfig.yaml:25-32`
- Modify: `ext/vector/README.md`
- Modify: `ext/vector/tests/TestVectorVrl.rs`
- Test: `go -C ext/otel test -v ./...`, `cargo test --manifest-path ext/vector/Cargo.toml`

**Implementation Details:**
1. In `ext/otel/RazobillProcessor.go`:
   - In `Start()`: invoke `C.RzAbiVersion()` and `C.RzVersion()` to verify compatibility at startup:
     ```go
     if int(C.RzAbiVersion()) != 1 {
         return fmt.Errorf("incompatible librazobill ABI version: got %d, want 1", int(C.RzAbiVersion()))
     }
     ```
   - In `processRecord()`: align empty-body behavior with Python/DuckDB (set template to `""` rather than skipping attributes).
   - In `ConsumeLogs`: check context cancellation `ctx.Err() != nil` per resource.
2. In `ext/otel/go.mod`:
   - Run `go mod tidy` to clean indirect dependencies and direct `cespare/xxhash/v2`.
3. In `ext/vector/Cargo.toml`:
   - Change `crate-type = ["lib", "cdylib"]` to `crate-type = ["lib", "rlib"]`.
4. In `ext/vector/RazobillVrl.rs`:
   - Delete dead `RawRecord` struct.
5. In `ext/vector/VectorConfig.yaml`:
   - Remove phantom `.hash = .razobill.hash` key.
6. In `ext/vector/README.md`:
   - Remove unverified 2.38 M lines/s throughput claim.
7. In `ext/vector/tests/TestVectorVrl.rs`:
   - Add tests for empty input, error handling, and non-UTF-8 content.

---

### Task 9: W4 — Build System, Supply Chain, and CI Pipeline Hardening
**Files:**
- Modify: `CMakeLists.txt:125-145`
- Modify: `.github/workflows/CI.yml`
- Modify: `.github/workflows/Release.yml`
- Modify: `Makefile`
- Modify: `tools/AppendExtensionMetadata.py`
- Modify: `bench/BenchCore.cpp`
- Create: `.github/CODEOWNERS`
- Test: Full CMake build, Python/Rust/Go test runs

**Implementation Details:**
1. In `CMakeLists.txt`:
   - Add `-fvisibility=hidden` on `razobill_cabi` shared library so only `RZ_API` symbols are exported.
   - Add opt-in `RZ_WERROR` option (e.g. `-DRZ_WERROR=ON`) that turns compiler warnings into errors (`-Werror` / `/WX`), enabled in CI.
2. In `.github/workflows/CI.yml`:
   - Add enforcing jobs for `markdownlint-cli2`, `VerifyMarkdownLinks.py`, `LintDocs.py`, and Python `ruff`.
   - Unify environment variable `PYRAZOBILL_REQUIRE_NATIVE=1`.
   - Update `pip install -e .` to `pip install -e lib/python`.
3. In `.github/workflows/Release.yml`:
   - Replace fake SBOM stub with real SHA256 checksum generation (`sha256sum * > SHA256SUMS`).
   - Remove bogus `docs/governance/` artifact path.
   - Pin release-critical GitHub Actions to commit SHAs.
4. In `Makefile`:
   - Read `EXTENSION_VERSION` dynamically from `VERSION`.
5. In `tools/AppendExtensionMetadata.py`:
   - Provide a clear, actionable error message if any metadata field exceeds 32 characters.
6. In `bench/BenchCore.cpp`:
   - Correctly check `RZ_ERR_BUFFER_TOO_SMALL` in C ABI join benchmarking loops.
7. Create `.github/CODEOWNERS` defining ownership rules for repository subsystems.

---

### Task 10: W4 & W7 — Documentation Honesty Sweep, Version Bump to 1.1.6, and `AUDIT.md` Round 2 Deliverable
**Files:**
- Modify: `VERSION`
- Modify: `include/librazobill/Version.hpp`
- Modify: `lib/python/pyproject.toml`
- Modify: `lib/python/pyrazobill/__init__.py`
- Modify: `CITATION.cff`
- Modify: `CHANGELOG.md`
- Modify: `README.md`
- Modify: `web/src/data/numbers.ts`, `web/package.json`
- Modify: `docs/how-to/cpp-developer.md`
- Modify: `docs/explanation/limits.md`
- Modify: `docs/explanation/faq.md`
- Modify: `docs/reference/compatibility.md`
- Modify: `docs/reference/cpp-api.md`
- Modify: `docs/reference/release-governance.md`
- Modify: `docs/tutorials/packages.md`
- Modify: `CONTRIBUTING.md`
- Modify: `tools/LintDocs.py`
- Update: `AUDIT.md`
- Test: `tools/LintDocs.py`, `tools/VerifyMarkdownLinks.py`, `npx markdownlint-cli2`

**Implementation Details:**
1. Bump version to `1.1.6` across single-sourced files:
   - `VERSION`: `1.1.6`
   - `include/librazobill/Version.hpp`: `1.1.6`
   - `lib/python/pyproject.toml`: `1.1.6`
   - `lib/python/pyrazobill/__init__.py`: `1.1.6`
   - `CITATION.cff`: `1.1.6`
   - `CHANGELOG.md`: document `1.1.6` release entries.
2. In `docs/`:
   - Fix nonexistent `splitSlots/joinSlots` in `faq.md` → `parse/reconstruct`.
   - Fix header location in `limits.md`.
   - Update DuckDB target version in `packages.md`.
   - Update expected output in `cpp-developer.md`.
   - Reconcile compiler floors across `compatibility.md` and `cpp-api.md`.
3. In `tools/LintDocs.py`:
   - Add `splitSlots` and `joinSlots` to the banned terms list.
4. Update `AUDIT.md`:
   - Document all Round-2 findings (W1–W7), severity, file:line, root cause, fix, receipts, research paper citations (*Latitude and Doubt*, PVLDB, SIGMOD), and forward recommendations.

---

### Task 11: W5 & W6 — Test Hardening Sweep & Final Multi-Language Verification Receipts
**Files:**
- Modify: `tests/UnitCore.cpp`
- Modify: `tests/UnitCAPI.cpp`
- Modify: `tests/UnitChannel.cpp`
- Modify: `tests/fuzz/FuzzDecomposeReconstruct.cpp`
- Run: Full test suites and record official receipts in `AUDIT.md`.

**Implementation Details:**
1. In `tests/UnitCore.cpp`:
   - Add `\n`, `\r`, `\f` to the property fuzzing alphabet (lines 206, 224).
   - Add test cases verifying `templateOf` parity on raw control bytes.
2. In `tests/UnitCAPI.cpp`:
   - Add tests for zeroed records, `RzFree(NULL)`, record reuse, multithreaded split/join stress, `TOO_LARGE` rejection, and binary/NUL payloads via C ABI.
3. In `tests/UnitChannel.cpp`:
   - Add Monte-Carlo agreement verification tests for `exactFailureProbability`.
4. Run full verification suite:
   - `.\build\UnitTests.exe`
   - `.\build\IntegrationDuckdb.exe`
   - `.\build\FuzzDecomposeReconstruct.exe 1000000`
   - `python -m unittest discover -s lib/python/tests`
   - `python -m pytest lib/python/tests`
   - `cargo test --manifest-path ext/vector/Cargo.toml`
   - `go -C ext/otel test -v ./...`
   - `python tools/CheckSurfaceDuckdb.py`
   - `python tools/LintDocs.py README.md CONTRIBUTING.md docs/`
   - `python tools/VerifyMarkdownLinks.py`
   - `npx markdownlint-cli2 "**/*.md"`
   - `.\build\BenchCore.exe` (measure and document hot-path speedup)
