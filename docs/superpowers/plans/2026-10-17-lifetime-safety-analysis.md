# Clang lifetime safety analysis implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Integrate Clang Lifetime Safety Analysis into Razobill, annotate non-owning views and owner classes with portable lifetime contracts (`[[clang::lifetimebound]]`, `[[gsl::Owner]]`, `[[gsl::Pointer]]`), resolve all compiler diagnostics and suggestions across the codebase, and verify zero regressions against core invariants.

**Architecture:** Introduce portable lifetime macro abstractions (`RZ_LIFETIMEBOUND`, `RZ_GSL_POINTER`, `RZ_GSL_OWNER`, `RZ_NOESCAPE`) gated via `__has_cpp_attribute` in a core header. Apply lifetime contracts across public headers (`StringPool.hpp`, `Scanner.hpp`, `Razobill.hpp`, `MemoryArena.hpp`), private implementation guards (`BackendAdbc.cpp`, `Razobill.cpp`, `DuckdbExtension.cpp`), and test harnesses (`tests/Support.hpp`). Integrate a `-DRAZOBILL_ENABLE_LIFETIME_SAFETY=ON` CMake build flag and add negative/positive lifetime unit test validations.

**Tech Stack:** C++20, Clang 23 (`-Wlifetime-safety`, `-Wlifetime-safety-suggestions`, `-Wlifetime-safety-validations`), CMake 4.x, Ninja, Doctest.

