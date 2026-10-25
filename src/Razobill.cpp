// =============================================================================
//  file : src/Razobill.cpp
//  role : analytical database control plane and relational schema orchestrator
//  inv  : RzJoin(RzSplit(x)) == x; exact lossless decomp; atomic transactions
// =============================================================================

/// analytical database control plane coordinating DuckDB and ClickHouse engines
///
/// coordinates relational storage engines across two primary deployment modes:
/// 1. additive indexing layout (`logs`): preserves raw text in-place while
///    indexing each record with a 64-bit form_id (xxHash64, seed 0)
/// 2. compact columnar layout (`logs_c`): decomposes messages into typed slots
///    (form_no, i0..i{ki-1}, d0..d{ks-1}, oi, os) and synthesizes an exact
///    reconstruction view satisfying RzJoin(RzSplit(x)) == x

#include "librazobill/Razobill.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <map>
#include <sstream>

#include "librazobill/Hash.hpp"
#include "librazobill/Slots.hpp"

namespace razobill {

namespace {

// -- transaction and identifier safety helpers --------------------------------

/// scoped raii transaction guard for engines supporting multi-statement acid
///
/// inv     commits explicitly or executes rollback on scope exit if active
/// in      db must be connected; active only when flavor is Flavor::DuckDB
/// own     borrows backend reference db_
/// thread  not thread-safe; operates on caller's connection
/// err     suppresses rollback exceptions during stack unwinding
class TransactionGuard {
public:
    // ~ ClickHouse ADBC and DuckDB DDL/DML mixed flows avoid explicit multi-statement transactions; guard stays dormant
    explicit TransactionGuard(Backend& db RZ_LIFETIMEBOUND, Flavor flavor) : db_(db) {
        if (flavor == Flavor::DuckDB) {
            db_.lock();
            locked_ = true;
            active_ = true;
            try {
                db_.exec("BEGIN TRANSACTION");
            } catch (...) {
                active_ = false;
                if (locked_) {
                    db_.unlock();
                    locked_ = false;
                }
                throw;
            }
        }
    }

    /// commits active transaction and marks guard inert
    void commit() {
        if (active_ && !committed_) {
            try {
                db_.exec("COMMIT");
            } catch (...) {
                if (locked_) {
                    db_.unlock();
                    locked_ = false;
                }
                throw;
            }
            committed_ = true;
            if (locked_) {
                db_.unlock();
                locked_ = false;
            }
        }
    }

    /// rolls back uncommitted transaction on scope exit
    ~TransactionGuard() {
        if (active_ && !committed_) {
            try {
                db_.exec("ROLLBACK");
            } catch (...) {
                (void)0; // explicitly suppress rollback errors during exception unwinding
            }
            if (locked_) {
                db_.unlock();
                locked_ = false;
            }
        }
    }

    TransactionGuard(const TransactionGuard&) = delete;
    TransactionGuard& operator=(const TransactionGuard&) = delete;
    TransactionGuard(TransactionGuard&&) = delete;
    TransactionGuard& operator=(TransactionGuard&&) = delete;

private:
    Backend& db_;
    bool active_ = false;
    bool committed_ = false;
    bool locked_ = false;
};

/// validates sql table or column identifier to prevent injection
///
/// inv     matches [A-Za-z_][A-Za-z0-9_]{0,63}; first character cannot be digit
/// in      s non-empty identifier up to 64 bytes; what names diagnostic field
/// err     throws std::invalid_argument on syntax violation
void requireIdentifier(const std::string& s, const char* what) {
    auto isAsciiAlpha = [](char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    auto isAsciiAlnum = [&isAsciiAlpha](char c) noexcept { return isAsciiAlpha(c) || (c >= '0' && c <= '9'); };
    const bool ok = !s.empty() && s.size() <= 64 && (isAsciiAlpha(s[0]) || s[0] == '_') &&
                    std::ranges::all_of(s, [&isAsciiAlnum](char c) { return isAsciiAlnum(c) || c == '_'; });
    if (!ok) throw std::invalid_argument(std::string(what) + " must match [A-Za-z_][A-Za-z0-9_]{0,63}: '" + s + "'");
}

/// quotes identifier using ansi sql standard double-quote escaping
///
/// in      name identifier string view
/// out     allocated string wrapped in quotes with internal quotes doubled
std::string quoteIdentifier(std::string_view name) {
    std::string s;
    s.reserve(name.size() + 4);
    s.push_back('"');
    for (char ch : name) {
        if (ch == '"') s.push_back('"');
        s.push_back(ch);
    }
    s.push_back('"');
    return s;
}

/// validates semantic meaning name format
///
/// inv     matches [A-Za-z0-9_.-]{1,128}
/// in      s non-empty string up to 128 characters
/// err     throws std::invalid_argument on syntax violation
void requireName(const std::string& s) {
    if (s.empty() || s.size() > 128 || !std::ranges::all_of(s, [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
                   c == '-';
        }))
        throw std::invalid_argument("meaning must match [A-Za-z0-9_.-]{1,128}: '" + s + "'");
}

/// validates user pattern to guard against unbounded complexity or ReDoS
/// in     pattern regex pattern string
/// err    throws std::invalid_argument if pattern is empty or exceeds 4096 characters
void requirePattern(const std::string& pattern) {
    if (pattern.empty()) {
        throw std::invalid_argument("pattern must not be empty");
    }
    if (pattern.size() > 4096) {
        throw std::invalid_argument("pattern exceeds maximum length of 4096 characters");
    }
}

/// serializes floating-point number preserving full ieee-754 precision
///
/// in      finite double-precision value
/// out     string with 17 decimal digits of precision
std::string num(double d) {
#if defined(__APPLE__)
    std::array<char, 64> buf{};
    int n = std::snprintf(buf.data(), buf.size(), "%.17g", d);
    if (n <= 0 || static_cast<std::size_t>(n) >= buf.size()) return "0.0";
    return {buf.data(), static_cast<std::size_t>(n)};
#else
    std::array<char, 64> buf{};
    auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), d);
    if (ec != std::errc()) return "0.0";
    return {buf.data(), static_cast<std::size_t>(ptr - buf.data())};
#endif
}

/// checks whether candidate column name collides with internal engine slots
///
/// inv     prevents user columns from shadowing form_no, form_id, oi, os,
///         rbf, rbi, rbs, rb_*, i<digits>, or d<digits>
/// in      c column name to validate
/// out     true if name is reserved for engine decomposition
bool reservedColumn(std::string_view c) {
    if (c == "form_no" || c == "form_id" || c == "oi" || c == "os" || c == "rbf" || c == "rbi" || c == "rbs") {
        return true;
    }
    if (c.starts_with("rb_")) {
        return true;
    }
    if (c.size() > 1 && (c[0] == 'i' || c[0] == 'd')) {
        for (std::size_t i = 1; i < c.size(); ++i) {
            if (static_cast<unsigned char>(c[i]) < '0' || static_cast<unsigned char>(c[i]) > '9') {
                return false;
            }
        }
        return true;
    }
    return false;
}

/// maps distinct string values to contiguous zero-based 32-bit integer ids
///
/// inv     idempotent: identical strings receive identical ids
/// own     retains copies of interned strings in internal map
class Interner {
public:
    /// returns existing id or interns s with next available sequential id
    std::uint32_t operator()(const std::string& s) {
        if (ids_.size() >= std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("interner capacity exceeded UINT32_MAX");
        }
        return ids_.try_emplace(s, static_cast<std::uint32_t>(ids_.size())).first->second;
    }
    /// view of interned string-to-id dictionary
    [[nodiscard]] const std::unordered_map<std::string, std::uint32_t>& map() const RZ_LIFETIMEBOUND { return ids_; }

private:
    std::unordered_map<std::string, std::uint32_t> ids_;
};

} // namespace

