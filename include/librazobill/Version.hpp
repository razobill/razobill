#pragma once

// =============================================================================
//  file : include/librazobill/Version.hpp
//  role : semantic versioning constants and abi compatibility metadata
//  inv  : ABI version stability guaranteed across minor and patch bumps
// =============================================================================

// Semantic Versioning 2.0.0 and binary ABI specification.
//
// ABI COMPATIBILITY CONTRACT:
// 1. API Compatibility: Minor version increments maintain source compatibility.
// 2. ABI Compatibility: Shared library symbol interfaces remain stable while
//    LIBRAZOBILL_ABI_VERSION is unchanged. Breaking binary changes require
//    incrementing MAJOR per docs/reference/deprecation-policy.md.

#include <cstdint>
#include <string_view>

#define LIBRAZOBILL_VERSION_MAJOR 1
#define LIBRAZOBILL_VERSION_MINOR 1
#define LIBRAZOBILL_VERSION_PATCH 2
#define LIBRAZOBILL_VERSION_STRING "1.1.2"
#define LIBRAZOBILL_ABI_VERSION 1

#if defined(_MSVC_LANG)
static_assert(_MSVC_LANG >= 202002L, "librazobill requires C++20 or later (/std:c++20)");
#elif defined(__cplusplus)
static_assert(__cplusplus >= 202002L, "librazobill requires C++20 or later (-std=c++20)");
#endif

#if defined(__cplusplus)
#define LIBRAZOBILL_DEPRECATED(msg) [[deprecated(msg)]]
#elif defined(_MSC_VER)
#define LIBRAZOBILL_DEPRECATED(msg) __declspec(deprecated(msg))
#elif defined(__GNUC__) || defined(__clang__)
#define LIBRAZOBILL_DEPRECATED(msg) __attribute__((deprecated(msg)))
#else
#define LIBRAZOBILL_DEPRECATED(msg)
#endif

namespace razobill {

/// container holding compile-time semantic version and binary ABI values
struct VersionInfo {
    std::uint32_t major{LIBRAZOBILL_VERSION_MAJOR};
    std::uint32_t minor{LIBRAZOBILL_VERSION_MINOR};
    std::uint32_t patch{LIBRAZOBILL_VERSION_PATCH};
    std::uint32_t abi{LIBRAZOBILL_ABI_VERSION};
    const char* string{LIBRAZOBILL_VERSION_STRING};
};

/// returns VersionInfo aggregate struct
/// out: initialized VersionInfo with compile-time constants
/// thread: pure constexpr, noexcept
[[nodiscard]] constexpr VersionInfo version() noexcept {
    return VersionInfo{};
}

/// returns runtime C++ library ABI version
/// out: uint32 ABI version (currently 1)
/// thread: pure constexpr, noexcept
[[nodiscard]] constexpr std::uint32_t abiVersion() noexcept {
    return LIBRAZOBILL_ABI_VERSION;
}

/// returns semantic version string view
/// out: string_view referencing static string literal
/// thread: pure constexpr, noexcept
[[nodiscard]] constexpr std::string_view versionString() noexcept {
    return LIBRAZOBILL_VERSION_STRING;
}

} // namespace razobill
