// =============================================================================
//  file : src/BackendAdbc.cpp
//  role : arrow database connectivity (adbc) dynamic bridge and arrow decoder
//  inv  : zero-copy arrow buffer decoding; thread-safe connection mutex
// =============================================================================

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <cstring>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <vector>

#include "adbc.h"
#include "librazobill/Backend.hpp"

namespace razobill {

namespace {

// -- platform dynamic library loader ------------------------------------------

/// dynamically load shared library from filesystem path
/// in: path library file path, deepbind POSIX deepbind flag, global export
/// out: OS library handle or nullptr on failure
void* sysLoadLibrary(const char* path, bool deepbind, bool global) {
#ifdef _WIN32
    (void)deepbind;
    (void)global;
    return reinterpret_cast<void*>(LoadLibraryA(path));
#else
    int flags = RTLD_NOW;
    if (global)
        flags |= RTLD_GLOBAL;
    else
        flags |= RTLD_LOCAL;
    if (deepbind) {
#if defined(RTLD_DEEPBIND)
        flags |= RTLD_DEEPBIND;
#endif
    }
    return dlopen(path, flags);
#endif
}

/// unload dynamically loaded shared library handle
void sysUnloadLibrary(void* handle) {
    if (!handle) return;
#ifdef _WIN32
    FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

/// resolve exported symbol address from loaded shared library
void* sysLoadSymbol(void* handle, const char* symbol) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(handle), symbol));
#else
    return dlsym(handle, symbol);
#endif
}

/// format OS-specific last dynamic loading error into string
std::string sysLastError() {
#ifdef _WIN32
    DWORD err = GetLastError();
    if (err == 0) return "no error";
    LPSTR buf = nullptr;
    DWORD len = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPSTR>(&buf), 0, nullptr);
    if (!buf || len == 0) return "Windows error " + std::to_string(err);
    std::string msg(buf, len);
    LocalFree(buf);
    while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n'))
        msg.pop_back();
    return msg;
#else
    const char* e = dlerror();
    return e ? e : "unknown error";
#endif
}

/// throw BackendError with formatted message and release ADBC error struct
[[noreturn]] void fail(std::string_view what, AdbcError& e) {
    std::string m(what);
    if (e.message) m += std::string(": ") + e.message;
    if (e.release) {
        e.release(&e);
    }
    throw BackendError(m);
}

// -- arrow c data interface decoder -------------------------------------------

/// return null bitmap buffer pointer from Arrow array
const std::uint8_t* validity(const ArrowArray* a) {
    return static_cast<const std::uint8_t*>(a->n_buffers > 0 ? a->buffers[0] : nullptr);
}

/// test validity bit for row index i; null buffer implies all rows valid
bool isValid(const ArrowArray* a, std::int64_t i) {
    const auto* v = validity(a);
    const auto k = static_cast<std::uint64_t>(i + a->offset);
    return v == nullptr || (((static_cast<unsigned int>(v[k >> 3U]) >> (k & 7U)) & 1U) != 0U);
}

/// read scalar value of type T from primary data buffer (buffer index 1)
template <class T> T at(const ArrowArray* a, std::int64_t i) {
    if (!a || a->n_buffers < 2 || !a->buffers[1]) return T{};
    return static_cast<const T*>(a->buffers[1])[i + a->offset];
}

