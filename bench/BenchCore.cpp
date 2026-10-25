// =============================================================================
// single-core data-plane throughput and latency microbenchmark
//
// measures scalar parser throughput, form key generation, lossless round-trip,
// Universal C ABI overhead, SIMD scanning speed, and multi-core thread scaling
// ~ single-core scalar throughput against raw logs without locks or thread pool
// =============================================================================

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "librazobill/Hash.hpp"
#include "librazobill/RazobillC.h"
#include "librazobill/SimdScanner.hpp"
#include "librazobill/Slots.hpp"
#include "librazobill/ThreadPool.hpp"

/**
 * Entry point for Razobill single-core and multi-core throughput benchmarks.
 *
 * inv: measures uncorrupted input records; does not alter input files
 * in: argv[1] optional path to log corpus; argv[2] optional line limit
 * out: prints formatted tabular throughput and latency metrics to stdout
 * err: returns 1 on missing or unreadable corpus file, 0 on success
 */
int main(int argc, char** argv) {
    std::string path;
    std::size_t maxLines = 50000;
    if (argc > 2 && argv[2] != nullptr) {
        char* endp = nullptr;
        maxLines = std::strtoul(argv[2], &endp, 10);
        if (endp == argv[2] || *endp != '\0') {
            std::printf("[BENCH] Error: invalid line limit '%s'.\n", argv[2]);
            return 1;
        }
    }

    if (argc > 1 && argv[1] != nullptr) {
        std::error_code ec;
        std::filesystem::path inputPath(argv[1]);
        if (std::filesystem::is_regular_file(inputPath, ec) && !ec) {
            auto canonicalPath = std::filesystem::canonical(inputPath, ec);
            if (!ec) {
                path = canonicalPath.string();
            } else {
                std::printf("[BENCH] Error: Cannot resolve canonical path for '%s'.\n", argv[1]);
                return 1;
            }
        } else {
            std::printf("[BENCH] Error: '%s' is not a valid regular file.\n", argv[1]);
            return 1;
        }
    } else {
        const std::vector<std::string> candidates = {"build/corpus.txt",
                                                     "logs/SSH/SSH.log",
                                                     "logs/Linux/Linux.log",
                                                     "logs/Zookeeper/Zookeeper.log",
                                                     "/tmp/corpus.txt",
                                                     "tests/ConformanceCorpus.txt"};
        for (const auto& c : candidates) {
            std::ifstream check(c);
            if (check.is_open()) {
                path = c;
                break;
            }
        }
    }

    if (path.empty()) {
        std::puts("[BENCH] Error: No valid corpus log file found.");
        return 1;
    }

    std::ifstream in(path);
    if (!in.is_open()) {
        std::printf("[BENCH] Error: Could not open '%s'.\n", path.c_str());
        return 1;
    }

    std::vector<std::string> msgs;
    msgs.reserve(maxLines > 0 ? maxLines : 100000);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty()) {
            msgs.push_back(std::move(line));
            if (maxLines > 0 && msgs.size() >= maxLines) break;
        }
    }

    if (msgs.empty()) {
        std::puts("[BENCH] Error: Input corpus is empty. Nothing to benchmark.");
        return 1;
    }

    std::size_t totalBytes = 0;
    for (const auto& m : msgs) {
        totalBytes += m.size();
    }

    razobill::core::SlotRecord slots;
    std::string formKey;
    std::string reconstructed;
    char buffer[1024];

    auto timeOperation = [&](const char* operationName, auto&& benchmarkLambda) {
        double bestSeconds = 1e300;
        std::size_t sink = 0;

        for (int rep = 0; rep < 7; ++rep) {
            const auto t0 = std::chrono::steady_clock::now();
            for (const auto& m : msgs) {
                sink += benchmarkLambda(m);
            }
            const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            bestSeconds = std::min(bestSeconds, elapsed);
        }

        const double totalMsgs = static_cast<double>(msgs.size());
        const double nsPerMsg = (bestSeconds / totalMsgs) * 1e9;
        const double mbPerSec = (static_cast<double>(totalBytes) / bestSeconds) / 1e6;
        const double megaMsgsPerSec = (totalMsgs / bestSeconds) / 1e6;

        std::printf("%-40s %8.1f ns/msg  %7.1f MB/s  %6.2f M msg/s  (sink %zu)\n", operationName, nsPerMsg, mbPerSec,
                    megaMsgsPerSec, sink % 7);
    };

    std::printf(
        "===================================================================================================\n");
    std::printf("RAZOBILL SINGLE-CORE COMPREHENSIVE BENCHMARK (C++20, C ABI & HARDWARE SIMD)\n");
    std::printf(
        "===================================================================================================\n");
    std::printf("Corpus Dataset : %s\n", path.c_str());
    std::printf("Sample Events  : %zu\n", msgs.size());
    std::printf("Total Payload  : %zu bytes (%.2f MB)\n", totalBytes, static_cast<double>(totalBytes) / 1e6);
    std::printf("Mean Msg Length: %.1f bytes/msg\n",
                static_cast<double>(totalBytes) / static_cast<double>(msgs.size()));
    std::printf(
        "---------------------------------------------------------------------------------------------------\n");

    // 1. C++20 Core Lexical Parsing
    timeOperation("C++20 parse (form + slots)", [&](const std::string& m) {
        (void)razobill::core::parse(m, slots);
        return slots.ints.size() + slots.strs.size();
    });

    // 2. C++20 Parsing + Form Key String Generation
    timeOperation("C++20 parse + formKey", [&](const std::string& m) {
        (void)razobill::core::parse(m, slots);
        slots.formKey(formKey);
        return formKey.size();
    });

    // 3. C++20 Parsing + Form Key + XXH64 Hashing
    timeOperation("C++20 parse + formKey + xxh64", [&](const std::string& m) {
        (void)razobill::core::parse(m, slots);
        slots.formKey(formKey);
        return razobill::core::xxh64(formKey) & 1;
    });

    // 4. C++20 Full Round-Trip (Lossless Invertibility)
    timeOperation("C++20 parse + reconstruct (round-trip)", [&](const std::string& m) {
        (void)razobill::core::parse(m, slots);
        slots.formKey(formKey);
        (void)razobill::core::reconstruct(formKey, slots.ints, slots.strs, reconstructed);
        return reconstructed.size();
    });

    // 5. Universal C ABI: RzSplit
    timeOperation("C ABI RzSplit", [&](const std::string& m) {
        RzRecord r{};
        (void)RzSplit(m.data(), m.size(), &r);
        std::size_t count = r.intCount + r.strCount;
        RzFree(&r);
        return count;
    });

    // 6. Universal C ABI: RzSplit + RzJoin (round-trip)
    timeOperation("C ABI RzSplit + RzJoin (round-trip)", [&](const std::string& m) {
        RzRecord r{};
        (void)RzSplit(m.data(), m.size(), &r);
        std::size_t outLen = 0;
        RzStatus st = RzJoin(&r, buffer, sizeof(buffer), &outLen);
        if (st == RZ_ERR_BUFFER_TOO_SMALL && outLen > sizeof(buffer)) {
            std::vector<char> dynBuf(outLen);
            (void)RzJoin(&r, dynBuf.data(), dynBuf.size(), &outLen);
        }
        RzFree(&r);
        return outLen;
    });

    // 7. Universal C ABI: RzTemplate
    timeOperation("C ABI RzTemplate", [&](const std::string& m) {
        std::size_t outLen = 0;
        RzStatus st = RzTemplate(m.data(), m.size(), buffer, sizeof(buffer), &outLen);
        if (st == RZ_ERR_BUFFER_TOO_SMALL && outLen > sizeof(buffer)) {
            std::vector<char> dynBuf(outLen);
            (void)RzTemplate(m.data(), m.size(), dynBuf.data(), dynBuf.size(), &outLen);
        }
        return outLen;
    });

    // 9. Hardware SIMD Whitespace Scanner (AVX2 / ARM NEON multi-dispatch)
    timeOperation("SIMD findWhitespace (AVX2 / NEON)",
                  [&](const std::string& m) { return razobill::core::simd::findWhitespace(m.data(), m.size()); });

    // 10. Hardware SIMD Dynamic CPUID Dispatcher
    timeOperation("SIMD findWhitespaceDynamic (CPUID)", [&](const std::string& m) {
        return razobill::core::simd::findWhitespaceDynamic(m.data(), m.size());
    });

    // 11. Scalar Whitespace Scanner Fallback
    timeOperation("Scalar std::isspace delimiter check", [&](const std::string& m) {
        std::size_t idx = 0;
        while (idx < m.size() && !std::isspace(static_cast<unsigned char>(m[idx]))) {
            ++idx;
        }
        return idx;
    });

    // 12. XXH64 Raw Message Hash Theoretical Ceiling
    timeOperation("XXH64 raw message hash baseline",
                  [&](const std::string& m) { return razobill::core::xxh64(m) & 1; });

    // 13. Multi-Core Scaling Benchmarks (C++20 ThreadPool::parallelFor)
    std::printf(
        "---------------------------------------------------------------------------------------------------\n");
    std::printf("MULTI-CORE PARALLEL SCALING (C++20 ThreadPool::parallelFor, Coarse Chunks)\n");
    std::printf(
        "---------------------------------------------------------------------------------------------------\n");
    const std::vector<std::size_t> threadCounts = {1, 2, 4, 6, 12};
    double baselineThroughput = 0.0;
    std::atomic<std::size_t> parallelSink{0};
    for (std::size_t numThreads : threadCounts) {
        razobill::core::ThreadPool pool(numThreads);
        const std::size_t chunkSize = std::max<std::size_t>(1000, (msgs.size() + numThreads - 1) / numThreads);
        double bestSeconds = 1e300;
        for (int rep = 0; rep < 5; ++rep) {
            const auto t0 = std::chrono::steady_clock::now();
            pool.parallelFor(msgs.size(), chunkSize, [&](std::size_t start, std::size_t end) {
                razobill::core::SlotRecord localSlots;
                std::size_t localSink = 0;
                for (std::size_t i = start; i < end; ++i) {
                    (void)razobill::core::parse(msgs[i], localSlots);
                    localSink += localSlots.ints.size() + localSlots.strs.size();
                }
                parallelSink.fetch_add(localSink, std::memory_order_relaxed);
            });
            const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            bestSeconds = std::min(bestSeconds, elapsed);
        }
        const double totalMsgs = static_cast<double>(msgs.size());
        const double mbPerSec = (static_cast<double>(totalBytes) / bestSeconds) / 1e6;
        const double megaMsgsPerSec = (totalMsgs / bestSeconds) / 1e6;
        if (numThreads == 1) baselineThroughput = megaMsgsPerSec;
        const double speedup = baselineThroughput > 0 ? (megaMsgsPerSec / baselineThroughput) : 1.0;
        std::printf("%2zu Cores (%2zu Threads)                 %7.1f MB/s  %6.2f M msg/s  (Speedup %5.2fx, sink %zu)\n",
                    numThreads, numThreads, mbPerSec, megaMsgsPerSec, speedup,
                    parallelSink.load(std::memory_order_relaxed) & 1);
    }

    std::printf(
        "===================================================================================================\n");
    return 0;
}
