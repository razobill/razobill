# Razobill 1.1.5 Hardening & Audit Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement all 13 findings (F1 through F13) from the audit plan (`.zcode/plans/plan-session_94e47e3d06f1470d91833e3403c16c5a.md`) plus the DuckDB dialect `<*>` template resolution bug, restoring 100% test pass rate, exact lossless round-trip guarantees, zero-allocation hot paths, ABI stability, and documented receipts.

**Architecture:** 
1. Core data-plane escaping (`0x11` escape byte pairs for `0x11..0x13`) ensures unambiguous routing between modern brace templates and legacy skeletons without breaking ASCII payloads.
2. C ABI runtime uses `thread_local` scratch structures for `RzTemplate` and `RzJoin` to eliminate per-call heap allocations.
3. SQL control-plane activates `TransactionGuard` for DuckDB ACID transactions and aligns `formExpr` / `templateExpr` across DuckDB and ClickHouse so semantic rules `<*>` match templates and xxHash64 IDs stay bit-identical.
4. Bindings (Python GIL release, Rust byte vectors & `razobill_cabi` link, Go byte handling & panic-safe cleanup) restore multi-language losslessness and stability.
5. All fabricated benchmarks, outdated test receipts, and stale documentation APIs are corrected.

**Tech Stack:** C++20 (GCC 11+, Clang 13+, MSVC 2022), DuckDB C API & Extension v2.0.0, Arrow C Data Interface (ADBC), Python 3.10-3.13 / pybind11, Rust (cargo / vector VRL), Go 1.22+ (OpenTelemetry Collector).

**Spec:** `.zcode/plans/plan-session_94e47e3d06f1470d91833e3403c16c5a.md`

## Global Constraints

- **Exact Lossless Invertibility**: `join(split(x)) == x` must hold for all valid inputs across all language bindings.
- **Zero Heap Allocation on Hot Path**: Per-message loops in C ABI and core tokenization must use bump allocators or pre-allocated scratch buffers. Never call `malloc`, `new`, or resize containers per event.
- **Public ABI Stability**: Preserve the universal C ABI in `RazobillC.h` (7 functions, version 1) and DuckDB function signatures.
- **Untouchable Paths**: Never modify `LICENSE`, `NOTICE`, `thirdparty/`, `extension-ci-tools/`, `logs/`, or raw files in `evidence/`.
- **Honesty & Receipts Only**: Never invent versions, metrics, benchmarks, or APIs. Quote verified numbers only.

## Review Focus

1. **Adversarial control byte payloads**: Inputs like `x\x12\x13I` or `\x11\x12\x13` must not misroute into the legacy decoder or trigger `Status::Malformed` during `templateOf()`.
2. **DuckDB transaction abort recovery**: Errors inside queries (such as hostile regex syntax) must not leave active transactions un-rolled-back or poison subsequent queries.
3. **Template representation in SQL metadata**: `rb_form.template` must store normalized `<*>` templates in both DuckDB and ClickHouse so semantic rules match reliably.
4. **Foreign language FFI linkage**: Rust and Go must link against `razobill_cabi` rather than the DuckDB extension binary `razobill.dll` or non-existent `-lrazobill`.
5. **Zero-length buffer probes**: C ABI functions `RzJoin` and `RzTemplate` called with NULL output buffer and 0 length probe must return `RZ_OK` when output length is 0, not `RZ_ERR_BUFFER_TOO_SMALL`.

---

### Task 1: F1 — Data-Plane Round-Trip Format Escaping & Robustness

**Files:**
- Modify: `src/Slots.cpp:49-72, 150-250, 268-320, 350-398`
- Modify: `include/librazobill/Scanner.hpp:20-28`
- Test: `tests/UnitCore.cpp`

**Interfaces:**
- Consumes: `kPlaceholder` (0x12), `kTypeSeparator` (0x13), `kEscapePrefix` (0x11)
- Produces: `isLegacyFormKey(std::string_view form) noexcept -> bool`, `parse()`, `reconstructNew()`, `templateOf()`

