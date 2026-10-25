// =============================================================================
// end-to-end DuckDB ADBC integration and invariant verification suite
//
// validates compact storage, adversarial edge cases, cross-engine conformance,
// semantic ledger identities, SQL injection hardening, concurrency, dictionary
// garbage collection, native UDFs vs SQL regexes, BLOBs, and transactions
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <atomic>
#include <cmath>
#include <random>
#include <set>
#include <thread>

#include "doctest.h"
#include "librazobill/Hash.hpp"
#include "librazobill/Razobill.hpp"
#include "librazobill/Slots.hpp"
#include "Support.hpp"

using namespace razobill;

namespace {
bool duckdbAvailable() {
    static const bool avail = [] {
        try {
            const auto opt = support::duckdbOptions();
            std::error_code ec;
            if (opt.driverPath.empty() || !std::filesystem::exists(opt.driverPath, ec)) return false;
            auto b = makeAdbcBackend(opt);
            return b != nullptr;
        } catch (...) {
            return false;
        }
    }();
    return avail;
}

std::string corpusPath() {
    const char* env = std::getenv("RZB_CORPUS_PARQUET");
    if (env && *env) return std::string(env);
    std::error_code ec;
    if (std::filesystem::exists("/tmp/corpus.parquet", ec)) return "/tmp/corpus.parquet";
    auto tmp = std::filesystem::temp_directory_path(ec);
    if (!ec) {
        auto p = tmp / "corpus.parquet";
        if (std::filesystem::exists(p, ec)) {
            std::string s = p.string();
            std::replace(s.begin(), s.end(), '\\', '/');
            return s;
        }
    }
    if (std::filesystem::exists("build/corpus.parquet", ec)) return "build/corpus.parquet";
    if (std::filesystem::exists("corpus.parquet", ec)) return "corpus.parquet";
    return "";
}

bool corpusAvailable() {
    return !corpusPath().empty();
}

constexpr const char* kRules[][2] = {{"HDFS_RECEIVING", R"(^Receiving block <\*> src: )"},
                                     {"SSH_PASSWORD_FAIL", "^Failed password for"},
                                     {"LINUX_AUTH_FAIL", "^authentication failure"}};

// A skipped integration test must never look like a passing one. With
// RZB_REQUIRE_INTEGRATION=1 (set this in CI) a missing driver or corpus fails the run.
bool skipIntegration(const char* why) {
    if (const char* req = std::getenv("RZB_REQUIRE_INTEGRATION"); req != nullptr && req[0] != '\0' && req[0] != '0') {
        FAIL("integration prerequisite missing and RZB_REQUIRE_INTEGRATION is set: ", why);
    }
    MESSAGE("SKIPPED (not passed): ", why);
    return true;
}
std::unique_ptr<Backend> open() {
    return makeAdbcBackend(support::duckdbOptions());
}
void loadRaw(Backend& b, const char* name = "raw") {
    b.exec(std::string("CREATE TABLE ") + name + " AS SELECT * FROM read_parquet('" + corpusPath() + "')");
}
} // namespace

// pins exact reconstruction over 134,815 events and appendable re-compact
TEST_CASE("compact mode: lossless, appendable, original columns restored") {
    if (!duckdbAvailable() || !corpusAvailable()) {
        if (skipIntegration("DuckDB driver or /tmp/corpus.parquet (tools/MakeCorpus.py) not present")) return;
    }
    auto b = open();
    loadRaw(*b);
    RazobillEngine rz(*b, {.table = "logs", .purpose = "level"});
    const auto n = rz.compact("raw");
    CHECK(n == 134815);
    CHECK(rz.verify("raw") == 0);
    CHECK(b->query("SELECT count(*) FROM logs").i64(0, 0) == 134815);
    b->exec("CREATE TABLE raw2 AS SELECT * FROM raw UNION ALL SELECT * FROM raw");
    CHECK(rz.compact("raw") == 2 * 134815);
    CHECK(rz.verify("raw2") == 0);
    support::result("duckdb.compact.rows", static_cast<double>(n));
}

// pins byte-level round-trip over unicode, nulls, huge ints, and sentinels
TEST_CASE("compact mode: adversarial rows survive") {
    if (!duckdbAvailable()) {
        if (skipIntegration("DuckDB driver not present")) return;
    }
    // robustness test:
    // boundary conditions: unicode, large integers, and ASCII control bytes
    auto b = open();
    b->exec("CREATE TABLE edge(id INTEGER, msg VARCHAR)");
    b->exec("INSERT INTO edge VALUES (0, ''), (1, NULL), (2, 'no digits'), (3, '007 0 00 -5 1.5 1e5'), (4, "
            "'9223372036854775807 99999999999999999999 1234567890123456789'), "
            "(5, 'ctrl ' || chr(17) || ' ' || chr(18) || ' ' || chr(19) || ' 42'), (6, 'tabs' || chr(9) || 'and  "
            "double  spaces   7'), (7, '日本語 ログ 42 ünïcödé3'), "
            "(8, repeat('x', 5000) || ' 77'), (9, ' leading 1 and trailing 2 '), (10, 'it''s a \"quoted\" 1')");
    RazobillEngine rz(*b, {.table = "e2", .purpose = ""});
    rz.compact("edge");
    CHECK(rz.verify("edge") == 0);
    CHECK(b->query("SELECT msg IS NULL FROM e2 WHERE id = 1").i64(0, 0) == 1);
    CHECK(b->query("SELECT msg = '' FROM e2 WHERE id = 0").i64(0, 0) == 1);
}

