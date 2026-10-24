#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

// =============================================================================
//  file : tests/UnitCore.cpp
//  role : core unit test suite for lossless roundtrip and slot invariants
//  inv  : reconstruct(tmpl, ints, strs) == original for all test cases
//  deps : "doctest.h", "librazobill/Slots.hpp"
//  ~    : roundtripping 300,000 randomized byte runs to prove the iron law
// =============================================================================
#include <ostream>
#include <random>
#include <string>

#include "doctest.h"
#include "librazobill/Dialect.hpp"
#include "librazobill/SimdScanner.hpp"
#include "librazobill/Slots.hpp"
#include "librazobill/ThreadPool.hpp"
#include "librazobill/Version.hpp"

// pins core invariants: exact invertibility, slot bounds, scanner parity
using namespace razobill;
using namespace razobill::core;

namespace {
// helper performing roundtrip split and join, asserting join(split(msg)) == msg
std::string roundtrip(std::string_view msg, std::string* formOut = nullptr, ScanMode mode = ScanMode::DigitOnly) {
    SlotRecord s;
    REQUIRE(parse(msg, s, mode) == Status::Ok);
    std::string form = s.formKey();
    std::string rebuilt;
    REQUIRE(reconstruct(form, s.ints, s.strs, rebuilt) == Status::Ok);
    if (formOut) *formOut = form;
    return rebuilt;
}
} // namespace

// -- core round-trip and syntax verification ----------------------------------

// pins exact reconstruction and slot extraction for standard tokens
TEST_CASE("form keys of ordinary messages") {
    std::string f;
    CHECK(roundtrip("Receiving block blk_-1608999687919862906 src: /10.250.19.102:54106 dest: /10.250.19.102:50010",
                    &f) ==
          "Receiving block blk_-1608999687919862906 src: /10.250.19.102:54106 dest: /10.250.19.102:50010");
    CHECK(f == "Receiving block {s} src: /{s}:{i} dest: /{s}:{i}");
    CHECK(roundtrip("") == "");
    CHECK(roundtrip("no digits at all", &f) == "no digits at all");
    CHECK(f == "no digits at all");
    roundtrip("12345", &f);
    CHECK(f == "{i}");
}

// pins semantic variable extraction for keywords, key-values, and quotes
TEST_CASE("semantic text variables without digits eliminate over-splitting") {
    std::string f1, f2, f3;
    // Contextual variable (following 'for'):
    CHECK(roundtrip("Failed password for alice from 10.0.0.1 port 22", &f1, ScanMode::Semantic) ==
          "Failed password for alice from 10.0.0.1 port 22");
    CHECK(roundtrip("Failed password for bob from 10.0.0.1 port 22", &f2, ScanMode::Semantic) ==
          "Failed password for bob from 10.0.0.1 port 22");
    CHECK(roundtrip("Failed password for root from 10.0.0.1 port 22", &f3, ScanMode::Semantic) ==
          "Failed password for root from 10.0.0.1 port 22");
    CHECK(f1 == "Failed password for {s} from {s} port {i}");
    CHECK(f1 == f2);
    CHECK(f1 == f3);

    // Key-value text variables:
    std::string kv1, kv2;
    CHECK(roundtrip("auth failure; user=alice rhost=serverA", &kv1, ScanMode::Semantic) ==
          "auth failure; user=alice rhost=serverA");
    CHECK(roundtrip("auth failure; user=bob rhost=serverB", &kv2, ScanMode::Semantic) ==
          "auth failure; user=bob rhost=serverB");
    CHECK(kv1 == "auth failure; user={s} rhost={s}");
    CHECK(kv1 == kv2);

    // Quoted strings:
    std::string q1, q2;
    CHECK(roundtrip("error: \"file not found\" in job", &q1, ScanMode::Semantic) == "error: \"file not found\" in job");
    CHECK(roundtrip("error: \"permission denied\" in job", &q2, ScanMode::Semantic) ==
          "error: \"permission denied\" in job");
    CHECK(q1 == q2);
    CHECK(q1 == "error: {s} in job");
}

// pins integer token boundaries: 1-18 digits, no leading zeros or signs
TEST_CASE("integer rule") {
    for (auto [tok, isInt] : std::initializer_list<std::pair<const char*, bool>>{{"0", true},
                                                                                 {"7", true},
                                                                                 {"123456789012345678", true},
                                                                                 {"1234567890123456789", false},
                                                                                 {"00", false},
                                                                                 {"007", false},
                                                                                 {"-5", false},
                                                                                 {"+5", false},
                                                                                 {"1.5", false},
                                                                                 {"1e5", false},
                                                                                 {"0x1f", false},
                                                                                 {"12ab", false}})
        CHECK_MESSAGE(isIntToken(tok) == isInt, tok);
}