// -- engine lifecycle and catalog detection -----------------------------------

/// constructs engine control plane and attaches to target table
///
/// in      backend database connection; config specifying table and parameters
/// own     borrows backend reference; initializes dialect strategy
/// thread  acquires mu_ during catalog initialization
/// err     throws std::invalid_argument if table, column, tau, or eps invalid
RazobillEngine::RazobillEngine(Backend& backend, Config config) : db_(backend), cfg_(std::move(config)) {
    cfg_.messageColumn = cfg_.resolvedMessageColumn();
    cfg_.message_column = cfg_.messageColumn;
    cfg_.scanMode = cfg_.resolvedScanMode();
    cfg_.scan_mode = cfg_.scanMode;

    requireIdentifier(cfg_.table, "table");
    requireIdentifier(cfg_.messageColumn, "messageColumn");
    if (!cfg_.purpose.empty()) requireIdentifier(cfg_.purpose, "purpose");
    if (cfg_.tau <= 0.5 || cfg_.tau > 1.0) throw std::invalid_argument("tau must be in (0.5, 1]");
    if (cfg_.eps < 0) throw std::invalid_argument("eps must be >= 0");
    dx_ = makeDialect(db_.flavor());
    phys_ = view_ = cfg_.table;
    std::scoped_lock lock(mu_);
    // each engine owns a private catalog namespace so that dictionary GC,
    // layout and rules of one table can never touch another table; storage
    // written by older versions (global rb_* names) keeps its legacy names
    prefix_ = cfg_.table + "_";
    if (legacyStorageLocked()) prefix_.clear();
    installLocked();
    detectLocked();
}

/// true when this table's compact storage predates per-table namespaces
///
/// inv     caller must hold mu_; read-only catalog probes scoped to the
///         current database and schema
/// out     true iff global rb_form exists, <table>_rb_form does not, and the
///         table already has compact or indexed storage
bool RazobillEngine::legacyStorageLocked() const {
    const auto& d = *dx_;
    const std::string t = cfg_.table;
    const auto exists = [&](const std::string& name) {
        if (d.flavor() == Flavor::DuckDB) {
            return db_.query("SELECT count(*) FROM duckdb_tables() WHERE database_name = current_database() "
                             "AND schema_name = current_schema() AND table_name = " +
                             d.quote(name))
                       .i64(0, 0) > 0;
        }
        return db_.query("SELECT count() FROM system.tables WHERE database = currentDatabase() AND name = " +
                         d.quote(name))
                   .i64(0, 0) > 0;
    };
    if (exists(t + "_rb_form") || !exists("rb_form")) return false;
    if (d.flavor() == Flavor::DuckDB) {
        return exists(t + "_c") ||
               db_.query("SELECT count(*) FROM duckdb_columns() WHERE database_name = current_database() AND "
                         "schema_name = current_schema() AND table_name = " +
                         d.quote(t) + " AND column_name = 'form_no'")
                       .i64(0, 0) > 0;
    }
    return db_.query("SELECT count() FROM system.columns WHERE database = currentDatabase() AND table = " +
                     d.quote(t) + " AND name = 'form_id'")
               .i64(0, 0) > 0;
}

/// probes database catalog to detect existing table layout and form collisions
///
/// inv     caller must hold mu_
/// thread  guarded by mu_
void RazobillEngine::detectLocked() {
    const auto& d = *dx_;
    const std::string t = cfg_.table;
    if (d.flavor() == Flavor::DuckDB) {
        if (db_.query("SELECT count(*) FROM duckdb_tables() WHERE database_name = current_database() AND "
                     "schema_name = current_schema() AND table_name = " + d.quote(t + "_c")).i64(0, 0) > 0) {
            phys_ = t + "_c";
            view_ = t;
            compact_ = true;
            ready_ = true;
        } else if (db_.query("SELECT count(*) FROM duckdb_columns() WHERE database_name = current_database() AND "
                             "schema_name = current_schema() AND table_name = " + d.quote(t) +
                             " AND column_name = 'form_no'")
                       .i64(0, 0) > 0) {
            phys_ = view_ = t;
            ready_ = true;
        }
    } else if (db_.query("SELECT count() FROM system.columns WHERE database = currentDatabase() AND table = " +
                         d.quote(t) + " AND name = 'form_id'")
                   .i64(0, 0) > 0) {
        phys_ = view_ = t;
        ready_ = true;
    }
    if (ready_) {
        try {
            if (db_.query("SELECT count(*) FROM (SELECT id FROM " + rb("form") + " GROUP BY id HAVING count(*) > 1)").i64(0, 0) >
                0) {
                hasCollision_ = true;
            }
        } catch (const std::exception&) { // NOLINT(bugprone-empty-catch)
            // rb_form may not exist yet prior to installLocked; safe to ignore
        }
    }
}

/// verifies that target table has been indexed or compacted before querying
///
/// inv     caller must hold mu_
/// thread  guarded by mu_
/// err     throws std::logic_error if table is unindexed and uncompacted
void RazobillEngine::requireReadyLocked() const {
    if (!ready_) throw std::logic_error("no stored events for '" + cfg_.table + "': call refresh() or compact() first");
}

