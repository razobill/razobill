# C++20 engine reference

[Docs](../README.md) / [Reference](cpp-api.md) / C++ API

This document specifies the internal C++20 classes, headers, and core interfaces in `librazobill`.

- **Who this is for**: C++ developers contributing to the core engine or embedding native classes.
- **What you will have at the end**: Precise header organization, struct definitions, status codes, and memory allocators.

---

## Header overview

The C++20 engine is declared in `include/librazobill/`:

| Header | Primary role |
| :--- | :--- |
| `Version.hpp` | Version macros (`LIBRAZOBILL_VERSION_*`), ABI constants, and `VersionInfo` |
| `Status.hpp` | Error enumeration (`Status::Ok`, `Status::TooLarge`, `Status::Malformed`) |
| `Slots.hpp` | Data structures: `SlotRecord`, `ScanMode`, `parse()`, and `reconstruct()` |
| `Scanner.hpp` | 256-byte compile-time lookup table and tokenizer finite-state machine |
| `SimdScanner.hpp` | Hardware vector acceleration (AVX-512BW, AVX2, ARM NEON) and CPUID dispatch |
| `MemoryArena.hpp` | 64-byte cache-line aligned bump allocator for batch parsing |
| `StringPool.hpp` | String interning pool with generational compaction |
| `ThreadPool.hpp` | Work-stealing worker thread pool for parallel batch auditing |
| `Hash.hpp` | 64-bit unsigned hash functions (`xxh64`) |
| `Lifetime.hpp` | Compile-time lifetime safety attributes (`RZ_LIFETIMEBOUND`, `RZ_NOESCAPE`) |
| `Backend.hpp` | Abstract database control plane interface (`Backend`, `Table`, `AdbcOptions`) |
| `Dialect.hpp` | SQL expression dialects for DuckDB and ClickHouse |
| `FormCatalog.hpp` | Thread-safe dictionary cache and collision-resistant template registry |
| `Stats.hpp` | Information-theoretic ledger, entropy, leakage, and divergence |
| `Channel.hpp` | Steganographic and provenance channel over synonymous template choices |
| `Razobill.hpp` | Central engine coordinator for database compaction and verification |
| `RazobillC.h` | Universal C ABI declarations and status codes |

---

## Data structures

### struct razobill::core::SlotRecord

Defined in `include/librazobill/Slots.hpp`:

```cpp
namespace razobill::core {

struct SlotRecord {
    std::string templateStr;            // format string with {i} and {s} placeholders
    std::vector<std::int64_t> ints;     // values of the '{i}' slots
    std::vector<std::string_view> strs; // values of the '{s}' slots borrowing from input

    void clear() noexcept;
    void formKey(std::string& out) const;
    [[nodiscard]] std::uint64_t formKeyHash() const noexcept;
};

using Slots = SlotRecord;

} // namespace razobill::core
```

#### Memory contract

`strs` contains zero-copy `std::string_view` elements referencing the caller's source message.
The source buffer must outlive `SlotRecord`.

---

## Core functions

### parse

```cpp
[[nodiscard]] Status parse(
    std::string_view msg,
    SlotRecord& out,
    ScanMode mode = ScanMode::Semantic
);
```

Splits `msg` into `out.templateStr`, `out.ints`, and `out.strs`. `out.strs` borrows slices directly from `msg`.

### reconstruct / join

```cpp
[[nodiscard]] Status reconstruct(
    std::string_view tmpl,
    std::span<const std::int64_t> ints,
    std::span<const std::string_view> strs,
    std::string& out
);
```

Reconstructs the original message from a template and parameter slices with 100% byte-for-byte fidelity.

---

## Error handling

Status codes are defined in `include/librazobill/Status.hpp` as a 1-byte enum class:

```cpp
namespace razobill {

enum class Status : unsigned char {
    Ok = 0,
    TooLarge,       // message or rebuilt message exceeds 16 MiB
    Malformed,      // template syntax is invalid
    SlotUnderflow,  // fewer slot values than the template requires
    SlotOverflow,   // more slot values than the template requires
    BufferTooSmall, // destination buffer capacity is insufficient
    InvalidParam    // null pointer or out-of-range argument
};

[[nodiscard]] constexpr const char* describe(Status s) noexcept;

} // namespace razobill
```

Data-plane operations return `Status` directly rather than throwing C++ exceptions, maintaining deterministic microsecond throughput.

---

## Memory management

### MemoryArena

> [!NOTE]
> **Internal / Benchmark Component**: `MemoryArena` is retained for standalone benchmark comparisons and internal test harnesses; it is not on the primary DuckDB extension or C ABI production ingestion path.

Monotonic bump allocator aligned to 64-byte CPU cache lines (`include/librazobill/MemoryArena.hpp`).
Allows batch operations to allocate scratch memory without heap fragmentation:

