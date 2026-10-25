// =============================================================================
// unit test suite for Razobill Universal C ABI
//
// verifies RzSplit, RzJoin, RzTemplate, RzFree, and memory safety invariants
// across FFI boundary, buffer probing, escaping, and defensive validation
// =============================================================================

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "doctest.h"
#include "librazobill/RazobillC.h"

// pins round-trip byte identity, capacity probing, and RzFree memory release
TEST_CASE("Universal C ABI: RzSplit and RzJoin roundtrip") {
    const char* msg = "Failed password for root from 192.168.1.1 port 22";
    const size_t len = std::strlen(msg);

    RzRecord record{};
    RzStatus status = RzSplit(msg, len, &record);
    REQUIRE(status == RZ_OK);
    CHECK(record.templateStr != nullptr);
    CHECK(record.templateLength > 0);
    CHECK(record.intCount == 1);
    CHECK(record.intValues[0] == 22);
    CHECK(record.strCount == 2);

    // verify template content
    std::string tmpl(record.templateStr, record.templateLength);
    CHECK(tmpl == "Failed password for {s} from {s} port {i}");

    // buffer capacity probing with nullptr buffer
    size_t requiredLen = 0;
    status = RzJoin(&record, nullptr, 0, &requiredLen);
    REQUIRE(status == RZ_ERR_BUFFER_TOO_SMALL);
    CHECK(requiredLen == len);

    // exact buffer fill: buffer is exactly len bytes, no space for trailing \0
    std::vector<char> outBuf(len, 'X');
    size_t outLen = 0;
    status = RzJoin(&record, outBuf.data(), outBuf.size(), &outLen);
    REQUIRE(status == RZ_OK);
    CHECK(outLen == len);
    CHECK(std::string(outBuf.data(), outLen) == msg);

    // template extraction function
    std::vector<char> tmplBuf(tmpl.size());
    size_t tmplLen = 0;
    status = RzTemplate(msg, len, tmplBuf.data(), tmplBuf.size(), &tmplLen);
    REQUIRE(status == RZ_OK);
    CHECK(tmplLen == tmpl.size());
    CHECK(std::string(tmplBuf.data(), tmplLen) == tmpl);

    RzFree(&record);
    CHECK(record.templateStr == nullptr);
    CHECK(record.internalHandle == nullptr);
}

// pins literal brace escaping {{ and }} during C ABI template generation
TEST_CASE("Universal C ABI: Literal brace escaping in templates") {
    const char* msg = "Payload {action: login} port=443 {status}";
    const size_t len = std::strlen(msg);

    RzRecord record{};
    RzStatus status = RzSplit(msg, len, &record);
    REQUIRE(status == RZ_OK);

    std::string tmpl(record.templateStr, record.templateLength);
    // {action: login} -> {action: {s}} inside {{ ... }}
    CHECK(tmpl.find("{{") != std::string::npos);
    CHECK(tmpl.find("}}") != std::string::npos);

    // full roundtrip byte-exact fidelity
    std::vector<char> reconstructed(len);
    size_t recLen = 0;
    status = RzJoin(&record, reconstructed.data(), reconstructed.size(), &recLen);
    REQUIRE(status == RZ_OK);
    CHECK(recLen == len);
    CHECK(std::string(reconstructed.data(), recLen) == msg);

    RzFree(&record);
}

// pins external manual RzRecord construction without internal heap handle
TEST_CASE("Universal C ABI: FFI Caller manually populating record for join") {
    // foreign caller constructs record from scratch without calling RzSplit
    const char* tmpl = "System status: {s}, code={i}";
    const int64_t ints[] = {404};
    const char* strVal = "NOT_FOUND";
    const size_t strLen = std::strlen(strVal);
    const char* const strs[] = {strVal};
    const size_t strLens[] = {strLen};

    RzRecord record{};
    record.templateStr = tmpl;
    record.templateLength = std::strlen(tmpl);
    record.intValues = ints;
    record.intCount = 1;
    record.strValues = strs;
    record.strLengths = strLens;
    record.strCount = 1;
    record.internalHandle = nullptr; // manual FFI record

    char outBuf[128];
    size_t outLen = 0;
    RzStatus status = RzJoin(&record, outBuf, sizeof(outBuf), &outLen);
    REQUIRE(status == RZ_OK);
    CHECK(std::string(outBuf, outLen) == "System status: NOT_FOUND, code=404");

    // RzFree is safe on manually constructed or non-allocated records
    RzFree(&record);
}