- [ ] **Step 1: Write the failing regression tests in `tests/UnitCore.cpp`**

Add tests for adversarial control byte payloads that currently fail or misroute:
```cpp
TEST_CASE("core: adversarial control byte format ambiguity round-trip") {
    const std::vector<std::string> hostile = {
        std::string("x\x12\x13I"),
        std::string("a\x12b\x12c\x13IS"),
        std::string("prefix\x11\x12\x13suffix"),
        std::string("\x11\x11"),
        std::string("\x12\x13I")
    };
    for (const auto& msg : hostile) {
        SlotRecord r;
        REQUIRE(parse(msg, r, ScanMode::Semantic) == Status::Ok);
        CHECK(!isLegacyFormKey(r.templateStr));
        std::string rec;
        REQUIRE(reconstruct(r.templateStr, r.ints, r.strs, rec) == Status::Ok);
        CHECK(rec == msg);
        std::string display;
        CHECK(templateOf(r.templateStr, display) == Status::Ok);
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.\build\UnitTests.exe -tc="*adversarial control byte*"`
Expected: FAIL (either `isLegacyFormKey` returns true, or `templateOf` returns `Status::Malformed`)

- [ ] **Step 3: Implement 0x11-escaping in `src/Slots.cpp` and update `Scanner.hpp`**

1. In `include/librazobill/Scanner.hpp`:
   Define `inline constexpr unsigned char kEscapePrefix = 0x11;`.
2. In `src/Slots.cpp`:
   - Fix stale comment at line 51: state that placeholder is 0x12 and separator is 0x13.
   - In `isLegacyFormKey`: require that `form.find(static_cast<char>(kEscapePrefix)) == std::string_view::npos` (genuine legacy keys can never contain 0x11).
   - In `parse`: when copying gap/tail bytes into `templateStr`, escape `0x11`, `0x12`, and `0x13` by emitting `0x11` followed by the byte, alongside existing `{{` and `}}` escaping.
   - In `reconstructNew`: in pass 1 (sizing) and pass 2 (reconstruction), decode `0x11 <byte>` pairs as the literal `<byte>` (lone `0x11` at end or followed by non-escaped byte tolerated as literal). Sizing counts 1 byte per pair.
   - In `templateOf`: decode `0x11 <byte>` pairs back to `<byte>`, and remove the check that rejects `kPlaceholder` or `kTypeSeparator` if they were escaped.

- [ ] **Step 4: Run test to verify it passes**

Run: `.\build\UnitTests.exe`
Expected: ALL test cases PASS.

- [ ] **Step 5: Commit**

```bash
git add include/librazobill/Scanner.hpp src/Slots.cpp tests/UnitCore.cpp
git commit -m "fix(core): resolve format-ambiguity round-trip on control bytes via 0x11 escape pairs"
```

---

### Task 2: F2 — C ABI Zero-Heap Allocation & Contract Fixes

**Files:**
- Modify: `src/RazobillC.cpp:45-92, 101-149, 156-180`
- Modify: `include/librazobill/Slots.hpp:60-68`
- Test: `tests/UnitCAPI.cpp`

**Interfaces:**
- Consumes: `RzSplit`, `RzJoin`, `RzTemplate`, `RzFree`, `SlotRecord::formKeyHash`
- Produces: Zero-allocation hot path in C ABI, defensive record cleanup, zero-length probe support

- [ ] **Step 1: Write failing unit tests in `tests/UnitCAPI.cpp`**

