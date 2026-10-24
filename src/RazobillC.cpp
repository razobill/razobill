// =============================================================================
//  file : src/RazobillC.cpp
//  role : universal c abi implementation wrapping core c++ engine
//  inv  : catches all exceptions at boundary; never leaks c++ types across abi
// =============================================================================

#include "librazobill/RazobillC.h"
#include "librazobill/Slots.hpp"
#include "librazobill/Version.hpp"

#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace {

// -- internal allocation container --------------------------------------------

// backing container pinned on heap to back RzRecord pointer arrays for C
// ~ two parallel vectors just to give C callers contiguous array storage
struct InternalRecord {
    razobill::core::SlotRecord slots;
    std::vector<const char*> strPointers;
    std::vector<size_t> strLengths;

    void trim(std::size_t maxBytes = 4096) {
        slots.clear();
        if (slots.templateStr.capacity() > maxBytes) {
            std::string().swap(slots.templateStr);
        }
        if (slots.ints.capacity() * sizeof(std::int64_t) > maxBytes) {
            std::vector<std::int64_t>().swap(slots.ints);
        }
        if (slots.strs.capacity() * sizeof(std::string_view) > maxBytes) {
            std::vector<std::string_view>().swap(slots.strs);
        }
        strPointers.clear();
        if (strPointers.capacity() * sizeof(const char*) > maxBytes) {
            std::vector<const char*>().swap(strPointers);
        }
        strLengths.clear();
        if (strLengths.capacity() * sizeof(size_t) > maxBytes) {
            std::vector<size_t>().swap(strLengths);
        }
    }
};

// 1 MB retention cap for thread-local working buffers
constexpr std::size_t kScratchKeepBytes = std::size_t{1} << 20;

struct CAbiScratch {
    razobill::core::SlotRecord slots;
    std::vector<std::string_view> strViews;
    std::string result;

    void trim() {
        if (result.capacity() > kScratchKeepBytes) std::string().swap(result);
        if (slots.templateStr.capacity() > kScratchKeepBytes) std::string().swap(slots.templateStr);
        if (slots.ints.capacity() * sizeof(std::int64_t) > kScratchKeepBytes)
            std::vector<std::int64_t>().swap(slots.ints);
        if (slots.strs.capacity() * sizeof(std::string_view) > kScratchKeepBytes)
            std::vector<std::string_view>().swap(slots.strs);
        if (strViews.capacity() * sizeof(std::string_view) > kScratchKeepBytes)
            std::vector<std::string_view>().swap(strViews);
    }
};

struct ScratchTrimGuard {
    CAbiScratch& scratch;
    ~ScratchTrimGuard() noexcept { scratch.trim(); }
};

#if defined(__MINGW32__) || defined(__MINGW64__)
// On MinGW-w64, thread_local objects with non-trivial destructors trigger a known
// runtime heap corruption bug (0xc0000374) in tls_atexit.c upon worker thread exit.
// Using a pointer type avoids registering destructors with __cxa_thread_atexit.
CAbiScratch& getCAbiScratch() {
    thread_local CAbiScratch* p = nullptr;
    if (!p) {
        p = new CAbiScratch();
    }
    return *p;
}
#else
CAbiScratch& getCAbiScratch() {
    thread_local CAbiScratch s;
    return s;
}
#endif

} // namespace

// -- exported c abi functions -------------------------------------------------

