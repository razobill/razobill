// =============================================================================
//  file : include/librazobill/Status.hpp
//  role : zero-allocation status codes and error descriptions for data plane
//  inv  : Status fits in an 8-bit unsigned char register; never throws
// =============================================================================

#pragma once

#include <ostream>

namespace razobill {

// Zero-allocation error reporting for the engine hot path.
// Passing a 1-byte enum class fits directly inside an 8-bit CPU register (%al),
// keeping error propagation branch-predictable and eliminating stack unwinding.

/// result status codes for engine and slot operations
/// inv: Ok is 0; fits in single unsigned byte
enum class Status : unsigned char {
    Ok = 0,
    TooLarge,       // message or rebuilt message exceeds kMaxMessageBytes (16 MB)
    Malformed,      // form key is not "<skeleton>\x13<types>" or counts disagree
    SlotUnderflow,  // fewer integer or string slots supplied than the form requires
    SlotOverflow,   // more integer or string slots supplied than the form requires
    BufferTooSmall, // destination buffer capacity is insufficient
    InvalidParam    // null pointer or out-of-range argument
};

/// returns static human-readable description for a status code
/// in: s status code
/// out: pointer to static constant NUL-terminated string
/// own: static storage, caller must not free
/// thread: pure constexpr, noexcept
[[nodiscard]] constexpr const char* describe(Status s) noexcept {
    switch (s) {
    case Status::Ok:
        return "ok";
    case Status::TooLarge:
        return "message exceeds the size limit";
    case Status::Malformed:
        return "malformed form key";
    case Status::SlotUnderflow:
        return "fewer slot values than the form requires";
    case Status::SlotOverflow:
        return "more slot values than the form requires";
    case Status::BufferTooSmall:
        return "destination buffer capacity is insufficient";
    case Status::InvalidParam:
        return "invalid parameter or null pointer";
    default:
        return "unknown";
    }
}

/// streams human-readable status description into an output stream
inline std::ostream& operator<<(std::ostream& os, Status s) {
    return os << describe(s);
}

} // namespace razobill