// pins bit-for-bit form key parity between DuckDB SQL functions and C++ core
TEST_CASE("every stored form equals the C++ core's form (cross-implementation conformance)") {
    if (!duckdbAvailable() || !corpusAvailable()) {
        if (skipIntegration("DuckDB driver or /tmp/corpus.parquet (tools/MakeCorpus.py) not present")) return;
    }
    auto b = open();
    loadRaw(*b);
    RazobillEngine rz(*b, {.table = "logs", .purpose = "level"});
    rz.compact("raw");
    const auto msgs = b->query("SELECT msg FROM raw");
    std::set<std::string> coreForms;
    core::SlotRecord s;
    std::string key;
    for (std::size_t r = 0; r < msgs.rows.size(); ++r) {
        REQUIRE(core::parse(msgs.str(r, 0), s) == Status::Ok);
        key = s.templateStr;
        coreForms.insert(key);
    }
    const auto db = b->query("SELECT form, id FROM logs_rb_form");
    std::set<std::string> dbForms;
    std::size_t badHash = 0;
    for (std::size_t r = 0; r < db.rows.size(); ++r) {
        dbForms.insert(db.str(r, 0));
        badHash += db.u64(r, 1) != core::xxh64(db.str(r, 0));
    }
    CHECK(dbForms == coreForms);
    CHECK(badHash == 0);
    support::result("duckdb.forms", static_cast<double>(dbForms.size()));
}

// pins Theorem 1 ledger identities and JSD gate under DuckDB SQL execution
TEST_CASE("meaning layer: gate, doubt, versions, ledger identities") {
    if (!duckdbAvailable() || !corpusAvailable()) {
        if (skipIntegration("DuckDB driver or /tmp/corpus.parquet (tools/MakeCorpus.py) not present")) return;
    }
    auto b = open();
    loadRaw(*b);
    RazobillEngine rz(*b, {.table = "logs", .purpose = "level"});
    rz.compact("raw");
    for (const auto& r : kRules) {
        const auto d = rz.define(r[0], r[1]);
        CHECK(d.lossPerMergedEvent < 1e-9);
    }

    CHECK_THROWS_AS(rz.define("FAILISH", "(?i)fail|error|exception"), GateRefused);
    try {
        rz.define("LINUX_AUTH_FAIL", "(?i)^failed password for");
        FAIL("expected GateRefused");
    } catch (const GateRefused& e) {
        CHECK(e.jsd > 0.9);
        support::result("duckdb.gate.union_jsd", e.jsd);
    }
    CHECK(rz.version() == 3);

    const auto forced = rz.define("AUTH_PROBLEM", "(?i)authentication|password|invalid user", 1.0, /*force=*/true);
    CHECK(forced.version == 4);
    const auto l = rz.ledger();
    CHECK(l.expression == doctest::Approx(l.conveyance + l.latitude).epsilon(1e-9));
    CHECK(l.content == doctest::Approx(l.conveyance + l.doubt).epsilon(1e-9));
    CHECK(l.doubt > 0);
    CHECK(rz.leakage() > 0);
    const auto amb = b->query("SELECT count(*) FROM logs_m WHERE meaning IS NULL AND doubt > 0").i64(0, 0);
    CHECK(amb > 0);

    const auto certified = [&] {
        return b->query("SELECT count(*) FROM logs_m WHERE meaning = 'LINUX_AUTH_FAIL'").i64(0, 0);
    };
    const auto ambiguous = [&] { return b->query("SELECT count(*) FROM logs_m WHERE doubt > 0").i64(0, 0); };
    const auto before = std::pair{certified(), ambiguous()};
    CHECK(rz.retire("AUTH_PROBLEM") == 5);
    const auto after = std::pair{certified(), ambiguous()};
    CHECK(after.second == 0);
    CHECK(after.first > 0);
    rz.pin(4);
    CHECK(std::make_pair(certified(), ambiguous()) == before);
    rz.pin(5);
    CHECK_THROWS_AS(rz.pin(99), std::invalid_argument);
    CHECK_THROWS_AS(rz.retire("NO_SUCH"), std::invalid_argument);

    const auto m = rz.meaningOf("Failed password for root from 1.2.3.4 port 22 ssh2");
    REQUIRE(m.has_value());
    CHECK(m->meaning == std::optional<std::string>("SSH_PASSWORD_FAIL"));
    CHECK(!rz.meaningOf("a shape that was never registered 1 zzz qqq").has_value());
    const auto todo = rz.todo(3);
    REQUIRE(todo.size() == 3); // REQUIRE: indexing below is undefined behaviour on a short vector
    CHECK(todo[0].second >= todo[1].second);
    CHECK(todo[1].second >= todo[2].second);

    support::result("duckdb.coverage", l.coverage);
    support::result("duckdb.C", l.content);
    support::result("duckdb.E", l.expression);
    support::result("duckdb.latitude", l.latitude);
    support::result("duckdb.doubt", l.doubt);
    support::result("duckdb.leakage", rz.leakage());
}