// pins null argument validation, defensive pointer checks, and ABI version
TEST_CASE("Universal C ABI: Edge cases, empty string, errors and metadata") {
    // empty message handling
    RzRecord emptyRecord{};
    RzStatus status = RzSplit("", 0, &emptyRecord);
    REQUIRE(status == RZ_OK);
    CHECK(emptyRecord.templateLength == 0);
    CHECK(emptyRecord.intCount == 0);
    CHECK(emptyRecord.strCount == 0);

    char emptyBuf[8];
    size_t emptyLen = 99;
    status = RzJoin(&emptyRecord, emptyBuf, sizeof(emptyBuf), &emptyLen);
    REQUIRE(status == RZ_OK);
    CHECK(emptyLen == 0);
    RzFree(&emptyRecord);

    // null argument handling
    CHECK(RzSplit(nullptr, 10, &emptyRecord) == RZ_ERR_INVALID_ARGUMENT);
    CHECK(RzSplit("abc", 3, nullptr) == RZ_ERR_INVALID_ARGUMENT);
    CHECK(RzJoin(nullptr, emptyBuf, sizeof(emptyBuf), &emptyLen) == RZ_ERR_INVALID_ARGUMENT);

    // status message strings
    CHECK(std::string(RzStatusMessage(RZ_OK)) == "Success");
    CHECK(std::string(RzStatusMessage(RZ_ERR_BUFFER_TOO_SMALL)).find("insufficient") != std::string::npos);

    // count > 0 with null pointer returns invalid argument
    RzRecord invalidRecord{};
    invalidRecord.templateStr = "test {i}";
    invalidRecord.templateLength = 8;
    invalidRecord.intCount = 1;
    invalidRecord.intValues = nullptr; // invalid
    CHECK(RzJoin(&invalidRecord, emptyBuf, sizeof(emptyBuf), &emptyLen) == RZ_ERR_INVALID_ARGUMENT);

    invalidRecord.intCount = 0;
    invalidRecord.strCount = 1;
    invalidRecord.strValues = nullptr; // invalid
    invalidRecord.strLengths = nullptr;
    CHECK(RzJoin(&invalidRecord, emptyBuf, sizeof(emptyBuf), &emptyLen) == RZ_ERR_INVALID_ARGUMENT);

    const char* dummyStr = "hello";
    invalidRecord.strValues = &dummyStr;
    invalidRecord.strLengths = nullptr; // invalid
    CHECK(RzJoin(&invalidRecord, emptyBuf, sizeof(emptyBuf), &emptyLen) == RZ_ERR_INVALID_ARGUMENT);

    // version and ABI contract query
    CHECK(std::string(RzVersion()) == "1.1.8");
    CHECK(RzAbiVersion() == 1);
}

TEST_CASE("Universal C ABI: zero-length probe and contract edge cases") {
    RzRecord rec;
    std::memset(&rec, 0, sizeof(rec));
    REQUIRE(RzSplit("", 0, &rec) == RZ_OK);
    size_t reqLen = 999;
    CHECK(RzJoin(&rec, nullptr, 0, &reqLen) == RZ_OK);
    CHECK(reqLen == 0);
    RzFree(&rec);

    // Defensive reuse without RzFree
    std::memset(&rec, 0, sizeof(rec));
    REQUIRE(RzSplit("test 123", 8, &rec) == RZ_OK);
    REQUIRE(RzSplit("second 456", 10, &rec) == RZ_OK);
    RzFree(&rec);

    // Probe RzTemplate with 0-capacity
    reqLen = 999;
    CHECK(RzTemplate("", 0, nullptr, 0, &reqLen) == RZ_OK);
    CHECK(reqLen == 0);
}

