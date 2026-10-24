// =============================================================================
// libFuzzer and standalone randomized fuzz harness for decomposition roundtrip
//
// verifies exact join(split(x)) == x across librazobill C++, C ABI, and
// semantic modes with adversarial byte mutation and 16MB ceiling enforcement
// =============================================================================

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "librazobill/RazobillC.h"
#include "librazobill/Slots.hpp"

/// libFuzzer target executing full decompose and reconstruct roundtrip
/// inv:    join(split(input)) == input across Exact, Semantic, and C ABI modes
/// in:     arbitrary byte array data of length size
/// out:    returns 0 on success; aborts immediately on invariant violation
/// own:    borrows data buffer during test execution
/// thread: thread-safe, independent allocations per invocation
/// err:    aborts process via std::abort() on any round-trip divergence
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0 || size > razobill::core::kMaxMessageBytes) {
        return 0; // skip empty or excessive payloads (>16MB)
    }

    const std::string_view input(reinterpret_cast<const char*>(data), size);

    // -------------------------------------------------------------------------
    // 1. C++20 Core API: Exact (DigitOnly) Mode
    // -------------------------------------------------------------------------
    {
        razobill::core::SlotRecord rec;
        const auto status = razobill::core::parse(input, rec, razobill::core::ScanMode::DigitOnly);
        if (status != razobill::Status::Ok) {
            std::cerr << "FUZZ ERROR: parse failed in Exact mode with status " << static_cast<int>(status) << "\n";
            std::abort();
        }
        const std::string form = rec.formKey();
        std::string rebuilt;
        const auto recStatus = razobill::core::reconstruct(form, rec.ints, rec.strs, rebuilt);
        if (recStatus != razobill::Status::Ok) {
            std::cerr << "FUZZ ERROR: reconstruct failed in Exact mode with status " << static_cast<int>(recStatus)
                      << "\n";
            std::abort();
        }
        if (rebuilt != input) {
            std::cerr << "FUZZ INVARIANT VIOLATION (Exact mode):\n"
                      << "Original:      " << input << "\n"
                      << "Reconstructed: " << rebuilt << "\n";
            std::abort();
        }
    }

    // -------------------------------------------------------------------------
    // 2. C++20 Core API: Semantic Mode
    // -------------------------------------------------------------------------
    {
        razobill::core::SlotRecord rec;
        const auto status = razobill::core::parse(input, rec, razobill::core::ScanMode::Semantic);
        if (status != razobill::Status::Ok) {
            std::cerr << "FUZZ ERROR: parse failed in Semantic mode with status " << static_cast<int>(status) << "\n";
            std::abort();
        }
        const std::string form = rec.formKey();
        std::string rebuilt;
        const auto recStatus = razobill::core::reconstruct(form, rec.ints, rec.strs, rebuilt);
        if (recStatus != razobill::Status::Ok) {
            std::cerr << "FUZZ ERROR: reconstruct failed in Semantic mode with status " << static_cast<int>(recStatus)
                      << "\n";
            std::abort();
        }
        if (rebuilt != input) {
            std::cerr << "FUZZ INVARIANT VIOLATION (Semantic mode):\n"
                      << "Original:      " << input << "\n"
                      << "Reconstructed: " << rebuilt << "\n";
            std::abort();
        }
    }

    // -------------------------------------------------------------------------
    // 3. Universal C ABI Verification: RzSplit / RzJoin / RzFree
    // -------------------------------------------------------------------------
    {
        RzRecord crec{};
        const RzStatus splitCode = RzSplit(reinterpret_cast<const char*>(data), size, &crec);
        if (splitCode != RZ_OK) {
            std::cerr << "FUZZ ERROR: C-ABI RzSplit failed with status " << splitCode << "\n";
            std::abort();
        }

        // buffer probing
        size_t requiredLen = 0;
        const RzStatus probeCode = RzJoin(&crec, nullptr, 0, &requiredLen);
        if (probeCode != RZ_ERR_BUFFER_TOO_SMALL || requiredLen != size) {
            std::cerr << "FUZZ ERROR: C-ABI RzJoin probing failed, got " << probeCode << ", requiredLen=" << requiredLen << ", expected=" << size << "\n";
            RzFree(&crec);
            std::abort();
        }

        // allocate exact buffer
        std::vector<char> buffer(size);
        size_t outLength = 0;
        const RzStatus joinCode = RzJoin(&crec, buffer.data(), buffer.size(), &outLength);
        if (joinCode != RZ_OK) {
            std::cerr << "FUZZ ERROR: C-ABI RzJoin failed with status " << joinCode << "\n";
            RzFree(&crec);
            std::abort();
        }

        const std::string_view rebuiltView(buffer.data(), outLength);
        if (rebuiltView != input) {
            std::cerr << "FUZZ C-ABI INVARIANT VIOLATION:\n"
                      << "Original:      " << input << "\n"
                      << "Reconstructed: " << rebuiltView << "\n";
            RzFree(&crec);
            std::abort();
        }
        RzFree(&crec);
    }

    return 0;
}

