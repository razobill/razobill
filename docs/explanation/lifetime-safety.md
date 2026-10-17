# Lifetime safety analysis

[Docs](../README.md) > [Explanations](../README.md#4-explanations-and-architecture-understand) > Lifetime safety analysis

How Razobill uses Clang lifetime safety analysis, GSL contracts, and borrow checker annotations to statically eliminate dangling pointers, use-after-free, and view invalidations without runtime overhead.
Read this article if you want to understand compile-time lifetime analysis in modern C++, how Razobill enforces zero-allocation buffer safety, or how to enable lifetime checks in custom builds.
Skip this article if you consume Razobill through Python or DuckDB without modifying the C++ engine.

---

## The zero-allocation safety challenge

Razobill's primary performance invariant requires zero heap allocation on the hot path.
To parse and reconstruct millions of log lines per second on a single core, the engine avoids `std::string` allocations by relying heavily on non-owning string views:

- [`std::string_view`](../reference/c-abi.md) spans representing template prefixes, delimiters, and string slot values.
- Monotonic arenas ([`MemoryArena.hpp`](../../include/librazobill/MemoryArena.hpp)) providing fast bump-allocated scratchpads for parsed tokens.
- Non-owning token records ([`Scanner.hpp`](../../include/librazobill/Scanner.hpp)) referencing transient line buffers.

Non-owning views introduce a classic memory safety risk: dangling references.
If a view outlives the underlying buffer, or if a buffer is modified while a view is active, undefined behavior occurs.
Runtime sanitizers (such as AddressSanitizer) detect invalid accesses only if a test case triggers that exact execution path.
Razobill enforces lifetime correctness at compile time using static lifetime analysis.

---

## Borrow checking model in C++20

Starting with Clang 20, LLVM introduces static lifetime safety analysis based on Herb Sutter's Lifetime profile (P1179) and modern borrow checking principles (such as Polonius).
The analysis constructs a function-level control-flow graph (CFG) tracking the points-to set and origin of every pointer, reference, and view.

The analysis validates three core rules:

1. **Origin tracking**: Every non-owning view (`[[gsl::Pointer]]`) must point to an active owner (`[[gsl::Owner]]`) or parameter with an explicit lifetime contract.
2. **Lifetime bounds**: A function returning a view must declare which input parameters govern the returned lifetime via `[[clang::lifetimebound]]`.
3. **Borrow invalidation**: When an underlying owner is mutated, all active borrows to that owner are invalidated within the local control-flow graph.

---

## Portable macro contracts in Lifetime.hpp

Because Razobill compiles across GCC, Clang, and MSVC, lifetime annotations are defined via feature-tested preprocessor macros in [`include/librazobill/Lifetime.hpp`](../../include/librazobill/Lifetime.hpp):

| Macro | Attribute | Purpose |
| --- | --- | --- |
| `RZ_LIFETIMEBOUND` | `[[clang::lifetimebound]]` | Specifies that a returned reference or view is bounded by a parameter or `this`. |
| `RZ_GSL_OWNER` | `[[gsl::Owner]]` | Declares that a class owns memory resources (e.g. `MemoryArena`, `StringPool`). |
| `RZ_GSL_POINTER` | `[[gsl::Pointer]]` | Declares that a struct or class is a non-owning view (e.g. `ScanResult`). |
| `RZ_NOESCAPE` | `[[clang::noescape]]` | Asserts that a pointer or view argument does not escape past the function invocation. |

When compiled with toolchains that do not support Clang lifetime attributes (such as GCC or older MSVC releases), the macros expand to empty definitions, ensuring full cross-platform compatibility.

---

## Applying lifetime annotations in Razobill

Lifetime attributes are applied across critical engine abstractions:

### 1. View and owner types

Types that own storage are marked with `RZ_GSL_OWNER`:

```cpp
class RZ_GSL_OWNER MemoryArena {
    // Manages underlying chunk allocations
};

class RZ_GSL_OWNER StringPool {
    // Owns interned string storage
};
```

Types that act as views are marked with `RZ_GSL_POINTER`:

```cpp
struct RZ_GSL_POINTER ScanResult {
    const char* tail{nullptr};
    size_t length{0};
    uint32_t slot_id{0};
};
```

### 2. Method return bounds

When a method yields a view derived from its container, `RZ_LIFETIMEBOUND` ensures the returned view cannot outlive the container:

```cpp
class StringPool {
public:
    std::string_view lookup(uint32_t id) const RZ_LIFETIMEBOUND;
};
```

If user code attempts to assign the result of `lookup` from a temporary `StringPool`, Clang emits a compile-time warning:

```cpp
// Statically caught by Clang lifetime analysis:
std::string_view bad = StringPool().lookup(42); // Warning: temporary destroyed
```

### 3. Engine dependencies and RAII guards

Engine constructors bind their references to prevent unanchored references:

```cpp
RazobillEngine(Backend& backend RZ_LIFETIMEBOUND, Config config);
```

Similarly, RAII guards such as `TransactionGuard` and ADBC resource wrappers bind their reference to the parent engine or batch to prevent dangling transaction handles.

---

## Resolving false positives and mutation boundaries

Clang's experimental lifetime analysis tracks mutations and invalidations strictly within local scopes.
During integration, two patterns required refactoring to satisfy static verification:

### 1. Allocation pointers across reinterpretation

In `MemoryArena::allocate`, pointers are computed via integer arithmetic on bump offsets:

```cpp
void* ptr = reinterpret_cast<void*>(current_chunk_->data + offset_);
```

Because Clang cannot intra-procedurally verify that raw integer casting originates from `this`, placing `RZ_LIFETIMEBOUND` on the return type of `allocate` triggers `-Wlifetime-safety-validations`.
The class itself is designated as `RZ_GSL_OWNER`, while arena allocations remain governed by arena scope.

### 2. Lambda capture and container mutation

In batch ingestion loops, passing a container by reference into a local closure (`flush`) while clearing or mutating that container in the surrounding loop triggered false invalidation warnings under `-Wlifetime-safety-invalidation`.
The closure was refactored to take the container as an explicit parameter:

```cpp
// Avoid capturing mutable container across loop iterations:
auto flush = [](Backend& b, std::string& vals) {
    b.insert_batch(vals);
    vals.clear();
};

flush(backend, values);
```

Explicit argument passing isolates the mutation boundary within the CFG, proving to the borrow checker that no concurrent alias exists.

---

## Continuous verification with CMake and CI

Static lifetime analysis can be enabled during local builds with the `ENABLE_LIFETIME_SAFETY` CMake flag:

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_LIFETIME_SAFETY=ON
cmake --build build
```

When enabled on Clang compilers, CMake activates:

- `-Wlifetime-safety`: Reports dangling pointers and views outliving owners.
- `-Wlifetime-safety-suggestions`: Suggests missing lifetimebound annotations.
- `-Wlifetime-safety-validations`: Validates existing lifetimebound annotations for mathematical consistency.

In automated CI ([`.github/workflows/lifetime-safety.yml`](../../.github/workflows/lifetime-safety.yml)), every pull request is checked with Clang 20.
The workflow also runs diagnostic verification tests ([`tests/UnitLifetime.cpp`](../../tests/UnitLifetime.cpp)) ensuring that improper view retention produces compile errors.

---

## Related

- [Architecture overview](architecture.md)
- [Data-oriented design](data-oriented-design.md)
- [Universal C ABI reference](../reference/c-abi.md)