// pins SQL injection immunity for malicious table names and regex patterns
TEST_CASE("hardening: hostile identifiers and patterns never reach SQL unquoted") {
    if (!duckdbAvailable() || !corpusAvailable()) {
        if (skipIntegration("DuckDB driver or /tmp/corpus.parquet (tools/MakeCorpus.py) not present")) return;
    }
    auto b = open();
    loadRaw(*b);
    CHECK_THROWS_AS(RazobillEngine(*b, {.table = "logs; DROP TABLE raw; --"}), std::invalid_argument);
    CHECK_THROWS_AS(RazobillEngine(*b, {.table = "t", .messageColumn = "msg\" FROM raw --"}), std::invalid_argument);
    CHECK_THROWS_AS(RazobillEngine(*b, {.table = "t", .purpose = "level) UNION"}), std::invalid_argument);
    CHECK_THROWS_AS(RazobillEngine(*b, {.table = "t", .tau = 0.3}), std::invalid_argument);
    RazobillEngine rz(*b, {.table = "logs", .purpose = "level"});
    rz.compact("raw");
    CHECK_THROWS_AS(rz.define("bad name; --", "x"), std::invalid_argument);
    CHECK_THROWS_AS(rz.define("OK", "([unclosed"), std::invalid_argument);
    CHECK_THROWS_AS(rz.define("OK", "x", -1.0), std::invalid_argument);
    CHECK_THROWS_AS(rz.compact("raw; DROP TABLE raw"), std::invalid_argument);
    const auto before = b->query("SELECT count(*) FROM logs_rb_form").i64(0, 0);
    CHECK_NOTHROW(rz.define("QUOTES", "'; DROP TABLE logs_rb_form; --", 1.0, true));
    CHECK(b->query("SELECT count(*) FROM logs_rb_form").i64(0, 0) == before);
    CHECK(b->query("SELECT count(*) FROM raw").i64(0, 0) == 134815);
    RazobillEngine noPurpose(*b, {.table = "logs"});
    CHECK_THROWS_AS(noPurpose.define("X", "^a"), std::invalid_argument);
}

// pins concurrent read-write thread safety across shared engine instance
TEST_CASE("one instance shared by many threads: calls serialize, results stay consistent") {
    if (!duckdbAvailable() || !corpusAvailable()) {
        if (skipIntegration("DuckDB driver or /tmp/corpus.parquet (tools/MakeCorpus.py) not present")) return;
    }
    auto b = open();
    loadRaw(*b);
    RazobillEngine rz(*b, {.table = "logs", .purpose = "level"});
    rz.compact("raw");
    for (const auto& r : kRules)
        rz.define(r[0], r[1]);
    const auto expected = rz.ledger();
    std::atomic<int> bad{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t)
        ts.emplace_back([&] {
            for (int i = 0; i < 6; ++i) {
                const auto l = rz.ledger();
                if (std::abs(l.latitude - expected.latitude) > 1e-12 || std::abs(l.doubt - expected.doubt) > 1e-12)
                    ++bad;
                const auto m = rz.meaningOf("Failed password for root from 1.2.3.4 port 22 ssh2");
                if (!m || m->meaning != std::optional<std::string>("SSH_PASSWORD_FAIL")) ++bad;
            }
        });
    for (auto& t : ts)
        t.join();
    CHECK(bad == 0);
}

// pins additive mode ledger metrics and lossless round-trip on DuckDB
TEST_CASE("additive mode on DuckDB") {
    if (!duckdbAvailable() || !corpusAvailable()) {
        if (skipIntegration("DuckDB driver or /tmp/corpus.parquet (tools/MakeCorpus.py) not present")) return;
    }
    auto b = open();
    loadRaw(*b);
    RazobillEngine rz(*b, {.table = "raw", .purpose = "level"});
    const auto added = rz.refresh();
    CHECK(added > 0);
    CHECK(rz.refresh() == 0);
    for (const auto& r : kRules)
        rz.define(r[0], r[1]);
    rz.define("AUTH_PROBLEM", "(?i)authentication|password|invalid user", 1.0, true);
    const auto l = rz.ledger();
    support::result("duckdb.additive.forms", static_cast<double>(added));
    support::result("duckdb.additive.latitude", l.latitude);
    support::result("duckdb.additive.doubt", l.doubt);
    support::result("duckdb.additive.leakage", rz.leakage());
    CHECK_THROWS_AS(rz.verify("raw"), std::logic_error);
}

// pins persistence and seamless resume across process/instance restarts
TEST_CASE("DuckDB: a new instance resumes over existing compact storage (restart)") {
    if (!duckdbAvailable() || !corpusAvailable()) {
        if (skipIntegration("DuckDB driver or /tmp/corpus.parquet (tools/MakeCorpus.py) not present")) return;
    }
    auto b = open();
    loadRaw(*b);
    std::optional<stats::Ledger> before;
    {
        RazobillEngine first(*b, {.table = "logs", .purpose = "level"});
        first.compact("raw");
        for (const auto& r : kRules)
            first.define(r[0], r[1]);
        before = first.ledger();
    }
    RazobillEngine second(*b, {.table = "logs", .purpose = "level"});
    CHECK(second.version() == 3);
    const auto after = second.ledger();
    CHECK(after.latitude == doctest::Approx(before->latitude).epsilon(1e-12));
    CHECK(second.verify("raw") == 0);
    const auto m = second.meaningOf("Failed password for root from 1.2.3.4 port 22 ssh2");
    REQUIRE(m.has_value());
    CHECK(m->meaning == std::optional<std::string>("SSH_PASSWORD_FAIL"));
    CHECK(second.compact("raw") == 2 * 134815);
}