// pins delimiter matching for RE2 whitespace and punctuation classes
TEST_CASE("delimiters follow the RE2 class") {
    std::string f;
    roundtrip("a=1,b=2;c:3 (4) [5] {6} \"7\" '8' <9> |10| /11/", &f);
    CHECK(f == "a={i},b={i};c:{i} ({i}) [{i}] {{{i}}} \"{i}\" '{i}' <{i}> |{i}| /{i}/");
    roundtrip("a\v1 b\xC2\xA0"
              "2",
              &f);
    CHECK(f == "{s} {s}");
}

// pins payload transparency for internal framing bytes 0x11..0x13
TEST_CASE("escape hatch for reserved bytes") {
    for (const char* m : {"x \x11 y 1", "x \x12 y 1", "x \x13 y 1"}) {
        std::string f;
        CHECK(roundtrip(m, &f) == m);
    }
}

// pins lossless round-trip on empty, 1MB, embedded NUL, and UTF-8 inputs
TEST_CASE("empty and adversarial inputs") {
    CHECK(roundtrip(std::string(1 << 20, '1')).size() == (1u << 20));
    CHECK(roundtrip(std::string(100000, ' ') + "1").size() == 100001);
    CHECK(roundtrip(std::string("a\0b\0 1", 6)).size() == 6);
    const std::string utf8 = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E 42 \xC3\xBC"
                             "n\xC3\xAF"
                             "c3";
    CHECK(roundtrip(utf8) == utf8);
}

// pins 16 MiB message size ceiling enforcement on parse and reconstruction
TEST_CASE("size limits") {
    SlotRecord s;
    std::string big(kMaxMessageBytes + 1, 'a');
    CHECK(parse(big, s) == Status::TooLarge);
    std::string out;
    const std::string_view huge(big.data(), kMaxMessageBytes);
    std::string form("\x12\x13S");
    std::vector<std::string_view> strs{huge};
    CHECK(reconstruct(form, {}, strs, out) == Status::Ok);
    std::string form2("\x12\x12\x13SS");
    std::vector<std::string_view> two{huge, huge};
    CHECK(reconstruct(form2, {}, two, out) == Status::TooLarge);
}

// pins malformed template detection and slot underflow/overflow bounds
TEST_CASE("reconstruct rejects corrupt input without touching memory it should not") {
    std::string out;
    std::vector<std::int64_t> ints{1};
    std::vector<std::string_view> strs{"a"};
    std::vector<std::string_view> noStrs{};
    CHECK(reconstruct("no separator", ints, strs, out) == Status::Malformed);
    CHECK(reconstruct(std::string("\x12\x13X"), ints, strs, out) == Status::Malformed);
    CHECK(reconstruct(std::string("\x12\x12\x13I"), ints, strs, out) == Status::Malformed);
    CHECK(reconstruct(std::string("\x12\x13II"), ints, strs, out) == Status::Malformed);
    CHECK(reconstruct(std::string("\x12\x12\x13II"), ints, strs, out) == Status::SlotUnderflow);
    CHECK(reconstruct(std::string("\x12\x12\x13SS"), ints, strs, out) == Status::SlotUnderflow);
    std::vector<std::int64_t> neg{-1};
    CHECK(reconstruct(std::string("\x12\x13I"), neg, noStrs, out) == Status::Malformed);
    CHECK(reconstruct(std::string("\x12\x13I"), ints, noStrs, out) == Status::Ok);
    CHECK(out == "1");
    // excess string or int parameters produce SlotOverflow under contract
    CHECK(reconstruct(std::string("\x12\x13I"), ints, strs, out) == Status::SlotOverflow);
    std::vector<std::int64_t> padded{5, 99, 99};
    CHECK(reconstruct(std::string("a \x12\x13I"), padded, noStrs, out) == Status::SlotOverflow);
    CHECK(reconstruct("a {i}", padded, noStrs, out) == Status::SlotOverflow);
}

