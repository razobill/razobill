// =============================================================================
//  file : include/librazobill/SimdScanner.hpp
//  role : vectorized delimiter scanner routines (AVX-512, AVX2, NEON, Scalar)
//  inv  : vector lanes and scalar tail produce bit-identical delimiter indices
// =============================================================================

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#if defined(__ARM_NEON) || defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

namespace razobill::core::simd {

// -- hardware architecture tiers and bit manipulation -------------------------

/// supported hardware vector acceleration instruction sets
enum class SimdArchitecture : std::uint8_t { Scalar = 0, Neon = 1, Avx2 = 2, Avx512 = 3 };

/// count trailing zeros in 64-bit mask using branchless CPU instruction
/// in: 64-bit bitmask
/// out: index of first set bit, or 64 if mask == 0
/// thread: pure, thread-safe, noexcept
[[nodiscard]] inline int countTrailingZeros64(std::uint64_t mask) noexcept {
    return std::countr_zero(mask);
}

// -- runtime architecture detection -------------------------------------------

/// detect available SIMD instruction set via CPUID or compile-time target
/// out: highest supported SimdArchitecture tier on current CPU
/// thread: pure, thread-safe, noexcept
[[nodiscard]] inline SimdArchitecture detectSimdArchitecture() noexcept {
#if (defined(__x86_64__) || defined(_M_X64))
#if defined(__GNUC__) || defined(__clang__)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx512bw") && __builtin_cpu_supports("avx512f")) {
        return SimdArchitecture::Avx512;
    }
    if (__builtin_cpu_supports("avx2")) {
        return SimdArchitecture::Avx2;
    }
    return SimdArchitecture::Scalar;
#elif defined(_MSC_VER)
    int cpuInfo[4] = {0};
    __cpuid(cpuInfo, 0);
    const int nIds = cpuInfo[0];
    if (nIds < 1) return SimdArchitecture::Scalar;

    // Check OSXSAVE support (CPUID leaf 1, ECX bit 27)
    __cpuid(cpuInfo, 1);
    const bool osxsave = (cpuInfo[2] & (1 << 27)) != 0;
    if (!osxsave) return SimdArchitecture::Scalar;

    // Check if OS has enabled YMM state saving in XCR0 (bits 1 and 2:
    // XMM and YMM)
    const unsigned long long xcr0 = _xgetbv(0);
    const bool ymmSupported = (xcr0 & 0x6) == 0x6;
    if (!ymmSupported) return SimdArchitecture::Scalar;

    if (nIds >= 7) {
        __cpuidex(cpuInfo, 7, 0);
        const bool avx2 = (cpuInfo[1] & (1 << 5)) != 0;
        const bool avx512f = (cpuInfo[1] & (1 << 16)) != 0;
        const bool avx512bw = (cpuInfo[1] & (1 << 30)) != 0;
        // For AVX-512, check XCR0 bits 5, 6, 7 (opmask, ZMM_Hi256, Hi16_ZMM)
        // -> mask 0xE6
        const bool zmmSupported = (xcr0 & 0xE6) == 0xE6;
        if (zmmSupported && avx512f && avx512bw) return SimdArchitecture::Avx512;
        if (avx2) return SimdArchitecture::Avx2;
    }
    return SimdArchitecture::Scalar;
#else
    return SimdArchitecture::Scalar;
#endif
#elif defined(__ARM_NEON) || defined(__aarch64__) || defined(_M_ARM64)
    return SimdArchitecture::Neon;
#elif defined(__AVX2__)
    return SimdArchitecture::Avx2;
#else
    return SimdArchitecture::Scalar;
#endif
}

namespace detail {
/// process-level cached SIMD architecture eliminating repeated CPUID calls
inline const SimdArchitecture kSimdArch = detectSimdArchitecture();
} // namespace detail

// -- multi-tier vectorized scanners -------------------------------------------

