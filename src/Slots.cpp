// =============================================================================
//  file : src/Slots.cpp
//  role : fast zero-copy slot decomposition and byte-exact reconstruction
//  inv  : reconstruct(tmpl, ints, strs, out) restores msg byte for byte
//  deps : "librazobill/Slots.hpp", <charconv>
//  ~    : two passes over memory because cache misses hurt worse than scanning
// =============================================================================

#include "librazobill/Slots.hpp"

#include <charconv>

namespace razobill::core {

namespace {

/// branch table calculating exact decimal digit length for a 64-bit integer
/// inv: returns exact number of ASCII characters required by std::to_chars
/// in: v in [INT64_MIN, INT64_MAX]
/// out: character count in [1, 20] plus 1 for sign if negative
/// thread: pure constexpr, noexcept
// branch cascade avoids hardware integer division latency on the hot path
// ~ nineteen comparisons beating division on every single slot
constexpr std::size_t digits10(std::int64_t v) noexcept {
    const std::size_t sign = (v < 0) ? 1 : 0;
    const uint64_t u = (v < 0) ? static_cast<uint64_t>(-(v + 1)) + 1 : static_cast<uint64_t>(v);
    if (u < 10ULL) return 1 + sign;
    if (u < 100ULL) return 2 + sign;
    if (u < 1000ULL) return 3 + sign;
    if (u < 10000ULL) return 4 + sign;
    if (u < 100000ULL) return 5 + sign;
    if (u < 1000000ULL) return 6 + sign;
    if (u < 10000000ULL) return 7 + sign;
    if (u < 100000000ULL) return 8 + sign;
    if (u < 1000000000ULL) return 9 + sign;
    if (u < 10000000000ULL) return 10 + sign;
    if (u < 100000000000ULL) return 11 + sign;
    if (u < 1000000000000ULL) return 12 + sign;
    if (u < 10000000000000ULL) return 13 + sign;
    if (u < 100000000000000ULL) return 14 + sign;
    if (u < 1000000000000000ULL) return 15 + sign;
    if (u < 10000000000000000ULL) return 16 + sign;
    if (u < 100000000000000000ULL) return 17 + sign;
    if (u < 1000000000000000000ULL) return 18 + sign;
    if (u < 10000000000000000000ULL) return 19 + sign;
    return 20 + sign;
}

} // namespace

/// detects whether form string uses legacy skeleton and type suffix encoding
/// in: form is raw form key representation
/// out: true if legacy wire format (placeholder 0x12 and type separator 0x13)
bool isLegacyFormKey(std::string_view form) noexcept {
    if (form.find(static_cast<char>(kEscapePrefix)) != std::string_view::npos) {
        return false;
    }
    if (form.find("{i}") != std::string_view::npos || form.find("{s}") != std::string_view::npos ||
        form.find("{{") != std::string_view::npos || form.find("}}") != std::string_view::npos) {
        return false;
    }
    const auto sep = form.rfind(static_cast<char>(kTypeSeparator));
    if (sep == std::string_view::npos || sep == form.size() - 1) return false;
    std::string_view types = form.substr(sep + 1);
    if (types.empty()) return false;
    for (char c : types) {
        if (c != 'I' && c != 'S') return false;
    }
    std::string_view skeleton = form.substr(0, sep);
    std::size_t placeholders = 0;
    for (char c : skeleton) {
        if (static_cast<unsigned char>(c) == kPlaceholder) {
            ++placeholders;
        }
    }
    return placeholders > 0 && placeholders == types.size();
}

namespace {


/// separates legacy form key into skeleton, type tags, and slot counts
/// in: form contains single kTypeSeparator byte followed by 'I' and 'S' tags
/// out: skeleton and types string_views; nInt and nStr slot counts
/// err: returns Status::Malformed if separator missing or tags invalid
Status splitKey(std::string_view form, std::string_view& skeleton, std::string_view& types, std::size_t& nInt,
                std::size_t& nStr) noexcept {
    const auto sep = form.rfind(static_cast<char>(kTypeSeparator));
    if (sep == std::string_view::npos) return Status::Malformed;
    skeleton = form.substr(0, sep);
    types = form.substr(sep + 1);
    nInt = nStr = 0;
    for (char t : types) {
        if (t == 'I')
            ++nInt;
        else if (t == 'S')
            ++nStr;
        else
            return Status::Malformed;
    }
    std::size_t holes = 0;
    for (char c : skeleton)
        holes += static_cast<unsigned char>(c) == kPlaceholder;
    return holes == types.size() ? Status::Ok : Status::Malformed;
}

/// reconstructs original message from legacy skeleton and slot vectors
/// inv: exact byte restoration under legacy 0x11/0x12 wire format
/// in: form, ints, strs
/// out: out buffer receives reconstructed message
/// err: returns Status::SlotUnderflow, SlotOverflow, or Malformed
Status reconstructLegacy(std::string_view form, std::span<const std::int64_t> ints,
                         std::span<const std::string_view> strs, std::string& out) {
    std::string_view skeleton;
    std::string_view types;
    std::size_t nInt = 0;
    std::size_t nStr = 0;
    if (const auto s = splitKey(form, skeleton, types, nInt, nStr); s != Status::Ok) {
        return s;
    }
    if (ints.size() < nInt || strs.size() < nStr) {
        return Status::SlotUnderflow;
    }
    if (ints.size() > nInt || strs.size() > nStr) {
        return Status::SlotOverflow;
    }

    std::size_t size = skeleton.size() - types.size();
    if (size > kMaxMessageBytes) return Status::TooLarge;
    for (std::size_t i = 0; i < nInt; ++i) {
        if (ints[i] < 0) return Status::Malformed;
        const auto d = static_cast<std::size_t>(digits10(ints[i]));
        if (kMaxMessageBytes - size < d) return Status::TooLarge;
        size += d;
    }
    for (std::size_t i = 0; i < nStr; ++i) {
        const auto sLen = strs[i].size();
        if (kMaxMessageBytes - size < sLen) return Status::TooLarge;
        size += sLen;
    }
    if (size > kMaxMessageBytes) return Status::TooLarge;

    out.clear();
    out.reserve(size);
    std::size_t ti = 0;
    std::size_t ii = 0;
    std::size_t si = 0;
    std::array<char, 32> num{};
    for (char c : skeleton) {
        if (static_cast<unsigned char>(c) != kPlaceholder) {
            out.push_back(c);
        } else if (types[ti++] == 'I') {
            const auto r = std::to_chars(num.data(), num.data() + num.size(), ints[ii++]);
            out.append(num.data(), r.ptr);
        } else {
            out.append(strs[si++]);
        }
    }
    return Status::Ok;
}

// -- two-pass reconstruction and validation -----------------------------------

/// two-pass message reconstruction from template and typed slot vectors
/// inv: byte-for-byte exact restoration of original input
/// in: tmpl with {i} and {s} slots; ints 64-bit values; strs string views
/// out: out string populated with reconstructed payload
/// own: out buffer allocated once to exact size; ints and strs borrowed
/// thread: reentrant, zero shared state
/// err: returns Status::SlotUnderflow, SlotOverflow, TooLarge, Malformed
Status reconstructNew(std::string_view tmpl, std::span<const std::int64_t> ints, std::span<const std::string_view> strs,
                      std::string& out) {
    // two-pass sizing prevents reallocations and verifies slot counts
    // ~ measuring twice and allocating once keeps the hot path allocator-free
    const char* p = tmpl.data();
    const char* const end = p + tmpl.size();
    std::size_t nInt = 0;
    std::size_t nStr = 0;
    std::size_t estSize = 0;

    const auto checkAdd = [&](std::size_t delta) -> bool {
        if (kMaxMessageBytes - estSize < delta) return false;
        estSize += delta;
        return true;
    };

    while (p < end) {
        if (*p == '{') {
            if (p + 1 < end && *(p + 1) == '{') {
                if (!checkAdd(1)) return Status::TooLarge;
                p += 2;
            } else if (p + 2 < end && *(p + 1) == 'i' && *(p + 2) == '}') {
                if (nInt >= ints.size()) return Status::SlotUnderflow;
                if (ints[nInt] < 0) return Status::Malformed;
                if (!checkAdd(static_cast<std::size_t>(digits10(ints[nInt++])))) return Status::TooLarge;
                p += 3;
            } else if (p + 2 < end && *(p + 1) == 's' && *(p + 2) == '}') {
                if (nStr >= strs.size()) return Status::SlotUnderflow;
                if (!checkAdd(strs[nStr++].size())) return Status::TooLarge;
                p += 3;
            } else {
                return Status::Malformed;
            }
        } else if (*p == '}') {
            if (p + 1 < end && *(p + 1) == '}') {
                if (!checkAdd(1)) return Status::TooLarge;
                p += 2;
            } else {
                return Status::Malformed;
            }
        } else if (static_cast<unsigned char>(*p) == kEscapePrefix) {
            if (p + 1 < end && (static_cast<unsigned char>(*(p + 1)) == kEscapePrefix ||
                                static_cast<unsigned char>(*(p + 1)) == kPlaceholder ||
                                static_cast<unsigned char>(*(p + 1)) == kTypeSeparator)) {
                if (!checkAdd(1)) return Status::TooLarge;
                p += 2;
            } else {
                if (!checkAdd(1)) return Status::TooLarge;
                p += 1;
            }
        } else {
            if (!checkAdd(1)) return Status::TooLarge;
            p += 1;
        }
    }

    if (nInt == 0 && nStr == 0 && (!ints.empty() || !strs.empty())) {
        return Status::Malformed;
    }
    if (ints.size() < nInt || strs.size() < nStr) {
        return Status::SlotUnderflow;
    }
    if (ints.size() > nInt || strs.size() > nStr) {
        return Status::SlotOverflow;
    }
    if (estSize > kMaxMessageBytes) return Status::TooLarge;

    out.clear();
    out.reserve(estSize);

    p = tmpl.data();
    const char* literalStart = p;
    nInt = 0;
    nStr = 0;
    std::array<char, 32> numBuf{};
    while (p < end) {
        if (*p == '{') {
            if (p + 1 < end && *(p + 1) == '{') {
                if (p > literalStart) out.append(literalStart, static_cast<std::size_t>(p - literalStart));
                out.push_back('{');
                p += 2;
                literalStart = p;
            } else if (p + 2 < end && *(p + 1) == 'i' && *(p + 2) == '}') {
                if (p > literalStart) out.append(literalStart, static_cast<std::size_t>(p - literalStart));
                const auto r = std::to_chars(numBuf.data(), numBuf.data() + numBuf.size(), ints[nInt++]);
                out.append(numBuf.data(), r.ptr);
                p += 3;
                literalStart = p;
            } else if (p + 2 < end && *(p + 1) == 's' && *(p + 2) == '}') {
                if (p > literalStart) out.append(literalStart, static_cast<std::size_t>(p - literalStart));
                out.append(strs[nStr++]);
                p += 3;
                literalStart = p;
            } else {
                ++p;
            }
        } else if (*p == '}') {
            if (p + 1 < end && *(p + 1) == '}') {
                if (p > literalStart) out.append(literalStart, static_cast<std::size_t>(p - literalStart));
                out.push_back('}');
                p += 2;
                literalStart = p;
            } else {
                ++p;
            }
        } else if (static_cast<unsigned char>(*p) == kEscapePrefix) {
            if (p + 1 < end && (static_cast<unsigned char>(*(p + 1)) == kEscapePrefix ||
                                static_cast<unsigned char>(*(p + 1)) == kPlaceholder ||
                                static_cast<unsigned char>(*(p + 1)) == kTypeSeparator)) {
                if (p > literalStart) out.append(literalStart, static_cast<std::size_t>(p - literalStart));
                out.push_back(*(p + 1));
                p += 2;
                literalStart = p;
            } else {
                ++p;
            }
        } else {
            ++p;
        }
    }
    if (p > literalStart) {
        out.append(literalStart, static_cast<std::size_t>(p - literalStart));
    }
    return Status::Ok;
}

} // namespace

// -- slot parsing and template extraction -------------------------------------

/// scans raw log message and extracts template, 64-bit integers, and strings
/// inv: join(split(msg)) == msg for all valid messages
/// in: msg byte view, mode ScanMode::DigitOnly or ScanMode::Semantic
/// out: out populated with templateStr, ints, and string slices
/// own: out.strs elements borrow directly from msg memory
/// thread: reentrant
/// err: returns Status::TooLarge if msg.size() > kMaxMessageBytes (16 MiB)
Status parse(std::string_view msg, SlotRecord& out, ScanMode mode) {
    out.clear();
    if (msg.size() > kMaxMessageBytes) [[unlikely]]
        return Status::TooLarge;
    out.templateStr.reserve(msg.size() + 16);
    out.skeleton.reserve(msg.size() + 16);

    const auto appendEscaped = [&out](std::string_view s) {
        for (char c : s) {
            const auto uc = static_cast<unsigned char>(c);
            if (c == '{')
                out.templateStr.append("{{");
            else if (c == '}')
                out.templateStr.append("}}");
            else if (uc == kEscapePrefix || uc == kPlaceholder || uc == kTypeSeparator) {
                out.templateStr.push_back(static_cast<char>(kEscapePrefix));
                out.templateStr.push_back(c);
            } else
                out.templateStr.push_back(c);
        }
    };

    const auto result = scan(
        msg,
        [&out, &appendEscaped](std::string_view gap, std::string_view tok) {
            out.skeleton.append(gap);
            out.skeleton.push_back(static_cast<char>(kPlaceholder));
            appendEscaped(gap);
            // leading zeros (e.g. "007") stay strings to prevent byte loss
            if (isIntToken(tok)) {
                std::int64_t v = 0;
                std::from_chars(tok.data(), tok.data() + tok.size(), v);
                out.ints.push_back(v);
                out.types.push_back('I');
                out.templateStr.append("{i}");
            } else {
                out.strs.push_back(tok);
                out.types.push_back('S');
                out.templateStr.append("{s}");
            }
        },
        mode);

    out.skeleton.append(result.tail);
    appendEscaped(result.tail);

    // control characters 0x11..0x13 in payload invalidate legacy wire format
    if (result.reserved) {
        out.skeleton.clear();
        out.types.clear();
    }

    return Status::Ok;
}

/// dispatches reconstruction to legacy or modern template decoder
/// inv: exact lossless invertibility join(split(x)) == x
/// in: tmpl format key, ints array, strs array
/// out: out string receives reconstructed bytes
/// err: returns Status code matching decoder result
Status reconstruct(std::string_view tmpl, std::span<const std::int64_t> ints, std::span<const std::string_view> strs,
                   std::string& out) {
    if (isLegacyFormKey(tmpl)) {
        return reconstructLegacy(tmpl, ints, strs, out);
    }
    return reconstructNew(tmpl, ints, strs, out);
}

/// converts internal template or legacy form key into human-readable <*> format
/// inv: replaces {i} and {s} with <*>, unescapes {{ and }} to { and }
/// in: form template or legacy key
/// out: out receives normalized template string
/// err: returns Status::Malformed if syntax is invalid
Status templateOf(std::string_view form, std::string& out) {
    if (isLegacyFormKey(form)) {
        std::string_view skeleton;
        std::string_view types;
        std::size_t nInt = 0;
        std::size_t nStr = 0;
        if (const auto s = splitKey(form, skeleton, types, nInt, nStr); s != Status::Ok) {
            return s;
        }
        out.clear();
        out.reserve(skeleton.size());
        for (char c : skeleton) {
            if (static_cast<unsigned char>(c) == kPlaceholder)
                out.append("<*>");
            else
                out.push_back(c);
        }
        return Status::Ok;
    }

    out.clear();
    out.reserve(form.size());

    const char* p = form.data();
    const char* const end = p + form.size();

    while (p < end) {
        if (*p == '{') {
            if (p + 1 < end && *(p + 1) == '{') {
                out.push_back('{');
                p += 2;
            } else if (p + 2 < end && (*(p + 1) == 'i' || *(p + 1) == 's') && *(p + 2) == '}') {
                out.append("<*>");
                p += 3;
            } else {
                out.clear();
                return Status::Malformed;
            }
        } else if (*p == '}') {
            if (p + 1 < end && *(p + 1) == '}') {
                out.push_back('}');
                p += 2;
            } else {
                out.clear();
                return Status::Malformed;
            }
        } else if (static_cast<unsigned char>(*p) == kEscapePrefix) {
            if (p + 1 < end && (static_cast<unsigned char>(*(p + 1)) == kEscapePrefix ||
                                static_cast<unsigned char>(*(p + 1)) == kPlaceholder ||
                                static_cast<unsigned char>(*(p + 1)) == kTypeSeparator)) {
                out.push_back(*(p + 1));
                p += 2;
            } else {
                out.push_back(*p++);
            }
        } else if (static_cast<unsigned char>(*p) == kPlaceholder || static_cast<unsigned char>(*p) == kTypeSeparator) {
            out.clear();
            return Status::Malformed;
        } else {
            out.push_back(*p++);
        }
    }
    return Status::Ok;
}

} // namespace razobill::core