/// creates internal metadata tables rb_form, rb_event, rb_meta, and rb_doubt
///
/// inv     caller must hold mu_; tables created idempotently with if not exists
/// thread  guarded by mu_
void RazobillEngine::installLocked() {
    const auto& d = *dx_;
    db_.exec("CREATE TABLE IF NOT EXISTS " + rb("form") + "(id " + d.tU64() + ", no " + d.tI32() + ", form " + d.tStr() +
             ", template " + d.tStr() + ") " + d.tableTail("id"));
    db_.exec("CREATE TABLE IF NOT EXISTS " + rb("event") + "(v " + d.tI32() + ", kind " + d.tStr() + ", meaning " + d.tStr() +
             ", pattern " + d.tStr() + ", weight " + d.tF64() + ") " + d.tableTail("v"));
    db_.exec("CREATE TABLE IF NOT EXISTS " + rb("meta") + "(v " + d.tI32() + ") " + d.tableTail("v"));
    db_.exec("CREATE TABLE IF NOT EXISTS " + rb("doubt") + "(form_id " + d.tU64() + ", form " + d.tStr() +
             ", meaning " + d.tNullableStr() + ", doubt " + d.tF64() + ") " + d.tableTail("form_id, form"));
    if (db_.query("SELECT count(*) FROM " + rb("meta") + "").i64(0, 0) == 0) db_.exec("INSERT INTO " + rb("meta") + " VALUES (0)");
}

/// extracts unique log forms from query and registers them in rb_form
///
/// inv     each distinct form receives 64-bit xxHash64 id and dense 32-bit no;
///         detects 64-bit hash collisions and switches engine to dense form_no
/// in      selectForms subquery returning column named form
/// out     count of newly inserted forms
/// thread  guarded by mu_
std::size_t RazobillEngine::registerFormsLocked(const std::string& selectForms) {
    const auto& d = *dx_;
    const auto before = db_.query("SELECT count(*) FROM " + rb("form") + "").i64(0, 0);
    // sequential form numbers use signed 32-bit ints; 2 billion forms exceeds
    // log corpus needs while halving foreign key footprint in columnar storage
    db_.exec("INSERT INTO " + rb("form") + " SELECT " + d.hashExpr("form") +
             ", CAST((SELECT count(*) FROM " + rb("form") + ") + row_number() OVER (ORDER BY form) - 1 AS " + d.tI32() +
             "), form, " + d.templateExpr("form") + " FROM (SELECT DISTINCT form FROM (" + selectForms +
             ") WHERE form IS NOT NULL AND form NOT IN (SELECT form FROM " + rb("form") + " WHERE form IS NOT NULL))");
    const auto collisionCount =
        db_.query("SELECT count(*) FROM (SELECT id FROM " + rb("form") + " GROUP BY id HAVING count(*) > 1)").i64(0, 0);
    if (collisionCount != 0) {
        // 64-bit collision detected; fall back to dense 32-bit sequential keys
        hasCollision_ = true;
    }
    const auto inserted = static_cast<std::size_t>(db_.query("SELECT count(*) FROM " + rb("form") + "").i64(0, 0) - before);
    if (inserted > 0) {
        resolveCache_.clear();
    }
    return inserted;
}

/// resolves physical foreign key column name based on layout and collisions
///
/// inv     caller must hold mu_
/// out     "form_no" when compact, dense key, or collision; else "form_id"
/// thread  guarded by mu_
std::string_view RazobillEngine::physKeyLocked() const {
    return compact_ || dx_->denseFormKey() || hasCollision_ ? "form_no" : "form_id";
}

// -- indexing, compaction, and verification -----------------------------------

/// indexes unindexed log lines in additive mode without moving raw text bytes
///
/// inv     preserves raw text in-place; assigns form_no or form_id
/// out     count of newly registered unique forms
/// thread  acquires mu_
/// err     propagates backend sql execution errors
std::size_t RazobillEngine::refresh() {
    std::scoped_lock lock(mu_);
    const auto& d = *dx_;
    const std::string t = cfg_.table;
    const std::string m = cfg_.messageColumn;
    const auto sMode = cfg_.scanMode;
    std::size_t added = 0;
    if (d.denseFormKey()) {
        db_.exec("ALTER TABLE " + t + " ADD COLUMN IF NOT EXISTS form_no " + d.tI32());
        added = registerFormsLocked("SELECT " + d.formExpr(m, sMode) + " AS form FROM " + t + " WHERE form_no IS NULL");
        db_.exec("UPDATE " + t + " SET form_no = f.no FROM " + rb("form") + " f WHERE " + t +
                 ".form_no IS NULL AND f.form = " + d.formExpr(t + "." + m, sMode));
    } else {
        const bool have =
            db_.query("SELECT count() FROM system.columns WHERE database = currentDatabase() AND table = " +
                      d.quote(t) + " AND name = 'form_id'")
                .i64(0, 0) > 0;
        if (!have) {
            db_.exec("ALTER TABLE " + t + " ADD COLUMN form_id UInt64 MATERIALIZED " +
                     d.hashExpr(d.formExpr(m, sMode)));
            db_.exec("ALTER TABLE " + t + " MATERIALIZE COLUMN form_id SETTINGS mutations_sync = 2");
        }
        added = registerFormsLocked("SELECT " + d.formExpr(m, sMode) + " AS form FROM " + t);
    }
    phys_ = view_ = t;
    compact_ = false;
    ready_ = true;
    createViewsLocked();
    rebuildDerivedLocked();
    return added;
}