[[nodiscard]] inline std::size_t findWhitespaceAvx2(const char* data, std::size_t length) noexcept;
[[nodiscard]] inline std::size_t findWhitespaceAvx512(const char* data, std::size_t length) noexcept;
[[nodiscard]] inline std::size_t findWhitespaceNeon(const char* data, std::size_t length) noexcept;
[[nodiscard]] inline std::size_t findWhitespaceDynamic(const char* data, std::size_t length) noexcept;
[[nodiscard]] inline std::size_t findWhitespace(const char* data, std::size_t length) noexcept;

/// scan up to 32 bytes per iteration using AVX2 256-bit YMM registers
/// inv: scalar tail loop processes trailing <32 bytes; matches scalar result
/// in: data pointer to ASCII byte buffer, length buffer byte length
/// out: offset of first delimiter (' ', '\t', '\n', '\r'), or length if none
/// own: borrows buffer pointed to by data
/// thread: pure, thread-safe, noexcept
[[nodiscard]]
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(_M_X64))
__attribute__((target("avx2")))
#endif
inline std::size_t findWhitespaceAvx2(const char* data, std::size_t length) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    std::size_t i = 0;
    const __m256i spaceVec = _mm256_set1_epi8(' ');
    const __m256i tabVec = _mm256_set1_epi8('\t');
    const __m256i lfVec = _mm256_set1_epi8('\n');
    const __m256i crVec = _mm256_set1_epi8('\r');

    while (i + 32 <= length) {
        __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
        __m256i eqSpace = _mm256_cmpeq_epi8(chunk, spaceVec);
        __m256i eqTab = _mm256_cmpeq_epi8(chunk, tabVec);
        __m256i eqLf = _mm256_cmpeq_epi8(chunk, lfVec);
        __m256i eqCr = _mm256_cmpeq_epi8(chunk, crVec);

        __m256i combined = _mm256_or_si256(_mm256_or_si256(eqSpace, eqTab), _mm256_or_si256(eqLf, eqCr));
        const auto mask = static_cast<std::uint32_t>(_mm256_movemask_epi8(combined));
        if (mask != 0) {
            return i + static_cast<std::size_t>(countTrailingZeros64(mask));
        }
        i += 32;
    }
    while (i < length) {
        char c = data[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return i;
        ++i;
    }
    return length;
#else
    for (std::size_t i = 0; i < length; ++i) {
        char c = data[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return i;
    }
    return length;
#endif
}

/// scan up to 64 bytes per iteration using AVX-512BW 512-bit ZMM registers
/// inv: scalar tail loop processes trailing <64 bytes; matches scalar result
/// in: data pointer to ASCII byte buffer, length buffer byte length
/// out: offset of first delimiter (' ', '\t', '\n', '\r'), or length if none
/// own: borrows buffer pointed to by data
/// thread: pure, thread-safe, noexcept
[[nodiscard]]
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(_M_X64))
__attribute__((target("avx512bw,avx512f")))
#endif
inline std::size_t findWhitespaceAvx512(const char* data, std::size_t length) noexcept {
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(_M_X64))
    std::size_t i = 0;
    const __m512i spaceVec = _mm512_set1_epi8(' ');
    const __m512i tabVec = _mm512_set1_epi8('\t');
    const __m512i lfVec = _mm512_set1_epi8('\n');
    const __m512i crVec = _mm512_set1_epi8('\r');

    while (i + 64 <= length) {
        __m512i chunk = _mm512_loadu_si512(reinterpret_cast<const void*>(data + i));
        __mmask64 eqSpace = _mm512_cmpeq_epi8_mask(chunk, spaceVec);
        __mmask64 eqTab = _mm512_cmpeq_epi8_mask(chunk, tabVec);
        __mmask64 eqLf = _mm512_cmpeq_epi8_mask(chunk, lfVec);
        __mmask64 eqCr = _mm512_cmpeq_epi8_mask(chunk, crVec);

        __mmask64 combined = eqSpace | eqTab | eqLf | eqCr;
        if (combined != 0) {
            return i + static_cast<std::size_t>(countTrailingZeros64(static_cast<std::uint64_t>(combined)));
        }
        i += 64;
    }
    while (i < length) {
        char c = data[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return i;
        ++i;
    }
    return length;
#else
    return findWhitespaceAvx2(data, length);
#endif
}