// pins dead token GC in string dictionary without breaking reconstruction
TEST_CASE("dictionary garbage collection and compaction: purges dead strings and preserves lossless rebuild") {
    if (!duckdbAvailable()) {
        if (skipIntegration("DuckDB driver not present")) return;
    }
    auto b = open();
    b->exec("CREATE TABLE ephem(id INTEGER, msg VARCHAR)");
    b->exec("INSERT INTO ephem VALUES (1, 'worker 1 session-alpha-1 active'), (2, 'worker 2 session-beta-1 idle'), (3, "
            "'worker 3 session-gamma-1 busy')");
    RazobillEngine rz(*b, {.table = "eph", .purpose = ""});
    rz.compact("ephem");
    CHECK(rz.verify("ephem") == 0);

    const auto beforeGc = b->query("SELECT count(*) FROM eph_rb_str").i64(0, 0);
    CHECK(beforeGc >= 3);

    b->exec("DELETE FROM eph_c WHERE id = 3");
    b->exec("CREATE TABLE ephem_kept AS SELECT * FROM ephem WHERE id != 3");

    const auto stats = rz.gcDictionary();
    CHECK(stats.purgedCount > 0);
    CHECK(stats.activeAfter < static_cast<std::size_t>(beforeGc));

    rz.compactDictionary();
    const auto activeCount = b->query("SELECT count(*) FROM eph_rb_str").i64(0, 0);
    const auto maxId = b->query("SELECT coalesce(max(id), -1) FROM eph_rb_str").i64(0, 0);
    CHECK(maxId == activeCount - 1);

    CHECK(rz.verify("ephem_kept") == 0);
}

// pins differential parity between native C++ UDFs and pure SQL regexes
TEST_CASE("native functions equal the SQL regex specification (real logs + 60,000 random and adversarial messages)") {
    if (!duckdbAvailable() || !corpusAvailable()) {
        if (skipIntegration("DuckDB driver or /tmp/corpus.parquet (tools/MakeCorpus.py) not present")) return;
    }
    auto b = open();
    b->exec("SET threads = 4");
    b->exec("SET lambda_syntax = 'ENABLE_SINGLE_ARROW'");
    loadRaw(*b);
    const std::string tok = R"([^\s=,;:()\[\]{}"''<>|/]*[0-9][^\s=,;:()\[\]{}"''<>|/]*)";
    b->exec("CREATE MACRO s_esc(m) AS m LIKE '%' || chr(17) || '%' OR m LIKE '%' || chr(18) || '%' OR m LIKE '%' || "
            "chr(19) || '%'");
    b->exec("CREATE MACRO s_toks(m) AS CASE WHEN s_esc(m) THEN [m] ELSE regexp_extract_all(m, '" + tok + "') END");
    b->exec("CREATE MACRO s_isint(t) AS regexp_full_match(t, '0|[1-9][0-9]{0,17}') AND NOT s_esc(t)");
    b->exec("CREATE MACRO s_form(m) AS (CASE WHEN s_esc(m) THEN chr(18) ELSE regexp_replace(m, '" + tok +
            "', chr(18), 'g') END) || chr(19) || "
            "array_to_string(list_transform(s_toks(m), t -> CASE WHEN s_isint(t) THEN 'I' ELSE 'S' END), '')");
    b->exec(
        "CREATE MACRO s_ints(m) AS list_transform(list_filter(s_toks(m), t -> s_isint(t)), t -> CAST(t AS BIGINT))");
    b->exec("CREATE MACRO s_strs(m) AS list_filter(s_toks(m), t -> NOT s_isint(t))");

    const std::vector<std::string> alphabet = {"a",
                                               "b",
                                               "c",
                                               "X",
                                               "Y",
                                               "Z",
                                               "0",
                                               "1",
                                               "2",
                                               "3",
                                               "4",
                                               "5",
                                               "6",
                                               "7",
                                               "8",
                                               "9",
                                               " ",
                                               "\t",
                                               "\n",
                                               "\f",
                                               "\r",
                                               "=",
                                               ",",
                                               ";",
                                               ":",
                                               "(",
                                               ")",
                                               "[",
                                               "]",
                                               "{",
                                               "}",
                                               "\"",
                                               "'",
                                               "<",
                                               ">",
                                               "|",
                                               "/",
                                               ".",
                                               "_",
                                               "-",
                                               "+",
                                               "\xC3\xA9",
                                               "\xE6\x97\xA5",
                                               "\xF0\x9F\x98\x80",
                                               "\x0B",
                                               "\x1F",
                                               "\x7F",
                                               "\xC2\xA0"};
    std::mt19937_64 rng(2026);
    b->exec("CREATE TABLE adv(msg VARCHAR)");
    auto hex = [](const std::string& s) {
        static const char* h = "0123456789abcdef";
        std::string o;
        for (unsigned char c : s) {
            o.push_back(h[c >> 4]);
            o.push_back(h[c & 15]);
        }
        return o;
    };
    for (int chunk = 0; chunk < 30; ++chunk) {
        std::string values;
        for (int i = 0; i < 2000; ++i) {
            std::string m;
            const auto len = rng() % 48;
            for (std::size_t k = 0; k < len; ++k)
                m += alphabet[rng() % alphabet.size()];
            if (rng() % 400 == 0) m += static_cast<char>(0x11 + rng() % 3);
            values += (i ? ", " : "") + std::string("(decode(unhex('") + hex(m) + "')))";
        }
        b->exec("INSERT INTO adv VALUES " + values);
    }
    b->exec("INSERT INTO adv VALUES (''), (' '), ('0'), ('007'), ('-5'), ('" + std::string(18, '1') + "'), ('" +
            std::string(19, '1') + "'), ('" + std::string(20, '1') +
            "'), "
            "('9223372036854775807'), (repeat('x', 100000) || ' 1'), (repeat(' ', 5000) || '7'), (repeat('1 ', 4000))");
    b->exec("CREATE TABLE allm AS SELECT msg FROM raw UNION ALL SELECT msg FROM adv");
    const auto n = b->query("SELECT count(*) FROM allm").i64(0, 0);
    CHECK(n == 134815 + 60000 + 12);
    CHECK(b->query("SELECT count(*) FROM allm WHERE RzJoin((RzSplit(msg)).template, (RzSplit(msg)).ints, "
                   "(RzSplit(msg)).strs) IS DISTINCT FROM msg")
              .i64(0, 0) == 0);
    CHECK(b->query("SELECT count(*) FROM allm WHERE RzTemplate(msg) IS DISTINCT FROM (RzSplit(msg)).template")
              .i64(0, 0) == 0);
    support::result("duckdb.oracle.messages", static_cast<double>(n));
}