Add tests for zero-length probe on empty message, defensive re-initialization of `RzRecord`, and zero-allocation hash:
```cpp
TEST_CASE("capi: zero-length probe and contract edge cases") {
    RzRecord rec;
    std::memset(&rec, 0, sizeof(rec));
    REQUIRE(RzSplit("", 0, &rec) == RZ_OK);
    size_t reqLen = 999;
    CHECK(RzJoin(&rec, nullptr, 0, &reqLen) == RZ_OK);
    CHECK(reqLen == 0);
    RzFree(&rec);

    // Defensive reuse without RzFree
    REQUIRE(RzSplit("test 123", 8, &rec) == RZ_OK);
    REQUIRE(RzSplit("second 456", 10, &rec) == RZ_OK);
    RzFree(&rec);

    // Probe RzTemplate with 0-capacity
    reqLen = 999;
    CHECK(RzTemplate("", 0, nullptr, 0, &reqLen) == RZ_OK);
    CHECK(reqLen == 0);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.\build\UnitTests.exe -tc="*capi: zero-length probe*"`
Expected: FAIL on `RzJoin(&rec, nullptr, 0, &reqLen) == RZ_OK` (returns `RZ_ERR_BUFFER_TOO_SMALL`).

- [ ] **Step 3: Implement C ABI fixes in `src/RazobillC.cpp` and `Slots.hpp`**

1. In `src/RazobillC.cpp`:
   - In `RzSplit`: if `outRecord->internalHandle != nullptr`, delete `static_cast<InternalRecord*>(outRecord->internalHandle)` before zeroing.
   - In `RzJoin`: use `thread_local` scratch buffers for `strViews` and `result` (calling `.clear()` but preserving capacity).
   - In `RzJoin`: when `reqLen == 0`, if `outBuffer == nullptr || bufferCapacity == 0`, set `*outLength = 0` and return `RZ_OK`.
   - In `RzTemplate`: use a `thread_local razobill::core::SlotRecord scratchSlots` to avoid heap allocations per call after warmup.
2. In `include/librazobill/Slots.hpp:64`:
   - Change `formKeyHash()` from `return xxh64(formKey());` to `return xxh64(templateStr);`.

- [ ] **Step 4: Run test to verify it passes**

Run: `.\build\UnitTests.exe -tc="*capi*"`
Expected: PASS. Run full `.\build\UnitTests.exe` to verify no regressions.

- [ ] **Step 5: Commit**

```bash
git add src/RazobillC.cpp include/librazobill/Slots.hpp tests/UnitCAPI.cpp
git commit -m "fix(cabi): eliminate heap allocations on hot path, support zero-length probes, and harden record reuse"
```

---

### Task 3: F3 — Core C++ Robustness & Hardening Sweep

**Files:**
- Modify: `src/Channel.cpp:87-93`
- Modify: `src/Razobill.cpp:91-95, 166-168`
- Modify: `include/librazobill/SimdScanner.hpp:74-85`
- Modify: `include/librazobill/MemoryArena.hpp:36, 77-80`
- Modify: `include/librazobill/FormCatalog.hpp:118-120`
- Modify: `tests/Support.hpp:10-15`
- Test: `tests/UnitArena.cpp`, `tests/UnitChannel.cpp`

**Interfaces:**
- Consumes: `failureProbabilityBound`, `requireIdentifier`, `detectSimdArchitecture`, `MemoryArena::allocate`, `FormCatalog::add`
- Produces: Safe NaN/negative handling, locale-independent ASCII validation, MSVC AVX-512 detection clamp, arena alignment assertions

- [ ] **Step 1: Write failing tests in `tests/UnitChannel.cpp` and `tests/UnitArena.cpp`**

In `tests/UnitChannel.cpp`:
```cpp
TEST_CASE("channel: NaN and extreme negative s handling") {
    ChannelCapacity cap{.meanCapacityBits = 100.0, .variance = 10.0};
    CHECK(failureProbabilityBound(cap, 50, std::numeric_limits<double>::quiet_NaN()) == 1.0);
    CHECK(failureProbabilityBound(cap, 50, -1e12) == 1.0);
}
```
In `tests/UnitArena.cpp`:
Add tests for non-power-of-two alignment assertions/handling and zero defaultBlockSize normalization.

