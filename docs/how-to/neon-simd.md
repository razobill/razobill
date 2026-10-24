# Hardware acceleration with SIMD

[Docs](../README.md) > [How-to guides](../README.md#how-to-guides) > Hardware acceleration with SIMD

This guide shows how to enable and verify SIMD hardware vectorization (AVX-512, AVX2, and ARM NEON) when building Razobill from source.

- **Audience**: Platform engineers and performance specialists compiling native binaries for targeted server infrastructure.
- **Outcome**: A native build taking advantage of 128-bit, 256-bit, or 512-bit vector registers with verified multi-version CPUID dispatch.

---

## Prerequisites

- Supported processor:
  - x86-64 with AVX2 or AVX-512BW (Intel Haswell / AMD Zen 2 or newer)
  - AArch64 / ARM64 with NEON (AWS Graviton, Apple Silicon, Ampere Altra)
- C++20 compiler: GCC 12+, Clang 15+, or MSVC 2022 (v19.30+)
- CMake 3.20 or newer

---

## Step 1: Configure SIMD compiler flags

Razobill provides the CMake option `ENABLE_NATIVE_OPTIMIZATION` to automatically detect and configure host CPU features. You can also specify architecture-specific flags.

### Option A: Automatic host CPU optimization

```bash
# Automatically sets -march=native (GCC/Clang) or /arch:AVX2 (MSVC)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_NATIVE_OPTIMIZATION=ON
cmake --build build --config Release
```

### Option B: Target-specific compilation flags

When cross-compiling or building deployment artifacts for specific cluster tiers:

```bash
# x86-64 AVX2 and BMI2 (Broadwell / Zen 1+)
cmake -B build -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-O3 -mavx2 -mbmi -mbmi2"
cmake --build build --config Release

# x86-64 AVX-512BW (Intel Skylake-X / Sapphire Rapids / AMD Genoa)
cmake -B build -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-O3 -mavx512bw -mavx512vl -mbmi2"
cmake --build build --config Release

# ARM64 NEON (AWS Graviton2/3/4, Apple Silicon)
cmake -B build -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-O3 -march=armv8-a+simd"
cmake --build build --config Release
```

---

## Step 2: Runtime dynamic CPUID dispatch

When building universal binaries where compile-time AVX-512 cannot be assumed for all target machines, Razobill's `detectSimdArchitecture()` checks hardware CPUID and OSXSAVE feature flags at runtime.

The scanner tiers in `SimdScanner.hpp`:

| Architecture tier | Register width | Bytes per cycle | Supported instructions |
| --- | --- | --- | --- |
| `SimdArchitecture::Avx512` | 512-bit (ZMM) | 64 | `_mm512_loadu_si512`, `_mm512_cmpeq_epi8_mask`, `_tzcnt_u64` |
| `SimdArchitecture::Avx2` | 256-bit (YMM) | 32 | `_mm256_loadu_si256`, `_mm256_movemask_epi8`, `tzcnt` |
| `SimdArchitecture::Neon` | 128-bit (V) | 16 | `vld1q_u8`, `vceqq_u8`, `vshrn_n_u16`, `rbit` + `clz` |
| `SimdArchitecture::Scalar` | 64-bit general | 1 | `std::countr_zero`, `std::isspace` |

---

## Step 3: Run the microbenchmark to verify acceleration

Build and execute the single-core microbenchmark `BenchCore`:

```bash
# Linux / macOS
./build/BenchCore

# Windows
.\build\BenchCore.exe
```

Expected output confirms the vector throughput:

```text
===================================================================================================
RAZOBILL SINGLE-CORE COMPREHENSIVE BENCHMARK (C++20, C ABI & HARDWARE SIMD)
===================================================================================================
Corpus Dataset : logs/SSH/SSH.log
Sample Events  : 50000
Total Payload  : 5511038 bytes (5.51 MB)
Mean Msg Length: 110.2 bytes/msg
---------------------------------------------------------------------------------------------------
C++20 parse (form + slots)                  368.3 ns/msg    299.3 MB/s    2.72 M msg/s  (sink 0)
C++20 parse + formKey                       370.2 ns/msg    297.7 MB/s    2.70 M msg/s  (sink 0)
C++20 parse + formKey + xxh64               390.0 ns/msg    282.6 MB/s    2.56 M msg/s  (sink 0)
C++20 parse + reconstruct (round-trip)      626.5 ns/msg    175.9 MB/s    1.60 M msg/s  (sink 0)
C ABI RzSplit                               960.4 ns/msg    114.8 MB/s    1.04 M msg/s  (sink 0)
C ABI RzSplit + RzJoin (round-trip)        1356.6 ns/msg     81.2 MB/s    0.74 M msg/s  (sink 0)
C ABI RzTemplate                            792.9 ns/msg    139.0 MB/s    1.26 M msg/s  (sink 0)
SIMD findWhitespace (AVX2 / NEON)             4.2 ns/msg  26559.2 MB/s  240.96 M msg/s  (sink 0)
SIMD findWhitespaceDynamic (CPUID)            4.3 ns/msg  25788.7 MB/s  233.97 M msg/s  (sink 0)
Scalar std::isspace delimiter check           8.9 ns/msg  12373.2 MB/s  112.26 M msg/s  (sink 0)
XXH64 raw message hash baseline              17.2 ns/msg   6389.6 MB/s   57.97 M msg/s  (sink 0)
---------------------------------------------------------------------------------------------------
MULTI-CORE PARALLEL SCALING (C++20 ThreadPool::parallelFor, Coarse Chunks)
---------------------------------------------------------------------------------------------------
 1 Cores ( 1 Threads)                   307.8 MB/s    2.79 M msg/s  (Speedup  1.00x, sink 1)
 2 Cores ( 2 Threads)                   582.0 MB/s    5.28 M msg/s  (Speedup  1.89x, sink 0)
 4 Cores ( 4 Threads)                  1123.6 MB/s   10.19 M msg/s  (Speedup  3.65x, sink 1)
 6 Cores ( 6 Threads)                  1612.7 MB/s   14.63 M msg/s  (Speedup  5.24x, sink 0)
12 Cores (12 Threads)                  1918.2 MB/s   17.40 M msg/s  (Speedup  6.23x, sink 1)
===================================================================================================
```

---

## Troubleshooting

### Illegal instruction (SIGILL) on deployment host

If an executable compiled with `-march=native` crashes on older instances with `SIGILL`:

1. Do not hardcode host vector flags in production container images.
2. Compile with baseline x86-64 microarchitecture levels:

   ```bash
   cmake -B build -DCMAKE_CXX_FLAGS="-O3 -march=x86-64-v3"
   ```

3. Rely on runtime `detectSimdArchitecture()` to conditionally invoke AVX2/AVX-512 code paths.

---

## Related

- [SIMD vectorization architecture](../explanation/simd-vectorization.md)
- [Embed librazobill in C and C++](cpp-developer.md)
- [Benchmarks and measurements](../explanation/benchmarks.md)
