# Embed librazobill in C and C++

[Docs](../README.md) > [How-to guides](../README.md#how-to-guides) > Embed librazobill in C and C++

This guide shows how to link and execute `librazobill` directly in native C++20 and C99 applications.

- **Audience**: Systems engineers and telemetry agent developers embedding in-process log decomposition.
- **Outcome**: A compiled native binary that decomposes log lines, extracts typed parameters, and asserts round-trip reconstruction without external dependencies.

---

## Prerequisites

- C++20 compiler: GCC 12+, Clang 15+, or MSVC 2022 (v19.30+)
- CMake 3.20 or newer
- POSIX threads (`pthreads`) on Linux/macOS, or Windows SDK on Windows

---

## Step 1: Add librazobill to your CMake project

Razobill provides the CMake alias target `librazobill::librazobill` for static linking and `razobill_cabi` for shared library linking.

Add Razobill as a subdirectory in your `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(log_processor LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Add Razobill source repository
add_subdirectory(path/to/razobill EXCLUDE_FROM_ALL)

# Native C++ application target
add_executable(processor main.cpp)
target_link_libraries(processor PRIVATE librazobill::librazobill)
```

---

## Step 2: Parse and reconstruct logs using C++20

Include `librazobill/Slots.hpp` and `librazobill/Razobill.hpp` to access the native C++ engine.

Create `main.cpp`:

```cpp
#include "librazobill/Slots.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <string_view>

int main() {
    // Synthetic log line (RFC 1918 private IP)
    const std::string line = "Failed password for root from 192.168.1.100 port 22";

    // Decompose into template and parameters
    razobill::core::SlotRecord record;
    razobill::Status status = razobill::core::parse(line, record, razobill::core::ScanMode::Semantic);
    assert(status == razobill::Status::Ok);

    std::cout << "Template: " << record.templateStr << "\n";
    std::cout << "Integers (" << record.ints.size() << "): ";
    for (int64_t val : record.ints) {
        std::cout << val << " ";
    }
    std::cout << "\n";

    std::cout << "Strings  (" << record.strs.size() << "): ";
    for (std::string_view sv : record.strs) {
        std::cout << "'" << sv << "' ";
    }
    std::cout << "\n";

    // Byte-exact reconstruction invariant
    std::string reconstructed;
    razobill::Status reconStatus = razobill::core::reconstruct(
        record.templateStr,
        record.ints,
        record.strs,
        reconstructed
    );
    assert(reconStatus == razobill::Status::Ok);
    assert(reconstructed == line);
    std::cout << "Round trip verified: exact match.\n";

    return 0;
}
```

---

## Step 3: Embed using the C ABI

If your service is written in C99, Go (CGO), Rust (bindgen), or Zig, link against `razobill_cabi` and include `librazobill/RazobillC.h`.

Create `main.c`:

```c
#include "librazobill/RazobillC.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    const char message[] = "Failed password for root from 192.168.1.100 port 22";
    size_t length = strlen(message);

    RzRecord record = {0};
    RzStatus status = RzSplit(message, length, &record);
    assert(status == RZ_OK);

    printf("Template: %.*s\n", (int)record.templateLength, record.templateStr);
    printf("Integers: %zu extracted\n", record.intCount);
    printf("Strings:  %zu extracted\n", record.strCount);

    // Reconstruct into a preallocated destination buffer
    char out_buf[256];
    size_t out_len = 0;
    status = RzJoin(&record, out_buf, sizeof(out_buf), &out_len);
    assert(status == RZ_OK);
    assert(out_len == length);
    assert(memcmp(out_buf, message, length) == 0);

    // Free internal buffers allocated by RzSplit
    RzFree(&record);

    printf("C ABI round trip verified: exact match.\n");
    return 0;
}
```

---

## Memory lifetime and concurrency rules

1. **String view lifetimes**: `record.strValues` contains pointers pointing directly into the input `message` buffer. You must keep the input buffer alive while reading `record.strValues`.
2. **Buffer release**: Every successful call to `RzSplit` must be paired with `RzFree(&record)` to free allocated template and integer arrays.
3. **Thread safety**: Core split and join functions are reentrant and stateless. Multiple threads can parse concurrent streams without locking or global synchronization.

---

## Verification

Compile and run your application:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/processor
```

Expected output:

```text
Template: Failed password for root from {s} port {i}
Integers (1): 22 
Strings  (1): '192.168.1.100' 
Round trip verified: exact match.
```

---

## Troubleshooting

### Undefined reference to RzSplit

Ensure your target links against `librazobill::librazobill` or `razobill_cabi`:

```cmake
target_link_libraries(my_app PRIVATE librazobill::librazobill)
```

If linking the shared library dynamically on Linux, ensure `librazobill.so` is in `LD_LIBRARY_PATH` or specify `RPATH` in CMake:

```cmake
set_target_properties(my_app PROPERTIES BUILD_RPATH "${CMAKE_BINARY_DIR}")
```

### Segmentation fault accessing record.strValues

The input message string was destroyed, reallocated, or went out of scope while `record.strValues` was being accessed. Ensure the backing storage persists until you are done with the record or have copied the string views into owned buffers.

---

## Related

- [C API reference](../reference/c-abi.md)
- [C++ API reference](../reference/cpp-api.md)
- [Hardware acceleration with SIMD](neon-simd.md)