// pins semantic mode variable collapsing and exact round-trip in DuckDB
TEST_CASE("semantic form extraction in DuckDB: collapses text variables and round-trips exactly") {
    if (!duckdbAvailable()) {
        if (skipIntegration("DuckDB driver not present")) return;
    }
    auto b = open();
    b->exec("CREATE TABLE ssh_sample(msg VARCHAR)");
    b->exec("INSERT INTO ssh_sample VALUES "
            "('Failed password for root from 192.168.1.10 port 2222 ssh2'), "
            "('Failed password for admin from 10.0.0.1 port 5432 ssh2'), "
            "('Failed password for alice from 172.16.0.5 port 8080 ssh2')");

    const auto semanticForms = b->query("SELECT count(DISTINCT RzTemplate(msg)) FROM ssh_sample").i64(0, 0);
    CHECK(semanticForms == 1);

    CHECK(b->query("SELECT count(*) FROM ssh_sample WHERE RzJoin((RzSplit(msg)).template, (RzSplit(msg)).ints, "
                   "(RzSplit(msg)).strs) IS DISTINCT FROM msg")
              .i64(0, 0) == 0);

    RazobillEngine rz(*b, {.table = "ssh_tbl", .purpose = "", .scanMode = core::ScanMode::Semantic});
    rz.compact("ssh_sample");
    CHECK(rz.verify("ssh_sample") == 0);
}

// pins RzSplit composite STRUCT decomposition in vectorized DuckDB queries
TEST_CASE("composite struct decomposition in DuckDB SQL") {
    if (!duckdbAvailable()) {
        if (skipIntegration("DuckDB driver not present")) return;
    }
    auto b = open();
    b->exec("CREATE TABLE decomp_test(msg VARCHAR)");
    b->exec("INSERT INTO decomp_test VALUES "
            "('User admin logged in from 192.168.1.1 port 8080'), "
            "('User guest logged in from 10.0.0.1 port 22')");

    // verify RzSplit returns struct with template, ints, strs
    auto res =
        b->query("SELECT (d).template, len((d).ints), len((d).strs) FROM (SELECT RzSplit(msg) AS d FROM decomp_test)");
    CHECK(res.rows.size() == 2);

    auto match = b->query("SELECT count(*) FROM decomp_test WHERE "
                          "RzJoin((RzSplit(msg)).template, (RzSplit(msg)).ints, (RzSplit(msg)).strs) != msg")
                     .i64(0, 0);
    CHECK(match == 0);

    // verify NULL and empty string handling
    CHECK(b->query("SELECT count(*) FROM (SELECT RzSplit(NULL) AS x) WHERE x IS NULL").i64(0, 0) == 1);
    CHECK(b->query("SELECT count(*) FROM (SELECT RzTemplate(NULL) AS x) WHERE x IS NULL").i64(0, 0) == 1);
    CHECK(b->query("SELECT count(*) FROM (SELECT RzJoin(NULL, [1]::BIGINT[], ['a']::VARCHAR[]) AS x) WHERE x IS NULL")
              .i64(0, 0) == 1);
    CHECK(
        b->query("SELECT count(*) FROM (SELECT RzJoin('', []::BIGINT[], []::VARCHAR[]) AS x) WHERE x = ''").i64(0, 0) ==
        1);

    // verify RzJoin with diverse list integer types (INTEGER, SMALLINT, TINYINT, UBIGINT)
    CHECK(b->query("SELECT RzJoin('test {i} {i}', [10::INTEGER, 20::INTEGER], []::VARCHAR[])").str(0, 0) ==
          "test 10 20");
    CHECK(b->query("SELECT RzJoin('Port {i}', [22], []::VARCHAR[])").str(0, 0) == "Port 22");
    CHECK(b->query("SELECT RzJoin('val {i}', [42::SMALLINT], []::VARCHAR[])").str(0, 0) == "val 42");
    CHECK(b->query("SELECT RzJoin('val {i}', [7::TINYINT], []::VARCHAR[])").str(0, 0) == "val 7");
    CHECK(b->query("SELECT RzJoin('val {i}', [999::UBIGINT], []::VARCHAR[])").str(0, 0) == "val 999");

    // verify varargs RzJoin overloads
    CHECK(b->query("SELECT RzJoin('User {s} logged in with port {i}', 'alice', 443)").str(0, 0) ==
          "User alice logged in with port 443");
    CHECK(b->query("SELECT RzJoin('{s}: code={i} status={s}', 'auth', 200, 'ok')").str(0, 0) ==
          "auth: code=200 status=ok");

    // verify UBIGINT >= 2^63 is rejected instead of silently overflowing
    CHECK_THROWS(b->query("SELECT RzJoin('overflow {i}', [9223372036854775808::UBIGINT], []::VARCHAR[])"));
    CHECK_THROWS(b->query("SELECT RzJoin('overflow {i}', 9223372036854775808::UBIGINT)"));
}

