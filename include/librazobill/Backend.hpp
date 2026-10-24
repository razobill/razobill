// =============================================================================
//  file : include/librazobill/Backend.hpp
//  role : abstract database control plane interface for ADBC, DuckDB, and chDB
//  inv  : decouples control plane queries from physical database engine
// =============================================================================

// Database control plane abstraction.
// Decouples schema compaction, catalog management, and information entropy
// audits from the underlying analytical database engine (DuckDB, ClickHouse
// chDB) using Arrow Database Connectivity (ADBC).

#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace razobill {

/// exception thrown on database query execution failure or driver loading error
struct BackendError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// supported database dialects for SQL generation and type mapping
enum class Flavor { DuckDB, ClickHouse };

/// relational column value variant supporting null, integers, floats,
/// and strings
using Value = std::variant<std::monostate, std::int64_t, std::uint64_t, double, std::string>;

/// tabular in-memory representation of database query results
/// own: owns row data and column name strings
struct Table {
    std::vector<std::string> names;
    std::vector<std::vector<Value>> rows;

    [[nodiscard]] bool empty() const noexcept { return rows.empty(); }
    [[nodiscard]] std::int64_t i64(std::size_t r, std::size_t c) const;
    [[nodiscard]] std::uint64_t u64(std::size_t r, std::size_t c) const;
    [[nodiscard]] double f64(std::size_t r, std::size_t c) const;
    [[nodiscard]] const std::string& str(std::size_t r, std::size_t c) const;
    [[nodiscard]] bool isNull(std::size_t r, std::size_t c) const {
        if (r >= rows.size() || c >= rows[r].size()) throw BackendError("row or column index out of range");
        return std::holds_alternative<std::monostate>(rows[r][c]);
    }
    [[nodiscard]] bool is_null(std::size_t r, std::size_t c) const { return isNull(r, c); }
};

/// abstract interface for executing control plane SQL and tabular queries
/// thread: instances are single-threaded; use separate instances per thread
class Backend {
public:
    virtual ~Backend() = default;
    Backend() = default;
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;
    Backend(Backend&&) = delete;
    Backend& operator=(Backend&&) = delete;

    [[nodiscard]] virtual Flavor flavor() const noexcept = 0;
    virtual void exec(std::string_view sql) = 0;
    [[nodiscard]] virtual Table query(std::string_view sql) = 0;
};

/// configuration struct for loading and initializing ADBC driver connections
struct AdbcOptions {
    Flavor flavor = Flavor::DuckDB;
    std::string driverPath;
    std::string entrypoint;
    bool deepbind = false;
    std::vector<std::string> preloadGlobal;
    std::vector<std::pair<std::string, std::string>> databaseOptions;
    std::vector<std::string> initSql;

    // Backward compatibility mappings
    std::string driver_path;
    std::vector<std::string> preload_global;
    std::vector<std::pair<std::string, std::string>> database_options;
    std::vector<std::string> init_sql;
};

/// factory function instantiating an ADBC backend driver instance
/// in: options struct with driver path, entrypoint, and database options
/// out: unique_ptr to initialized Backend instance
/// own: caller takes ownership of returned Backend pointer
/// err: throws BackendError on driver loading or connection failure
[[nodiscard]] std::unique_ptr<Backend> makeAdbcBackend(const AdbcOptions& options);

// Legacy alias
[[nodiscard]] inline std::unique_ptr<Backend> make_adbc_backend(const AdbcOptions& options) {
    return makeAdbcBackend(options);
}

} // namespace razobill
