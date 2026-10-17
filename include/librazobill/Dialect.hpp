// =============================================================================
//  file : include/librazobill/Dialect.hpp
//  role : sql dialect abstraction for DuckDB and ClickHouse engines
//  inv  : encapsulates engine differences in types, quotes, and form extraction
// =============================================================================

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "librazobill/Backend.hpp"
#include "librazobill/Scanner.hpp"

namespace razobill {

// -- dialect interface --------------------------------------------------------

/// sql dialect strategy for analytical database backends
/// inv: encapsulates syntax, type names, and string quoting across engines
class Dialect {
public:
    virtual ~Dialect() = default;
    Dialect() = default;
    Dialect(const Dialect&) = default;
    Dialect& operator=(const Dialect&) = default;
    Dialect(Dialect&&) = default;
    Dialect& operator=(Dialect&&) = default;

    /// backend flavor identifying the target engine
    /// out: Flavor::DuckDB or Flavor::ClickHouse
    /// own: pure value
    /// thread: thread-safe, const
    /// err: noexcept
    [[nodiscard]] virtual Flavor flavor() const noexcept = 0;

    /// whether backend supports compact storage schema
    /// out: true for DuckDB, false for ClickHouse
    /// thread: thread-safe, const
    /// err: noexcept
    [[nodiscard]] virtual bool supportsCompact() const noexcept = 0;

    /// whether catalog assigns dense numeric keys to template forms
    /// out: true for DuckDB catalog, false for ClickHouse hash-keyed storage
    /// thread: thread-safe, const
    /// err: noexcept
    [[nodiscard]] virtual bool denseFormKey() const noexcept = 0;

    /// escape and quote string literal for sql query text
    /// in: arbitrary raw string view
    /// out: quoted string literal enclosed in single quotes
    /// own: returns owned std::string
    /// thread: thread-safe, const
    /// err: noexcept(false) on allocation failure
    [[nodiscard]] virtual std::string quote(std::string_view s) const = 0;

    /// dialect-specific uint64 sql type name
    /// out: "UBIGINT" (DuckDB) or "UInt64" (ClickHouse)
    /// own: returns owned std::string
    /// thread: thread-safe, const
    [[nodiscard]] virtual std::string tU64() const = 0;

    /// dialect-specific int32 sql type name
    /// out: "INTEGER" (DuckDB) or "Int32" (ClickHouse)
    /// own: returns owned std::string
    /// thread: thread-safe, const
    [[nodiscard]] virtual std::string tI32() const = 0;

    /// dialect-specific float64 sql type name
    /// out: "DOUBLE" (DuckDB) or "Float64" (ClickHouse)
    /// own: returns owned std::string
    /// thread: thread-safe, const
    [[nodiscard]] virtual std::string tF64() const = 0;

    /// dialect-specific string sql type name
    /// out: "VARCHAR" (DuckDB) or "String" (ClickHouse)
    /// own: returns owned std::string
    /// thread: thread-safe, const
    [[nodiscard]] virtual std::string tStr() const = 0;

    /// dialect-specific nullable string sql type name
    /// out: "VARCHAR" (DuckDB) or "Nullable(String)" (ClickHouse)
    /// own: returns owned std::string
    /// thread: thread-safe, const
    [[nodiscard]] virtual std::string tNullableStr() const = 0;

    /// dialect-specific table engine or ordering tail clause for ddl
    /// in: orderBy column name list
    /// out: empty string (DuckDB) or
    ///      "ENGINE = MergeTree ORDER BY <orderBy>" (ClickHouse)
    /// own: returns owned std::string
    /// thread: thread-safe, const
    [[nodiscard]] virtual std::string tableTail(std::string_view orderBy) const = 0;

    /// dialect-specific expression casting an arbitrary expression to text
    /// in: sql expression string view
    /// out: "CAST(<expr> AS VARCHAR)" or "toString(<expr>)"
    /// own: returns owned std::string
    /// thread: thread-safe, const
    [[nodiscard]] virtual std::string toText(std::string_view expr) const = 0;