// pins raw binary BLOB round-trip over invalid UTF-8 byte sequences
TEST_CASE("binary BLOB support in DuckDB: exact round-trip over invalid UTF-8 byte payloads") {
    if (!duckdbAvailable()) {
        if (skipIntegration("DuckDB driver not present")) return;
    }
    auto b = open();
    b->exec("CREATE TABLE bin_sample(raw_bin BLOB)");
    b->exec("INSERT INTO bin_sample VALUES ('\\xAA\\xBB\\xCC\\xFF\\xFE 1234 port 22'::BLOB)");

    const auto forms = b->query("SELECT count(DISTINCT RzTemplate(raw_bin)) FROM bin_sample").i64(0, 0);
    CHECK(forms == 1);

    const auto mismatches = b->query("SELECT count(*) FROM bin_sample WHERE RzJoin((RzSplit(raw_bin)).template, "
                                     "(RzSplit(raw_bin)).ints, (RzSplit(raw_bin)).strs) IS DISTINCT FROM raw_bin")
                                .i64(0, 0);
    CHECK(mismatches == 0);
}

// pins transactional rollback atomicity and config table synchronization
TEST_CASE("control plane: config synchronization, transactional atomicity and batch todo") {
    if (!duckdbAvailable()) {
        if (skipIntegration("DuckDB driver not present")) return;
    }
    auto b = open();
    b->exec("CREATE TABLE ctrl_test(id INTEGER, log_msg VARCHAR, level VARCHAR)");
    b->exec("INSERT INTO ctrl_test VALUES "
            "(1, 'Disk full on /dev/sda1 (capacity 98 percent)', 'ERROR'), "
            "(2, 'Disk full on /dev/sdb1 (capacity 95 percent)', 'ERROR'), "
            "(3, 'Connection timeout after 30 seconds', 'WARN'), "
            "(4, 'User root login successful', 'INFO')");

    // Test config precedence and snake_case synchronization
    RazobillEngine rz(
        *b,
        {.table = "ctrl_logs", .purpose = "level", .message_column = "log_msg", .scan_mode = core::ScanMode::Semantic});

    const auto rows = rz.compact("ctrl_test");
    CHECK(rows == 4);
    CHECK(rz.verify("ctrl_test") == 0);

    // Verify batched todo() functionality
    const auto todoList = rz.todo(10);
    CHECK(!todoList.empty());
    for (const auto& [tmpl, count] : todoList) {
        CHECK(!tmpl.empty());
        CHECK(count > 0);
    }

    // Verify transactional define(), retire(), and pin()
    const auto d1 = rz.define("DISK_ALERT", "Disk full on <*>", 1.0, /*force=*/true);
    CHECK(d1.version == 1);
    CHECK(rz.version() == 1);

    const auto d2 = rz.define("NET_TIMEOUT", "Connection timeout after <*>", 1.0, /*force=*/true);
    CHECK(d2.version == 2);
    CHECK(rz.version() == 2);

    // Pin to version 1
    rz.pin(1);
    CHECK(rz.version() == 1);

    // Retire meaning
    rz.pin(2);
    const auto v3 = rz.retire("DISK_ALERT");
    CHECK(v3 == 3);
    CHECK(rz.version() == 3);
}

// failure-injection proxy to verify TransactionGuard rollback on failure
class InjectedFailBackend final : public Backend {
public:
    explicit InjectedFailBackend(std::unique_ptr<Backend> inner) : inner_(std::move(inner)) {}
    Flavor flavor() const noexcept override { return inner_->flavor(); }
    void setFailOn(std::string pattern) {
        failPattern_ = std::move(pattern);
        failActive_ = true;
    }
    void exec(std::string_view sql) override {
        if (failActive_ && sql.find(failPattern_) != std::string::npos) {
            throw std::runtime_error("injected error in exec: " + std::string(sql));
        }
        inner_->exec(sql);
    }
    Table query(std::string_view sql) override {
        if (failActive_ && sql.find(failPattern_) != std::string::npos) {
            throw std::runtime_error("injected error in query: " + std::string(sql));
        }
        return inner_->query(sql);
    }
    Backend& inner() { return *inner_; }

private:
    std::unique_ptr<Backend> inner_;
    std::string failPattern_;
    bool failActive_ = false;
};

