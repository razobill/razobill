// =============================================================================
// ClickHouse chDB integration and cross-engine hash parity test suite
//
// validates additive mode layout indexing and verifies that ClickHouse's
// native xxHash64() SQL function produces identical hashes to librazobill
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <cmath>
#include <filesystem>
#include <set>

#include "doctest.h"
#include "librazobill/Hash.hpp"
#include "librazobill/Razobill.hpp"
#include "librazobill/Slots.hpp"
#include "Support.hpp"

using namespace razobill;

namespace {
bool chdbAvailable() {
    static const bool avail = [] {
        const auto opt = support::chdbOptions();
        std::error_code ec;
        return std::filesystem::exists(opt.driverPath, ec);
    }();
    return avail;
}

void loadRaw(Backend& b) {
    b.exec("CREATE TABLE raw (sys String, level String, comp String, msg String) ENGINE = MergeTree ORDER BY tuple()");
    b.exec("INSERT INTO raw SELECT sys, level, comp, msg FROM file('/tmp/corpus.parquet', Parquet)");
}
} // namespace

// pins xxHash64 parity and form keys between ClickHouse and C++ core
// ~ if ClickHouse xxHash64 diverged here, distributed joins would fail silently
TEST_CASE("chDB: additive mode, forms computed by ClickHouse SQL equal the C++ core's forms and hashes") {
    // hash parity contract:
    // asserts ClickHouse xxHash64() produces identical 64-bit integers to C++
    // core::xxh64() implementation across 134,815 rows
    if (!chdbAvailable()) {
        MESSAGE("Skipping chDB test: driver library not present");
        return;
    }
    auto b = makeAdbcBackend(support::chdbOptions());
    loadRaw(*b);
    CHECK(b->query("SELECT count() FROM raw").i64(0, 0) == 134815);
    RazobillEngine rz(*b, {.table = "raw", .purpose = "level"});
    const auto added = rz.refresh();
    CHECK(added > 0);
    CHECK(rz.refresh() == 0);

    const auto rows = b->query("SELECT msg, form_id FROM raw");
    REQUIRE(rows.rows.size() == 134815);
    core::SlotRecord s;
    std::string key;
    std::size_t bad = 0;
    std::set<std::string> coreForms;
    for (std::size_t r = 0; r < rows.rows.size(); ++r) {
        REQUIRE(core::parse(rows.str(r, 0), s) == Status::Ok);
        key = s.formKey();
        coreForms.insert(key);
        bad += core::xxh64(key) != rows.u64(r, 1);
    }
    CHECK(bad == 0);
    const auto db = b->query("SELECT form FROM raw_rb_form");
    std::set<std::string> dbForms;
    for (std::size_t r = 0; r < db.rows.size(); ++r)
        dbForms.insert(db.str(r, 0));
    CHECK(dbForms == coreForms);
}