/// scan up to 16 bytes per iteration using ARM NEON 128-bit vector registers
/// inv: scalar tail loop processes trailing <16 bytes; matches scalar result
/// in: data pointer to ASCII byte buffer, length buffer byte length
/// out: offset of first delimiter (' ', '\t', '\n', '\r'), or length if none
/// own: borrows buffer pointed to by data
/// thread: pure, thread-safe, noexcept
[[nodiscard]] inline std::size_t findWhitespaceNeon(const char* data, std::size_t length) noexcept {
#if defined(__ARM_NEON) || defined(__aarch64__) || defined(_M_ARM64)
    std::size_t i = 0;
    const uint8x16_t spaceVec = vdupq_n_u8(' ');
    const uint8x16_t tabVec = vdupq_n_u8('\t');
    const uint8x16_t lfVec = vdupq_n_u8('\n');
    const uint8x16_t crVec = vdupq_n_u8('\r');

    while (i + 16 <= length) {
        uint8x16_t chunk = vld1q_u8(reinterpret_cast<const uint8_t*>(data + i));

        uint8x16_t eqSpace = vceqq_u8(chunk, spaceVec);
        uint8x16_t eqTab = vceqq_u8(chunk, tabVec);
        uint8x16_t eqLf = vceqq_u8(chunk, lfVec);
        uint8x16_t eqCr = vceqq_u8(chunk, crVec);

        uint8x16_t combined = vorrq_u8(vorrq_u8(eqSpace, eqTab), vorrq_u8(eqLf, eqCr));

#if defined(__aarch64__) || defined(_M_ARM64)
        if (vmaxvq_u8(combined) == 0) {
            i += 16;
            continue;
        }
#endif

        uint8x8_t narrowed = vshrn_n_u16(vreinterpretq_u16_u8(combined), 4);
        uint64_t scalarMask = vget_lane_u64(vreinterpret_u64_u8(narrowed), 0);
        if (scalarMask != 0) {
            int bitPos = countTrailingZeros64(scalarMask);
            return i + static_cast<std::size_t>(bitPos / 4);
        }
        i += 16;
    }
    while (i < length) {
        char c = data[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return i;
        ++i;
    }
    return length;
#else
    for (std::size_t i = 0; i < length; ++i) {
        char c = data[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return i;
    }
    return length;
#endif
}

// -- dynamic dispatch and unified entry point ---------------------------------

/// dispatch delimiter search dynamically based on runtime CPU architecture
/// in: data pointer to ASCII byte buffer, length buffer byte length
/// out: offset of first delimiter (' ', '\t', '\n', '\r'), or length if none
/// own: borrows buffer pointed to by data
/// thread: pure, thread-safe, noexcept
[[nodiscard]] inline std::size_t findWhitespaceDynamic(const char* data, std::size_t length) noexcept {
    switch (detail::kSimdArch) {
    case SimdArchitecture::Avx512:
        return findWhitespaceAvx512(data, length);
    case SimdArchitecture::Avx2:
        return findWhitespaceAvx2(data, length);
    case SimdArchitecture::Neon:
        return findWhitespaceNeon(data, length);
    case SimdArchitecture::Scalar:
    default:
        for (std::size_t i = 0; i < length; ++i) {
            char c = data[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return i;
        }
        return length;
    }
}

/// unified delimiter search with compile-time or runtime dispatch
/// inv: bit-identical results across all hardware instruction set paths
/// in: data pointer to ASCII byte buffer, length buffer byte length
/// out: offset of first delimiter (' ', '\t', '\n', '\r'), or length if none
/// own: borrows buffer pointed to by data
/// thread: pure, thread-safe, noexcept
[[nodiscard]] inline std::size_t findWhitespace(const char* data, std::size_t length) noexcept {
#if defined(__AVX512BW__) && defined(__AVX512F__)
    return findWhitespaceAvx512(data, length);
#elif defined(__AVX2__)
    return findWhitespaceAvx2(data, length);
#elif defined(__ARM_NEON) || defined(__aarch64__) || defined(_M_ARM64)
    return findWhitespaceNeon(data, length);
#else
    return findWhitespaceDynamic(data, length);
#endif
}

} // namespace razobill::core::simd
