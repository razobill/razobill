// =============================================================================
// unit test suite for memory management planes and concurrency primitives
//
// verifies MemoryArena alignment, StringPool interning and GC, FormCatalog
// collision resilience, and ThreadPool chunked parallel work distribution
// =============================================================================

#include <atomic>
#include <ostream>
#include <string>
#include <thread>
#include <unordered_set>

#include "doctest.h"
#include "librazobill/FormCatalog.hpp"
#include "librazobill/MemoryArena.hpp"
#include "librazobill/StringPool.hpp"
#include "librazobill/ThreadPool.hpp"

using namespace razobill::core;

// pins 64-byte alignment and monotonic pointer progression without heap realloc
TEST_CASE("MemoryArena monotonic allocation and cache-line alignment") {
    // assert strictly that all returned pointers are multiples of 64 bytes
    // prevents AVX2 vector loads from suffering cache-line split penalties
    MemoryArena arena(4096);
    void* p1 = arena.allocate(100, 64);
    REQUIRE(p1 != nullptr);
    CHECK((reinterpret_cast<std::size_t>(p1) % 64) == 0);

    void* p2 = arena.allocate(200, 64);
    REQUIRE(p2 != nullptr);
    CHECK((reinterpret_cast<std::size_t>(p2) % 64) == 0);
    CHECK(p2 > p1);

    arena.reset();
    void* p3 = arena.allocate(100, 64);
    REQUIRE(p3 != nullptr);
    CHECK((reinterpret_cast<std::size_t>(p3) % 64) == 0);
}

// pins multi-block chunk growth and buffer retention across resets
TEST_CASE("MemoryArena multi-block chunk expansion and reset reuse") {
    // initial arena with 1024-byte default block size
    MemoryArena arena(1024);
    CHECK(arena.allocatedBytes() == 0);

    // first allocation lazily creates block 0 with capacity >= 1024
    void* p1 = arena.allocate(800, 64);
    REQUIRE(p1 != nullptr);
    CHECK(arena.capacity() >= 1024);

    // second allocation exceeds block 0 and forces block 1 creation
    void* p2 = arena.allocate(800, 64);
    REQUIRE(p2 != nullptr);
    CHECK(p2 != p1);
    const std::size_t capAfterGrowth = arena.capacity();
    CHECK(capAfterGrowth >= 2048);
    CHECK(arena.allocatedBytes() >= 1600);

    // reset rewinds to block 0 without freeing allocated block buffers
    arena.reset();
    CHECK(arena.allocatedBytes() == 0);
    CHECK(arena.capacity() == capAfterGrowth);

    // reallocate through block 0 and block 1 to certify reuse
    void* p3 = arena.allocate(800, 64);
    REQUIRE(p3 != nullptr);
    void* p4 = arena.allocate(800, 64);
    REQUIRE(p4 != nullptr);
    CHECK(arena.capacity() == capAfterGrowth);
}

TEST_CASE("MemoryArena trim releases excess blocks exceeding capacity threshold") {
    MemoryArena arena(1024);
    for (int i = 0; i < 10; ++i) {
        void* p = arena.allocate(1500, 64);
        REQUIRE(p != nullptr);
    }
    CHECK(arena.totalAllocatedBlocks() >= 10);
    const std::size_t capBefore = arena.capacity();
    CHECK(capBefore >= 15000);

    // trim retaining at most 2048 bytes
    arena.trim(2048);
    CHECK(arena.allocatedBytes() == 0);
    CHECK(arena.capacity() < capBefore);
    CHECK(arena.totalAllocatedBlocks() <= 2);
}

// pins deduplicating string interning, ID stability, and generational GC
TEST_CASE("StringPool interning and garbage collection") {
    StringPool pool;
    auto id0 = pool.intern("apple");
    auto id1 = pool.intern("banana");
    auto id2 = pool.intern("cherry");
    auto id0Again = pool.intern("apple");

    CHECK(id0 == id0Again);
    CHECK(id0 != id1);
    CHECK(pool.size() == 3);
    CHECK(pool.lookup(id0) == "apple");
    CHECK(pool.lookup(id1) == "banana");

    // retain only "banana" and "cherry"
    std::unordered_set<std::uint32_t> active{id1, id2};
    auto stats = pool.gc(active);
    CHECK(stats.totalBefore == 3);
    CHECK(stats.activeAfter == 2);
    CHECK(stats.purgedCount == 1);
    CHECK(pool.size() == 2);
}