TEST_CASE("TransactionGuard: failure injection triggers rollback") {
    if (!duckdbAvailable()) {
        if (skipIntegration("DuckDB driver not present")) return;
    }
    auto rawDb = open();
    rawDb->exec("CREATE TABLE fail_test(id INTEGER, msg VARCHAR, level VARCHAR)");
    rawDb->exec("INSERT INTO fail_test VALUES (1, 'Disk error on sda1', 'ERROR')");

    auto proxy = std::make_unique<InjectedFailBackend>(std::move(rawDb));
    proxy->setFailOn("CREATE OR REPLACE VIEW fail_logs AS");

    RazobillEngine rz(*proxy, {.table = "fail_logs", .purpose = "level"});
    CHECK_THROWS_AS(rz.compact("fail_test"), std::runtime_error);

    // Verify that the view was NOT created due to rollback
    const auto viewCount =
        proxy->inner().query("SELECT count(*) FROM duckdb_views() WHERE view_name = 'fail_logs'").i64(0, 0);
    CHECK(viewCount == 0);
}

// -- regression: defects found by the 2026-10-25 audit -------------------------

// rb_str / rb_layout / rb_form / rb_event / rb_meta / rb_doubt used to be global to the
// database, so gcDictionary() on one table purged strings another table still referenced
TEST_CASE("two tables on one connection are isolated: dictionary GC of one never damages the other") {
    if (!duckdbAvailable() && skipIntegration("DuckDB driver not present")) return;
    auto b = open();
    b->exec("CREATE TABLE raw_a AS SELECT i AS id, 'login user alice from host' || i AS msg FROM range(1000) t(i)");
    b->exec("CREATE TABLE raw_b AS SELECT i AS id, 'payment failed card visa order' || i || ' for customer zed' AS msg "
            "FROM range(1000) t(i)");
    RazobillEngine a(*b, {.table = "ta"});
    RazobillEngine c(*b, {.table = "tb"});
    CHECK(a.compact("raw_a") == 1000);
    CHECK(c.compact("raw_b") == 1000);
    const auto gc = a.gcDictionary();
    CHECK(gc.purgedCount == 0);
    CHECK(c.verify("raw_b") == 0);
    CHECK(a.verify("raw_a") == 0);
}

// compact() used to flip phys_/view_/compact_/ready_ before its last steps could throw, so a
// rolled-back compact() left an engine that believed in tables that did not exist
TEST_CASE("a failed compact() leaves the engine exactly as it was") {
    if (!duckdbAvailable() && skipIntegration("DuckDB driver not present")) return;
    auto b = open();
    b->exec("CREATE TABLE raw AS SELECT i AS id, 'login user alice from host' || i AS msg FROM range(100) t(i)");
    b->exec("CREATE TABLE logs_m(x INTEGER)"); // a table where compact() must create the view logs_m
    RazobillEngine e(*b, {.table = "logs"});
    CHECK_THROWS(e.compact("raw"));
    CHECK_THROWS_AS(e.verify("raw"), std::logic_error);
    CHECK_THROWS_AS((void)e.ledger(), std::logic_error);
    CHECK_THROWS_AS(e.gcDictionary(), std::logic_error);
}

// the staging table holds the whole decomposed batch and used to live until the connection closed
TEST_CASE("compact() releases its staging table") {
    if (!duckdbAvailable() && skipIntegration("DuckDB driver not present")) return;
    auto b = open();
    b->exec("CREATE TABLE raw AS SELECT i AS id, 'login user alice from host' || i AS msg FROM range(1000) t(i)");
    RazobillEngine e(*b, {.table = "ta"});
    CHECK(e.compact("raw") == 1000);
    CHECK(b->query("SELECT count(*) FROM duckdb_tables() WHERE table_name LIKE '%rb_s'").i64(0, 0) == 0);
}

// catalog probes must not see same-named tables of other schemas or attached databases
TEST_CASE("catalog detection is scoped to the current database and schema") {
    if (!duckdbAvailable() && skipIntegration("DuckDB driver not present")) return;
    auto b = open();
    b->exec("ATTACH ':memory:' AS other");
    b->exec("CREATE TABLE other.logs_c(x INTEGER)");
    b->exec("CREATE TABLE raw AS SELECT i AS id, 'login user alice from host' || i AS msg FROM range(100) t(i)");
    RazobillEngine e(*b, {.table = "logs"});
    CHECK(e.compact("raw") == 100); // used to take the INSERT branch against a table that is not ours
    CHECK(e.verify("raw") == 0);
}

// O1: calling compact() twice on identical source batch is idempotent
TEST_CASE("compact() is idempotent on identical source batches") {
    if (!duckdbAvailable() && skipIntegration("DuckDB driver not present")) return;
    auto b = open();
    b->exec("CREATE TABLE raw AS SELECT i AS id, 'event ' || i AS msg FROM range(200) t(i)");
    RazobillEngine e(*b, {.table = "logs_idemp", .idempotent = true});
    CHECK(e.compact("raw") == 200);
    CHECK(e.verify("raw") == 0);
    // second compact call must not duplicate rows
    CHECK(e.compact("raw") == 200);
    CHECK(e.verify("raw") == 0);
    CHECK(b->query("SELECT count(*) FROM logs_idemp").i64(0, 0) == 200);
}

// O1: compact() with explicit token deduplicates identical client requests
TEST_CASE("compact() with explicit idempotency token prevents duplicate insertion") {
    if (!duckdbAvailable() && skipIntegration("DuckDB driver not present")) return;
    auto b = open();
    b->exec("CREATE TABLE raw AS SELECT i AS id, 'event ' || i AS msg FROM range(100) t(i)");
    RazobillEngine e(*b, {.table = "logs_token"});
    CHECK(e.compact("raw", "req-uuid-1") == 100);
    CHECK(e.verify("raw") == 0);
    // retry with same token
    CHECK(e.compact("raw", "req-uuid-1") == 100);
    CHECK(b->query("SELECT count(*) FROM logs_token").i64(0, 0) == 100);
}

