# Frequently asked questions

[Docs](../README.md) > [Explanations](../README.md#4-explanations-and-architecture-understand) > Frequently asked questions

Direct answers to common technical and operational questions about Razobill.

---

## What is Razobill?

Razobill is a high-performance log decomposition engine that splits log lines into templates, integers, and strings, and reconstructs them byte for byte. It provides native bindings for DuckDB, Python (`pyrazobill`), ClickHouse/chDB, and a C99 ABI.

---

## Does Razobill alter or compress my original log text?

No. Razobill decomposes a line into relational columns `(template, ints, strs)`. Reconstructing the line via `RzJoin` or `rz.join()` produces the original byte stream bit for bit, including whitespace, punctuation, and non-printable bytes.

---

## How does Razobill differ from Drain3?

Drain3 is an unsupervised clustering tool that replaces variable words with wildcards (`<*>`), discarding the original variable strings. Razobill is an algebraic decomposition engine that extracts variables into structured arrays, enabling both SQL queries and lossless recovery. See [System comparisons: Drain3, CLP, and Razobill](comparisons.md).

---

## How does Razobill differ from CLP?

CLP compresses logs into proprietary binary archives (`.clp`) queried via specialized CLI tools. Razobill produces standard relational columns queryable directly inside DuckDB, ClickHouse, and Apache Parquet files using standard SQL. See [System comparisons: Drain3, CLP, and Razobill](comparisons.md).

---

## Why does DuckDB `RzJoin('Port {i}', [22], [])` return strange numbers?

In DuckDB, the array literal `[22]` is typed as `INTEGER[]` (32-bit). Razobill's C extension expects `BIGINT[]` (64-bit). Passing 32-bit values causes the engine to read adjacent memory strides. Always explicitly cast your array literal: `[22]::BIGINT[]`.

---

## Does `pyrazobill` release the Python GIL?

Yes, but only during multithreaded batch processing in `split_many` (or `RzSplitMany`) when processing more than 128 lines. Single-line `split()` evaluates directly on the calling Python thread without releasing the GIL.

---

## Is Razobill thread-safe?

Yes. The core C++20 parsing and reconstruction functions (`splitSlots`, `joinSlots`, `RzSplit`, `RzJoin`) are reentrant and operate on independent stack frames and thread-local buffers. No global locks or shared mutable states are used on the execution hot path.

---

## Can Razobill parse non-ASCII or invalid UTF-8 bytes?

Yes. The engine operates on raw byte sequences (`const char*` and lengths). Non-ASCII characters, international UTF-8 strings, and non-printable binary characters are handled safely without decoding errors or crashes.

---

## What is the maximum supported line length?

Razobill enforces a hard limit of 16 MiB (`razobill::kMaxMessageBytes` = $16 \times 1024 \times 1024$ bytes) per log line. Passing a larger line returns `Status::TooLarge` in C++ and raises `ValueError` in Python.

---

## What DuckDB versions are supported?

Razobill is built and validated against DuckDB 1.5.x and 2.0.x via the stable DuckDB C extension ABI (v1.5.6). Binary packages are tested against DuckDB 1.5.6 and DuckDB 2.0.0 in CI. See [Compatibility matrix](../reference/compatibility.md).

---

## What is the project license?

Razobill is open source under the Apache-2.0 license.

---

## Related

- [Known limits and engineering boundaries](limits.md)
- [Troubleshooting guide](../how-to/troubleshooting.md)
- [Compatibility matrix](../reference/compatibility.md)
