// =============================================================================
//  file : include/librazobill/Scanner.hpp
//  role : zero-allocation single-pass lexical tokenizer and integer validator
//  inv  : linear O(N) scan; zero heap allocation; exact lossless tokenization
// =============================================================================

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "librazobill/SimdScanner.hpp"

namespace razobill::core {

// -- control framing bytes and scanning modes ---------------------------------

/// ASCII control bytes used for internal form key framing
/// 0x12 (Device Control 2) marks variable slots in skeleton strings
/// 0x13 (Device Control 3) separates skeleton string from type signature
/// non-printable control bytes eliminate collision with literal '{}' or '<*>'
inline constexpr unsigned char kPlaceholder = 0x12;
inline constexpr unsigned char kTypeSeparator = 0x13;

/// maximum allowed message size (16 MB)
inline constexpr std::size_t kMaxMessageBytes = std::size_t{16} << 20;

/// tokenization strategy governing variable slot extraction
enum class ScanMode : std::uint8_t {
    Exact = 0,     // strict digit-bearing variable tokens only
    DigitOnly = 0, // legacy alias for Exact
    Semantic = 1   // multi-modal: digits, key-values, paths, and context keywords
};

// -- character classification table -------------------------------------------

namespace detail {

inline constexpr unsigned char kDelim = 1, kDigit = 2, kReserved = 4, kAlpha = 8;

/// precompute 256-entry character classification lookup table at compile time
/// inv: replaces runtime conditionals with an L1 cached table lookup
constexpr std::array<unsigned char, 256> makeClasses() {
    std::array<unsigned char, 256> t{};
    for (char c :
         {'\t', '\n', '\f', '\r', ' ', '=', ',', ';', ':', '(', ')', '[', ']', '{', '}', '"', '\'', '<', '>', '|', '/'})
        t[static_cast<unsigned char>(c)] = kDelim;
    for (unsigned c = '0'; c <= '9'; ++c)
        t[c] = kDigit;
    for (unsigned c = 'a'; c <= 'z'; ++c)
        t[c] = kAlpha;
    for (unsigned c = 'A'; c <= 'Z'; ++c)
        t[c] = kAlpha;
    t['_'] = kAlpha;
    // Bytes 0x11..0x13 are reserved for form key framing; triggers escape mode
    for (unsigned c = 0x11; c <= 0x13; ++c)
        t[c] = kReserved;
    return t;
}

inline constexpr std::array<unsigned char, 256> kClass = makeClasses();

// -- context keywords ---------------------------------------------------------

/// maximum supported context keyword character length
inline constexpr std::size_t kMaxKeywordLen = 10;

/// check whether word matches a contextual semantic keyword
/// in: w candidate keyword string view
/// out: true if lowercase normalized word matches known semantic keyword
/// thread: pure, thread-safe, noexcept
inline bool isContextKeyword(std::string_view w) noexcept {
    if (w.size() < 2 || w.size() > kMaxKeywordLen) return false;
    std::array<char, 12> buf{};
    static_assert(kMaxKeywordLen < buf.size(), "kMaxKeywordLen must fit inside stack buffer");
    for (std::size_t i = 0; i < w.size(); ++i) {
        char c = w[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        buf[i] = c;
    }
    std::string_view s(buf.data(), w.size());
    return s == "for" || s == "user" || s == "by" || s == "from" || s == "ruser" || s == "rhost" || s == "host" ||
           s == "account" || s == "client" || s == "src" || s == "dest" || s == "to" || s == "at" || s == "proxy" ||
           s == "server" || s == "node" || s == "target" || s == "app" || s == "device";
}

} // namespace detail

// -- single-pass tokenizer ----------------------------------------------------

/// result of a single-pass tokenization scan
struct ScanResult {
    std::string_view tail; // constant text remaining after final extracted token
    bool reserved;         // true if input contains reserved bytes (0x11..0x13)
};

/// single-pass linear tokenizer splitting log line into gaps and tokens
/// inv: zero heap allocations; tokens and gaps partition the input exactly
/// in: m source message view, onToken callback(gap, token), mode scan mode
/// out: ScanResult with tail view and reserved byte indicator
/// own: all views passed to onToken and returned borrow directly from m
/// thread: pure, reentrant, thread-safe
/// err: noexcept conditional on onToken callback
template <class OnToken>
[[nodiscard]] inline ScanResult scan(std::string_view m, OnToken&& onToken,
                                     ScanMode mode = ScanMode::Semantic) noexcept(noexcept(onToken(m, m))) {
    const auto* p = reinterpret_cast<const unsigned char*>(m.data());
    const auto* const end = p + m.size();
    const auto* gap = p;
    unsigned char all = 0;
    std::string_view lastWord;

    const unsigned char* unclosedDoubleQuoteEnd = nullptr;
    const unsigned char* unclosedSingleQuoteEnd = nullptr;

    while (p < end) {
        const auto* const dStart = p;
        while (p < end && (detail::kClass[*p] & detail::kDelim)) {
            if (mode == ScanMode::Semantic && (*p == '"' || *p == '\'')) {
                const unsigned char quote = *p;
                if ((quote == '"' && p < unclosedDoubleQuoteEnd) || (quote == '\'' && p < unclosedSingleQuoteEnd)) {
                    // Suffix confirmed to have no matching unescaped closing quote
                    all |= detail::kClass[*p];
                    ++p;
                    continue;
                }
                break; // Potential quoted string literal
            }
            all |= detail::kClass[*p];
            ++p;
        }
        const auto* const dEnd = p;
        const auto dLen = static_cast<std::size_t>(dEnd - dStart);
        const char lastDelimChar = dLen > 0 ? static_cast<char>(*(dEnd - 1)) : '\0';

        if (p >= end) break;

        // Quoted string literal handling in Semantic mode
        if (mode == ScanMode::Semantic && (*p == '"' || *p == '\'')) {
            const unsigned char quote = *p;
            const auto* const qStart = p;
            ++p;
            while (p < end && *p != quote && *p != '\n') {
                if (*p == '\\' && p + 1 < end)
                    p += 2;
                else
                    ++p;
            }
            if (p < end && *p == quote) {
                ++p; // Include closing quote
                for (const auto* cp = qStart; cp < p; ++cp)
                    all |= detail::kClass[*cp];
                const std::string_view tok(reinterpret_cast<const char*>(qStart), static_cast<std::size_t>(p - qStart));
                onToken(std::string_view(reinterpret_cast<const char*>(gap), static_cast<std::size_t>(qStart - gap)),
                        tok);
                gap = p;
                lastWord = {};
                continue;
            } else {
                if (quote == '"')
                    unclosedDoubleQuoteEnd = p;
                else
                    unclosedSingleQuoteEnd = p;
                all |= detail::kClass[quote];
                p = qStart + 1; // Unclosed quote: proceed past qStart
                continue;
            }
        }

        const auto* const start = p;
        unsigned char seen = 0;
        while (p < end && !(detail::kClass[*p] & detail::kDelim))
            seen |= detail::kClass[*p++];
        all |= seen;

        const std::string_view tok(reinterpret_cast<const char*>(start), static_cast<std::size_t>(p - start));
        bool isVar = false;

        if (seen & detail::kDigit) {
            isVar = true;
        } else if (mode == ScanMode::Semantic) {
            if (lastDelimChar == '=' || lastDelimChar == '@') {
                isVar = true;
            } else if (lastDelimChar == '"' || lastDelimChar == '\'') {
                isVar = true;
            } else if (!lastWord.empty() && detail::isContextKeyword(lastWord)) {
                isVar = true;
            } else if (p < end && *p == ':' && tok.find('.') != std::string_view::npos) {
                isVar = true;
            } else if (tok.size() > 4 && (tok.ends_with(".exe") || tok.ends_with(".dll") || tok.ends_with(".so"))) {
                isVar = true;
            }
        }

        if (isVar) {
            onToken(std::string_view(reinterpret_cast<const char*>(gap), static_cast<std::size_t>(start - gap)), tok);
            gap = p;
            lastWord = {};
        } else {
            lastWord = tok;
        }
    }
    return {std::string_view(reinterpret_cast<const char*>(gap), static_cast<std::size_t>(end - gap)),
            (all & detail::kReserved) != 0};
}

// -- integer slot validation --------------------------------------------------

/// test whether token is a decimal integer representable in signed int64_t
/// inv: preserves lossless invertibility:
///      1. leading zeros rejected ("007" stays string) to prevent format loss
///      2. signs (+/-) rejected ("-1" stays string) to avoid sign bit drop
///      3. tokens > 18 digits rejected (fits under INT64_MAX without overflow)
/// in: t token view
/// out: true if token is strictly non-negative decimal integer <= 18 digits
/// thread: pure, thread-safe, noexcept
[[nodiscard]] constexpr bool isIntToken(std::string_view t) noexcept {
    if (t.empty() || t.size() > 18) return false;
    if (t.front() == '0') return t.size() == 1;
    return std::ranges::all_of(t, [](char c) { return c >= '0' && c <= '9'; });
}

} // namespace razobill::core