**Spec:** [LLVM Clang Lifetime Safety Analysis Documentation](https://clang.llvm.org/docs/LifetimeSafety.html)

## Global Constraints

- **Exact Lossless Invertibility:** `join(split(x)) == x` must hold byte-for-byte across all valid inputs.
- **Zero Heap Allocation on Hot Path:** Never introduce heap allocations or container resizes in per-message loops.
- **Public ABI Stability:** Maintain universal C ABI (`RazobillC.h`) and DuckDB loadable extension signatures.
- **Portability:** Macro annotations must compile cleanly across Clang, GCC (11+), and MSVC (19.29+) with zero unrecognized attribute warnings.
- **Untouchable Paths:** Never modify `LICENSE`, `NOTICE`, `thirdparty/`, `extension-ci-tools/`, `logs/`, or raw files in `evidence/`.

## Review Focus

- Temporary object lifetime binding: `StringPool().lookup(id)` must be flagged if assigned to a persisting `std::string_view`.
- Sub-object loan tracking: `RazobillEngine` constructor taking `Backend&` must bind the engine instance lifetime to the backend lifetime.
- View struct classification: `ScanResult` must be annotated as `[[gsl::Pointer]]` borrowing from the input `std::string_view`.
- Intra-TU and cross-TU suggestion resolution: All suggestions reported by `-Wlifetime-safety-suggestions` must be resolved.
- Safe buffer mutation: Ensure `-Wlifetime-safety-invalidation` false positives are documented and addressed with idiomatic buffer clearing patterns.

---

### Task 1: Core lifetime safety macro abstractions

**Files:**

- Create: `include/librazobill/Lifetime.hpp`
- Modify: `include/librazobill/Version.hpp:1-30`
- Test: `tests/UnitLifetime.cpp`

**Interfaces:**

- Consumes: C++20 standard feature test macros `__has_cpp_attribute`.
- Produces: `RZ_LIFETIMEBOUND`, `RZ_GSL_POINTER`, `RZ_GSL_OWNER`, `RZ_NOESCAPE` macro definitions.

- [ ] **Step 1: Write the failing test in `tests/UnitLifetime.cpp`**

```cpp
#include "doctest.h"
#include "librazobill/Lifetime.hpp"
#include <string>
#include <string_view>

namespace {
struct RZ_GSL_POINTER TestView {
    std::string_view sv;
};

struct RZ_GSL_OWNER TestOwner {
    std::string data;
    std::string_view view() const RZ_LIFETIMEBOUND { return data; }
};

TestView createView(std::string_view s RZ_LIFETIMEBOUND) {
    return TestView{s};
}
} // namespace

TEST_CASE("lifetime annotations compile cleanly and preserve semantics") {
    TestOwner owner{"hello world"};
    std::string_view v = owner.view();
    CHECK(v == "hello world");

    TestView tv = createView(owner.data);
    CHECK(tv.sv == "hello world");
}
```

- [ ] **Step 2: Run test compilation to verify it fails**

Run: `clang++ -std=c++20 -fsyntax-only -Iinclude -isystem thirdparty tests/UnitLifetime.cpp`
Expected: FAIL with "librazobill/Lifetime.hpp file not found"

- [ ] **Step 3: Implement `include/librazobill/Lifetime.hpp`**

Implement `Lifetime.hpp` with standard `__has_cpp_attribute` guards for `clang::lifetimebound`, `gsl::Pointer`, `gsl::Owner`, and `clang::noescape`. Include `Lifetime.hpp` in `include/librazobill/Version.hpp` and project headers.

- [ ] **Step 4: Run test compilation to verify it passes**

Run: `clang++ -std=c++20 -fsyntax-only -Wlifetime-safety -Wlifetime-safety-validations -Iinclude -isystem thirdparty tests/UnitLifetime.cpp`
Expected: PASS with 0 errors and 0 warnings.

- [ ] **Step 5: Commit**

```bash
git add include/librazobill/Lifetime.hpp include/librazobill/Version.hpp tests/UnitLifetime.cpp
git commit -m "feat(core): add portable Clang lifetime safety macros"
```

---

### Task 2: Annotate public view and owner headers (`StringPool`, `Scanner`, `MemoryArena`, `Razobill`)

**Files:**

- Modify: `include/librazobill/StringPool.hpp:55-125`
- Modify: `include/librazobill/Scanner.hpp:90-120`
- Modify: `include/librazobill/MemoryArena.hpp:20-80`
- Modify: `include/librazobill/Razobill.hpp:85-105`
- Test: `tests/UnitLifetime.cpp`

**Interfaces:**

- Consumes: `Lifetime.hpp` (`RZ_LIFETIMEBOUND`, `RZ_GSL_POINTER`, `RZ_GSL_OWNER`).
- Produces: Annotated `StringPool::lookup`, `ScanResult`, `Scanner::scan`, `MemoryArena`, and `RazobillEngine::RazobillEngine`.

- [ ] **Step 1: Write test exercising annotated methods**

Extend `tests/UnitLifetime.cpp` to verify that `StringPool::lookup` returns a valid view bound to the pool instance, and `Scanner::scan` produces a `ScanResult` bound to the input string view.

- [ ] **Step 2: Apply `RZ_GSL_OWNER` and `RZ_LIFETIMEBOUND` to `include/librazobill/StringPool.hpp`**

Add `RZ_GSL_OWNER` to `class StringPool`.
Add `RZ_LIFETIMEBOUND` to `std::string_view lookup(std::uint32_t id) const RZ_LIFETIMEBOUND`.

- [ ] **Step 3: Apply `RZ_GSL_POINTER` and `RZ_LIFETIMEBOUND` to `include/librazobill/Scanner.hpp`**

Add `RZ_GSL_POINTER` to `struct ScanResult`.
Add `RZ_LIFETIMEBOUND` to `std::string_view m RZ_LIFETIMEBOUND` in `scan()`.

- [ ] **Step 4: Apply `RZ_GSL_OWNER` to `include/librazobill/MemoryArena.hpp`**

Add `RZ_GSL_OWNER` to `class MemoryArena`.
Add `RZ_LIFETIMEBOUND` to `allocate(std::size_t bytes, std::size_t alignment = 8) RZ_LIFETIMEBOUND`.

- [ ] **Step 5: Apply `RZ_LIFETIMEBOUND` to `include/librazobill/Razobill.hpp`**

Annotate `RazobillEngine(Backend& backend RZ_LIFETIMEBOUND, Config config);`.

- [ ] **Step 6: Run Clang lifetime analysis on modified headers**

Run: `clang++ -std=c++20 -fsyntax-only -Wlifetime-safety -Wlifetime-safety-suggestions -Wlifetime-safety-validations -DLIBRAZOBILL_STATIC -DLIBRAZOBILL_EXPORTS -Iinclude -isystem thirdparty tests/UnitLifetime.cpp`
Expected: PASS with 0 lifetime warnings.

- [ ] **Step 7: Commit**

```bash
git add include/librazobill/StringPool.hpp include/librazobill/Scanner.hpp include/librazobill/MemoryArena.hpp include/librazobill/Razobill.hpp tests/UnitLifetime.cpp
git commit -m "feat(core): apply lifetime annotations to StringPool, Scanner, MemoryArena, and RazobillEngine"
```

---

### Task 3: Resolve implementation diagnostics in `src/Razobill.cpp` and `src/BackendAdbc.cpp`

**Files:**

- Modify: `src/Razobill.cpp:40-85, 160-180`
- Modify: `src/BackendAdbc.cpp:275-350`
- Test: `tests/UnitCore.cpp`

**Interfaces:**

- Consumes: `Lifetime.hpp`.
- Produces: Clean compilation under `-Wlifetime-safety -Wlifetime-safety-suggestions`.

- [ ] **Step 1: Check baseline warnings in `src/Razobill.cpp` and `src/BackendAdbc.cpp`**

Run: `clang++ -std=c++20 -fsyntax-only -Wlifetime-safety -Wlifetime-safety-suggestions -DLIBRAZOBILL_STATIC -DLIBRAZOBILL_EXPORTS -Iinclude -isystem thirdparty -isystem thirdparty/adbc -isystem thirdparty/duckdbcapi src/Razobill.cpp src/BackendAdbc.cpp`
Expected: Emits constructor suggestions for `TransactionGuard`, `Guard`, `StreamGuard`, `SchemaGuard`, `BatchGuard`, and `map()`.

- [ ] **Step 2: Update `src/Razobill.cpp`**

Mark `TransactionGuard(Backend& db RZ_LIFETIMEBOUND, Flavor flavor)`.
Mark `const std::unordered_map<std::string, std::uint32_t>& map() const RZ_LIFETIMEBOUND`.

- [ ] **Step 3: Update `src/BackendAdbc.cpp`**

Mark constructor parameters in `Guard`, `StreamGuard`, `SchemaGuard`, and `BatchGuard` with `RZ_LIFETIMEBOUND`.

- [ ] **Step 4: Verify warnings are eliminated**

Run: `clang++ -std=c++20 -fsyntax-only -Wlifetime-safety -Wlifetime-safety-suggestions -DLIBRAZOBILL_STATIC -DLIBRAZOBILL_EXPORTS -Iinclude -isystem thirdparty -isystem thirdparty/adbc -isystem thirdparty/duckdbcapi src/Razobill.cpp src/BackendAdbc.cpp`
Expected: 0 lifetime warnings.

- [ ] **Step 5: Commit**

```bash
git add src/Razobill.cpp src/BackendAdbc.cpp
git commit -m "fix(engine): resolve lifetimebound suggestions in Razobill and BackendAdbc"
```

---

### Task 4: Resolve extension, test, and benchmark lifetime annotations

**Files:**

- Modify: `ext/duckdb/DuckdbExtension.cpp:455-475`
- Modify: `tests/Support.hpp:18-30`
- Test: `tests/IntegrationDuckdb.cpp`, `bench/BenchStorage.cpp`

**Interfaces:**

- Consumes: `Lifetime.hpp`.
- Produces: Clean compilation of DuckDB extension and test/benchmark support headers.

- [ ] **Step 1: Update `ext/duckdb/DuckdbExtension.cpp`**

Annotate `explicit FunctionSet(const char* name RZ_LIFETIMEBOUND)` in `DuckdbExtension.cpp`.

- [ ] **Step 2: Update `tests/Support.hpp`**

Annotate `inline const char* env(const char* k, const char* d RZ_LIFETIMEBOUND)`.
Add `#ifndef _CRT_SECURE_NO_WARNINGS \n #define _CRT_SECURE_NO_WARNINGS \n #endif` to silence MSVC deprecated standard library notices.

- [ ] **Step 3: Verify with Clang Lifetime Safety Analysis**

Run:

```bash
clang++ -std=c++20 -fsyntax-only -Wlifetime-safety -Wlifetime-safety-suggestions -DLIBRAZOBILL_STATIC -DLIBRAZOBILL_EXPORTS -DDUCKDB_EXTENSION_NAME=razobill -Iinclude -Itests -isystem thirdparty -isystem thirdparty/duckdbcapi ext/duckdb/DuckdbExtension.cpp tests/IntegrationDuckdb.cpp bench/BenchStorage.cpp
```

Expected: 0 warnings.

- [ ] **Step 4: Commit**

```bash
git add ext/duckdb/DuckdbExtension.cpp tests/Support.hpp
git commit -m "fix(ext): resolve lifetime annotations in DuckdbExtension and Support.hpp"
```

---

### Task 5: CMake option and quality gate integration

**Files:**

- Modify: `CMakeLists.txt:40-85`
- Test: Build configuration with `-DRAZOBILL_ENABLE_LIFETIME_SAFETY=ON`

**Interfaces:**

- Consumes: CMake 3.20+ `CMAKE_CXX_COMPILER_ID MATCHES "Clang"`.
- Produces: `RAZOBILL_ENABLE_LIFETIME_SAFETY` option adding `-Wlifetime-safety`, `-Wlifetime-safety-suggestions`, and `-Wlifetime-safety-validations`.

- [ ] **Step 1: Add CMake option in `CMakeLists.txt`**

```cmake
option(RAZOBILL_ENABLE_LIFETIME_SAFETY "Enable Clang Lifetime Safety Analysis (-Wlifetime-safety)" OFF)
if(RAZOBILL_ENABLE_LIFETIME_SAFETY)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wlifetime-safety -Wlifetime-safety-suggestions -Wlifetime-safety-validations)
  else()
    message(WARNING "RAZOBILL_ENABLE_LIFETIME_SAFETY requires Clang; current compiler is ${CMAKE_CXX_COMPILER_ID}")
  endif()
endif()
```

- [ ] **Step 2: Add `tests/UnitLifetime.cpp` to UnitTests target in `CMakeLists.txt`**

Ensure `tests/UnitLifetime.cpp` is linked into `UnitTests`.

- [ ] **Step 3: Build and run test suites**

Run:

```bash
cmake --build build --config Release
./build/UnitTests.exe
./build/IntegrationDuckdb.exe
```

Expected: 100% test pass with 0 failures.

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt
git commit -m "build(cmake): add RAZOBILL_ENABLE_LIFETIME_SAFETY configuration option"
```

---

### Task 6: Documentation and verification linters

**Files:**

- Create: `docs/explanation/lifetime-safety.md`
- Modify: `docs/reference/architecture.md` (or relevant reference docs)

**Interfaces:**

- Consumes: Diataxis documentation suite.
- Produces: Exhaustive explanation of Clang Lifetime Safety, the Polonius model, Razobill's memory architecture, and how to verify code.

- [ ] **Step 1: Write `docs/explanation/lifetime-safety.md`**

Explain the Polonius borrow checker inspiration, Owner vs Pointer types, lifetime bounding on zero-allocation accessors, and how to build with `-DRAZOBILL_ENABLE_LIFETIME_SAFETY=ON`.

- [ ] **Step 2: Run documentation linters per `AGENTS.md`**

Run:

```bash
npx markdownlint-cli2 "**/*.md"
python tools/LintDocs.py README.md CONTRIBUTING.md docs/
python tools/VerifyMarkdownLinks.py
```

Expected: 0 errors across all linters.

- [ ] **Step 3: Commit**

```bash
git add docs/
git commit -m "docs: add Clang Lifetime Safety Analysis architecture explanation"
```
