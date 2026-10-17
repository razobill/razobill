// =============================================================================
//  file : include/librazobill/Hash.hpp
//  role : standalone xxHash64 implementation with seed 0 for form hashing
//  inv  : bit-identical to ClickHouse xxHash64() and Python RzHash
// =============================================================================

// Standalone xxHash64 implementation with seed 0.
// Bit-identical to ClickHouse xxHash64() and pyrazobill RzHash.
// Parity pinned by tests/IntegrationChdb.cpp and lib/python/tests/TestPyRazobill.py.
//
// DESIGN RATIONALE:
// Form hashes serve as distributed partition keys across C++, DuckDB, and ClickHouse.
// Standardizing on Yann Collet's xxHash64 specification with seed 0 guarantees that
// hash values computed on ingest match database indexes without translation.

#pragma once

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace razobill::core {

/// computes 64-bit xxHash64 with seed 0 over a byte slice
/// inv: bit-identical to ClickHouse xxHash64() and Python RzHash; pinned by tests/IntegrationChdb.cpp
/// in: s is arbitrary byte view, valid or invalid UTF-8
/// out: 64-bit unsigned hash integer
/// thread: pure, reentrant, noexcept
[[nodiscard]] inline std::uint64_t xxh64(std::string_view s) noexcept {
    // prime constants derived from golden ratio expansion for maximum bit diffusion
    constexpr std::uint64_t P1 = 11400714785074694791ULL; // 0x9E3779B185EBCA87
    constexpr std::uint64_t P2 = 14029467366897019727ULL; // 0xC2B2AE3D27D4EB4F
    constexpr std::uint64_t P3 = 1609587929392839161ULL;  // 0x165667B19E3779F9
    constexpr std::uint64_t P4 = 9650029242287828579ULL;  // 0x85EBCA77C2B2AE63
    constexpr std::uint64_t P5 = 2870177450012600261ULL;  // 0x27D4EB2F165667C5

    // std::memcpy handles unaligned memory accesses cleanly without undefined behavior
    auto rd64 = [](const char* p) noexcept -> std::uint64_t {
        std::uint64_t v = 0;
        std::memcpy(&v, p, 8);
        if constexpr (std::endian::native == std::endian::big) {
#if defined(_MSC_VER)
            return _byteswap_uint64(v);
#else
            return __builtin_bswap64(v);
#endif
        } else {
            return v;
        }
    };
    auto rd32 = [](const char* p) noexcept -> std::uint32_t {
        std::uint32_t v = 0;
        std::memcpy(&v, p, 4);
        if constexpr (std::endian::native == std::endian::big) {
#if defined(_MSC_VER)
            return _byteswap_ulong(v);
#else
            return __builtin_bswap32(v);
#endif
        } else {
            return v;
        }
    };
    auto round = [&](std::uint64_t acc, std::uint64_t in) { return std::rotl(acc + in * P2, 31) * P1; };
    auto merge = [&](std::uint64_t h, std::uint64_t v) { return (h ^ round(0, v)) * P1 + P4; };

    const char* p = s.data();
    const char* const end = p + s.size();
    std::uint64_t h = 0;

    // Fast 32-byte 4-lane parallel processing loop.
    // Breaks data into 4 independent accumulators (v1..v4) to saturate CPU instruction-level parallelism (ILP).
    if (s.size() >= 32) [[likely]] {
        std::uint64_t v1 = P1 + P2;
        std::uint64_t v2 = P2;
        std::uint64_t v3 = 0;
        std::uint64_t v4 = 0 - P1;
        while (p + 32 <= end) {
            v1 = round(v1, rd64(p));
            v2 = round(v2, rd64(p + 8));
            v3 = round(v3, rd64(p + 16));
            v4 = round(v4, rd64(p + 24));
            p += 32;
        }

        // Convergence: mix the 4 parallel accumulators into a single 64-bit state
        h = std::rotl(v1, 1) + std::rotl(v2, 7) + std::rotl(v3, 12) + std::rotl(v4, 18);
        h = merge(h, v1);
        h = merge(h, v2);
        h = merge(h, v3);
        h = merge(h, v4);
    } else {
        // For inputs smaller than 32 bytes, seed directly with P5
        h = P5;
    }

    h += s.size();

    // Consume remaining 8-byte chunks
    for (; end - p >= 8; p += 8) {
        h = std::rotl(h ^ round(0, rd64(p)), 27) * P1 + P4;
    }
    // Consume remaining 4-byte chunk
    if (end - p >= 4) {
        h = std::rotl(h ^ (rd32(p) * P1), 23) * P2 + P3;
        p += 4;
    }
    // Consume trailing tail bytes (0 to 3 bytes)
    for (; p < end; ++p) {
        h = std::rotl(h ^ (static_cast<unsigned char>(*p) * P5), 11) * P1;
    }

    // Final Avalanche Mixer: Bit shifts and prime multiplies guarantee that
    // flipping even 1 single bit in the input string flips ~50% of the bits in the output hash.
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    h ^= h >> 32;

    return h;
}

} // namespace razobill::core