/// decomposes source table into typed columnar slots and reconstruction view
///
/// inv     exact lossless invertibility: RzJoin(RzSplit(m)) == m for every row;
///         pins law join(split(x)) == x in test/integration/test_compact.py
/// in      src source table containing messageColumn and sidecar columns
/// out     total event count in newly created compact physical table
/// thread  acquires mu_
/// err     throws std::logic_error if dialect lacks compact support;
///         throws std::invalid_argument if src lacks messageColumn or reserved
std::uint64_t RazobillEngine::compact(const std::string& src, const std::string& token) {
    std::scoped_lock lock(mu_);
    const auto& d = *dx_;
    if (!d.supportsCompact()) throw std::logic_error("compact storage is not supported on this engine");
    requireIdentifier(src, "source table");
    const std::string m = cfg_.resolvedMessageColumn();
    const std::string dst = cfg_.table + "_c";
    const std::string name = cfg_.table;

    std::vector<std::string> oth;
    const auto cols = db_.query("DESCRIBE " + src);
    bool hasMsg = false;
    for (std::size_t r = 0; r < cols.rows.size(); ++r) {
        const auto& c = cols.str(r, 0);
        if (c == m) {
            hasMsg = true;
            continue;
        }
        if (reservedColumn(c)) throw std::invalid_argument("source column name is reserved: " + c);
        oth.push_back(c);
    }
    if (!hasMsg) throw std::invalid_argument("source table has no column " + m);

    // O1: Check idempotency tracking table rb("sources")
    db_.exec("CREATE TABLE IF NOT EXISTS " + rb("sources") + "(src VARCHAR, token VARCHAR, row_count BIGINT, row_checksum UBIGINT)");
    const auto srcStats = db_.query("SELECT count(*), coalesce(bit_xor(hash(" + quoteIdentifier(m) + ")), 0) FROM " + src);
    const std::int64_t srcRows = srcStats.i64(0, 0);
    const std::uint64_t srcChecksum = srcStats.u64(0, 1);

    const bool checkIdemp = !token.empty() || cfg_.idempotent;
    if (checkIdemp) {
        const bool dstExists = db_.query("SELECT count(*) FROM duckdb_tables() WHERE database_name = current_database() AND "
                                         "schema_name = current_schema() AND table_name = " + d.quote(dst)).i64(0, 0) > 0;
        if (dstExists) {
            std::string q = "SELECT count(*) FROM " + rb("sources") + " WHERE src = " + d.quote(src);
            if (!token.empty()) {
                q += " AND token = " + d.quote(token);
            } else {
                q += " AND row_count = " + std::to_string(srcRows) + " AND row_checksum = " + std::to_string(srcChecksum);
            }
            if (db_.query(q).i64(0, 0) > 0) {
                // Batch has already been compacted into dst; ensure state & views are up to date and return committed count
                phys_ = dst;
                view_ = name;
                compact_ = true;
                ready_ = true;
                return static_cast<std::uint64_t>(db_.query("SELECT count(*) FROM " + dst).i64(0, 0));
            }
        }
    }

    TransactionGuard tx(db_, d.flavor());
    auto list = [&](const std::string& prefix) {
        std::string s;
        for (const auto& c : oth)
            s += prefix + quoteIdentifier(c) + ", ";
        return s;
    };

    db_.exec("CREATE OR REPLACE TEMP TABLE " + rb("s") + " AS WITH parsed AS (SELECT " + list("") + "RzSplit(" + m +
             ") AS rz_parsed FROM " + src + ") SELECT " + list("") +
             "rz_parsed.template AS rbf, rz_parsed.ints AS rbi, rz_parsed.strs AS rbs FROM parsed");
    db_.exec("CREATE TABLE IF NOT EXISTS " + rb("str") + "(id INTEGER, s VARCHAR)");
    db_.exec("CREATE TABLE IF NOT EXISTS " + rb("layout") + "(ki INTEGER, ks INTEGER)");
    // compute 99th percentile variable counts capped at 32 to size slot schema
    // ~ quantile 0.99 caps outliers so 50-variable lines do not widen table
    if (db_.query("SELECT count(*) FROM " + rb("layout") + "").i64(0, 0) == 0)
        db_.exec("INSERT INTO " + rb("layout") + " SELECT CAST(least(coalesce(quantile_disc(len(rbi), 0.99), 0), 32) AS INTEGER), "
                 "CAST(least(coalesce(quantile_disc(len(rbs), 0.99), 0), 32) AS INTEGER) FROM " + rb("s") + "");
    const auto lay = db_.query("SELECT ki, ks FROM " + rb("layout") + "");
    const auto ki = std::clamp<std::int64_t>(lay.i64(0, 0), 0, 32);
    const auto ks = std::clamp<std::int64_t>(lay.i64(0, 1), 0, 32);

    registerFormsLocked("SELECT rbf AS form FROM " + rb("s") + "");
    db_.exec("INSERT INTO " + rb("str") + " SELECT CAST(coalesce((SELECT max(id) + 1 FROM " + rb("str") + "), 0) + row_number() OVER () - 1 "
             "AS INTEGER), s "
             "FROM (SELECT DISTINCT unnest(rbs[1:" +
             std::to_string(ks) +
             "]) AS s FROM " + rb("s") + ") WHERE s IS NOT NULL AND s NOT IN (SELECT s FROM " + rb("str") + " WHERE s IS NOT NULL)");

    std::string sel = "SELECT " + list("s.") + "f.no AS form_no, ";
    for (long long j = 0; j < ki; ++j)
        sel += "rbi[" + std::to_string(j + 1) + "] AS i" + std::to_string(j) + ", ";
    for (long long j = 0; j < ks; ++j)
        sel += "d" + std::to_string(j) + ".id AS d" + std::to_string(j) + ", ";
    sel += "CASE WHEN len(rbi) > " + std::to_string(ki) + " THEN rbi[" + std::to_string(ki + 1) +
           ":] END AS oi, CASE WHEN len(rbs) > " + std::to_string(ks) + " THEN rbs[" + std::to_string(ks + 1) +
           ":] END AS os FROM " + rb("s") + " s LEFT JOIN " + rb("form") + " f ON f.form = s.rbf";
    for (long long j = 0; j < ks; ++j)
        sel += " LEFT JOIN " + rb("str") + " d" + std::to_string(j) + " ON d" + std::to_string(j) + ".s = s.rbs[" +
               std::to_string(j + 1) + "]";
    if (db_.query("SELECT count(*) FROM duckdb_tables() WHERE database_name = current_database() AND "
                 "schema_name = current_schema() AND table_name = " + d.quote(dst)).i64(0, 0) == 0)
        db_.exec("CREATE TABLE " + dst + " AS " + sel);
    else
        db_.exec("INSERT INTO " + dst + " BY NAME " + sel);

    std::string ivs;
    std::string svs;
    std::string joins;
    for (long long j = 0; j < ki; ++j)
        ivs += "c.i" + std::to_string(j) + ", ";
    for (long long j = 0; j < ks; ++j) {
        svs += "d" + std::to_string(j) + ".s, ";
        joins +=
            " LEFT JOIN " + rb("str") + " d" + std::to_string(j) + " ON d" + std::to_string(j) + ".id = c.d" + std::to_string(j);
    }
    // reconstructs original message byte-for-byte from slots and dictionary
    db_.exec("CREATE OR REPLACE VIEW " + name + " AS SELECT " + list("c.") + "c.form_no, RzJoin(f.form, " + ivs + svs +
             "c.oi, c.os) AS " + m + " FROM " + dst + " c LEFT JOIN " + rb("form") + " f ON f.no = c.form_no" + joins);
    // in-memory state must mirror committed catalog state: snapshot, and restore
    // on any failure so a rolled-back compact() leaves the engine as it was
    const std::string savedPhys = phys_;
    const std::string savedView = view_;
    const bool savedCompact = compact_;
    const bool savedReady = ready_;
    const bool savedCollision = hasCollision_;
    try {
        phys_ = dst;
        view_ = name;
        compact_ = true;
        ready_ = true;
        createViewsLocked();
        rebuildDerivedLocked();
        // staging holds the whole decomposed batch; release it with the commit
        db_.exec("DROP TABLE IF EXISTS " + rb("s"));
        db_.exec("INSERT INTO " + rb("sources") + " VALUES (" + d.quote(src) + ", " + d.quote(token) + ", " +
                 std::to_string(srcRows) + ", " + std::to_string(srcChecksum) + ")");
        tx.commit();
    } catch (...) {
        phys_ = savedPhys;
        view_ = savedView;
        compact_ = savedCompact;
        ready_ = savedReady;
        hasCollision_ = savedCollision;
        throw;
    }
    return static_cast<std::uint64_t>(db_.query("SELECT count(*) FROM " + dst).i64(0, 0));
}