/// decode Arrow C Data Interface cell into typed Value variant
/// in: fmt Arrow format string, a Arrow array pointer, i row index
/// out: Value holding decoded primitive or monostate if null
/// err: throws BackendError on malformed or unsupported format codes
Value decode(const char* fmt, const ArrowArray* a, std::int64_t i) {
    if (!fmt) return std::monostate{};
    if (!std::strcmp(fmt, "n")) return std::monostate{};
    if (!isValid(a, i)) return std::monostate{};
    if (!std::strcmp(fmt, "l")) return at<std::int64_t>(a, i);
    if (!std::strcmp(fmt, "i")) return static_cast<std::int64_t>(at<std::int32_t>(a, i));
    if (!std::strcmp(fmt, "s")) return static_cast<std::int64_t>(at<std::int16_t>(a, i));
    if (!std::strcmp(fmt, "c")) return static_cast<std::int64_t>(at<std::int8_t>(a, i));
    if (!std::strcmp(fmt, "L")) return at<std::uint64_t>(a, i);
    if (!std::strcmp(fmt, "I")) return static_cast<std::uint64_t>(at<std::uint32_t>(a, i));
    if (!std::strcmp(fmt, "S")) return static_cast<std::uint64_t>(at<std::uint16_t>(a, i));
    if (!std::strcmp(fmt, "C")) return static_cast<std::uint64_t>(at<std::uint8_t>(a, i));
    if (!std::strcmp(fmt, "g")) return at<double>(a, i);
    if (!std::strcmp(fmt, "f")) return static_cast<double>(at<float>(a, i));
    if (!std::strcmp(fmt, "b")) {
        if (!a || a->n_buffers < 2 || !a->buffers[1]) return static_cast<std::int64_t>(0);
        const auto k = static_cast<std::uint64_t>(i + a->offset);
        return static_cast<std::int64_t>(
            ((static_cast<unsigned int>(static_cast<const std::uint8_t*>(a->buffers[1])[k >> 3U]) >> (k & 7U)) & 1U));
    }
    if (!std::strncmp(fmt, "d:", 2)) {
        int precision = 0;
        int scale = 0;
        int bits = 128;
        // NOLINTNEXTLINE(cert-err34-c,cppcoreguidelines-pro-type-vararg,bugprone-unchecked-string-to-number-conversion)
        if (std::sscanf(fmt + 2, "%d,%d,%d", &precision, &scale, &bits) < 2) {
            throw BackendError(std::string("malformed decimal format string: ") + fmt);
        }
        if (bits != 128) throw BackendError(std::string("unsupported decimal width: ") + fmt);
        // converts 128-bit fixed-point arrow decimal into double with 2^64
        // scaling
        // ~ manual 2^64 arithmetic because c++ lacks standard int128_t
        const auto* w = static_cast<const std::uint64_t*>(a->buffers[1]) + (2 * (i + a->offset));
        const auto hi = static_cast<std::int64_t>(w[1]);
        double v = 0.0;
        if (hi < 0) {
            const auto uhi = static_cast<std::uint64_t>(hi);
            const std::uint64_t magLo = (w[0] == 0) ? 0ULL : (~w[0] + 1ULL);
            const std::uint64_t magHi = (w[0] == 0) ? (0ULL - uhi) : ~uhi;
            const double mag = (static_cast<double>(magHi) * 18446744073709551616.0) + static_cast<double>(magLo);
            v = -mag;
        } else {
            v = (static_cast<double>(hi) * 18446744073709551616.0) + static_cast<double>(w[0]);
        }
        for (int k = 0; k < scale; ++k)
            v /= 10.0;
        return v;
    }
    if (!std::strcmp(fmt, "u")) {
        if (!a || a->n_buffers < 2 || !a->buffers[1]) return std::string{};
        const auto* off = static_cast<const std::int32_t*>(a->buffers[1]) + a->offset;
        if (off[i + 1] <= off[i]) return std::string{};
        const auto len = static_cast<std::size_t>(off[i + 1] - off[i]);
        if (a->n_buffers < 3 || !a->buffers[2]) return std::string{};
        return std::string(static_cast<const char*>(a->buffers[2]) + off[i], len);
    }
    if (!std::strcmp(fmt, "U")) {
        if (!a || a->n_buffers < 2 || !a->buffers[1]) return std::string{};
        const auto* off = static_cast<const std::int64_t*>(a->buffers[1]) + a->offset;
        if (off[i + 1] <= off[i]) return std::string{};
        const auto len = static_cast<std::size_t>(off[i + 1] - off[i]);
        if (a->n_buffers < 3 || !a->buffers[2]) return std::string{};
        return std::string(static_cast<const char*>(a->buffers[2]) + off[i], len);
    }
    throw BackendError(std::string("unsupported Arrow type in result: ") + fmt + " (cast the column in the query)");
}

// -- adbc backend implementation ----------------------------------------------

/// dynamic adbc driver backend loading driver shared library at runtime
class AdbcBackend final : public Backend {
public:
    AdbcBackend(const AdbcBackend&) = delete;
    AdbcBackend& operator=(const AdbcBackend&) = delete;
    AdbcBackend(AdbcBackend&&) = delete;
    AdbcBackend& operator=(AdbcBackend&&) = delete;