- [ ] **Step 2: Run tests to verify failures**

Run: `.\build\UnitTests.exe -tc="*channel: NaN*"`
Expected: FAIL or UB.

- [ ] **Step 3: Implement fixes across Core C++ headers and sources**

1. `src/Channel.cpp:87-93`: Check `if (std::isnan(s) || s < 0.0) return 1.0;`. Clamp `s` to `[0.0, 100.0]` before `static_cast<int>`.
2. `src/Razobill.cpp:91-95`: Replace `std::isalpha` and `std::isalnum` with explicit lambda: `[](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }`.
3. `include/librazobill/SimdScanner.hpp:74-85`: On MSVC (`_MSC_VER`), cap runtime architecture detection at `SimdArchitecture::Avx2` because MSVC lacks the GCC/Clang per-function target attribute implementation.
4. `include/librazobill/MemoryArena.hpp`:
   - Normalize `defaultBlockSize`: `defaultBlockSize ? defaultBlockSize : 256 * 1024`.
   - Assert `alignment > 0 && (alignment & (alignment - 1)) == 0` for power-of-two alignment.
5. `include/librazobill/FormCatalog.hpp:119` & `src/Razobill.cpp:167`:
   - Throw `std::length_error` if size reaches `std::numeric_limits<std::uint32_t>::max()`.
6. `tests/Support.hpp`: Add `#include <array>`.

- [ ] **Step 4: Run test to verify it passes**

Run: `.\build\UnitTests.exe`
Expected: ALL 59 test cases PASS.

- [ ] **Step 5: Commit**

```bash
git add src/Channel.cpp src/Razobill.cpp include/librazobill/SimdScanner.hpp include/librazobill/MemoryArena.hpp include/librazobill/FormCatalog.hpp tests/Support.hpp tests/UnitChannel.cpp tests/UnitArena.cpp
git commit -m "fix(core): harden channel bound calculation, memory arena alignment, and locale-independent identifiers"
```

---

### Task 4: F4 & F5 — SQL Control Plane: TransactionGuard & Dialect Parity

**Files:**
- Modify: `src/Razobill.cpp:40-84`
- Modify: `src/Dialect.cpp:70-76, 122-148`
- Test: `tests/IntegrationDuckdb.cpp`, `tests/IntegrationChdb.cpp`

**Interfaces:**
- Consumes: `TransactionGuard`, `ClickHouseDialect::formExpr`, `ClickHouseDialect::templateExpr`, `DuckDialect::templateExpr`
- Produces: True ACID transactions for DuckDB; `<*>` display template normalization in `rb_form.template`

- [ ] **Step 1: Write failing transaction rollback and `<*>` template matching tests**

In `tests/IntegrationDuckdb.cpp`:
1. Verify that `rb_form.template` contains `<*>` instead of `{i}` and `{s}`, and that rules with `<*>` like `^Receiving block <\*> src:` match forms correctly.
2. Verify that an aborted query inside a transaction rolls back cleanly and leaves the DuckDB connection usable for subsequent queries.

- [ ] **Step 2: Run test to verify it fails**

Run: `.\build\IntegrationDuckdb.exe -tc="*meaning layer*"`
Expected: FAIL (`CHECK_THROWS_AS(..., GateRefused)` fails because rules never matched templates).

- [ ] **Step 3: Implement TransactionGuard activation and Dialect `<*>` template conversion**

1. In `src/Razobill.cpp:40-84`:
   In `TransactionGuard`:
   ```cpp
   explicit TransactionGuard(Backend& db RZ_LIFETIMEBOUND, Flavor flavor)
       : db_(db) {
       if (flavor == Flavor::DuckDB) {
           active_ = true;
           try {
               db_.exec("BEGIN TRANSACTION");
           } catch (...) {
               active_ = false;
               throw;
           }
       }
   }
   ```