- `void* allocate(std::size_t bytes)`: Allocates aligned memory.
- `void reset() noexcept`: Resets the bump pointer to zero without releasing underlying virtual memory blocks.

### StringPool

> [!NOTE]
> **Internal / Benchmark Component**: `StringPool` provides generational mark-and-sweep string interning for standalone test harnesses; production table compaction utilizes database-native dictionary tables.

Thread-safe string interning pool with generational mark-and-sweep garbage collection (`include/librazobill/StringPool.hpp`):

- `std::uint32_t intern(std::string_view sv)`: Interns a string and returns a dense 32-bit ID.
- `std::string_view resolve(std::uint32_t id) const`: Resolves an ID to its string view.
- `StringPoolStats gc(std::span<const std::uint32_t> rootIds)`: Purges unreferenced strings.

---

## Concurrency and lifetime primitives

### ThreadPool

Work-stealing task execution pool amortizing queue contention across coarse chunks (`include/librazobill/ThreadPool.hpp`):

- `explicit ThreadPool(std::size_t threadCount)`: Initializes workers.
- `std::future<T> submit(F&& func)`: Enqueues a task and returns an asynchronous future.

### Lifetime annotations

Macro contracts for compile-time escape and lifetime analysis (`include/librazobill/Lifetime.hpp`):

- `RZ_LIFETIMEBOUND`: Enforces that caller-supplied views do not outlive referenced buffers.
- `RZ_NOESCAPE`: Ensures pointer parameters are not retained beyond function scope.

---

## Database control plane

### Backend and Table

Database abstraction layer (`include/librazobill/Backend.hpp`):

- `class Backend`: Abstract base for executing queries (`exec`, `query`). Thread-safe in `AdbcBackend` via internal mutex.
- `struct Table`: Relational query results with typed accessors (`i64`, `u64`, `f64`, `str`, `isNull`).
- `std::unique_ptr<Backend> makeAdbcBackend(const AdbcOptions& options)`: Instantiates an ADBC database driver.

### SQL Dialects

Engine-specific SQL dialect generation (`include/librazobill/Dialect.hpp`):

- `class DuckDialect`: Generates DuckDB SQL for template extraction, array reconstruction, and array concatenation.
- `class ClickHouseDialect`: Generates ClickHouse SQL using `arrayStringConcat(arrayZipWith(...))`.

### FormCatalog

> [!NOTE]
> **Internal / Research Component**: `FormCatalog` provides an in-memory template registry for native microbenchmarks; production database compaction manages template registration in SQL catalog tables.

Thread-safe registry mapping template strings to dense 32-bit IDs with full string comparison on hash collisions (`include/librazobill/FormCatalog.hpp`):

- `std::uint32_t registerForm(std::string_view templateStr)`: Registers a template and returns a unique sequential ID.
- `std::optional<FormEntry> lookup(std::uint32_t id) const`: Looks up template metadata.

---

## Information theory and channels

### Statistical accounting

Ledger metrics and conservation identities (`include/librazobill/Stats.hpp`):

- `struct Ledger`: Tracks coverage, content entropy $C = H(W)$, expression entropy $E = H(X)$, latitude $\Lambda = H(X \mid W)$, and doubt $\Delta = H(W \mid X)$.
- Conservation laws: $E = I(X; W) + \Lambda$ and $C = I(X; W) + \Delta$.

### Steganographic channel

> [!NOTE]
> **Theoretical Proof Realization**: `Channel` realizes the covert provenance and steganographic channel of Theorem 8; it is an experimental C++ research API and not exposed via the universal C ABI or SQL scalar functions.

Provenance encoding and decoding over synonymous template forms (`include/librazobill/Channel.hpp`):

- Realizes Theorem 8 (Capacity) and Proposition 9 (Finite-Precision Bound).
- `encode(payload, forms)`: Embeds metadata bits into template selections.
- `decode(forms)`: Extracts metadata bits, catching interval underflows safely.

### Razobill coordinator

Central compaction engine orchestrator (`include/librazobill/Razobill.hpp`):

- `compact()`: Decomposes unstructured tables into columnar compact form families (`logs_c`, `logs_t`, `logs_d`).
- `verify(table)`: Validates that compact storage reconstructs the raw source table byte for byte.
- `GateRefused`: Exception raised when an information gate merge exceeds the configured loss threshold $\epsilon$.

---

## Related

- [Universal C ABI reference](c-abi.md): C99 interface for FFI consumers.
- [Embedding librazobill in C++](../how-to/cpp-developer.md): CMake linking and compiler flags.
- [Data-oriented design](../explanation/data-oriented-design.md): Structure-of-Arrays cache locality.
