# Compatibility and platform support

[Docs](../README.md) / [Reference](compatibility.md) / Compatibility

This page specifies the supported operating systems, architectures, runtimes, database engines, and hardware vector extensions for Razobill.

- **Who this is for**: Systems administrators, deployment engineers, and packagers.
- **What you will have at the end**: The definitive matrix of supported platforms, compiler floors, and maturity classifications.

---

## Maturity classifications

Every Razobill component is assigned one of three maturity levels:

- **Stable**: Tested across our multi-platform CI matrix. Guaranteed API and ABI compatibility within major versions.
- **Beta**: Functional and verified by integration tests, but requires external drivers, custom collector builders, or host-specific configuration.
- **Prototype**: Experimental proof-of-concept under active investigation.

---

## Operating systems and architectures

| Operating system | Architectures | C++ engine | Python wheels | DuckDB extension |
| :--- | :--- | :---: | :---: | :---: |
| **Linux** (glibc 2.31+) | `x86_64`, `aarch64` | Stable | Stable | Stable |
| **Linux** (musl) | `x86_64`, `aarch64` | Stable | Source (`sdist`) | Beta |
| **macOS** (11.0+) | Apple Silicon (`arm64`), Intel (`x86_64`) | Stable | Stable | Stable |
| **Windows** (10+) | `x86_64` (MSVC) | Stable | Stable | Stable |
| **WebAssembly** | `wasm32-emscripten` | Unsupported | Unsupported | Excluded |
| **Windows MinGW** | `x86_64` | Experimental | Source (`sdist`) | Excluded |

---

## Python runtimes

| Python version | CPython support | PyPy support | Distribution method |
| :--- | :---: | :---: | :--- |
| **Python 3.13** | Supported | Untested | Pre-compiled binary wheels and sdist |
| **Python 3.12** | Supported | Untested | Pre-compiled binary wheels and sdist |
| **Python 3.11** | Supported | Untested | Pre-compiled binary wheels and sdist |
| **Python 3.10** | Supported | Untested | Pre-compiled binary wheels and sdist |
| **Python 3.9** | Supported | Untested | Pre-compiled binary wheels and sdist |
| **Python < 3.9** | Unsupported | Unsupported | Not available (end of life) |

Pre-compiled binary wheels are published for `cp39`, `cp310`, `cp311`, `cp312`, and `cp313` across Windows, Linux manylinux, and macOS.
If pre-compiled wheels are unavailable for your specific architecture, `pip` falls back to the pure-Python module (`pyrazobill._fallback`) with identical test parity.

---

## Database engines and analytical tools

| Engine or tool | Version or ABI | Integration type | Maturity |
| :--- | :--- | :--- | :---: |
| **DuckDB** | 1.5.x, 2.0.x (ABI `v1.5.6`) | Native C extension (`razobill.duckdb_extension`) | Stable |
| **ClickHouse** | 23.x, 24.x | xxHash64 bit-parity in MergeTree tables | Beta |
| **chDB** | 2.0+ | Python embedded ClickHouse engine | Beta |
| **OpenTelemetry Collector** | Collector v0.111.0+ | Go processor component (`razobillprocessor`) | Beta |
| **Vector** | 0.38+ | Rust crate transform (`razobill-vector`) | Beta |
| **Amazon S3 / Parquet** | Parquet 2.0+ (Zstd) | Columnar Parquet stripes via DuckDB/Arrow | Stable |

---

## Compilers and build toolchain

| Tool | Minimum version | Standard or flag |
| :--- | :--- | :--- |
| **GCC** | 12.0 or newer | `-std=c++20` |
| **Clang / LLVM** | 15.0 or newer | `-std=c++20` |
| **MSVC** | Visual Studio 2022 (19.30+) | `/std:c++20` |
| **CMake** | 3.20 or newer | Required for build configuration |
| **Go** | 1.25 or newer | Required for `ext/otel` |
| **Rust / Cargo** | 1.75 or newer (2021 edition) | Required for `ext/vector` |

---

## Hardware vector acceleration (SIMD)

Razobill detects available vector instructions at runtime using CPUID on x86-64 and feature registers on ARM:

| Instruction set | Vector register width | Hardware targets | Status |
| :--- | :--- | :--- | :---: |
| **AVX-512BW** | 512-bit (ZMM) | Intel Xeon (Sapphire Rapids+), AMD EPYC (Genoa+) | Supported |
| **AVX2 / BMI2** | 256-bit (YMM) | Intel Core 4th gen+, AMD Zen+ | Supported |
| **ARM NEON** | 128-bit | Apple Silicon (M1/M2/M3/M4), AWS Graviton (2/3/4) | Supported |
| **Scalar fallback** | 64-bit / 8-bit table | Any standard POSIX or Windows x86/ARM CPU | Supported |

---

## Related

- [Packages reference](packages.md): which package to install for your environment.
- [Deprecation policy](deprecation-policy.md): SemVer and ABI stability contracts.
- [SIMD vectorization explanation](../explanation/simd-vectorization.md): hardware implementation mechanics.
