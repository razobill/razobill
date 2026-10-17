#pragma once

// =============================================================================
// test harness support: backend factories, path normalizer, and metric reporter
//
// discovers embedded DuckDB and chDB shared libraries via ADBC, normalizes SQL
// filesystem paths across platforms, and formats structured result metrics
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "librazobill/Backend.hpp"

namespace support {

/// reads environment variable with fallback default
inline const char* env(const char* k, const char* d) {
    const char* v = std::getenv(k);
    return v ? v : d;
}

/// converts Windows backslashes to forward slashes for SQL string literals
/// in:  filesystem path string
/// out: normalized path with all backslashes replaced by forward slashes
inline std::string normalizePath(std::string p) {
    for (char& c : p)
        if (c == '\\') c = '/';
    return p;
}

/// configures ADBC options and dynamic library discovery for embedded DuckDB
/// out: populated AdbcOptions configured with extension load command and memory
inline razobill::AdbcOptions duckdbOptions() {
    razobill::AdbcOptions o;
    o.flavor = razobill::Flavor::DuckDB;
#if defined(_WIN32)
    const char* defLib = "duckdb.dll";
    const char* defExt = "build/librazobill.duckdb_extension";
    std::string chosenLib = env("RZB_DUCKDB_LIB", defLib);
    std::error_code ec;
    if (!std::filesystem::exists(chosenLib, ec) && !std::getenv("RZB_DUCKDB_LIB")) {
        const std::array<const char*, 4> candidateDirs = {"build", "build/Release", "build/Debug", "bin"};
        for (const char* dir : candidateDirs) {
            auto candidate = std::filesystem::path(dir) / defLib;
            if (std::filesystem::exists(candidate, ec)) {
                chosenLib = candidate.string();
                break;
            }
        }
        if (!std::filesystem::exists(chosenLib, ec)) {
            const char* appData = std::getenv("APPDATA");
            if (appData) {
                const std::filesystem::path pyDir = std::filesystem::path(appData) / "Python";
                if (std::filesystem::exists(pyDir, ec)) {
                    for (const auto& pyVer : std::filesystem::directory_iterator(pyDir, ec)) {
                        if (pyVer.is_directory(ec)) {
                            const auto sitePkg = pyVer.path() / "site-packages";
                            if (std::filesystem::exists(sitePkg, ec)) {
                                for (const auto& entry : std::filesystem::directory_iterator(sitePkg, ec)) {
                                    const auto name = entry.path().filename().string();
                                    if (name.find("_duckdb") != std::string::npos &&
                                        (name.ends_with(".pyd") || name.ends_with(".dll"))) {
                                        chosenLib = entry.path().string();
                                        break;
                                    }
                                }
                                if (std::filesystem::exists(chosenLib, ec)) break;
                            }
                        }
                    }
                }
            }
        }
    }
#elif defined(__APPLE__)
    const char* defLib = "libduckdb.dylib";
    const char* defExt = "build/librazobill.duckdb_extension";
    const char* chosenLib = env("RZB_DUCKDB_LIB", defLib);
#else
    const char* defLib = "/usr/local/lib/libduckdb.so";
    const char* defExt = "build/librazobill.duckdb_extension";
    const char* chosenLib = env("RZB_DUCKDB_LIB", defLib);
#endif
    o.driverPath = o.driver_path = normalizePath(chosenLib);
    o.entrypoint = "duckdb_adbc_init";
    o.databaseOptions = o.database_options = {{"path", ":memory:"}, {"allow_unsigned_extensions", "true"}};
    o.initSql = o.init_sql = {std::string("LOAD '") + normalizePath(env("RZB_EXT", defExt)) + "'", "PRAGMA threads=4",
                              "PRAGMA max_memory='2GB'", "PRAGMA temp_directory='build/tmp_duckdb'"};
    return o;
}

/// configures ADBC options for embedded ClickHouse chDB runtime
/// out: populated AdbcOptions configured with chDB driver and preloaded Python
inline razobill::AdbcOptions chdbOptions() {
    razobill::AdbcOptions o;
    o.flavor = razobill::Flavor::ClickHouse;
    o.driverPath = o.driver_path = env("RZB_CHDB_LIB", "/usr/local/lib/python3.12/dist-packages/chdb/_chdb.abi3.so");
    o.entrypoint = "AdbcDriverInit";
    o.preloadGlobal = o.preload_global = {env("RZB_LIBPYTHON", "/lib/x86_64-linux-gnu/libpython3.12.so.1.0")};
    o.deepbind = true;
    return o;
}

/// emits structured machine-readable result metric to stdout
inline void result(const char* name, double v) {
    std::printf("RESULT %s %.12g\n", name, v);
    std::fflush(stdout);
}

// backward compatibility aliases
inline razobill::AdbcOptions duckdb_options() {
    return duckdbOptions();
}
inline razobill::AdbcOptions chdb_options() {
    return chdbOptions();
}
inline std::string normalize_path(std::string p) {
    return normalizePath(p);
}

} // namespace support