2. In `src/Dialect.cpp`:
   - In `DuckDialect::templateExpr(std::string_view f)`:
     Convert `{i}` and `{s}` into `<*>`:
     `return "replaceAll(replaceAll(" + std::string(f) + ", '{i}', '<*>'), '{s}', '<*>')";`
   - In `ClickHouseDialect::formExpr`:
     ClickHouse `formExpr` produces the modern template (matching C++ `templateStr` output), identity format for both dialects.
   - In `ClickHouseDialect::templateExpr`:
     `return "replaceAll(replaceAll(" + std::string(f) + ", '{i}', '<*>'), '{s}', '<*>')";`

- [ ] **Step 4: Run test to verify it passes**

Run: `& "C:\Program Files\CMake\bin\cmake.exe" --build build --config Release --target IntegrationDuckdb`
Run: `.\build\IntegrationDuckdb.exe`
Expected: ALL test cases PASS.

- [ ] **Step 5: Commit**

```bash
git add src/Razobill.cpp src/Dialect.cpp tests/IntegrationDuckdb.cpp
git commit -m "fix(sql): activate DuckDB TransactionGuard and unify templateExpr to <*> across dialects"
```

---

### Task 5: F6 — BackendAdbc Driver Hardening

**Files:**
- Modify: `src/BackendAdbc.cpp:100-196, 285-315, 388-420`
- Modify: `include/librazobill/Backend.hpp:38-51`
- Test: `tests/IntegrationDuckdb.cpp`

**Interfaces:**
- Consumes: `ArrowArrayStream`, `AdbcStatement`, `Table`
- Produces: Exception-safe Arrow stream release, checked buffer offsets, unified `BackendError`

- [ ] **Step 1: Write test case for error propagation and resource cleanup in ADBC queries**

Add a test in `IntegrationDuckdb.cpp` asserting that malformed SQL statements throw `BackendError` and do not leak driver statement/stream handles.

- [ ] **Step 2: Run test to verify behavior**

Run: `.\build\IntegrationDuckdb.exe`

- [ ] **Step 3: Implement ADBC driver hardening in `src/BackendAdbc.cpp`**

1. Stream lifecycle guard:
   Define `StreamGuard` *before* `driver_.StatementExecuteQuery(&st, &stream, &rows, &err)`, ensuring that if execute returns an error after partially initializing `stream`, `stream.release(&stream)` is safely invoked.
2. Buffer bounds validation:
   In `decode`: check that `a->offset >= 0`, `a->length >= 0`, and string/binary offsets `off[i] >= 0 && off[i+1] >= off[i] && off[i+1] <= buffer_size`.
3. Little-endian check:
   Add `static_assert(std::endian::native == std::endian::little, "librazobill ADBC decoder requires little-endian architecture");`.
4. Unify errors:
   In `Table` accessors (`i64`, `u64`, `f64`, `str`, `isNull`), catch `std::out_of_range` and rethrow as `BackendError("table cell index out of range")`.

- [ ] **Step 4: Run test to verify it passes**