// pins templateOf conversion to <*> with brace unescape and validation
TEST_CASE("templateOf") {
    std::string t;
    // Legacy format
    CHECK(templateOf(std::string("Failed password for \x12 from \x12 port \x12\x13SSI"), t) == Status::Ok);
    CHECK(t == "Failed password for <*> from <*> port <*>");
    // New format with {i} and {s} placeholders
    CHECK(templateOf("Failed password for {s} from {s} port {i}", t) == Status::Ok);
    CHECK(t == "Failed password for <*> from <*> port <*>");
    // Escaped braces unescaping
    CHECK(templateOf("Object {{key}} has value {i}", t) == Status::Ok);
    CHECK(t == "Object {key} has value <*>");
    // Plain string without placeholders
    CHECK(templateOf("system initialized successfully", t) == Status::Ok);
    CHECK(t == "system initialized successfully");
    // malformed syntax: unclosed brace, invalid placeholder, stray brace
    CHECK(templateOf("junk {", t) == Status::Malformed);
    CHECK(templateOf("junk {x}", t) == Status::Malformed);
    CHECK(templateOf("junk }", t) == Status::Malformed);
    CHECK(templateOf(std::string("bad \x12 char"), t) == Status::Malformed);
    CHECK(templateOf(std::string("bad \x13 char"), t) == Status::Malformed);
}

// pins bitwise trailing zero count across boundary masks (0, 1, and MSB)
TEST_CASE("SimdScanner: countTrailingZeros64 zero-mask safety") {
    CHECK(razobill::core::simd::countTrailingZeros64(0) == 64);
    CHECK(razobill::core::simd::countTrailingZeros64(1) == 0);
    CHECK(razobill::core::simd::countTrailingZeros64(0x8000000000000000ULL) == 63);
}

// -- randomized property tests and differential verification ------------------

// pins bijection join(split(x)) == x across 300,000 randomized byte runs
TEST_CASE("property: parse then reconstruct is the identity (random structured messages)") {
    std::mt19937_64 rng(0xC0FFEE);
    const std::string alphabet = "abcXYZ0123456789 \t=,;:()[]{}\"'<>|/._-+\xC3\xA9\xE6\x97\xA5\x0B\x1F\x7F\x80\xFF";
    SlotRecord s;
    std::string form, rebuilt;
    for (int iter = 0; iter < 300000; ++iter) {
        std::string m(rng() % 64, ' ');
        for (auto& c : m)
            c = alphabet[rng() % alphabet.size()];
        if (iter % 997 == 0 && !m.empty()) m[rng() % m.size()] = static_cast<char>(0x11 + rng() % 3);
        REQUIRE(parse(m, s) == Status::Ok);
        form = s.formKey();
        REQUIRE(reconstruct(form, s.ints, s.strs, rebuilt) == Status::Ok);
        REQUIRE(rebuilt == m);
    }
}

// pins semantic bijection join(split(x)) == x across 300,000 random runs
TEST_CASE("property: parse then reconstruct is the identity (random structured messages, semantic mode)") {
    std::mt19937_64 rng(0xBEEFCAFE);
    const std::string alphabet = "abcXYZ0123456789 \t=,;:()[]{}\"'<>|/._-+\xC3\xA9\xE6\x97\xA5\x0B\x1F\x7F\x80\xFF";
    SlotRecord s;
    std::string form, rebuilt;
    for (int iter = 0; iter < 300000; ++iter) {
        std::string m(rng() % 64, ' ');
        for (auto& c : m)
            c = alphabet[rng() % alphabet.size()];
        if (iter % 997 == 0 && !m.empty()) m[rng() % m.size()] = static_cast<char>(0x11 + rng() % 3);
        REQUIRE(parse(m, s, ScanMode::Semantic) == Status::Ok);
        form = s.formKey();
        REQUIRE(reconstruct(form, s.ints, s.strs, rebuilt) == Status::Ok);
        REQUIRE(rebuilt == m);
    }
}

// pins template form invariance when slot variable values vary
TEST_CASE("property: forms are insensitive to slot values") {
    SlotRecord a, b;
    REQUIRE(parse("Receiving block blk_1 src: /10.0.0.1:1 dest: /10.0.0.2:2", a) == Status::Ok);
    REQUIRE(parse("Receiving block blk_99999 src: /192.168.1.1:50010 dest: /10.9.9.9:7", b) == Status::Ok);
    std::string fa = a.formKey();
    std::string fb = b.formKey();
    CHECK(fa == fb);
}

// pins semantic version metadata and ABI version constancy
TEST_CASE("semantic versioning and ABI contract") {
    auto v = razobill::version();
    CHECK(v.major == 1);
    CHECK(v.minor == 1);
    CHECK(v.patch == 4);
    CHECK(v.abi == 1);
    CHECK(std::string_view(v.string) == "1.1.4");
    CHECK(razobill::abiVersion() == 1);
    CHECK(razobill::versionString() == "1.1.4");
}

