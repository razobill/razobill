// =============================================================================
//  file : src/Dialect.cpp
//  role : sql dialect implementations for DuckDB and ClickHouse engines
//  inv  : bit-identical xxHash64 parity; exact syntax generation per engine
// =============================================================================

#include "librazobill/Dialect.hpp"

#include <string>

#include "librazobill/Scanner.hpp"

namespace razobill {

namespace {

// Token pattern matching any sequence containing at least one decimal digit
constexpr std::string_view kTokenRegex = R"([^\s=,;:()\[\]{}"'<>|/]*[0-9][^\s=,;:()\[\]{}"'<>|/]*)";
// integer pattern: 0 or a positive integer of up to 19 digits (the int64 range is checked separately)
constexpr std::string_view kIntRegex = "^(0|[1-9][0-9]{0,18})$";
constexpr unsigned char kControlReserved17 = 0x11;

// -- duckdb dialect implementation --------------------------------------------

/// concrete dialect strategy for DuckDB
/// inv: leverages loadable C extension functions (RzTemplate, RzHash)
class DuckDialect final : public Dialect {
public:
    Flavor flavor() const noexcept override { return Flavor::DuckDB; }
    bool supportsCompact() const noexcept override { return true; }
    bool denseFormKey() const noexcept override { return true; }

    /// quote string literal by doubling single quotes per SQL standard
    std::string quote(std::string_view s) const override {
        std::string o;
        o.reserve(s.size() + 16);
        o.push_back('\'');
        for (char c : s) {
            if (c == '\'') o.push_back('\'');
            o.push_back(c);
        }
        o.push_back('\'');
        return o;
    }

    std::string tU64() const override { return "UBIGINT"; }
    std::string tI32() const override { return "INTEGER"; }
    std::string tF64() const override { return "DOUBLE"; }
    std::string tStr() const override { return "VARCHAR"; }
    std::string tNullableStr() const override { return "VARCHAR"; }

    std::string tableTail(std::string_view orderBy) const override {
        (void)orderBy;
        return "";
    }

    std::string toText(std::string_view e) const override { return "CAST(" + std::string(e) + " AS VARCHAR)"; }

    std::string matches(std::string_view h, std::string_view p) const override {
        return "regexp_matches(" + std::string(h) + ", " + std::string(p) + ")";
    }

    /// generates call RzTemplate(col) for vectorized template extraction
    std::string formExpr(std::string_view col, core::ScanMode /*mode*/ = core::ScanMode::Semantic) const override {
        return "RzTemplate(" + std::string(col) + ")";
    }

    /// generates extension call RzHash(col) producing bit-identical xxHash64
    std::string hashExpr(std::string_view t) const override { return "RzHash(" + std::string(t) + ")"; }

    std::string templateExpr(std::string_view f) const override {
        return "replace(replace(" + std::string(f) + ", '{i}', '<*>'), '{s}', '<*>')";
    }

    // uses DELETE FROM because DuckDB TRUNCATE varies across table types
    // ~ delete from because truncate was restricted on attached databases
    std::string truncate(std::string_view t) const override { return "DELETE FROM " + std::string(t); }
};

// -- clickhouse dialect implementation ----------------------------------------

/// concrete dialect strategy for ClickHouse and chDB
/// inv: xxHash64 parity pinned in tests/IntegrationChdb.cpp
class ClickHouseDialect final : public Dialect {
public:
    Flavor flavor() const noexcept override { return Flavor::ClickHouse; }
    bool supportsCompact() const noexcept override { return false; }
    bool denseFormKey() const noexcept override { return false; }

    /// quote string literal using C-style backslash escaping
    std::string quote(std::string_view s) const override {
        std::string o;
        o.reserve(s.size() + 16);
        o.push_back('\'');
        for (char c : s) {
            if (c == '\0') {
                o.push_back('\\');
                o.push_back('0');
            } else {
                if (c == '\\' || c == '\'') o.push_back('\\');
                o.push_back(c);
            }
        }
        o.push_back('\'');
        return o;
    }

    std::string tU64() const override { return "UInt64"; }
    std::string tI32() const override { return "Int32"; }
    std::string tF64() const override { return "Float64"; }
    std::string tStr() const override { return "String"; }
    std::string tNullableStr() const override { return "Nullable(String)"; }

    std::string tableTail(std::string_view orderBy) const override {
        return "ENGINE = MergeTree ORDER BY " + std::string(orderBy);
    }

    std::string toText(std::string_view e) const override { return "toString(" + std::string(e) + ")"; }

    std::string matches(std::string_view h, std::string_view p) const override {
        return "match(" + std::string(h) + ", " + std::string(p) + ")";
    }

    /// builds pure-SQL regex pipeline to extract templates in ClickHouse
    // ~ pure sql regex because running custom c++ in chdb is an adventure
    std::string formExpr(std::string_view col, core::ScanMode /*mode*/ = core::ScanMode::Semantic) const override {
        const std::string c(col);
        const std::string tok = quote(kTokenRegex);
        const std::string ints = quote(kIntRegex);
        // arrayMap over two aligned arrays (gaps, placeholders); gaps escape literal braces as {{ }} and an
        // integer token must also fit int64, exactly as the C++ scanner decides (ScanMode::Exact)
        return "arrayStringConcat(arrayMap((sep, repl) -> concat(replaceAll(replaceAll(sep, '{', '{{'), '}', '}}'), repl), "
               "splitByRegexp(" + tok + ", " + c + "), arrayPushBack(arrayMap(t -> if(match(t, " + ints +
               ") AND toInt64OrNull(t) IS NOT NULL, '{i}', '{s}'), extractAll(" + c + ", " + tok + ")), '')))";
    }

    /// maps to native xxHash64 built-in function
    /// inv: xxHash64 matches RzHash bit-for-bit (tests/IntegrationChdb.cpp)
    std::string hashExpr(std::string_view t) const override { return "xxHash64(" + std::string(t) + ")"; }

    std::string templateExpr(std::string_view f) const override {
        const std::string s(f);
        const std::string ch18 = "char(" + std::to_string(static_cast<int>(core::kPlaceholder)) + ")";
        const std::string ch19 = "char(" + std::to_string(static_cast<int>(core::kTypeSeparator)) + ")";
        return "if(position(" + s + ", " + ch19 + ") > 0, replaceAll(substring(" + s + ", 1, position(" + s + ", " +
               ch19 + ") - 1), " + ch18 + ", '<*>'), replaceAll(replaceAll(" + s + ", '{i}', '<*>'), '{s}', '<*>'))";
    }

    std::string truncate(std::string_view t) const override { return "TRUNCATE TABLE " + std::string(t); }
};

} // namespace

// -- dialect factory ----------------------------------------------------------

/// construct concrete SQL dialect strategy for target analytical engine
/// in: f Flavor::DuckDB or Flavor::ClickHouse
/// out: owning unique_ptr to concrete Dialect implementation
/// thread: thread-safe, stateless factory
std::unique_ptr<Dialect> makeDialect(Flavor f) {
    if (f == Flavor::DuckDB) return std::make_unique<DuckDialect>();
    return std::make_unique<ClickHouseDialect>();
}

} // namespace razobill
