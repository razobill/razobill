// =============================================================================
//  file : include/librazobill/RazobillC.h
//  role : stable universal C ABI over the C++20 engine
//  inv  : RzJoin(RzSplit(x)) == x, byte for byte on all valid inputs
//  deps : <stdint.h>, <stddef.h>
// =============================================================================

// High-performance, zero-allocation log decomposition and byte-exact reconstruction.
//
// DATA MODEL:
// Every log line x decomposes into three relational components:
// 1. Template (T): constant character skeleton containing {i} for 64-bit integer
//    slots and {s} for string variable slots. Literal braces are escaped as {{ and }}.
// 2. Integers (I): array of 64-bit non-negative integers extracted from decimal tokens.
// 3. Strings (S): array of variable string slices borrowed from the input byte stream.
//
// THE IRON LAW:
// RzJoin(RzSplit(x)) == x holds for every byte string x, including invalid UTF-8,
// embedded NUL bytes, binary payloads, and whitespace-only lines.
//
// SCANNING MODES:
// At the C ABI layer, RzSplit invokes the semantic scanner:
// - Decimal tokens without leading zeros (except single "0") become {i} slots.
// - Tokens following contextual keywords (for, from, user, port) or enclosed
//   in single or double quotes become {s} slots.
// - Literal braces in message text are escaped to {{ and }} in the template.
//
// MEMORY OWNERSHIP AND LIFETIMES:
// - Borrowed string views: pointers in outRecord->strValues point directly into the
//   caller's input message buffer. The input buffer must outlive outRecord.
// - Heap allocation: RzSplit allocates internal buffers (templateStr, intValues,
//   strValues, strLengths). The caller must release them via RzFree(outRecord).
// - Reconstruction buffers: RzJoin and RzTemplate write into a caller-supplied buffer
//   (outBuffer) without appending a trailing NUL byte. If bufferCapacity is insufficient,
//   RZ_ERR_BUFFER_TOO_SMALL is returned and *outLength receives the required size.
// - Manual records: foreign language runtimes (Go, Rust, Python ctypes) may allocate
//   an RzRecord with internalHandle = NULL and populate it directly for RzJoin.
//
// CONCURRENCY & THREAD SAFETY:
// All functions are reentrant and stateless. Concurrent calls operating on distinct
// record and buffer instances require no synchronization.
//
// ABI STABILITY:
// C99 calling convention and frozen struct member order governed by RZ_ABI_VERSION.
// All C++ exceptions are intercepted at the ABI boundary and converted to RzStatus codes.
#ifndef LIBRAZOBILL_CAPI_H
#define LIBRAZOBILL_CAPI_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(LIBRAZOBILL_EXPORTS)
#define RZ_API __declspec(dllexport)
#elif defined(LIBRAZOBILL_STATIC)
#define RZ_API
#else
#define RZ_API __declspec(dllimport)
#endif
#else
#define RZ_API __attribute__((visibility("default")))
#endif
#define RZ_ABI_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

/// result status codes returned by Razobill C functions
/// inv: RZ_OK is 0; non-zero values denote failure
typedef enum RzStatus {
    RZ_OK = 0,
    RZ_ERR_INVALID_ARGUMENT = 1,
    RZ_ERR_BUFFER_TOO_SMALL = 2,
    RZ_ERR_MALFORMED_TEMPLATE = 3,
    RZ_ERR_COUNT_MISMATCH = 4,
    RZ_ERR_MESSAGE_TOO_LARGE = 5,
    RZ_ERR_OUT_OF_MEMORY = 6,
    RZ_ERR_INTERNAL = 7
} RzStatus;

/// container representing a decomposed log message
/// inv: template placeholders {i} count == intCount and {s} count == strCount
/// own: strValues pointers borrow from caller message; heap arrays freed by RzFree
typedef struct RzRecord {
    const char* templateStr;
    size_t templateLength;
    const int64_t* intValues;
    size_t intCount;
    const char* const* strValues;
    const size_t* strLengths;
    size_t strCount;
    void* internalHandle;
} RzRecord;