/// performs bidirectional symmetric difference check between source and view
///
/// inv     returns 0 if and only if view matches source byte-for-byte;
///         verifies (src EXCEPT ALL view) + (view EXCEPT ALL src) == 0
/// in      src table containing original uncompacted events
/// out     number of mismatched or missing rows across all columns
/// thread  acquires mu_
/// err     throws std::logic_error if not in compact mode
std::uint64_t RazobillEngine::verify(const std::string& src) {
    std::scoped_lock lock(mu_);
    requireIdentifier(src, "source table");
    requireReadyLocked();
    if (!compact_) throw std::logic_error("verify() applies to compact mode");
    const auto cols = db_.query("DESCRIBE " + src);
    std::string sel;
    for (std::size_t r = 0; r < cols.rows.size(); ++r)
        sel += (r ? ", " : "") + quoteIdentifier(cols.str(r, 0));
    const std::string a = "SELECT " + sel + " FROM " + src;
    const std::string b = "SELECT " + sel + " FROM " + view_;
    return static_cast<std::uint64_t>(db_.query("SELECT (SELECT count(*) FROM (" + a + " EXCEPT ALL " + b +
                                                ")) + (SELECT count(*) FROM (" + b + " EXCEPT ALL " + a + "))")
                                          .i64(0, 0));
}

// -- dictionary maintenance and garbage collection ----------------------------

/// purges unreferenced string dictionary entries from rb_str
///
/// inv     leaves referenced strings intact; preserves ids of active entries
/// out     DictionaryStats reporting entries before, after, and purged count
/// thread  acquires mu_
/// err     throws std::logic_error if not in compact mode
DictionaryStats RazobillEngine::gcDictionary() {
    std::scoped_lock lock(mu_);
    requireReadyLocked();
    if (!compact_) throw std::logic_error("gcDictionary() applies to compact mode");

    const auto lay = db_.query("SELECT ks FROM " + rb("layout") + "");
    if (lay.empty()) return {};
    const auto ks = lay.i64(0, 0);
    const auto before = static_cast<std::size_t>(db_.query("SELECT count(*) FROM " + rb("str") + "").i64(0, 0));
    if (before == 0 || ks == 0) {
        DictionaryStats res;
        res.totalBefore = res.total_before = before;
        res.activeAfter = res.active_after = before;
        res.purgedCount = res.purged_count = 0;
        return res;
    }

    std::string refUnion;
    for (long long j = 0; j < ks; ++j) {
        if (!refUnion.empty()) refUnion += " UNION ALL ";
        refUnion +=
            "SELECT d" + std::to_string(j) + " AS id FROM " + phys_ + " WHERE d" + std::to_string(j) + " IS NOT NULL";
    }

    db_.exec("CREATE OR REPLACE TEMP TABLE " + rb("active_str_ids") + " AS SELECT DISTINCT id FROM (" + refUnion +
             ") WHERE id IS NOT NULL");
    const auto active = static_cast<std::size_t>(db_.query("SELECT count(*) FROM " + rb("active_str_ids") + "").i64(0, 0));
    const std::size_t purged = before > active ? before - active : 0;

    if (purged > 0) {
        db_.exec("DELETE FROM " + rb("str") + " WHERE id IS NOT NULL AND id NOT IN (SELECT id FROM " + rb("active_str_ids") + " WHERE id IS "
                 "NOT NULL)");
    }
    db_.exec("DROP TABLE IF EXISTS " + rb("active_str_ids") + "");

    DictionaryStats res;
    res.totalBefore = res.total_before = before;
    res.activeAfter = res.active_after = active;
    res.purgedCount = res.purged_count = purged;
    return res;
}

/// renumbers sparse string dictionary ids in rb_str to contiguous [0, count-1]
///
/// inv     rewrites keys d0..d{ks-1} in compact physical table atomically
/// thread  acquires mu_
/// err     throws std::logic_error if not in compact mode
void RazobillEngine::compactDictionary() {
    std::scoped_lock lock(mu_);
    requireReadyLocked();
    if (!compact_) throw std::logic_error("compactDictionary() applies to compact mode");

    const auto lay = db_.query("SELECT ks FROM " + rb("layout") + "");
    if (lay.empty()) return;
    const auto ks = lay.i64(0, 0);
    if (ks == 0) return;

    const auto count = db_.query("SELECT count(*) FROM " + rb("str") + "").i64(0, 0);
    if (count == 0) return;
    const auto maxId = db_.query("SELECT coalesce(max(id), -1) FROM " + rb("str") + "").i64(0, 0);
    if (maxId == count - 1) return;

    TransactionGuard tx(db_, dx_->flavor());
    db_.exec("CREATE OR REPLACE TEMP TABLE " + rb("str_remap") + " AS SELECT id AS old_id, CAST(row_number() OVER (ORDER BY id) - "
             "1 AS INTEGER) AS new_id, s FROM " + rb("str") + "");
    for (long long j = 0; j < ks; ++j) {
        const std::string dj = "d" + std::to_string(j);
        std::string updateSql;
        updateSql.reserve((phys_.size() * 2) + (dj.size() * 2) + 64);
        updateSql.append("UPDATE ")
            .append(phys_)
            .append(" SET ")
            .append(dj)
            .append(" = remap.new_id FROM " + rb("str_remap") + " remap WHERE ")
            .append(phys_)
            .append(".")
            .append(dj)
            .append(" = remap.old_id");
        db_.exec(updateSql);
    }
    db_.exec("CREATE OR REPLACE TABLE " + rb("str") + " AS SELECT new_id AS id, s FROM " + rb("str_remap") + " ORDER BY id");
    db_.exec("DROP TABLE IF EXISTS " + rb("str_remap") + "");
    tx.commit();
}

// -- semantic views, rule evaluation, and doubt -------------------------------

/// constructs analytical view <table_name>_m joining logs with form and doubt
///
/// inv     caller must hold mu_
/// thread  guarded by mu_
void RazobillEngine::createViewsLocked() {
    const std::string join =
        compact_ || dx_->denseFormKey() || hasCollision_
            ? " LEFT JOIN " + rb("form") + " f ON f.no = e.form_no LEFT JOIN " + rb("doubt") + " d ON d.form_id = f.id AND d.form = f.form"
            : " LEFT JOIN " + rb("form") + " f ON f.id = e.form_id LEFT JOIN " + rb("doubt") + " d ON d.form_id = e.form_id AND d.form = f.form";
    db_.exec("CREATE OR REPLACE VIEW " + view_ +
             "_m AS SELECT e.*, f.template AS template, d.meaning AS meaning, d.doubt AS doubt FROM " + view_ + " e" +
             join);
}

