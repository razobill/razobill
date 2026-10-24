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

## 2. Benchmark measurements are author-run

All performance figures, throughput numbers, and compression ratios published in these docs and in `evidence/` were collected on a single workstation:

- **Hardware**: Windows 11, Intel 6-Core / 12-Thread CPU, 7.78 GB RAM (8 GB physical).
- **Verification status**: Every test script is checked into the repository (`tools/BenchDrain.py`, `tools/BenchCLP.py`, `bench/BenchCore.cpp`), but external multi-cloud validation on Linux server fleets has not been independently published.
- **Recommendation**: Run `tools/BenchDrain.py` and `./build/BenchCore` on your target hardware to measure throughput in your deployment environment.

---

## 3. S3 cost models rely on local emulation

The storage savings reported in evidence benchmarks were measured using **LocalStack** and simulated AWS cost formulas, rather than multi-month production AWS billing statements. Real-world S3 costs include API request charges (`PUT`, `GET`), cross-region transfer fees, and minimum object storage billing intervals that may affect net savings.

---

## 4. Hard limit of 16 MiB per log event

The core C++ engine enforces a hard message ceiling (`kMaxMessageBytes` in `librazobill/Slots.hpp`):

- **Limit**: $16 \times 1024 \times 1024$ bytes (16 MiB).
- **Behavior**: Passing an input string larger than 16 MiB returns `Status::TooLarge` in C++, `RZ_ERR_MESSAGE_TOO_LARGE` in the C ABI, and raises `ValueError` in Python.
- **Rationale**: Prevents malicious or corrupted payloads from triggering unbounded memory allocations in monotonic arenas.

---

## 5. Integer parsing constraints and leading zeros

Razobill extracts numbers into 64-bit signed integers (`int64_t`):

- **Leading zeros**: Numeric tokens with leading zeros (such as `'05'` in time strings `06:01:05`) are treated as **string slots (`{s}`)**, not integers. Parsing `'05'` as integer `5` would lose the leading zero during reconstruction (`"5" != "05"`).
- **Numeric range**: Numbers larger than `9,223,372,036,854,775,807` ($2^{63}-1$) cannot fit in a 64-bit signed integer and are extracted as string slots (`{s}`).
- **Negative numbers**: Hyphens attached to numbers are evaluated against delimiter context; standalone negative values or GUIDs may be preserved as strings.

---

## 6. DuckDB array type casting requirement

In DuckDB SQL, integer array literals like `[22]` default to `INTEGER[]` (32-bit). Because Razobill's C extension expects `BIGINT[]` (64-bit), passing an uncast literal causes DuckDB's C ABI to read 32-bit values with a 64-bit stride, yielding aliased numbers.

- **Mitigation**: Always explicitly cast integer array literals:

  ```sql
  SELECT RzJoin('Port {i}', [22]::BIGINT[], []::VARCHAR[]);
  ```

---

## 7. Platform packaging and binary availability

- **Supported official binaries**:
  - Linux x86-64 and aarch64 (glibc 2.28+)
  - macOS arm64 (Apple Silicon) and x86-64
  - Windows x86-64 (MSVC)
- **Currently unsupported / untested**:
  - WebAssembly (Wasm) builds
  - Windows MinGW runtime builds
  - 32-bit CPU architectures (x86 32-bit, armv7)

---

## Related

- [System comparisons: Drain3, CLP, and Razobill](comparisons.md)
- [Lossless invertibility mechanics](lossless-invertibility.md)
- [Troubleshooting guide](../how-to/troubleshooting.md)