/**
 * Splits a log message into a template string, 64-bit integers, and variable string slices.
 *
 * inv: RzJoin(outRecord, ...) restores original bytes exactly
 * in: message points to messageLength bytes; does not require NUL termination
 * out: outRecord fields populated on RZ_OK
 * own: outRecord borrows string slices from message; caller must free via RzFree
 * thread: reentrant, safe across concurrent threads with separate records
 * err: returns RzStatus; on error outRecord is zeroed and no memory leaks
 *
 * @param message Pointer to the input message bytes (does not require NUL termination).
 * @param messageLength Length of the message in bytes.
 * @param outRecord Output record struct populated on success.
 * @return RZ_OK on success, or an error status code.
 */
RZ_API RzStatus RzSplit(const char* message, size_t messageLength, RzRecord* outRecord);

/**
 * Reconstructs the exact original message from a decomposed record.
 *
 * inv: byte-for-byte exact restoration of original input
 * in: record points to valid RzRecord with matching slot counts
 * out: outBuffer receives reconstructed bytes up to bufferCapacity; outLength receives written or required length
 * own: writes into caller-owned outBuffer; no internal allocations
 * thread: reentrant, safe across concurrent threads with distinct buffers
 * err: returns RZ_ERR_BUFFER_TOO_SMALL if bufferCapacity < required
 *
 * @param record Pointer to the record to join.
 * @param outBuffer Target buffer to receive the reconstructed bytes (may be NULL if probing capacity).
 * @param bufferCapacity Capacity of outBuffer in bytes.
 * @param outLength Pointer to receive the actual written or required length in bytes (may be NULL).
 * @return RZ_OK on success, or an error status code.
 */
RZ_API RzStatus RzJoin(const RzRecord* record, char* outBuffer, size_t bufferCapacity, size_t* outLength);

/**
 * Extracts only the template representation of a message without allocating parsed arrays.
 *
 * in: message points to messageLength bytes
 * out: outBuffer receives template skeleton; outLength receives written or needed bytes
 * own: writes into caller-owned outBuffer; zero heap allocation
 * thread: reentrant
 * err: returns RZ_ERR_BUFFER_TOO_SMALL if bufferCapacity is insufficient
 *
 * @param message Pointer to the input message bytes.
 * @param messageLength Length of the message in bytes.
 * @param outBuffer Target buffer to receive the template (may be NULL if probing capacity).
 * @param bufferCapacity Capacity of outBuffer in bytes.
 * @param outLength Pointer to receive the actual written or required length in bytes (may be NULL).
 * @return RZ_OK on success, or an error status code.
 */
RZ_API RzStatus RzTemplate(const char* message, size_t messageLength, char* outBuffer, size_t bufferCapacity,
                           size_t* outLength);

/**
 * Releases heap memory allocated for a record by RzSplit.
 * Safe to call with NULL or on zeroed records.
 *
 * in: record may be NULL or pointer to RzRecord
 * out: record fields reset to NULL and 0
 * own: frees memory tracked by internalHandle; does not touch caller input message
 * thread: safe on distinct records
 * err: noexcept, no error return
 */
RZ_API void RzFree(RzRecord* record);

/**
 * Returns a static human-readable description for a status code.
 *
 * out: pointer to static constant NUL-terminated string
 * own: static storage, caller must not free
 * thread: reentrant and thread-safe
 * err: returns fallback string for unknown status codes
 */
RZ_API const char* RzStatusMessage(RzStatus status);

/**
 * Returns the runtime library semantic version string (e.g. "1.1.2").
 *
 * out: pointer to static constant NUL-terminated string
 * own: static storage
 * thread: reentrant
 */
RZ_API const char* RzVersion(void);

/**
 * Returns the runtime library ABI version (e.g. 1).
 *
 * out: integer ABI version (currently 1)
 * thread: reentrant
 */
RZ_API int RzAbiVersion(void);

#ifdef __cplusplus
}
#endif

#endif /* LIBRAZOBILL_CAPI_H */