// O2: form hash collision semantic isolation
TEST_CASE("semantic layer isolates doubt and meaning per form and form_id") {
    if (!duckdbAvailable() && skipIntegration("DuckDB driver not present")) return;
    auto b = open();
    b->exec("CREATE TABLE raw AS SELECT 1 AS id, 'disk read failure on dev0' AS msg, 'ERR' AS lvl "
            "UNION ALL SELECT 2, 'disk write failure on dev1', 'ERR' "
            "UNION ALL SELECT 3, 'user login alice success', 'INFO'");
    RazobillEngine e(*b, {.table = "logs_sem", .purpose = "lvl"});
    e.compact("raw");
    e.define("DISK_ERR", "^disk .* failure", 1.0, true);
    e.define("AUTH_INFO", "^user ", 1.0, true);
    const auto m1 = e.meaningOf("disk read failure on dev0");
    const auto m2 = e.meaningOf("user login alice success");
    REQUIRE(m1.has_value());
    REQUIRE(m2.has_value());
    CHECK(m1->meaning.value_or("") == "DISK_ERR");
    CHECK(m2->meaning.value_or("") == "AUTH_INFO");
}

// O3: define() and retire() are rejected while pinned to historical version
TEST_CASE("define() and retire() are rejected while pinned to historical version") {
    if (!duckdbAvailable() && skipIntegration("DuckDB driver not present")) return;
    auto b = open();
    b->exec("CREATE TABLE raw AS SELECT i AS id, 'event message ' || i AS msg, 'WARN' AS lvl FROM range(20) t(i)");
    RazobillEngine e(*b, {.table = "logs_pin", .purpose = "lvl"});
    e.compact("raw");
    e.define("RULE_A", ".*", 1.0, true);
    e.define("RULE_B", ".*", 1.0, true);
    CHECK(e.version() == 2);
    e.pin(1);
    CHECK(e.version() == 1);
    // must reject define and retire while pinned
    CHECK_THROWS_AS(e.define("RULE_C", ".*", 1.0, true), std::logic_error);
    CHECK_THROWS_AS(e.retire("RULE_A"), std::logic_error);
    // unpin back to latest version allows operations
    e.pin(2);
    CHECK(e.version() == 2);
}

// Control-plane concurrency stress test: two threads, two engines, one connection
TEST_CASE("control plane concurrency: two threads, two engines, one connection") {
    if (!duckdbAvailable() && skipIntegration("DuckDB driver not present")) return;
    auto b = open();
    b->exec("CREATE TABLE conc_raw AS SELECT i AS id, 'system task ' || (i % 5) || ' status ok' AS msg, "
            "CASE WHEN i % 2 = 0 THEN 'INFO' ELSE 'WARN' END AS lvl FROM range(100) t(i)");

    Config cfg1{.table = "conc_logs_1", .purpose = "lvl", .idempotent = true};
    Config cfg2{.table = "conc_logs_2", .purpose = "lvl", .idempotent = true};
    RazobillEngine e1(*b, cfg1);
    RazobillEngine e2(*b, cfg2);

    e1.compact("conc_raw", "init-tok-1");
    e2.compact("conc_raw", "init-tok-2");

    std::atomic<bool> start{false};
    std::atomic<int> errorsA{0};
    std::atomic<int> errorsB{0};
    constexpr int kIterations = 100;

    std::thread t1([&]() {
        while (!start.load(std::memory_order_acquire)) {}
        for (int i = 0; i < kIterations; ++i) {
            try {
                // repeated compaction with same token tests idempotent dedup concurrency
                auto cnt = e1.compact("conc_raw", "init-tok-1");
                if (cnt != 100) errorsA.fetch_add(1);
                e1.define("RULE_T1_" + std::to_string(i % 5), "^system task [0-9] status", 1.0, true);
                auto m = e1.meaningOf("system task 1 status ok");
                (void)m;
                if (i % 3 == 0) {
                    e1.retire("RULE_T1_" + std::to_string(i % 5));
                }
            } catch (const std::exception&) {
                errorsA.fetch_add(1);
            }
        }
    });

    std::thread t2([&]() {
        while (!start.load(std::memory_order_acquire)) {}
        for (int i = 0; i < kIterations; ++i) {
            try {
                // repeated compaction with same token tests idempotent dedup concurrency
                auto cnt = e2.compact("conc_raw", "init-tok-2");
                if (cnt != 100) errorsB.fetch_add(1);
                e2.define("RULE_T2_" + std::to_string(i % 5), "^system task .* ok", 1.0, true);
                auto m = e2.meaningOf("system task 2 status ok");
                (void)m;
                (void)e2.ledger();
                if (i % 4 == 0) {
                    e2.retire("RULE_T2_" + std::to_string(i % 5));
                }
            } catch (const std::exception&) {
                errorsB.fetch_add(1);
            }
        }
    });

    start.store(true, std::memory_order_release);
    t1.join();
    t2.join();

    CHECK(errorsA.load() == 0);
    CHECK(errorsB.load() == 0);
    CHECK(e1.verify("conc_raw") == 0);
    CHECK(e2.verify("conc_raw") == 0);
}


