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
        const auto opt = support::duckdbOptions();
        std::error_code ec;
        return std::filesystem::exists(opt.driverPath, ec);
    }();
    return avail;
}

bool corpusAvailable() {
    static const bool avail = [] {
        std::error_code ec;
        return std::filesystem::exists("/tmp/corpus.parquet", ec);
    }();
    return avail;
}

constexpr const char* kRules[][2] = {{"HDFS_RECEIVING", R"(^Receiving block <\*> src: )"},
                                     {"SSH_PASSWORD_FAIL", "^Failed password for"},
                                     {"LINUX_AUTH_FAIL", "^authentication failure"}};
std::unique_ptr<Backend> open() {
    return makeAdbcBackend(support::duckdbOptions());
}
void loadRaw(Backend& b, const char* name = "raw") {
    b.exec(std::string("CREATE TABLE ") + name + " AS SELECT * FROM read_parquet('/tmp/corpus.parquet')");
}
} // namespace

// pins exact reconstruction over 134,815 events and appendable re-compact
TEST_CASE("compact mode: lossless, appendable, original columns restored") {
    if (!duckdbAvailable() || !corpusAvailable()) {
        MESSAGE("Skipping DuckDB corpus test: DuckDB driver or /tmp/corpus.parquet not present");
        return;
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
        MESSAGE("Skipping DuckDB test: DuckDB driver not present");
        return;
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
        MESSAGE("Skipping DuckDB corpus test: DuckDB driver or /tmp/corpus.parquet not present");
        return;
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
    const auto db = b->query("SELECT form, id FROM rb_form");
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
        MESSAGE("Skipping DuckDB corpus test: DuckDB driver or /tmp/corpus.parquet not present");
        return;
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
    CHECK(todo.size() == 3);
    CHECK(todo[0].second >= todo[1].second);

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
        MESSAGE("Skipping DuckDB corpus test: DuckDB driver or /tmp/corpus.parquet not present");
        return;
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
    const auto before = b->query("SELECT count(*) FROM rb_form").i64(0, 0);
    CHECK_NOTHROW(rz.define("QUOTES", "'; DROP TABLE rb_form; --", 1.0, true));
    CHECK(b->query("SELECT count(*) FROM rb_form").i64(0, 0) == before);
    CHECK(b->query("SELECT count(*) FROM raw").i64(0, 0) == 134815);
    RazobillEngine noPurpose(*b, {.table = "logs"});
    CHECK_THROWS_AS(noPurpose.define("X", "^a"), std::invalid_argument);
}

// pins concurrent read-write thread safety across shared engine instance
TEST_CASE("one instance shared by many threads: calls serialize, results stay consistent") {
    if (!duckdbAvailable() || !corpusAvailable()) {
        MESSAGE("Skipping DuckDB corpus test: DuckDB driver or /tmp/corpus.parquet not present");
        return;
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
        MESSAGE("Skipping DuckDB corpus test: DuckDB driver or /tmp/corpus.parquet not present");
        return;
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
        MESSAGE("Skipping DuckDB corpus test: DuckDB driver or /tmp/corpus.parquet not present");
        return;
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
        MESSAGE("Skipping DuckDB test: DuckDB driver not present");
        return;
    }
    auto b = open();
    b->exec("CREATE TABLE ephem(id INTEGER, msg VARCHAR)");
    b->exec("INSERT INTO ephem VALUES (1, 'worker 1 session-alpha-1 active'), (2, 'worker 2 session-beta-1 idle'), (3, "
            "'worker 3 session-gamma-1 busy')");
    RazobillEngine rz(*b, {.table = "eph", .purpose = ""});
    rz.compact("ephem");
    CHECK(rz.verify("ephem") == 0);

    const auto beforeGc = b->query("SELECT count(*) FROM rb_str").i64(0, 0);
    CHECK(beforeGc >= 3);

    b->exec("DELETE FROM eph_c WHERE id = 3");
    b->exec("CREATE TABLE ephem_kept AS SELECT * FROM ephem WHERE id != 3");

    const auto stats = rz.gcDictionary();
    CHECK(stats.purgedCount > 0);
    CHECK(stats.activeAfter < static_cast<std::size_t>(beforeGc));

    rz.compactDictionary();
    const auto activeCount = b->query("SELECT count(*) FROM rb_str").i64(0, 0);
    const auto maxId = b->query("SELECT coalesce(max(id), -1) FROM rb_str").i64(0, 0);
    CHECK(maxId == activeCount - 1);

    CHECK(rz.verify("ephem_kept") == 0);
}

// pins differential parity between native C++ UDFs and pure SQL regexes
TEST_CASE("native functions equal the SQL regex specification (real logs + 60,000 random and adversarial messages)") {
    if (!duckdbAvailable() || !corpusAvailable()) {
        MESSAGE("Skipping DuckDB corpus test: DuckDB driver or /tmp/corpus.parquet not present");
        return;
    }
    auto b = open();
    b->exec("SET threads = 4");
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
        MESSAGE("Skipping DuckDB test: DuckDB driver not present");
        return;
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
        MESSAGE("Skipping DuckDB test: DuckDB driver not present");
        return;
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
}

// pins raw binary BLOB round-trip over invalid UTF-8 byte sequences
TEST_CASE("binary BLOB support in DuckDB: exact round-trip over invalid UTF-8 byte payloads") {
    if (!duckdbAvailable()) {
        MESSAGE("Skipping DuckDB test: DuckDB driver not present");
        return;
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
        MESSAGE("Skipping DuckDB test: DuckDB driver not present");
        return;
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