/// replays rb_event log up to version to compute active semantic rule set
///
/// inv     caller must hold mu_; retire events purge matching previous rules
/// in      version upper bound event version to evaluate
/// out     vector of active classification rules with pattern and weight
/// thread  guarded by mu_
std::vector<RazobillEngine::Rule> RazobillEngine::activeRulesLocked(int version) {
    const auto ev = db_.query(
        "SELECT kind, meaning, pattern, weight FROM " + rb("event") + " WHERE v <= " + std::to_string(version) + " ORDER BY v");
    std::vector<Rule> rules;
    for (std::size_t r = 0; r < ev.rows.size(); ++r) {
        const auto& kind = ev.str(r, 0);
        const auto& meaning = ev.str(r, 1);
        if (kind == "define")
            rules.push_back({.meaning = meaning, .pattern = ev.str(r, 2), .weight = ev.f64(r, 3)});
        else
            std::erase_if(rules, [&](const Rule& x) { return x.meaning == meaning; });
    }
    return rules;
}

/// matches active rules against template dictionary to compute posterior P(W|F)
///
/// inv     weights normalized per form so sum of P(W|F) equals 1.0
/// in      rules active semantic classification rules
/// out     vector of FormP tuples (form_id, meaning, normalized probability)
/// thread  guarded by mu_
/// err     throws std::invalid_argument if regex pattern fails engine syntax
std::vector<RazobillEngine::FormP> RazobillEngine::resolveLocked(const std::vector<Rule>& rules) {
    std::map<std::tuple<std::uint64_t, std::string, std::string>, double> w;
    std::map<std::pair<std::uint64_t, std::string>, double> total;
    for (const auto& rule : rules) {
        Table t;
        try {
            t = db_.query("SELECT id, form FROM " + rb("form") + " WHERE " + dx_->matches("template", dx_->quote(rule.pattern)));
        } catch (const BackendError& e) {
            throw std::invalid_argument(std::string("invalid pattern '") + rule.pattern + "': " + e.what());
        }
        for (std::size_t r = 0; r < t.rows.size(); ++r) {
            const auto id = t.u64(r, 0);
            const auto formStr = t.str(r, 1);
            w[{id, formStr, rule.meaning}] += rule.weight;
            total[{id, formStr}] += rule.weight;
        }
    }
    std::vector<FormP> out;
    out.reserve(w.size());
    for (const auto& [key, weight] : w) {
        const auto& [id, formStr, meaning] = key;
        out.push_back({.form = id, .formStr = formStr, .meaning = meaning, .p = weight / total[{id, formStr}]});
    }
    return out;
}

std::vector<RazobillEngine::FormP> RazobillEngine::resolveVersionLocked(int version) {
    auto it = resolveCache_.find(version);
    if (it != resolveCache_.end()) {
        return it->second;
    }
    auto res = resolveLocked(activeRulesLocked(version));
    resolveCache_[version] = res;
    return res;
}

/// recalculates rb_doubt table containing maximum posterior meaning and entropy
///
/// inv     caller must hold mu_; assigns meaning if pmax >= tau, else null;
///         doubt H(W|F) is computed as Shannon entropy in bits
/// thread  guarded by mu_
void RazobillEngine::rebuildDerivedLocked() {
    const int curV = static_cast<int>(db_.query("SELECT v FROM " + rb("meta") + "").i64(0, 0));
    const auto map = resolveVersionLocked(curV);
    struct Row {
        std::string top;
        double pmax = 0, h = 0;
    };
    std::map<std::pair<std::uint64_t, std::string>, Row> rows;
    for (const auto& m : map) {
        auto& r = rows[{m.form, m.formStr}];
        if (m.p > r.pmax) {
            r.pmax = m.p;
            r.top = m.meaning;
        }
        if (m.p > 0) r.h -= m.p * std::log2(m.p);
    }
    const bool isClickHouse = (dx_->flavor() == Flavor::ClickHouse);
    const std::string targetTable = isClickHouse ? rb("doubt_swap") : rb("doubt");
    if (isClickHouse) {
        db_.exec("CREATE TABLE IF NOT EXISTS " + targetTable + "(form_id " + dx_->tU64() + ", form " + dx_->tStr() +
                 ", meaning " + dx_->tNullableStr() + ", doubt " + dx_->tF64() + ") " + dx_->tableTail("form_id, form"));
    }
    db_.exec(dx_->truncate("" + targetTable + ""));
    std::string values;
    std::size_t k = 0;
    auto flush = [&](std::string& vals) {
        if (!vals.empty()) {
            db_.exec("INSERT INTO " + targetTable + " VALUES " + vals);
            vals.clear();
        }
    };
    for (const auto& [key, r] : rows) {
        const auto& [id, formStr] = key;
        values += (values.empty() ? "(" : ", (") + std::to_string(id) + ", " + dx_->quote(formStr) + ", " +
                  (r.pmax >= cfg_.tau ? dx_->quote(r.top) : std::string("NULL")) + ", " + num(std::max(r.h, 0.0)) + ")";
        if (++k % 400 == 0) flush(values);
    }
    flush(values);
    if (isClickHouse) {
        db_.exec("EXCHANGE TABLES " + rb("doubt") + " AND " + targetTable);
        db_.exec("DROP TABLE IF EXISTS " + targetTable);
    }
}

// -- semantic rule administration and gating ----------------------------------

/// computes joint empirical event count per form and purpose column value
///
/// inv     caller must hold mu_; purpose strings interned into 32-bit ids
/// in      yids output map receiving interned purpose string-to-id mapping
/// out     vector of XY tuples (form, interned purpose y, event count n)
/// thread  guarded by mu_
/// err     throws std::length_error if distinct pairs exceed kMaxXyRows (5M)
std::vector<stats::XY> RazobillEngine::xyLocked(std::unordered_map<std::string, std::uint32_t>& yids) {
    const auto& d = *dx_;
    const std::string y = cfg_.purpose.empty() ? "'-'" : d.toText("e." + cfg_.purpose);
    std::string sql;
    if (compact_ || d.denseFormKey() || hasCollision_)
        sql = "SELECT f.id AS fid, " + y + " AS yv, count(*) AS n FROM " + phys_ +
              " e JOIN " + rb("form") + " f ON f.no = e.form_no GROUP BY f.id, yv";
    else
        sql = "SELECT e.form_id AS fid, " + y + " AS yv, count(*) AS n FROM " + phys_ +
              " e WHERE e.form_id IS NOT NULL GROUP BY e.form_id, yv";
    const auto t = db_.query(sql);
    constexpr std::size_t kMaxXyRows = 5'000'000;
    // ~ 5 million pairs is 120mb before the interner even stretches
    if (t.rows.size() > kMaxXyRows) throw std::length_error("purpose column has too many distinct values per form");
    Interner in;
    std::vector<stats::XY> out;
    out.reserve(t.rows.size());
    for (std::size_t r = 0; r < t.rows.size(); ++r)
        out.push_back({
            .form = t.u64(r, 0),
            .y = t.isNull(r, 1) ? in("\x01null") : in(t.str(r, 1)),
            .n = static_cast<double>(t.i64(r, 2)),
        });
    yids = in.map();
    return out;
}