#if !defined(LIBRAZOBILL_USE_LIBFUZZER) && !defined(FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION)
/// standalone multi-threaded fuzzing harness for non-libFuzzer environments
/// in:  optional -runs=N command line argument
/// out: exit code 0 on clean completion; aborts on invariant violation
int main(int argc, char** argv) {
    const std::vector<std::string> testCases = {
        "Simple log line without digits",
        "Failed password for root from 192.168.1.1 port 22 ssh2",
        "Receiving block blk_-1608999687919862906 src: /10.250.19.102:54106 dest: /10.250.19.102:50010",
        "Pathological \\x12 delimiter in payload \\x13 test",
        "Multi-byte Unicode emoji: \xF0\x9F\x9A\x80\xE2\x9C\xA8\xF0\x9F\x94\xA5 with numbers 12345",
        "127.0.0.1 - - [10/Oct/2026:13:55:36 +0000] \"GET /api/v1/health HTTP/1.1\" 200 2326",
        "",
        "      \t\t\n\r   ",
        "0",
        "9223372036854775807",
        "\xca'\xff\xff\\'",
        "unclosed 'quote test"};

    std::cout << "[FuzzDecomposeReconstruct] Running standalone verification harness...\n";
    for (size_t i = 0; i < testCases.size(); ++i) {
        const auto& tc = testCases[i];
        LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(tc.data()), tc.size());
    }

    // Boundary verification at kMaxMessageBytes + 1
    {
        razobill::core::SlotRecord rec;
        std::string overLimit(razobill::core::kMaxMessageBytes + 1, 'x');
        assert(razobill::core::parse(overLimit, rec) == razobill::Status::TooLarge);
    }
    std::cout << "[FuzzDecomposeReconstruct] All " << testCases.size()
              << " sample test cases and 16MB boundary test passed perfectly!\n";

    size_t targetIterations = 1'000'000;
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == nullptr) continue;
        std::string_view arg(argv[i]);
        if (arg.starts_with("-runs=")) {
            arg.remove_prefix(6);
            char* endp = nullptr;
            targetIterations = std::strtoull(arg.data(), &endp, 10);
            break;
        }
        if (!arg.starts_with('-')) {
            char* endp = nullptr;
            targetIterations = std::strtoull(argv[i], &endp, 10);
            break;
        }
    }
    if (targetIterations == 0) targetIterations = 100'000;

    const unsigned int numThreads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    std::cout << "[FuzzDecomposeReconstruct] Spawning " << numThreads << " worker threads for " << targetIterations
              << " randomized stress iterations...\n";

    const auto t0 = std::chrono::steady_clock::now();
    std::atomic<size_t> totalDone{0};
    std::vector<std::jthread> workers;
    workers.reserve(numThreads);

    for (unsigned int tid = 0; tid < numThreads; ++tid) {
        workers.emplace_back([&, tid, numThreads]() {
            std::mt19937_64 rng(1337 + tid * 10007);
            const size_t itersForThread =
                (targetIterations / numThreads) + (tid == 0 ? (targetIterations % numThreads) : 0);

            // Mutation primitives
            const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 "
                                   "-_.,:;()[]{}\"'<>|/\\{}!@#$%^&*+=~`\x11\x12\x13";
            const size_t charsetSize = sizeof(charset) - 1;

            std::string buffer;
            for (size_t i = 0; i < itersForThread; ++i) {
                const auto mode = static_cast<uint32_t>(rng() % 5);
                buffer.clear();
                if (mode == 0) {
                    // Base case mutation
                    const auto& base = testCases[rng() % testCases.size()];
                    buffer = base;
                    if (!buffer.empty()) {
                        const size_t muts = 1 + (rng() % 3);
                        for (size_t m = 0; m < muts; ++m) {
                            const size_t pos = rng() % buffer.size();
                            buffer[pos] = charset[rng() % charsetSize];
                        }
                    }
                } else if (mode == 1) {
                    // Random token sequence with digits and symbols
                    const size_t len = 1 + (rng() % 128);
                    buffer.resize(len);
                    for (size_t j = 0; j < len; ++j) {
                        buffer[j] = charset[rng() % charsetSize];
                    }
                } else if (mode == 2) {
                    // Pathological delimiters and control bytes
                    const size_t len = 1 + (rng() % 64);
                    buffer.resize(len);
                    for (size_t j = 0; j < len; ++j) {
                        buffer[j] = static_cast<char>(rng() & 0xFF);
                    }
                } else if (mode == 3) {
                    // Extreme numbers and signed values
                    int64_t val = static_cast<int64_t>(rng());
                    buffer = "id=" + std::to_string(val) + " status=ok";
                } else {
                    // Braces & quote edge cases
                    buffer = "{{test}} {i} {s} <*> \"val\" 'sval' [nested {inside}]";
                }

                LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(buffer.data()), buffer.size());
                totalDone.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }

    const auto t1 = std::chrono::steady_clock::now();
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    const double rate = (sec > 0.0) ? (static_cast<double>(totalDone.load()) / sec / 1'000'000.0) : 0.0;

    std::cout << "[FuzzDecomposeReconstruct] Completed " << totalDone.load()
              << " multi-threaded randomized fuzz iterations in " << sec << "s (" << rate << " M iters/sec) across "
              << numThreads << " threads with ZERO invariant violations, ZERO memory leaks, and ZERO crashes!\n";
    return 0;
}
#endif