    /// dialect-specific regex match boolean expression
    /// in: haystack column expression and pattern literal
    /// out: "regexp_matches(h, p)" (DuckDB) or "match(h, p)" (ClickHouse)
    /// own: returns owned std::string
    /// thread: thread-safe, const
    [[nodiscard]] virtual std::string matches(std::string_view haystack, std::string_view patternLiteral) const = 0;

    /// dialect-specific expression extracting form template from column
    /// in: col column name, scan mode
    /// out: "RzTemplate(col)" (DuckDB) or regex pipeline expression
    ///      (ClickHouse)
    /// own: returns owned std::string
    /// thread: thread-safe, const
    [[nodiscard]] virtual std::string formExpr(std::string_view col,
                                               core::ScanMode mode = core::ScanMode::Semantic) const = 0;

    /// dialect-specific expression computing 64-bit template hash
    /// inv: xxHash64 bit-identical across DuckDB RzHash and ClickHouse xxHash64
    /// in: text expression
    /// out: "RzHash(text)" (DuckDB) or "xxHash64(text)" (ClickHouse)
    /// own: returns owned std::string
    /// thread: thread-safe, const
    [[nodiscard]] virtual std::string hashExpr(std::string_view text) const = 0;

    /// dialect-specific expression converting form skeleton to display template
    /// in: form skeleton expression with placeholder bytes (0x12)
    /// out: expression replacing placeholder byte with '<*>'
    /// own: returns owned std::string
    /// thread: thread-safe, const
    [[nodiscard]] virtual std::string templateExpr(std::string_view form) const = 0;

    /// dialect-specific sql statement truncating table data
    /// in: table name
    /// out: "DELETE FROM <table>" (DuckDB) or "TRUNCATE TABLE <table>" (ClickHouse)
    /// own: returns owned std::string
    /// thread: thread-safe, const
    [[nodiscard]] virtual std::string truncate(std::string_view table) const = 0;

    // -- legacy compatibility aliases -----------------------------------------

    [[nodiscard]] bool supports_compact() const noexcept { return supportsCompact(); }
    [[nodiscard]] bool dense_form_key() const noexcept { return denseFormKey(); }
    [[nodiscard]] std::string t_u64() const { return tU64(); }
    [[nodiscard]] std::string t_i32() const { return tI32(); }
    [[nodiscard]] std::string t_f64() const { return tF64(); }
    [[nodiscard]] std::string t_str() const { return tStr(); }
    [[nodiscard]] std::string t_nullable_str() const { return tNullableStr(); }
    [[nodiscard]] std::string table_tail(std::string_view orderBy) const { return tableTail(orderBy); }
    [[nodiscard]] std::string to_text(std::string_view expr) const { return toText(expr); }
    [[nodiscard]] std::string form_expr(std::string_view col, core::ScanMode mode = core::ScanMode::Semantic) const {
        return formExpr(col, mode);
    }
    [[nodiscard]] std::string hash_expr(std::string_view text) const { return hashExpr(text); }
    [[nodiscard]] std::string template_expr(std::string_view form) const { return templateExpr(form); }
};

// -- factory and legacy aliases -----------------------------------------------

/// create dialect strategy instance for given backend flavor
/// in: f backend flavor (DuckDB or ClickHouse)
/// out: non-null pointer to concrete Dialect implementation
/// own: caller owns returned unique_ptr
/// thread: thread-safe factory
/// err: noexcept(false) on allocation failure
[[nodiscard]] std::unique_ptr<Dialect> makeDialect(Flavor f);

/// legacy snake_case alias for makeDialect
[[nodiscard]] inline std::unique_ptr<Dialect> make_dialect(Flavor f) {
    return makeDialect(f);
}

} // namespace razobill