Run: `.\build\IntegrationDuckdb.exe`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/BackendAdbc.cpp include/librazobill/Backend.hpp
git commit -m "fix(adbc): guard Arrow stream on execution failure, validate buffer offsets, and unify table errors"
```

---

### Task 6: F7 — Python Bindings Fidelity & GIL Release

**Files:**
- Modify: `lib/python/PyRazobill.cpp:135-160, 225-250, 290-310`
- Modify: `lib/python/MANIFEST.in`
- Modify: `lib/python/pyrazobill/__init__.py`
- Test: `lib/python/tests/TestPyRazobill.py`

**Interfaces:**
- Consumes: `splitOne`, `joinOne`, `templateOne`
- Produces: GIL-released native execution, clean packaging without dead files

- [ ] **Step 1: Write multi-threaded concurrency test in `lib/python/tests/TestPyRazobill.py`**

Add a test that runs `splitOne` and `joinOne` concurrently across multiple Python threads and measures real parallel execution without GIL starvation.

- [ ] **Step 2: Run test to verify behavior**

Run: `python -m unittest lib/python/tests/TestPyRazobill.py`

- [ ] **Step 3: Implement GIL release and clean packaging in `lib/python/`**

1. In `lib/python/PyRazobill.cpp`:
   Wrap pure C++ parsing and reconstruction in `splitOne`, `joinOne`, and `templateOne` with `py::gil_scoped_release release;` (reacquiring before interacting with Python objects).
2. Fix docstrings/comments referring to "OpenMP" to say "custom worker pool".
3. In `lib/python/pyrazobill/__init__.py`:
   Scope Windows `os.add_dll_directory` to the package directory rather than iterating every entry in `PATH`.
4. In `lib/python/MANIFEST.in`:
   Remove reference to stale `Guide.md`.

- [ ] **Step 4: Run test to verify it passes**

Run: `python -m unittest discover -s lib/python/tests`
Run: `python -m pytest lib/python/tests/TestProperties.py`
Expected: ALL PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/python/PyRazobill.cpp lib/python/MANIFEST.in lib/python/pyrazobill/__init__.py lib/python/tests/TestPyRazobill.py
git commit -m "fix(python): release GIL around C++ hot path, scope DLL directory loading, and clean packaging"
```

---

### Task 7: F8 — Rust Vector VRL Crate Losslessness & C ABI Linking

**Files:**
- Modify: `ext/vector/build.rs`
- Modify: `ext/vector/RazobillVrl.rs`
- Modify: `ext/vector/Cargo.toml`
- Modify: `ext/vector/README.md`
- Test: `ext/vector/tests/TestVectorVrl.rs`

**Interfaces:**
- Consumes: `razobill_cabi` shared library (`RzSplit`, `RzJoin`, `RzFree`, `RzStatusMessage`)
- Produces: `razobill-vector` v2.0.0 crate with lossless byte slices

- [ ] **Step 1: Reproduce `cargo test` failure in `ext/vector`**

Run: `cargo test --manifest-path ext/vector/Cargo.toml`
Verify: `STATUS_ENTRYPOINT_NOT_FOUND` because `build.rs` links against DuckDB extension `razobill.dll` instead of `razobill_cabi`.

- [ ] **Step 2: Fix `build.rs` to link `razobill_cabi`**

In `ext/vector/build.rs`:
Change link target to `razobill_cabi`:
`println!("cargo:rustc-link-lib=dylib=razobill_cabi");`
Copy `razobill_cabi.dll` (and `.so`/`.dylib`) to target profile directory and deps directory.

- [ ] **Step 3: Implement lossless byte support and error handling in `RazobillVrl.rs`**

1. Store templates and variable slots as `Vec<u8>` (lossless byte preservation).
2. Call `RzStatusMessage(status)` to format error strings on failure.
3. Replace `#[repr(C)]` enum match with integer constant comparisons against `RZ_OK`, `RZ_ERR_*` to avoid undefined behavior if unknown status codes arrive.
4. In `Cargo.toml`: Bump version to `2.0.0` (breaking API change from `String` to `Vec<u8>`).
5. In `ext/vector/README.md`: Update documentation to reflect byte-level API and v2.0.0.

- [ ] **Step 4: Run test to verify it passes**

Run: `cargo test --manifest-path ext/vector/Cargo.toml`
Expected: PASS with 0 errors.

- [ ] **Step 5: Commit**

```bash
git add ext/vector/build.rs ext/vector/RazobillVrl.rs ext/vector/Cargo.toml ext/vector/README.md
git commit -m "fix(rust): link razobill_cabi, restore byte losslessness, and bump razobill-vector to 2.0.0"
```

---

### Task 8: F9 — OpenTelemetry Go Processor Robustness

