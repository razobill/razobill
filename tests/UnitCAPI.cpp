// =============================================================================
// unit test suite for Razobill Universal C ABI
//
// verifies RzSplit, RzJoin, RzTemplate, RzFree, and memory safety invariants
// across FFI boundary, buffer probing, escaping, and defensive validation
// =============================================================================

#include <cstring>
#include <string>
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
    CHECK(std::string(RzVersion()) == "1.1.4");
    CHECK(RzAbiVersion() == 1);
}