/// queries total event count in physical table
///
/// inv     caller must hold mu_
/// out     total event count as floating-point value
/// thread  guarded by mu_
double RazobillEngine::totalEventsLocked() {
    return static_cast<double>(db_.query("SELECT count(*) FROM " + phys_).i64(0, 0));
}

/// registers semantic classification rule subject to information loss gate
///
/// inv     docs/explanation/latitude-and-doubt.md theorem 4: candidate rule
///         admitted only if volume-weighted JSD <= eps or forced;
///         guarantees information leakage I(F;Y|W) remains bounded
/// in      meaning semantic label; pattern regex; weight positive finite;
///         force bypasses gate check
/// out     Defined record with version, affected forms, and Jensen-Shannon loss
/// thread  acquires mu_
/// err     throws std::invalid_argument if invalid or purpose undeclared;
///         throws GateRefused if information loss exceeds eps without force
Defined RazobillEngine::define(const std::string& meaning, const std::string& pattern, double weight, bool force) {
    std::scoped_lock lock(mu_);
    requireName(meaning);
    requirePattern(pattern);
    if (!(weight > 0) || !std::isfinite(weight)) throw std::invalid_argument("weight must be positive and finite");
    if (cfg_.purpose.empty() && !force) throw std::invalid_argument("declare a purpose first");
    requireReadyLocked();
    const int cur = static_cast<int>(db_.query("SELECT v FROM " + rb("meta") + "").i64(0, 0));
    const int latest = static_cast<int>(db_.query("SELECT coalesce(max(v), 0) FROM " + rb("event") + "").i64(0, 0));
    if (cur < latest) {
        throw std::logic_error("cannot define() while pinned to historical version " + std::to_string(cur) +
                               " (latest is " + std::to_string(latest) + ")");
    }

    std::vector<Rule> probe = activeRulesLocked(cur);
    std::erase_if(probe, [&](const Rule& r) { return r.meaning != meaning; });
    probe.push_back({.meaning = meaning, .pattern = pattern, .weight = weight});
    std::unordered_map<std::uint64_t, bool> merged;
    for (const auto& f : resolveLocked(probe))
        merged[f.form] = true;

    std::unordered_map<std::string, std::uint32_t> yids;
    const auto xy = xyLocked(yids);

    // docs/explanation/latitude-and-doubt.md theorem 4: semantic gate protects
    // against lossy over-clustering by rejecting merges with jsd > eps
    const auto cost = stats::mergeCost(xy, merged, totalEventsLocked());
    if (cost.jsd > cfg_.eps && !force) {
        std::ostringstream o;
        o << meaning << ": merging " << merged.size() << " forms loses " << cost.jsd << " bits per merged event about '"
          << cfg_.purpose << "'";
        throw GateRefused(o.str(), cost.jsd, cost.share);
    }
    TransactionGuard tx(db_, dx_->flavor());
    const int v = static_cast<int>(db_.query("SELECT coalesce(max(v), 0) FROM " + rb("event") + "").i64(0, 0)) + 1;
    db_.exec("INSERT INTO " + rb("event") + " VALUES (" + std::to_string(v) + ", 'define', " + dx_->quote(meaning) + ", " +
             dx_->quote(pattern) + ", " + num(weight) + ")");
    db_.exec(dx_->truncate("" + rb("meta") + ""));
    db_.exec("INSERT INTO " + rb("meta") + " VALUES (" + std::to_string(v) + ")");
    resolveCache_.clear();
    rebuildDerivedLocked();
    tx.commit();

    Defined res;
    res.version = v;
    res.forms = merged.size();
    res.lossPerMergedEvent = res.loss_per_merged_event = cost.jsd;
    res.lossTableWide = res.loss_table_wide = cost.jsd * cost.share;
    return res;
}

/// appends retire event for specified meaning and updates derived views
///
/// inv     appends version event to rb_event; removes meaning from views
/// in      meaning active semantic label to retire
/// out     new metadata version number
/// thread  acquires mu_
/// err     throws std::invalid_argument if meaning is invalid or not active
int RazobillEngine::retire(const std::string& meaning) {
    std::scoped_lock lock(mu_);
    requireName(meaning);
    const int cur = static_cast<int>(db_.query("SELECT v FROM " + rb("meta") + "").i64(0, 0));
    const int latest = static_cast<int>(db_.query("SELECT coalesce(max(v), 0) FROM " + rb("event") + "").i64(0, 0));
    if (cur < latest) {
        throw std::logic_error("cannot retire() while pinned to historical version " + std::to_string(cur) +
                               " (latest is " + std::to_string(latest) + ")");
    }
    const auto rules = activeRulesLocked(cur);
    auto matchesMeaning = [&](const Rule& r) { return r.meaning == meaning; };
    if (std::ranges::none_of(rules, matchesMeaning)) {
        throw std::invalid_argument("no active meaning named " + meaning);
    }
    TransactionGuard tx(db_, dx_->flavor());
    const int v = static_cast<int>(db_.query("SELECT coalesce(max(v), 0) FROM " + rb("event") + "").i64(0, 0)) + 1;
    db_.exec("INSERT INTO " + rb("event") + " VALUES (" + std::to_string(v) + ", 'retire', " + dx_->quote(meaning) + ", '', 0)");
    db_.exec(dx_->truncate("" + rb("meta") + ""));
    db_.exec("INSERT INTO " + rb("meta") + " VALUES (" + std::to_string(v) + ")");
    resolveCache_.clear();
    rebuildDerivedLocked();
    tx.commit();
    return v;
}

/// rewinds or advances semantic metadata to a specific historical version
///
/// inv     updates rb_meta version and rebuilds rb_doubt derived state
/// in      version target version in range [0, latest]
/// thread  acquires mu_
/// err     throws std::invalid_argument if version < 0 or exceeds latest
void RazobillEngine::pin(int version) {
    std::scoped_lock lock(mu_);
    const int latest = static_cast<int>(db_.query("SELECT coalesce(max(v), 0) FROM " + rb("event") + "").i64(0, 0));
    if (version < 0 || version > latest) throw std::invalid_argument("version out of range");
    TransactionGuard tx(db_, dx_->flavor());
    db_.exec(dx_->truncate("" + rb("meta") + ""));
    db_.exec("INSERT INTO " + rb("meta") + " VALUES (" + std::to_string(version) + ")");
    rebuildDerivedLocked();
    tx.commit();
}

