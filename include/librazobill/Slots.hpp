// =============================================================================
//  file : include/librazobill/Slots.hpp
//  role : data plane slot record, parser, and exact byte-for-byte reconstructor
//  inv  : RzJoin(RzSplit(x)) == x for every valid input byte string x
// =============================================================================

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "librazobill/Hash.hpp"
#include "librazobill/Scanner.hpp"
#include "librazobill/Status.hpp"

namespace razobill::core {

// -- parsed slot record -------------------------------------------------------

/// structure-of-arrays representation of parsed log slots
/// inv: strs elements borrow directly from the caller's message buffer
/// own: caller must keep source message alive while strs views are accessed
struct SlotRecord {
    std::string templateStr;            // format string containing {i} and {s} placeholders
    std::vector<std::int64_t> ints;     // values parsed into '{i}' integer slots
    std::vector<std::string_view> strs; // borrowed views into '{s}' string slots

    // Legacy fields for internal backward compatibility
    std::string skeleton;
    std::string types;

    /// clear all slot fields for scratch buffer recycling
    /// inv: preserves vector capacities, avoiding per-event reallocations
    /// thread: not thread-safe
    /// err: noexcept
    void clear() noexcept {
        templateStr.clear();
        ints.clear();
        strs.clear();
        skeleton.clear();
        types.clear();
    }

    /// populate output string with format template key
    /// in: out destination string buffer
    /// out: out assigned with templateStr
    void formKey(std::string& out) const { out.assign(templateStr); }

    /// return format template key as an owned string
    /// out: copy of templateStr
    [[nodiscard]] std::string formKey() const {
        std::string res;
        formKey(res);
        return res;
    }

    /// 64-bit xxHash64 hash of the format template key
    /// inv: bit-identical with Python pyrazobill, DuckDB RzHash, and
    ///      ClickHouse xxHash64
    /// out: 64-bit integer hash
    [[nodiscard]] std::uint64_t formKeyHash() const noexcept { return xxh64(formKey()); }

    /// legacy snake_case alias for formKey
    void form_key(std::string& out) const { formKey(out); }
};

using Slots = SlotRecord;

// -- parsing and reconstruction engine ----------------------------------------

/// parse message into format template and typed integer/string slot vectors
/// inv: lossless invertibility:
///      reconstruct(out.templateStr, out.ints, out.strs) == msg
/// in: msg input byte string view, out destination record,
///     mode scanning strategy
/// out: out.templateStr populated, out.ints and out.strs populated
/// own: out.strs elements borrow directly from msg; caller must keep msg valid
/// thread: pure, thread-safe, reentrant; zero allocation if out has capacity
/// err: returns Status::Ok, Status::TooLarge, or Status::Malformed
[[nodiscard]] Status parse(std::string_view msg, SlotRecord& out, ScanMode mode = ScanMode::Semantic);

/// reconstruct original message from format template and typed slot values
/// inv: exact byte-for-byte fidelity: join(split(x)) == x for all valid inputs
/// in: tmpl format template string, ints integer slots, strs string slot views
/// out: out populated with reconstructed original message bytes
/// own: assigns reconstructed bytes into caller-provided out buffer
/// thread: pure, thread-safe, reentrant
/// err: returns Status::Ok, Status::SlotUnderflow, Status::SlotOverflow,
///      or Status::Malformed
[[nodiscard]] Status reconstruct(std::string_view tmpl, std::span<const std::int64_t> ints,
                                 std::span<const std::string_view> strs, std::string& out);

/// inline alias for reconstruct
[[nodiscard]] inline Status join(std::string_view tmpl, std::span<const std::int64_t> ints,
                                 std::span<const std::string_view> strs, std::string& out) {
    return reconstruct(tmpl, ints, strs, out);
}

/// convert internal form key or message into display template representation
/// in: form input form key or raw message view
/// out: out assigned with template string replacing slots with <*>
/// thread: pure, thread-safe, reentrant
/// err: returns Status::Ok or Status::Malformed
[[nodiscard]] Status templateOf(std::string_view form, std::string& out);

/// legacy snake_case alias for templateOf
[[nodiscard]] inline Status template_of(std::string_view form, std::string& out) {
    return templateOf(form, out);
}

} // namespace razobill::core
