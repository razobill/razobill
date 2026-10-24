# Packages and distributions

[Docs](../README.md) / [Reference](packages.md) / Packages

This page lists all distribution packages, libraries, and extensions in the Razobill project to help you select the right package for your application.

- **Who this is for**: Developers, operators, and pipeline maintainers choosing a package to install.
- **What you will have at the end**: An explicit understanding of every package name, target runtime, installation method, and link to documentation.

---

## Package decision matrix

| Package name | Target environment | Install or link command | Primary use case |
| :--- | :--- | :--- | :--- |
| **`pyrazobill`** | Python 3.9–3.13 | `pip install pyrazobill` | Python data scripts, batch pipelines, ML preprocessing |
| **`razobill`** (DuckDB) | DuckDB 1.5.x | `INSTALL razobill FROM community;` | Analytical SQL queries, log aggregation, Parquet compaction |
| **`librazobill`** (C ABI) | C, C++, Go, Rust, Zig, C# | `#include "librazobill/RazobillC.h"` | Foreign-function interfaces, high-throughput embedded services |
| **`librazobill`** (C++20) | C++20 applications | `target_link_libraries(app PRIVATE librazobill)` | Direct in-process C++ engine embedding with zero heap overhead |
| **`razobillprocessor`** | OpenTelemetry Collector | OpenTelemetry Collector Builder manifest | Edge telemetry reduction, stripping verbose string bodies |
| **`razobill-vector`** | Vector (Datadog) | Cargo crate dependency in Vector transform | High-speed edge transform streaming structured stripes |

---

## Detailed package descriptions

### pyrazobill

The official Python package published on PyPI.

- **Artifact type**: Pre-compiled binary wheels (`.whl`) and source distribution (`.tar.gz`).
- **Dependencies**: Zero runtime dependencies.
- **Fallback**: Contains `pyrazobill._fallback`, a pure-Python implementation that runs transparently if native compilation is unavailable.
- **Documentation**: [Python guide](../how-to/python.md) and [Python API reference](python-api.md).

### razobill (DuckDB extension)

Loadable binary extension for DuckDB.

- **Artifact type**: Loadable dynamic extension (`razobill.duckdb_extension`).
- **Distribution**: DuckDB Official Community Extensions repository.
- **Dual entrypoint**: Can be loaded as `LOAD razobill;` or `LOAD librazobill;`.
- **Documentation**: [DuckDB guide](../how-to/duckdb.md) and [DuckDB SQL reference](sql-functions.md).

### librazobill (C ABI shared library)

Shared library exposing a C99-compatible foreign-function interface.

- **Artifact type**: Shared library (`librazobill.so` on Linux, `razobill.dll` on Windows, `librazobill.dylib` on macOS).
- **Header**: `include/librazobill/RazobillC.h`.
- **Memory contract**: Zero-copy string slices referencing input buffers; memory managed via `RzFree`.
- **Documentation**: [C ABI reference](c-abi.md).

### librazobill (C++20 static library)

Static library providing direct access to the C++20 data plane.

- **Artifact type**: Static library (`librazobill_static.a` or `razobill.lib`).
- **Headers**: `include/librazobill/*.hpp`.
- **Standard**: C++20 required (`/std:c++20` or `-std=c++20`).
- **Documentation**: [C++ developer guide](../how-to/cpp-developer.md) and [C++ API reference](cpp-api.md).

### razobillprocessor (OpenTelemetry)

OpenTelemetry Collector processor component written in Go.

- **Source path**: `ext/otel/`.
- **Distribution**: Go module (`github.com/razobill/razobill/ext/otel`).
- **Build method**: OpenTelemetry Collector Builder (`builder`).
- **Documentation**: [OpenTelemetry guide](../how-to/opentelemetry.md).

### razobill-vector (Vector transform)

Native transform for Vector written in Rust.

- **Source path**: `ext/vector/`.
- **Distribution**: Cargo crate (`razobill-vector`).
- **Integration**: Vector Remap Language (VRL) function `razobill_split(.message)`.
- **Documentation**: [Vector guide](../how-to/vector.md).

---

## Related

- [Compatibility matrix](compatibility.md): supported platforms and compiler floors.
- [Getting started tutorial](../tutorials/getting-started.md): first steps in Python and DuckDB.
- [Deprecation policy](deprecation-policy.md): versioning guarantees across packages.