    /// initialize ADBC driver and connection from options struct
    /// in: o AdbcOptions containing driver path, entrypoint, and options
    explicit AdbcBackend(const AdbcOptions& o) : flavor_(o.flavor) {
        const auto& preloads = !o.preloadGlobal.empty() ? o.preloadGlobal : o.preload_global;
        for (const auto& lib : preloads) {
            void* h = sysLoadLibrary(lib.c_str(), /*deepbind=*/false, /*global=*/true);
            if (!h) {
                throw BackendError("preload failed: " + sysLastError());
            }
            preloadHandles_.push_back(h);
        }
        const std::string driverPath = !o.driverPath.empty() ? o.driverPath : o.driver_path;
        lib_ = sysLoadLibrary(driverPath.c_str(), o.deepbind, /*global=*/false);
        if (!lib_) throw BackendError("cannot load driver: " + sysLastError());
        auto init = reinterpret_cast<AdbcDriverInitFunc>(sysLoadSymbol(lib_, o.entrypoint.c_str()));
        if (!init) throw BackendError("driver entry point not found: " + o.entrypoint);
        AdbcError err{};
        std::memset(&driver_, 0, sizeof driver_);
        if (init(ADBC_VERSION_1_1_0, &driver_, &err) != ADBC_STATUS_OK) fail("driver init", err);
        if (driver_.DatabaseNew(&db_, &err) != ADBC_STATUS_OK) fail("DatabaseNew", err);
        have_db_ = true;
        const auto& dbOpts = !o.databaseOptions.empty() ? o.databaseOptions : o.database_options;
        for (const auto& [k, v] : dbOpts)
            if (driver_.DatabaseSetOption(&db_, k.c_str(), v.c_str(), &err) != ADBC_STATUS_OK)
                fail("DatabaseSetOption " + k, err);
        if (driver_.DatabaseInit(&db_, &err) != ADBC_STATUS_OK) fail("DatabaseInit", err);
        if (driver_.ConnectionNew(&conn_, &err) != ADBC_STATUS_OK) fail("ConnectionNew", err);
        have_conn_ = true;
        if (driver_.ConnectionInit(&conn_, &db_, &err) != ADBC_STATUS_OK) fail("ConnectionInit", err);
        const auto& inits = !o.initSql.empty() ? o.initSql : o.init_sql;
        for (const auto& s : inits)
            exec(s);
    }

    /// release ADBC connection, database, and dynamic library handles
    ~AdbcBackend() override {
        AdbcError err{};
        if (have_conn_) {
            driver_.ConnectionRelease(&conn_, &err);
            if (err.release) {
                err.release(&err);
                err = AdbcError{};
            }
        }
        if (have_db_) {
            driver_.DatabaseRelease(&db_, &err);
            if (err.release) {
                err.release(&err);
                err = AdbcError{};
            }
        }
        if (lib_) {
            sysUnloadLibrary(lib_);
            lib_ = nullptr;
        }
        for (void* handle : std::views::reverse(preloadHandles_)) {
            sysUnloadLibrary(handle);
        }
        preloadHandles_.clear();
    }

    Flavor flavor() const noexcept override { return flavor_; }
    void exec(std::string_view sql) override { run(sql, nullptr); }

