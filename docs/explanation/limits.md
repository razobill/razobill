# Known limits and engineering boundaries

[Docs](../README.md) > [Explanations](../README.md#explanations-and-architecture) > Known limits and engineering boundaries

An honest audit of Razobill's operational limits, architectural trade-offs, and verification boundaries.

- **Question**: Where does Razobill fall short, and what are its hard operational constraints?
- **Audience**: Systems architects, database administrators, and evaluation teams considering deployment.
- **Key takeaway**: Razobill prioritizes exact invertibility and SQL-native execution over minimal cluster counts. Know your workload constraints before deploying.

---

## 1. It is not a minimal clustering tool

Razobill is designed for exact algebraic decomposition rather than unsupervised machine learning clustering.

- **Grouping accuracy**: On the project's 16-dataset benchmark, median grouping accuracy was **0.808 for Razobill versus 0.871 for Drain3**.
- **Template cardinality**: Razobill retains more distinct templates (median **70.5 clusters versus 45.5 for Drain3**).
- **Why this happens**: Traditional clustering algorithms like Drain3 replace dynamic tokens with wildcards (`<*>`), collapsing distinct formats into shared buckets. Razobill preserves exact token boundaries so that `join(template, ints, strs)` can reconstruct the original log byte-for-byte. If your sole objective is minimizing template count and you do not need to recover the original logs, a heuristic clustering tool like Drain3 is better suited.

---

## 2. Benchmark measurements and CI receipts

Performance figures, throughput metrics, and compression ratios published in these docs and in `evidence/` originate from local workstation runs alongside CI receipts:

- **Workstation environment**: Windows 11, Intel 6-Core / 12-Thread CPU, 7.78 GB RAM (8 GB physical).
- **CI benchmark receipts**: Non-gated CI benchmark runs execute `BenchCore` across the platform matrix (Ubuntu x86-64, Ubuntu ARM64, Windows MSVC, macOS) in `.github/workflows/CI.yml` to produce reproducible receipts on cloud virtual machines.
- **Verification status**: Every test script is checked into the repository (`tools/BenchDrain.py`, `tools/BenchCLP.py`, `bench/BenchCore.cpp`).
- **Recommendation**: Run `tools/BenchDrain.py` and `./build/BenchCore` on your target hardware to measure throughput in your deployment environment.

---

## 3. S3 cost models rely on local emulation

The storage savings reported in evidence benchmarks were measured using **LocalStack** and simulated AWS cost formulas, rather than multi-month production AWS billing statements. Real-world S3 costs include API request charges (`PUT`, `GET`), cross-region transfer fees, and minimum object storage billing intervals that may affect net savings.

---

## 4. Hard limit of 16 MiB per log event

The core C++ engine enforces a hard message ceiling (`kMaxMessageBytes` in `librazobill/Scanner.hpp`):

- **Limit**: $16 \times 1024 \times 1024$ bytes (16 MiB).
- **Behavior**: Passing an input string larger than 16 MiB returns `Status::TooLarge` in C++, `RZ_ERR_MESSAGE_TOO_LARGE` in the C ABI, and raises `ValueError` in Python.
- **Rationale**: Prevents malicious or corrupted payloads from triggering unbounded memory allocations in monotonic arenas.

---

## 5. Integer parsing constraints and leading zeros

Razobill extracts numbers into 64-bit signed integers (`int64_t`):

- **Leading zeros**: Numeric tokens with leading zeros (such as `'05'` in time strings `06:01:05`) are treated as **string slots (`{s}`)**, not integers. Parsing `'05'` as integer `5` would lose the leading zero during reconstruction (`"5" != "05"`).
- **Numeric range**: Numbers up to `9,223,372,036,854,775,807` ($2^{63}-1$, 19 digits) are extracted into integer slots (`{i}`). Numbers larger than `INT64_MAX` or with more than 19 digits cannot fit in a 64-bit signed integer and fall back safely to string slots (`{s}`).
- **Negative numbers**: Hyphens attached to numbers are evaluated against delimiter context; standalone negative values or GUIDs may be preserved as strings.

---

## 6. DuckDB integer array automatic widening

Earlier versions of Razobill required explicit `BIGINT[]` casting for DuckDB integer array literals (such as `[22]::BIGINT[]`).
In Razobill 1.1.6, DuckDB integer lists across all physical widths (`INTEGER[]`, `SMALLINT[]`, `TINYINT[]`, and all unsigned variants `UBIGINT[]`, `UINTEGER[]`, `USMALLINT[]`, `UTINYINT[]`) are dynamically classified and widened automatically to 64-bit integers on the fly.

- **Signed range limit**: Any integer value exceeding signed 64-bit capacity ($\ge 2^{63}$) returns a clear DuckDB error: `"RzJoin: integer value exceeds signed 64-bit range (>= 2^63) or is null"`.
- **Uncast literals**: Uncast literals such as `SELECT RzJoin('Port {i}', [22], []::VARCHAR[]);` execute directly without casting.

---

## 7. Platform packaging and binary availability

- **Supported and CI-tested platforms**:
  - Linux x86-64 and aarch64 (glibc 2.28+)
  - macOS arm64 (Apple Silicon) and x86-64
  - Windows x86-64 (MSVC and MinGW-w64 GCC)
- **Currently unsupported architectures and targets**:
  - WebAssembly (Wasm) builds
  - 32-bit CPU architectures (x86 32-bit, armv7)

---

## Related

- [System comparisons: Drain3, CLP, and Razobill](comparisons.md)
- [Lossless invertibility mechanics](lossless-invertibility.md)
- [Troubleshooting guide](../how-to/troubleshooting.md)