**Files:**
- Modify: `ext/otel/RazobillProcessor.go`
- Modify: `ext/otel/Config.go`
- Modify: `ext/otel/BuilderConfig.yaml`
- Test: `ext/otel/tests/RazobillProcessor_test.go`

**Interfaces:**
- Consumes: `razobill_cabi`, `plog.LogRecord`
- Produces: Byte-body support, honest configuration validation, panic-safe C resource cleanup

- [ ] **Step 1: Write test for byte-type log bodies and invalid scan modes**

In `ext/otel/tests/RazobillProcessor_test.go`:
Add test cases for `plog.ValueTypeBytes` log bodies and assert that `cfg.Validate()` returns an error if mode is not `"semantic"`.

- [ ] **Step 2: Run test to verify it fails**

Run: `go test ./ext/otel/...` (if Go installed)

- [ ] **Step 3: Implement Go processor fixes**

1. In `ext/otel/RazobillProcessor.go`:
   - Change `#cgo LDFLAGS` to link `-lrazobill_cabi`.
   - Scope `-lstdc++` to linux via `#cgo linux LDFLAGS: -lstdc++`.
   - Support both `rec.Body().Type() == plog.ValueTypeStr` and `plog.ValueTypeBytes`.
   - Wrap `C.free(unsafe.Pointer(cMsg))` and `C.RzFree(&cRecord)` in `defer` statements to guarantee panic safety.
2. In `ext/otel/Config.go`:
   - `Validate()`: return `errors.New("mode must be 'semantic'")` if `cfg.Mode != "semantic"`, since C ABI operates strictly in semantic mode.
3. In `ext/otel/BuilderConfig.yaml`:
   - Update tag version from `v1.0.0` to current repository release tag `v1.1.5`.

- [ ] **Step 4: Run test to verify it passes**

Run: `go test -v ./ext/otel/...`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add ext/otel/RazobillProcessor.go ext/otel/Config.go ext/otel/BuilderConfig.yaml ext/otel/tests/RazobillProcessor_test.go
git commit -m "fix(otel): link razobill_cabi, handle byte bodies, enforce honest config, and ensure defer cleanup"
```

---

### Task 9: F10 & F11 — Build System & CI Pipelines

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `librazobill.pc.in`
- Modify: `tools/AppendExtensionMetadata.py`
- Modify: `.github/workflows/ci.yml` (and related workflows)

**Interfaces:**
- Consumes: CMake build targets, GitHub Actions CI
- Produces: Single-sourced DuckDB version, accurate pkg-config file, comprehensive multi-language CI

- [ ] **Step 1: Audit and verify CMake targets and pkg-config file**

Check `librazobill.pc.in` for `Libs: -lrazobill`. Verify target name is `razobill_cabi`.

- [ ] **Step 2: Update `CMakeLists.txt` and `AppendExtensionMetadata.py`**

1. In `CMakeLists.txt`:
   - Single-source the DuckDB target version: `set(DUCKDB_TARGET_VERSION "v2.0.0" CACHE STRING "Target DuckDB ABI version")`.
   - Pass `-dv ${DUCKDB_TARGET_VERSION}` to `AppendExtensionMetadata.py`.
   - Ensure `librazobill.pc.in` uses `-lrazobill_cabi` for shared library linking.
   - Cleanly separate `LIBRAZOBILL_STATIC` on `librazobill` and `LIBRAZOBILL_EXPORTS` on `razobill_cabi`.
2. In `tools/AppendExtensionMetadata.py`:
   - Set default DuckDB version to `v2.0.0` to match vendored headers.
3. In `.github/workflows/`:
   - Add Go test step and Rust cargo test step.
   - Set `RZB_REQUIRE_NATIVE=1` for Python build & test.
   - Add dependabot monitoring for `gomod` and `cargo`.

- [ ] **Step 3: Test CMake build across all targets**

Run:
```powershell
& "C:\Program Files\CMake\bin\cmake.exe" --build build --config Release
```
Expected: All targets (`librazobill`, `razobill_cabi`, `RazobillDuckdb`, `UnitTests`, `IntegrationDuckdb`) build cleanly.

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt librazobill.pc.in tools/AppendExtensionMetadata.py .github/
git commit -m "build(ci): single-source DuckDB v2.0.0, fix pkg-config library names, and gate Rust/Go in CI"
```

