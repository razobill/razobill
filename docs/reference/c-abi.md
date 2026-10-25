# Universal C ABI reference

[Docs](../README.md) / [Reference](c-abi.md) / C ABI

This document specifies the C99-compatible foreign function interface exported by `include/librazobill/RazobillC.h`.

- **Who this is for**: Systems programmers building FFI bindings in Rust, Go, Zig, C#, Java, or C.
- **What you will have at the end**: The exact function declarations, memory lifetime contracts, status codes, and container structures.

---

## Function summary

| Function | Signature | Return type | Stability |
| :--- | :--- | :--- | :---: |
| `RzSplit` | `RzSplit(message, messageLength, outRecord)` | `RzStatus` | Stable |
| `RzJoin` | `RzJoin(record, outBuffer, bufferCapacity, outLength)` | `RzStatus` | Stable |
| `RzTemplate` | `RzTemplate(message, messageLength, outBuffer, bufferCapacity, outLength)` | `RzStatus` | Stable |
| `RzFree` | `RzFree(record)` | `void` | Stable |
| `RzStatusMessage` | `RzStatusMessage(status)` | `const char*` | Stable |
| `RzVersion` | `RzVersion()` | `const char*` | Stable |
| `RzAbiVersion` | `RzAbiVersion()` | `int` | Stable |

---

## Data structures

### RzRecord

The core container for decomposed log events:

```c
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
```

#### Memory ownership contract

1. **Zero-initialization**: Callers must zero-initialize `RzRecord rec = {0};` before passing it to `RzSplit`. If `rec.internalHandle` is non-NULL from an earlier `RzSplit` call, `RzSplit` safely reuses the allocated capacity without reallocating.
2. **Shallow-copy hazard**: `RzRecord` owns heap-allocated metadata via `internalHandle`. Copying `RzRecord` by value (shallow copy) causes a double-free hazard if both copies are passed to `RzFree`. Always pass `RzRecord` by pointer, or move and zero the source record.
3. **8-byte integer alignment**: The `intValues` pointer must refer to 8-byte aligned memory (`alignof(int64_t)`).
4. **Zero-copy string views**: Pointers in `strValues` borrow directly from the caller's input `message` buffer. The source buffer must remain valid and unmodified while `strValues` is accessed.
5. **Deallocation**: Caller must pass `outRecord` to `RzFree` when finished to release arrays allocated for `templateStr` and `intValues`.
6. **Manual initialization and bounds**: Foreign callers may manually construct an `RzRecord` with `internalHandle = NULL` to call `RzJoin`. Manual records must satisfy `templateLength <= 16 MiB`, `intCount <= templateLength/3 + 1`, `strCount <= templateLength/3 + 1`, and `strLengths[i] <= 16 MiB`. Violations return `RZ_ERR_MESSAGE_TOO_LARGE` or `RZ_ERR_COUNT_MISMATCH`.

### RzStatus

Enumeration of status codes returned by C ABI functions:

```c
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
```

---

## RzSplit

Decomposes a message into a template, extracted 64-bit integers, and zero-copy string views.

### Signature

```c
RZ_API RzStatus RzSplit(const char* message, size_t messageLength, RzRecord* outRecord);
```

### Parameters

- `message`: Pointer to input message bytes. Does not require a trailing NUL byte.
- `messageLength`: Length of the message in bytes.
- `outRecord`: Pointer to caller-allocated `RzRecord` struct populated on success.

### Returns

`RZ_OK` (0) on success, or an error status code.

### Edge behavior

- **NULL message or outRecord**: Returns `RZ_ERR_INVALID_ARGUMENT`.
- **Empty input (`messageLength == 0`)**: Returns an empty template with zero integers and zero strings.
- **Message > 16 MiB**: Returns `RZ_ERR_MESSAGE_TOO_LARGE`.

### Example

```c
#include "librazobill/RazobillC.h"
#include <stdio.h>
#include <string.h>

const char* msg = "Failed password for root from 192.168.1.100 port 22";
RzRecord record = {0};
if (RzSplit(msg, strlen(msg), &record) == RZ_OK) {
    printf("Template: %.*s\n", (int)record.templateLength, record.templateStr);
    RzFree(&record);
}
```

- **Stability**: Stable.
- **Since version**: 1.0.0.

---

## RzJoin

Reconstructs the original message from an `RzRecord` into a caller-supplied buffer.

### Signature

```c
RZ_API RzStatus RzJoin(const RzRecord* record, char* outBuffer, size_t bufferCapacity, size_t* outLength);
```

### Parameters

- `record`: Pointer to the record to join.
- `outBuffer`: Destination buffer to receive reconstructed bytes. May be `NULL` when probing required capacity.
- `bufferCapacity`: Byte capacity of `outBuffer`.
- `outLength`: Pointer receiving the count of bytes written (or required capacity if `outBuffer` is too small).

### Returns

- `RZ_OK` on successful reconstruction.
- `RZ_ERR_BUFFER_TOO_SMALL` if `bufferCapacity` is insufficient. `*outLength` receives the required byte capacity.

### Example

```c
size_t needed = 0;
RzJoin(&record, NULL, 0, &needed);

char* buf = (char*)malloc(needed);
size_t written = 0;
if (RzJoin(&record, buf, needed, &written) == RZ_OK) {
    printf("Reconstructed: %.*s\n", (int)written, buf);
}
free(buf);
```

- **Stability**: Stable.
- **Since version**: 1.0.0.

---

## RzTemplate

Extracts the template representation directly without allocating parsed parameter arrays.

### Signature

```c
RZ_API RzStatus RzTemplate(const char* message, size_t messageLength, char* outBuffer, size_t bufferCapacity, size_t* outLength);
```

- **Stability**: Stable.
- **Since version**: 1.0.0.

---

## RzFree

Releases heap memory associated with an `RzRecord` populated by `RzSplit`.

### Signature

```c
RZ_API void RzFree(RzRecord* record);
```

Safe to call on zero-initialized records or with `NULL`.

- **Stability**: Stable.
- **Since version**: 1.0.0.

---

## RzStatusMessage

Returns a static string description for any status code.

### Signature

```c
RZ_API const char* RzStatusMessage(RzStatus status);
```

- **Stability**: Stable.
- **Since version**: 1.0.0.

---

## RzVersion and RzAbiVersion

Returns library semantic version string and ABI version integer.

### Signature

```c
RZ_API const char* RzVersion(void);
RZ_API int RzAbiVersion(void);
```

`RzVersion()` returns `"1.1.8"`. `RzAbiVersion()` returns `1`.

- **Stability**: Stable.
- **Since version**: 1.0.0.

---

## Related

- [C++ developer guide](../how-to/cpp-developer.md): embedding `librazobill` via CMake.
- [Packages reference](packages.md): library distributions and linking commands.
- [Lossless invertibility](../explanation/lossless-invertibility.md): two-pass reconstruction mechanics.