// pins reader-writer thread safety during concurrent interning and lookups
TEST_CASE("StringPool concurrent lookup safety and withString accessor") {
    StringPool pool;
    auto id0 = pool.intern("thread_safe_token");
    std::string_view sv = pool.lookup(id0);
    CHECK(sv == "thread_safe_token");
    CHECK(pool.lookupCopy(id0) == "thread_safe_token");

    std::string outBuf;
    CHECK(pool.lookup(id0, outBuf));
    CHECK(outBuf == "thread_safe_token");

    bool inspected = false;
    CHECK(pool.withString(id0, [&](std::string_view v) { inspected = (v == "thread_safe_token"); }));
    CHECK(inspected);

    // invalid ID bounds check
    CHECK(pool.lookup(9999).empty());
    CHECK_FALSE(pool.lookup(9999, outBuf));
    CHECK_FALSE(pool.withString(9999, [](std::string_view) {}));

    // concurrent growth and lookup stress test
    std::atomic<bool> stop{false};
    std::atomic<bool> readerMatches{true};
    std::thread reader([&]() {
        while (!stop.load()) {
            std::string s = pool.lookupCopy(id0);
            if (s != "thread_safe_token") readerMatches.store(false);
            pool.withString(id0, [&](std::string_view val) {
                if (val != "thread_safe_token") readerMatches.store(false);
            });
        }
    });

    for (int i = 0; i < 1000; ++i) {
        pool.intern("token_" + std::to_string(i));
    }
    stop.store(true);
    reader.join();
    CHECK(readerMatches.load());
}

// pins template form key registration and atomic ID generation
TEST_CASE("FormCatalog thread-safe registration") {
    FormCatalog catalog;
    auto h1 = catalog.registerForm("test_key_1\x13I", "test_key_1 <*>", 1, 0);
    auto h2 = catalog.registerForm("test_key_2\x13S", "test_key_2 <*>", 0, 1);
    CHECK(catalog.contains(h1));
    CHECK(catalog.contains(h2));
    CHECK(catalog.contains("test_key_1\x13I"));
    CHECK(catalog.contains("test_key_2\x13S"));
    CHECK(catalog.size() == 2);
}

// pins full-key equality check preventing hash collision aliasing
TEST_CASE("FormCatalog full-key comparison and hash collision safety") {
    FormCatalog catalog;
    // two distinct keys simulating a 64-bit hash collision
    const std::uint64_t simulatedHash = 0xABCD1234EFEF9988ULL;
    catalog.registerFormWithHash("login_failed_user\x13S", "login failed for <*>", 0, 1, simulatedHash);
    catalog.registerFormWithHash("query_timeout_ms\x13I", "query timeout after <*> ms", 1, 0, simulatedHash);

    // in a hash-only keyed map, the second form would be dropped or aliased
    // under full-key comparison, both distinct forms must be preserved
    CHECK(catalog.size() == 2);
    CHECK(catalog.collisionCount() == 1);
    CHECK(catalog.contains("login_failed_user\x13S"));
    CHECK(catalog.contains("query_timeout_ms\x13I"));
    CHECK(catalog.contains(simulatedHash));

    FormEntry e1{}, e2{};
    REQUIRE(catalog.findForm("login_failed_user\x13S", e1));
    CHECK(e1.id == 1);
    CHECK(e1.templateStr == "login failed for <*>");
    CHECK(e1.strSlots == 1);

    REQUIRE(catalog.findForm("query_timeout_ms\x13I", e2));
    CHECK(e2.id == 2);
    CHECK(e2.templateStr == "query timeout after <*> ms");
    CHECK(e2.intSlots == 1);

    // verify dense unique ID lookup
    FormEntry byId1{}, byId2{};
    REQUIRE(catalog.findFormById(1, byId1));
    CHECK(byId1.key == "login_failed_user\x13S");
    REQUIRE(catalog.findFormById(2, byId2));
    CHECK(byId2.key == "query_timeout_ms\x13I");

    // verify collision queries
    CHECK(catalog.isCollisionHash(simulatedHash));
    std::vector<std::string> collidingKeys;
    REQUIRE(catalog.findKeysByHash(simulatedHash, collidingKeys));
    CHECK(collidingKeys.size() == 2);
    CHECK(collidingKeys[0] == "login_failed_user\x13S");
    CHECK(collidingKeys[1] == "query_timeout_ms\x13I");
}

// pins uniform work distribution across thread pool workers with chunk size
TEST_CASE("ThreadPool chunked parallelFor") {
    ThreadPool pool(4);
    constexpr std::size_t N = 10000;
    std::vector<int> data(N, 0);

    pool.parallelFor(N, 1000, [&](std::size_t start, std::size_t end) {
        for (std::size_t i = start; i < end; ++i) {
            data[i] = 1;
        }
    });

    for (std::size_t i = 0; i < N; ++i) {
        REQUIRE(data[i] == 1);
    }
}

// pins zero chunkSize fallback avoiding division by zero
TEST_CASE("ThreadPool parallelFor handles zero chunkSize safely") {
    ThreadPool pool(4);
    constexpr std::size_t N = 1000;
    std::vector<int> data(N, 0);

    // if unhandled, chunkSize == 0 causes hardware division-by-zero (SIGFPE)
    pool.parallelFor(N, 0, [&](std::size_t start, std::size_t end) {
        for (std::size_t i = start; i < end; ++i) {
            data[i] = 1;
        }
    });

    for (std::size_t i = 0; i < N; ++i) {
        REQUIRE(data[i] == 1);
    }
}

TEST_CASE("MemoryArena degenerate parameters and zero block size") {
    MemoryArena arenaZero(0);
    void* p = arenaZero.allocate(64, 8);
    REQUIRE(p != nullptr);
    CHECK(arenaZero.allocate(0, 8) == nullptr);
}