/// retrieves currently active semantic metadata version number
///
/// out     integer version from rb_meta
/// thread  acquires mu_
int RazobillEngine::version() {
    std::scoped_lock lock(mu_);
    return static_cast<int>(db_.query("SELECT v FROM " + rb("meta") + "").i64(0, 0));
}

// -- information metrics, inspection, and prioritization ----------------------

/// computes exact Shannon entropy decomposition across forms and meanings
///
/// inv     satisfies laws from docs/explanation/latitude-and-doubt.md:
///         H(F) = I(F;W) + H(F|W) (expression = conveyance + latitude)
///         H(W) = I(F;W) + H(W|F) (content = conveyance + doubt)
/// out     Ledger with expression, conveyance, latitude, content, and doubt
/// thread  acquires mu_
/// err     throws std::logic_error if conservation violated by > 1e-9 bits
stats::Ledger RazobillEngine::ledger() {
    std::scoped_lock lock(mu_);
    requireReadyLocked();
    const auto map = resolveVersionLocked(static_cast<int>(db_.query("SELECT v FROM " + rb("meta") + "").i64(0, 0)));
    std::unordered_map<std::string, std::uint32_t> yids;
    const auto xy = xyLocked(yids);
    std::unordered_map<std::uint64_t, double> n;
    for (const auto& r : xy)
        n[r.form] += r.n;
    Interner mi;
    std::vector<stats::Cell> cells;
    cells.reserve(map.size());
    for (const auto& m : map)
        if (n.contains(m.form)) cells.push_back({.form = m.form, .meaning = mi(m.meaning), .mass = n[m.form] * m.p});
    const auto l = stats::ledger(cells, totalEventsLocked());
    const double tolE = std::max(1e-7, 1e-9 * std::max(1.0, l.expression));
    const double tolC = std::max(1e-7, 1e-9 * std::max(1.0, l.content));
    if (std::abs(l.expression - l.conveyance - l.latitude) > tolE ||
        std::abs(l.content - l.conveyance - l.doubt) > tolC) {
        if (std::abs(l.expression - l.conveyance - l.latitude) > 1e-3 ||
            std::abs(l.content - l.conveyance - l.doubt) > 1e-3) {
            throw std::logic_error("ledger identity violated");
        }
    }
    return l;
}

/// measures residual mutual information I(F;Y|W) between forms and purpose
///
/// inv     leakage == 0 indicates zero-loss semantic compression for purpose
/// out     conditional mutual information in bits per event
/// thread  acquires mu_
double RazobillEngine::leakage() {
    std::scoped_lock lock(mu_);
    requireReadyLocked();
    const auto map = resolveVersionLocked(static_cast<int>(db_.query("SELECT v FROM " + rb("meta") + "").i64(0, 0)));
    std::unordered_map<std::string, std::uint32_t> yids;
    const auto xy = xyLocked(yids);
    Interner mi;
    std::vector<stats::FormMeaning> fm;
    fm.reserve(map.size());
    for (const auto& m : map)
        fm.push_back({.form = m.form, .meaning = mi(m.meaning), .p = m.p});
    return stats::leakage(xy, fm);
}

/// parses single log line and retrieves its assigned meaning and doubt score
///
/// inv     exact parsing via core::parse; hashes template with xxHash64
/// in      text raw log message line
/// out     MeaningOf with formId, meaning, and doubt; nullopt if unknown form
/// thread  acquires mu_
/// err     throws std::invalid_argument if message fails parser validation
std::optional<MeaningOf> RazobillEngine::meaningOf(const std::string& text) {
    core::SlotRecord slots;
    if (const auto s = core::parse(text, slots, cfg_.resolvedScanMode()); s != Status::Ok)
        throw std::invalid_argument(describe(s));
    const auto id = core::xxh64(slots.templateStr);
    std::scoped_lock lock(mu_);
    if (db_.query("SELECT count(*) FROM " + rb("form") + " WHERE id = " + std::to_string(id) +
                  " AND form = " + dx_->quote(slots.templateStr))
            .i64(0, 0) == 0)
        return std::nullopt;
    MeaningOf out{.formId = id, .meaning = std::nullopt, .doubt = std::nullopt, .form_id = id};
    const auto t = db_.query("SELECT meaning, doubt FROM " + rb("doubt") + " WHERE form_id = " + std::to_string(id) +
                             " AND form = " + dx_->quote(slots.templateStr));
    if (!t.empty()) {
        if (!t.isNull(0, 0)) out.meaning = t.str(0, 0);
        out.doubt = t.f64(0, 1);
    }
    return out;
}

/// ranks unclassified log templates by event frequency to guide rule definition
///
/// in      n maximum number of unclassified templates to return
/// out     pairs of (template_string, count) sorted descending by frequency
/// thread  acquires mu_
std::vector<std::pair<std::string, std::int64_t>> RazobillEngine::todo(std::size_t n) {
    std::scoped_lock lock(mu_);
    requireReadyLocked();
    const auto map = resolveVersionLocked(static_cast<int>(db_.query("SELECT v FROM " + rb("meta") + "").i64(0, 0)));
    std::unordered_map<std::string, std::uint32_t> yids;
    const auto xy = xyLocked(yids);
    std::unordered_map<std::uint64_t, std::int64_t> count;
    for (const auto& r : xy)
        count[r.form] += static_cast<std::int64_t>(r.n);
    for (const auto& m : map)
        count.erase(m.form);
    std::vector<std::pair<std::uint64_t, std::int64_t>> top(count.begin(), count.end());
    std::ranges::sort(top, [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    if (top.size() > n) top.resize(n);
    if (top.empty()) return {};

    std::string inList;
    for (std::size_t i = 0; i < top.size(); ++i) {
        if (i > 0) inList += ", ";
        inList += std::to_string(top[i].first);
    }
    const auto t = db_.query("SELECT id, template FROM " + rb("form") + " WHERE id IN (" + inList + ")");
    std::unordered_map<std::uint64_t, std::string> idToTmpl;
    for (std::size_t r = 0; r < t.rows.size(); ++r) {
        idToTmpl[t.u64(r, 0)] = t.str(r, 1);
    }
    std::vector<std::pair<std::string, std::int64_t>> out;
    out.reserve(top.size());
    for (const auto& [id, c] : top) {
        auto it = idToTmpl.find(id);
        out.emplace_back(it != idToTmpl.end() ? it->second : "<unknown>", c);
    }
    return out;
}

} // namespace razobill