    Table query(std::string_view sql) override {
        Table t;
        run(sql, &t);
        return t;
    }

private:
    /// execute SQL statement and decode output Arrow batches into Table
    void run(std::string_view sql, Table* out) {
        std::scoped_lock lock(connMutex_);
        AdbcError err{};
        AdbcStatement st{};
        if (driver_.StatementNew(&conn_, &st, &err) != ADBC_STATUS_OK) fail("StatementNew", err);
        struct Guard {
            AdbcDriver* d{nullptr};
            AdbcStatement* s{nullptr};
            explicit Guard(AdbcDriver* d_in, AdbcStatement* s_in) noexcept : d(d_in), s(s_in) {}
            Guard(const Guard&) = delete;
            Guard& operator=(const Guard&) = delete;
            Guard(Guard&&) = delete;
            Guard& operator=(Guard&&) = delete;
            ~Guard() {
                AdbcError e{};
                if (d && s) d->StatementRelease(s, &e);
            }
        } guard(&driver_, &st);
        const std::string q(sql);
        if (driver_.StatementSetSqlQuery(&st, q.c_str(), &err) != ADBC_STATUS_OK) fail("SetSqlQuery", err);
        ArrowArrayStream stream{};
        std::int64_t rows = -1;
        if (driver_.StatementExecuteQuery(&st, &stream, &rows, &err) != ADBC_STATUS_OK)
            fail("ExecuteQuery [" + q.substr(0, 160) + "]", err);
        struct StreamGuard {
            ArrowArrayStream* s{nullptr};
            explicit StreamGuard(ArrowArrayStream* s_in) noexcept : s(s_in) {}
            StreamGuard(const StreamGuard&) = delete;
            StreamGuard& operator=(const StreamGuard&) = delete;
            StreamGuard(StreamGuard&&) = delete;
            StreamGuard& operator=(StreamGuard&&) = delete;
            ~StreamGuard() {
                if (s && s->release) s->release(s);
            }
        } sg(&stream);
        if (!stream.release) return;
        ArrowSchema schema{};
        if (stream.get_schema(&stream, &schema) != 0) throw BackendError("cannot read result schema");
        struct SchemaGuard {
            ArrowSchema* s{nullptr};
            explicit SchemaGuard(ArrowSchema* s_in) noexcept : s(s_in) {}
            SchemaGuard(const SchemaGuard&) = delete;
            SchemaGuard& operator=(const SchemaGuard&) = delete;
            SchemaGuard(SchemaGuard&&) = delete;
            SchemaGuard& operator=(SchemaGuard&&) = delete;
            ~SchemaGuard() {
                if (s && s->release) s->release(s);
            }
        } schg(&schema);
        if (out) {
            for (std::int64_t c = 0; c < schema.n_children; ++c)
                out->names.emplace_back(schema.children[c]->name ? schema.children[c]->name : "");
        }
        for (;;) {
            ArrowArray batch{};
            if (stream.get_next(&stream, &batch) != 0) throw BackendError("cannot read result batch");
            if (!batch.release) break;
            struct BatchGuard {
                ArrowArray* a{nullptr};
                explicit BatchGuard(ArrowArray* a_in) noexcept : a(a_in) {}
                BatchGuard(const BatchGuard&) = delete;
                BatchGuard& operator=(const BatchGuard&) = delete;
                BatchGuard(BatchGuard&&) = delete;
                BatchGuard& operator=(BatchGuard&&) = delete;
                ~BatchGuard() {
                    if (a && a->release) a->release(a);
                }
            } bg(&batch);
            if (!out) continue;
            if (batch.n_children < schema.n_children) {
                throw BackendError("Arrow batch n_children (" + std::to_string(batch.n_children) +
                                   ") less than schema n_children (" + std::to_string(schema.n_children) + ")");
            }
            for (std::int64_t r = 0; r < batch.length; ++r) {
                std::vector<Value> row;
                row.reserve(static_cast<std::size_t>(schema.n_children));
                for (std::int64_t c = 0; c < schema.n_children; ++c) {
                    if (schema.children[c]->dictionary) throw BackendError("dictionary-encoded result column");
                    row.push_back(decode(schema.children[c]->format, batch.children[c], r));
                }
                out->rows.push_back(std::move(row));
            }
        }
    }

    Flavor flavor_;
    void* lib_ = nullptr;
    std::vector<void*> preloadHandles_;
    mutable std::mutex connMutex_;
    AdbcDriver driver_{};
    AdbcDatabase db_{};
    AdbcConnection conn_{};
    bool have_db_ = false, have_conn_ = false;
};

template <class T> const T& get(const Table& t, std::size_t r, std::size_t c, const char* want) {
    const auto& v = t.rows.at(r).at(c);
    if (const auto* p = std::get_if<T>(&v)) return *p;
    throw BackendError(std::string("result cell is not ") + want);
}

} // namespace

// -- table accessors and factory ----------------------------------------------

std::int64_t Table::i64(std::size_t r, std::size_t c) const {
    const auto& v = rows.at(r).at(c);
    if (const auto* p = std::get_if<std::int64_t>(&v)) return *p;
    if (const auto* u = std::get_if<std::uint64_t>(&v)) return static_cast<std::int64_t>(*u);
    throw BackendError("result cell is not an integer");
}
std::uint64_t Table::u64(std::size_t r, std::size_t c) const {
    const auto& v = rows.at(r).at(c);
    if (const auto* p = std::get_if<std::uint64_t>(&v)) return *p;
    if (const auto* i = std::get_if<std::int64_t>(&v)) return static_cast<std::uint64_t>(*i);
    throw BackendError("result cell is not an integer");
}
double Table::f64(std::size_t r, std::size_t c) const {
    const auto& v = rows.at(r).at(c);
    if (const auto* p = std::get_if<double>(&v)) return *p;
    if (const auto* i = std::get_if<std::int64_t>(&v)) return static_cast<double>(*i);
    if (const auto* u = std::get_if<std::uint64_t>(&v)) return static_cast<double>(*u);
    throw BackendError("result cell is not a number");
}
const std::string& Table::str(std::size_t r, std::size_t c) const {
    return get<std::string>(*this, r, c, "a string");
}

/// factory function creating ADBC backend driver instance
std::unique_ptr<Backend> makeAdbcBackend(const AdbcOptions& options) {
    return std::make_unique<AdbcBackend>(options);
}

} // namespace razobill