TEST_CASE("capi: corrupt manual record rejections") {
    // 1. Template length exceeding kMaxMessageBytes (16 MiB)
    {
        RzRecord rec{};
        const char dummy[] = "short";
        rec.templateStr = dummy;
        rec.templateLength = (16ULL << 20) + 1;
        size_t outLen = 0;
        CHECK(RzJoin(&rec, nullptr, 0, &outLen) == RZ_ERR_MESSAGE_TOO_LARGE);
    }

    // 2. Disproportionate slot count relative to template length
    {
        RzRecord rec{};
        const char tmpl[] = "{i}";
        int64_t ints[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        rec.templateStr = tmpl;
        rec.templateLength = sizeof(tmpl) - 1; // 3 bytes -> max possible slots is 2
        rec.intValues = ints;
        rec.intCount = 10;
        size_t outLen = 0;
        CHECK(RzJoin(&rec, nullptr, 0, &outLen) == RZ_ERR_COUNT_MISMATCH);
    }

    // 3. String length exceeding kMaxMessageBytes
    {
        RzRecord rec{};
        const char tmpl[] = "{s}";
        const char* sPtr = "hello";
        size_t sLen = (16ULL << 20) + 10;
        rec.templateStr = tmpl;
        rec.templateLength = sizeof(tmpl) - 1;
        rec.strValues = &sPtr;
        rec.strLengths = &sLen;
        rec.strCount = 1;
        size_t outLen = 0;
        CHECK(RzJoin(&rec, nullptr, 0, &outLen) == RZ_ERR_MESSAGE_TOO_LARGE);
    }
}

TEST_CASE("capi: record reuse retains capacity") {
    RzRecord rec{};
    const char msg1[] = "msg one 100";
    REQUIRE(RzSplit(msg1, sizeof(msg1) - 1, &rec) == RZ_OK);
    void* handle1 = rec.internalHandle;
    CHECK(handle1 != nullptr);
    CHECK(rec.intCount == 1);
    CHECK(rec.intValues[0] == 100);

    // Reuse without calling RzFree: internal handle memory should be preserved
    const char msg2[] = "msg two 200 and three 300";
    REQUIRE(RzSplit(msg2, sizeof(msg2) - 1, &rec) == RZ_OK);
    CHECK(rec.internalHandle == handle1);
    CHECK(rec.intCount == 2);
    CHECK(rec.intValues[0] == 200);
    CHECK(rec.intValues[1] == 300);

    RzFree(&rec);
    CHECK(rec.internalHandle == nullptr);
}

TEST_CASE("capi: RzFree safety with nullptr and zeroed record") {
    // Calling RzFree with nullptr must not crash
    RzFree(nullptr);

    // Calling RzFree with zero-initialized record must not crash
    RzRecord zeroRec{};
    RzFree(&zeroRec);
    CHECK(zeroRec.templateStr == nullptr);
}

TEST_CASE("capi: binary payload with embedded NUL roundtrips losslessly") {
    const std::string binMsg = std::string("Header\0 data 12345 \0trailer", 27);

    RzRecord rec{};
    REQUIRE(RzSplit(binMsg.data(), binMsg.size(), &rec) == RZ_OK);
    CHECK(rec.intCount == 1);
    CHECK(rec.intValues[0] == 12345);

    char outBuf[128];
    size_t outLen = 0;
    REQUIRE(RzJoin(&rec, outBuf, sizeof(outBuf), &outLen) == RZ_OK);
    CHECK(outLen == binMsg.size());
    CHECK(std::memcmp(outBuf, binMsg.data(), outLen) == 0);

    RzFree(&rec);
}

TEST_CASE("capi: legacy form key with 5 or more slots reconstructs successfully") {
    // Legacy form key uses \x12 placeholder and \x13 type separator
    // For 5 integer slots, length is 11 bytes: 5 * \x12 + \x13 + 5 * 'I'
    std::string legacyTmpl = "\x12\x12\x12\x12\x12\x13IIIII";
    int64_t ints[5] = {10, 20, 30, 40, 50};

    RzRecord rec{};
    rec.templateStr = legacyTmpl.data();
    rec.templateLength = legacyTmpl.size();
    rec.intValues = ints;
    rec.intCount = 5;

    char outBuf[64];
    size_t outLen = 0;
    REQUIRE(RzJoin(&rec, outBuf, sizeof(outBuf), &outLen) == RZ_OK);
    CHECK(std::string_view(outBuf, outLen) == "1020304050");
}

TEST_CASE("capi: multithreaded concurrent split and join") {
    constexpr int kNumThreads = 4;
    constexpr int kItersPerThread = 500;
    std::vector<std::thread> workers;
    std::atomic<bool> allPassed{true};

    for (int t = 0; t < kNumThreads; ++t) {
        workers.emplace_back([t, &allPassed]() {
            for (int i = 0; i < kItersPerThread; ++i) {
                std::string msg = "Thread " + std::to_string(t) + " event " + std::to_string(i) + " status ok";
                RzRecord rec{};
                if (RzSplit(msg.data(), msg.size(), &rec) != RZ_OK) {
                    allPassed = false;
                    break;
                }
                char buf[256];
                size_t outLen = 0;
                if (RzJoin(&rec, buf, sizeof(buf), &outLen) != RZ_OK || outLen != msg.size() ||
                    std::memcmp(buf, msg.data(), outLen) != 0) {
                    allPassed = false;
                    RzFree(&rec);
                    break;
                }
                RzFree(&rec);
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }
    CHECK(allPassed.load());
}