---

### Task 10: F12 & F13 — Documentation Honesty, Benchmark Accuracy & Final Deliverable

**Files:**
- Modify: `docs/how-to/cpp-developer.md`
- Modify: `docs/reference/cpp-api.md`
- Modify: `docs/explanation/benchmarks.md`
- Modify: `README.md`
- Modify: `CHANGELOG.md`
- Modify: `bench/s3/BenchDrain3VsRazobillS3.py`
- Modify: `tools/BenchScale.py`
- Modify: `tools/LintDocs.py`
- Modify: `.coderabbit.yaml`
- Create: `AUDIT.md`

**Interfaces:**
- Consumes: Real measured test and benchmark receipts
- Produces: Markdownlint-clean, honest documentation and the final audit deliverable `AUDIT.md`

- [ ] **Step 1: Correct C++ guide in `docs/how-to/cpp-developer.md`**

Replace non-existent APIs (`splitSlots`, 9-argument `RzJoin`, `templateString`, `intSlots`, `strSlots`) with actual C++20 and C ABI signatures:
- C++20: `core::SlotRecord slots; core::parse(line, slots); core::reconstruct(slots.templateStr, slots.ints, slots.strs, out);`
- C99: `RzRecord rec; RzSplit(msg, len, &rec); RzJoin(&rec, out_buf, sizeof(out_buf), &out_len); RzFree(&rec);`

- [ ] **Step 2: Correct benchmark scripts and documentation receipts**

1. In `bench/s3/BenchDrain3VsRazobillS3.py:265`: Remove the fabricated `69400000.0` constant; fail loudly with `RuntimeError` if native benchmark is requested but unavailable.
2. In `tools/BenchScale.py`: Update to use `pyrazobill.RzSplit` / worker pool batching instead of non-existent `auditParallel`.
3. In `docs/reference/cpp-api.md`: Complete documentation for `Channel`, `Razobill`, `Backend`, `Dialect`, `FormCatalog`, `MemoryArena`, `StringPool`.
4. In `CHANGELOG.md`: Document missing 1.1.0 release and add complete 1.1.5 entry.
5. In `.coderabbit.yaml`: Fix `RzFreeRecord` -> `RzFree`.

- [ ] **Step 3: Create `AUDIT.md` at repository root**

Draft `AUDIT.md` documenting:
- Every finding F1 through F13 (plus F14 dialect template fix) with file:line citations.
- Root cause analysis, implemented fix, and verification commands.
- Verified test receipts (actual test counts and assertion counts).
- Deliberate non-changes with engineering rationale.

- [ ] **Step 4: Verify full test and documentation suite**

Run:
```powershell
& "C:\Program Files\CMake\bin\cmake.exe" --build build --config Release
.\build\UnitTests.exe
.\build\IntegrationDuckdb.exe
python -m unittest discover -s lib/python/tests
python -m pytest lib/python/tests/TestProperties.py
npx markdownlint-cli2 "**/*.md"
python tools/LintDocs.py README.md CONTRIBUTING.md docs/
python tools/VerifyMarkdownLinks.py
python tools/CheckSurfaceC.py
python tools/CheckSurfacePython.py
python tools/CheckSurfaceDuckdb.py
```
Expected: 100% SUCCESS across all linters, checkers, unit tests, and integration tests.

- [ ] **Step 5: Commit**

```bash
git add docs/ README.md CHANGELOG.md bench/ tools/ .coderabbit.yaml AUDIT.md
git commit -m "docs(audit): publish AUDIT.md, align C++ developer guide to real ABI, and purge fabricated numbers"
```