// pins SIMD delimiter scanning on >64 byte inputs across instruction sets
TEST_CASE("hardware vector SIMD scanner and dynamic CPUID dispatcher") {
    using namespace razobill::core::simd;
    SimdArchitecture arch = detectSimdArchitecture();
    CHECK(static_cast<int>(arch) >= 0);

    // Test delimiter scans on strings > 64 bytes
    std::string test64(70, 'a');
    test64[60] = ' ';
    CHECK(findWhitespaceDynamic(test64.data(), test64.size()) == 60);

    // Test multiple delimiters across 64-byte block
    std::string test128(130, 'x');
    test128[35] = '\t';
    test128[90] = '\n';
    CHECK(findWhitespaceDynamic(test128.data(), test128.size()) == 35);
    CHECK(findWhitespaceDynamic(test128.data() + 36, test128.size() - 36) == 54);

    // Test unified multi-dispatcher
    CHECK(findWhitespace(test64.data(), test64.size()) == 60);
    CHECK(findWhitespace(test128.data(), test128.size()) == 35);
}

// pins differential identity between scalar and SIMD delimiter scanners
TEST_CASE("differential scalar vs SIMD delimiter scanner verification") {
    using namespace razobill::core::simd;
    auto scalarFind = [](const char* data, std::size_t len) -> std::size_t {
        for (std::size_t i = 0; i < len; ++i) {
            char c = data[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return i;
        }
        return len;
    };

    std::mt19937_64 rng(0x12345678);
    const char delims[] = {' ', '\t', '\n', '\r'};

    for (std::size_t len = 0; len <= 160; ++len) {
        std::string s(len, 'a');
        CHECK(findWhitespaceDynamic(s.data(), s.size()) == scalarFind(s.data(), s.size()));
        CHECK(findWhitespace(s.data(), s.size()) == scalarFind(s.data(), s.size()));

        // probe delimiter across every byte offset (tests vector chunk edges)
        for (std::size_t pos = 0; pos < len; ++pos) {
            char orig = s[pos];
            for (char d : delims) {
                s[pos] = d;
                CHECK(findWhitespaceDynamic(s.data(), s.size()) == scalarFind(s.data(), s.size()));
                CHECK(findWhitespace(s.data(), s.size()) == scalarFind(s.data(), s.size()));
            }
            s[pos] = orig;
        }

        // Probe random combinations
        for (int trial = 0; trial < 5; ++trial) {
            std::string randStr(len, 'x');
            for (std::size_t i = 0; i < len; ++i) {
                if (rng() % 7 == 0) {
                    randStr[i] = delims[rng() % 4];
                } else {
                    randStr[i] = static_cast<char>('A' + (rng() % 26));
                }
            }
            CHECK(findWhitespaceDynamic(randStr.data(), randStr.size()) == scalarFind(randStr.data(), randStr.size()));
            CHECK(findWhitespace(randStr.data(), randStr.size()) == scalarFind(randStr.data(), randStr.size()));
        }
    }
}

// pins parser recovery and lossless round-trip on unclosed or malformed quotes
TEST_CASE("unclosed quotes with escaped and unescaped characters roundtrip") {
    // regression test for fuzzer timeout on unclosed quotes with trailing chars
    const std::string pathological = "\xca'\xff\xff\\'";
    CHECK(roundtrip(pathological, nullptr, ScanMode::Semantic) == pathological);
    CHECK(roundtrip("unclosed 'quote with trailing", nullptr, ScanMode::Semantic) == "unclosed 'quote with trailing");
    CHECK(roundtrip("unclosed \"double quote with trailing", nullptr, ScanMode::Semantic) ==
          "unclosed \"double quote with trailing");
}

// pins exact round-trip for escaped quotes and trailing backslashes
TEST_CASE("quote-backslash edge cases and trailing backslashes") {
    const std::vector<std::string> cases = {
        "\"\\\"",
        "'\\'",
        "\"\\\"\"",
        "'\\''",
        "\"\\\\\"",
        "\"\\\\\\\"",
        "opened \"C:\\Users\\bob\\\" ok",
        "opened 'C:\\Users\\bob\\' ok",
        "foo \"bar\\\" baz",
        "\"\"\"",
        "'''",
        "\"\\\\\\\"\"",
    };
    for (const auto& s : cases) {
        CHECK(roundtrip(s, nullptr, ScanMode::Semantic) == s);
    }
}

// -- concurrency and dialect execution ----------------------------------------

// pins thread settlement and task cancellation on exception in parallelFor
TEST_CASE("ThreadPool: parallelFor exception safety and worker settlement") {
    razobill::core::ThreadPool pool(4);
    std::atomic<int> completedTasks{0};

    CHECK_THROWS_AS(pool.parallelFor(100, 10,
                                     [&](std::size_t start, std::size_t end) {
                                         if (start == 20) {
                                             throw std::runtime_error("Simulated task failure");
                                         }
                                         completedTasks.fetch_add(static_cast<int>(end - start));
                                     }),
                    std::runtime_error);

    // after exception is caught, worker tasks settle and pool remains usable
    std::atomic<int> nextTasks{0};
    pool.parallelFor(50, 10,
                     [&](std::size_t start, std::size_t end) { nextTasks.fetch_add(static_cast<int>(end - start)); });
    CHECK(nextTasks.load() == 50);
}

// pins SQL string quoting, NUL byte escaping, and template expressions
TEST_CASE("Dialect: quoting and SQL expression generation (DuckDB & ClickHouse)") {
    auto duck = makeDialect(Flavor::DuckDB);
    auto ch = makeDialect(Flavor::ClickHouse);
    REQUIRE(duck != nullptr);
    REQUIRE(ch != nullptr);

    CHECK(duck->flavor() == Flavor::DuckDB);
    CHECK(ch->flavor() == Flavor::ClickHouse);

    // Standard string quoting
    CHECK(duck->quote("hello") == "'hello'");
    CHECK(duck->quote("it's a test") == "'it''s a test'");
    CHECK(duck->quote("back\\slash") == "'back\\slash'");

    CHECK(ch->quote("hello") == "'hello'");
    CHECK(ch->quote("it's a test") == "'it\\'s a test'");
    CHECK(ch->quote("back\\slash") == "'back\\\\slash'");

    // NUL byte escaping in ClickHouse dialect (F-DIAL-01)
    std::string nulStr("hello\0world", 11);
    std::string chQuoted = ch->quote(nulStr);
    CHECK(chQuoted == "'hello\\0world'");

    // ClickHouse template expression substring underflow protection
    std::string tmplCh = ch->templateExpr("raw_form");
    CHECK(!tmplCh.empty());

    // DuckDB template expression
    std::string tmplDuck = duck->templateExpr("raw_form");
    CHECK(!tmplDuck.empty());

    // Type expressions
    CHECK(duck->tU64() == "UBIGINT");
    CHECK(ch->tU64() == "UInt64");
    CHECK(duck->supportsCompact() == true);
    CHECK(ch->supportsCompact() == false);
}

TEST_CASE("core: adversarial control byte format ambiguity round-trip") {
    const std::vector<std::string> hostile = {
        std::string("x\x12\x13I"),
        std::string("a\x12" "b\x12" "c\x13IS"),
        std::string("prefix\x11\x12\x13suffix"),
        std::string("\x11\x11"),
        std::string("\x12\x13I")
    };
    for (const auto& msg : hostile) {
        SlotRecord r;
        REQUIRE(parse(msg, r, ScanMode::Semantic) == Status::Ok);
        CHECK(!isLegacyFormKey(r.templateStr));
        std::string rec;
        REQUIRE(reconstruct(r.templateStr, r.ints, r.strs, rec) == Status::Ok);
        CHECK(rec == msg);
        std::string display;
        CHECK(templateOf(r.templateStr, display) == Status::Ok);
    }
}

TEST_CASE("table: accessors bounds checking and BackendError") {
    Table t;
    t.names = {"id", "val", "label"};
    t.rows = {
        {std::int64_t(42), double(3.14), std::string("test")},
        {std::monostate{}, std::uint64_t(100), std::monostate{}}
    };

    CHECK(t.i64(0, 0) == 42);
    CHECK(t.f64(0, 1) == 3.14);
    CHECK(t.str(0, 2) == "test");
    CHECK(!t.isNull(0, 0));
    CHECK(t.isNull(1, 0));
    CHECK(t.u64(1, 1) == 100);

    // Uniform BackendError on out of range
    CHECK_THROWS_AS((void)t.i64(2, 0), BackendError);
    CHECK_THROWS_AS((void)t.i64(0, 5), BackendError);
    CHECK_THROWS_AS((void)t.u64(5, 0), BackendError);
    CHECK_THROWS_AS((void)t.f64(0, 9), BackendError);
    CHECK_THROWS_AS((void)t.str(9, 9), BackendError);
    CHECK_THROWS_AS((void)t.isNull(9, 0), BackendError);

    // Type mismatch error
    CHECK_THROWS_AS((void)t.str(0, 0), BackendError);
    CHECK_THROWS_AS((void)t.i64(0, 2), BackendError);
}