extern "C" {

/// split log line into template, integers, and string slices
/// inv: exact lossless invertibility: RzJoin(RzSplit(x)) == x
/// in: message non-null buffer unless messageLength is 0; outRecord non-null
/// out: outRecord populated with pointers into message and internalHandle
/// own: outRecord->internalHandle owned by record; freed by RzFree
/// thread: pure, thread-safe, no shared state
/// err: returns RzStatus code; converts C++ exceptions at ABI boundary
RZ_API RzStatus RzSplit(const char* message, size_t messageLength, RzRecord* outRecord) {
    if (!outRecord) return RZ_ERR_INVALID_ARGUMENT;
    if (!message && messageLength > 0) return RZ_ERR_INVALID_ARGUMENT;

    std::unique_ptr<InternalRecord> internal;
    if (outRecord->internalHandle) {
        internal.reset(static_cast<InternalRecord*>(outRecord->internalHandle));
        internal->trim();
        outRecord->internalHandle = nullptr;
    } else {
        internal = std::make_unique<InternalRecord>();
    }
    std::memset(outRecord, 0, sizeof(RzRecord));

    if (messageLength == 0) {
        static const char emptyStr = '\0';
        outRecord->templateStr = &emptyStr;
        outRecord->templateLength = 0;
        outRecord->internalHandle = internal.release();
        return RZ_OK;
    }

    try {
        const std::string_view msgView(message, messageLength);

        const razobill::Status status =
            razobill::core::parse(msgView, internal->slots, razobill::core::ScanMode::Semantic);
        if (status != razobill::Status::Ok) {
            outRecord->internalHandle = internal.release();
            if (status == razobill::Status::TooLarge) return RZ_ERR_MESSAGE_TOO_LARGE;
            if (status == razobill::Status::Malformed) return RZ_ERR_MALFORMED_TEMPLATE;
            return RZ_ERR_INVALID_ARGUMENT;
        }

        const auto& strs = internal->slots.strs;
        internal->strPointers.reserve(strs.size());
        internal->strLengths.reserve(strs.size());
        for (const auto& sv : strs) {
            internal->strPointers.push_back(sv.data());
            internal->strLengths.push_back(sv.size());
        }

        outRecord->templateStr = internal->slots.templateStr.data();
        outRecord->templateLength = internal->slots.templateStr.size();
        outRecord->intValues = internal->slots.ints.data();
        outRecord->intCount = internal->slots.ints.size();
        outRecord->strValues = internal->strPointers.data();
        outRecord->strLengths = internal->strLengths.data();
        outRecord->strCount = internal->strPointers.size();
        outRecord->internalHandle = internal.release();

        return RZ_OK;
    } catch (const std::bad_alloc&) {
        outRecord->internalHandle = internal.release();
        return RZ_ERR_OUT_OF_MEMORY;
    } catch (...) {
        outRecord->internalHandle = internal.release();
        return RZ_ERR_INTERNAL;
    }
}

/// reconstruct original log line from template and slot values
/// inv: exact byte-for-byte reconstruction: RzJoin(RzSplit(x)) == x
/// in: record non-null parsed record; outBuffer buffer or nullptr for sizing
/// out: outBuffer populated with bytes; outLength receives required byte size
/// own: borrows record pointers; caller owns outBuffer
/// thread: pure, thread-safe, no shared state
/// err: returns RZ_ERR_BUFFER_TOO_SMALL if capacity insufficient
RZ_API RzStatus RzJoin(const RzRecord* record, char* outBuffer, size_t bufferCapacity, size_t* outLength) {
    if (!record) return RZ_ERR_INVALID_ARGUMENT;
    if (!record->templateStr && record->templateLength > 0) return RZ_ERR_INVALID_ARGUMENT;
    if (record->templateLength > razobill::core::kMaxMessageBytes) return RZ_ERR_MESSAGE_TOO_LARGE;
    if (record->intCount > 0 && !record->intValues) return RZ_ERR_INVALID_ARGUMENT;
    if (record->strCount > 0 && (!record->strValues || !record->strLengths)) return RZ_ERR_INVALID_ARGUMENT;

    const size_t maxPossibleSlots = (record->templateLength / 2) + 1;
    if (record->intCount > maxPossibleSlots || record->strCount > maxPossibleSlots) {
        return RZ_ERR_COUNT_MISMATCH;
    }

    for (size_t i = 0; i < record->strCount; ++i) {
        if (record->strLengths[i] > razobill::core::kMaxMessageBytes) {
            return RZ_ERR_MESSAGE_TOO_LARGE;
        }
    }

    try {
        const std::string_view tmplView(record->templateStr ? record->templateStr : "", record->templateLength);
        std::span<const int64_t> intSpan;
        if (record->intCount > 0) {
            intSpan = std::span<const int64_t>(record->intValues, record->intCount);
        }

        auto& scratch = getCAbiScratch();
        ScratchTrimGuard guard{scratch};
        scratch.strViews.clear();
        scratch.result.clear();

        scratch.strViews.reserve(record->strCount);
        for (size_t i = 0; i < record->strCount; ++i) {
            const char* sPtr = record->strValues[i];
            const size_t sLen = record->strLengths[i];
            if (!sPtr && sLen > 0) return RZ_ERR_INVALID_ARGUMENT;
            scratch.strViews.emplace_back(sPtr ? sPtr : "", sLen);
        }

        const razobill::Status status =
            razobill::core::reconstruct(tmplView, intSpan, scratch.strViews, scratch.result);
        if (status != razobill::Status::Ok) {
            scratch.trim();
            if (status == razobill::Status::SlotUnderflow || status == razobill::Status::SlotOverflow)
                return RZ_ERR_COUNT_MISMATCH;
            if (status == razobill::Status::Malformed) return RZ_ERR_MALFORMED_TEMPLATE;
            if (status == razobill::Status::TooLarge) return RZ_ERR_MESSAGE_TOO_LARGE;
            return RZ_ERR_INVALID_ARGUMENT;
        }

        const size_t reqLen = scratch.result.size();
        if (outLength) *outLength = reqLen;

        if (reqLen == 0) {
            scratch.trim();
            return RZ_OK;
        }

        if (!outBuffer || bufferCapacity < reqLen) {
            scratch.trim();
            return RZ_ERR_BUFFER_TOO_SMALL;
        }

        if (reqLen > 0) {
            std::memcpy(outBuffer, scratch.result.data(), reqLen);
        }
        scratch.trim();
        return RZ_OK;
    } catch (const std::bad_alloc&) {
        return RZ_ERR_OUT_OF_MEMORY;
    } catch (...) {
        return RZ_ERR_INTERNAL;
    }
}

/// extract display template directly from raw message without allocating slots
/// in: message raw input buffer; outBuffer destination buffer
/// out: outBuffer populated with template bytes; outLength receives byte size
/// thread: pure, thread-safe, no shared state
/// err: returns RZ_ERR_BUFFER_TOO_SMALL if capacity is insufficient
RZ_API RzStatus RzTemplate(const char* message, size_t messageLength, char* outBuffer, size_t bufferCapacity,
                           size_t* outLength) {
    if (!message && messageLength > 0) return RZ_ERR_INVALID_ARGUMENT;

    try {
        if (messageLength == 0) {
            if (outLength) *outLength = 0;
            return RZ_OK;
        }

        auto& scratch = getCAbiScratch();
        ScratchTrimGuard guard{scratch};
        scratch.slots.clear();
        const std::string_view msgView(message, messageLength);
        const razobill::Status status =
            razobill::core::parse(msgView, scratch.slots, razobill::core::ScanMode::Semantic);
        if (status != razobill::Status::Ok) {
            if (status == razobill::Status::TooLarge) return RZ_ERR_MESSAGE_TOO_LARGE;
            if (status == razobill::Status::Malformed) return RZ_ERR_MALFORMED_TEMPLATE;
            return RZ_ERR_INVALID_ARGUMENT;
        }

        const size_t reqLen = scratch.slots.templateStr.size();
        if (outLength) *outLength = reqLen;

        if (reqLen == 0) {
            scratch.trim();
            return RZ_OK;
        }

        if (!outBuffer || bufferCapacity < reqLen) {
            scratch.trim();
            return RZ_ERR_BUFFER_TOO_SMALL;
        }

        if (reqLen > 0) {
            std::memcpy(outBuffer, scratch.slots.templateStr.data(), reqLen);
        }
        scratch.trim();
        return RZ_OK;
    } catch (const std::bad_alloc&) {
        return RZ_ERR_OUT_OF_MEMORY;
    } catch (...) {
        return RZ_ERR_INTERNAL;
    }
}

/// release heap resources allocated during RzSplit
/// inv: safe to call with nullptr; zeroes record upon deallocation
/// in: record pointer to RzRecord previously populated by RzSplit
/// thread: thread-safe for distinct records
/// err: noexcept
RZ_API void RzFree(RzRecord* record) {
    if (!record) return;
    if (record->internalHandle) {
        delete static_cast<InternalRecord*>(record->internalHandle);
        record->internalHandle = nullptr;
    }
    std::memset(record, 0, sizeof(RzRecord));
}

/// return static human-readable description for an RzStatus code
/// in: status error code
/// out: static null-terminated error string
/// own: static storage; caller must not free
/// thread: pure, thread-safe, noexcept
RZ_API const char* RzStatusMessage(RzStatus status) {
    switch (status) {
    case RZ_OK:
        return "Success";
    case RZ_ERR_INVALID_ARGUMENT:
        return "Invalid argument or null pointer";
    case RZ_ERR_BUFFER_TOO_SMALL:
        return "Output buffer capacity is insufficient";
    case RZ_ERR_MALFORMED_TEMPLATE:
        return "Template contains malformed placeholders or unescaped braces";
    case RZ_ERR_COUNT_MISMATCH:
        return "Number of provided values does not match template placeholders";
    case RZ_ERR_MESSAGE_TOO_LARGE:
        return "Message exceeds maximum allowed size";
    case RZ_ERR_OUT_OF_MEMORY:
        return "Out of memory";
    case RZ_ERR_INTERNAL:
        return "Internal engine error";
    default:
        return "Unknown error";
    }
}

/// return semantic library version string
/// out: static null-terminated version string
/// own: static storage; caller must not free
/// thread: pure, thread-safe, noexcept
RZ_API const char* RzVersion(void) {
    return LIBRAZOBILL_VERSION_STRING;
}

/// return integer ABI version number
/// out: ABI version number matching RZ_ABI_VERSION
/// thread: pure, thread-safe, noexcept
RZ_API int RzAbiVersion(void) {
    return RZ_ABI_VERSION;
}

} // extern "C"
