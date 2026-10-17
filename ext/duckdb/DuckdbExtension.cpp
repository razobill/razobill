// =============================================================================
// DuckDB loadable C extension for librazobill
//
// exposes RzSplit, RzJoin, RzTemplate, and RzHash as vectorized SQL scalar
// functions using the DuckDB C extension API; achieves zero per-row heap
// allocation through thread-local scratch reuse across chunks
// =============================================================================

// architecture: vectorized SQL scalar execution
// DuckDB processes queries in vector batches (typically 2,048 rows per chunk);
// inputs read directly from raw duckdb_string_t memory pointers and written
// into output column vectors with zero intermediate string copies
//
// concurrency & zero allocation:
// DuckDB executes scalar functions across worker threads in parallel;
// thread-local Scratch buffers isolate state with zero locks or atomics on
// the query hot path; buffers grow once to steady-state capacity and a 1 MB
// retention cap (kScratchKeepBytes) prunes anomalous spike rows

#include <cstring>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include "duckdb_extension.h"
#include "librazobill/Lifetime.hpp"
#include "librazobill/Slots.hpp"

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
DUCKDB_EXTENSION_EXTERN

namespace {

using razobill::Status;
namespace core = razobill::core;

// 1 MB retention cap for thread-local working buffers
constexpr std::size_t kScratchKeepBytes = std::size_t{1} << 20U;

// supported DuckDB physical column types for dynamic argument binding
enum class Kind : unsigned char { I64, I32, I16, I8, U64, U32, U16, U8, Str, IntList, StrList, Unsupported };

// column argument binding handle unpacked from incoming data chunk
struct Arg {
    Kind kind = Kind::Unsupported;
    void* data = nullptr;
    std::uint64_t* validity = nullptr;
    void* childData = nullptr;
    std::uint64_t* childValidity = nullptr;
};

/// classify DuckDB logical type into internal Kind enum
/// in: valid duckdb_logical_type handle
/// out: Kind enum identifying scalar integer, string, or list
/// own: destroys temporary child logical type handles internally
/// thread: thread-safe
inline Kind classify(duckdb_logical_type t) {
    auto id = duckdb_get_type_id(t);
    switch (id) {
    case DUCKDB_TYPE_BIGINT:
        return Kind::I64;
    case DUCKDB_TYPE_INTEGER:
        return Kind::I32;
    case DUCKDB_TYPE_SMALLINT:
        return Kind::I16;
    case DUCKDB_TYPE_TINYINT:
        return Kind::I8;
    case DUCKDB_TYPE_UBIGINT:
        return Kind::U64;
    case DUCKDB_TYPE_UINTEGER:
        return Kind::U32;
    case DUCKDB_TYPE_USMALLINT:
        return Kind::U16;
    case DUCKDB_TYPE_UTINYINT:
        return Kind::U8;
    case DUCKDB_TYPE_VARCHAR:
    case DUCKDB_TYPE_BLOB:
        return Kind::Str;
    case DUCKDB_TYPE_LIST: {
        auto* elem = duckdb_list_type_child_type(t);
        auto elemId = duckdb_get_type_id(elem);
        duckdb_destroy_logical_type(&elem);
        if (elemId == DUCKDB_TYPE_BIGINT || elemId == DUCKDB_TYPE_INTEGER || elemId == DUCKDB_TYPE_UBIGINT) {
            return Kind::IntList;
        }
        if (elemId == DUCKDB_TYPE_VARCHAR || elemId == DUCKDB_TYPE_BLOB) {
            return Kind::StrList;
        }
        return Kind::Unsupported;
    }
    default:
        return Kind::Unsupported;
    }
}

/// thread-local working memory for vectorized log parsing and reconstruction
/// thread: thread-local instance per OS execution thread
struct Scratch {
    core::SlotRecord record;
    std::string text;
    std::vector<std::int64_t> ints;
    std::vector<std::string_view> strs;
    std::vector<Arg> args;

    /// prune oversized vectors back to initial capacity if exceeding 1 MB cap
    void trim() {
        if (text.capacity() > kScratchKeepBytes) std::string().swap(text);
        if (record.skeleton.capacity() > kScratchKeepBytes) std::string().swap(record.skeleton);
        if (record.templateStr.capacity() > kScratchKeepBytes) std::string().swap(record.templateStr);
        if (record.ints.capacity() * sizeof(std::int64_t) > kScratchKeepBytes)
            std::vector<std::int64_t>().swap(record.ints);
        if (record.strs.capacity() * sizeof(std::string_view) > kScratchKeepBytes)
            std::vector<std::string_view>().swap(record.strs);
        if (ints.capacity() * sizeof(std::int64_t) > kScratchKeepBytes) std::vector<std::int64_t>().swap(ints);
        if (strs.capacity() * sizeof(std::string_view) > kScratchKeepBytes) std::vector<std::string_view>().swap(strs);
    }
};

/// retrieve or allocate thread-local scratch buffer instance
/// out: reference to private Scratch structure for current thread
/// thread: thread-safe, zero locks on hot path
Scratch& scratch() {
    thread_local Scratch s;
    return s;
}

/// wrap DuckDB string struct into non-owning string_view
/// in: s is valid or inline duckdb_string_t
/// out: string_view borrowing memory from DuckDB vector
/// own: borrowed, valid for duration of data chunk processing
inline std::string_view viewOf(duckdb_string_t& s) {
    return {duckdb_string_t_data(&s), duckdb_string_t_length(s)};
}

/// test whether row at index is non-null in DuckDB validity bitmask
/// in: validity pointer may be null if all rows in chunk are valid
inline bool isValid(std::uint64_t* validity, idx_t row) {
    return validity == nullptr || duckdb_validity_row_is_valid(validity, row);
}

/// mark row in output vector as SQL NULL
inline void setNull(duckdb_vector out, idx_t row) {
    duckdb_vector_ensure_validity_writable(out);
    duckdb_validity_set_row_invalid(duckdb_vector_get_validity(out), row);
}

/// assign string_view contents into DuckDB string output vector element
inline void put(duckdb_vector out, idx_t row, std::string_view v) {
    duckdb_vector_assign_string_element_len(out, row, v.data(), v.size());
}

/// format actionable error diagnostic and set error on DuckDB scalar function
inline void fail(duckdb_function_info info, const char* fn, Status s, std::string_view tmpl = {}) {
    std::string why;
    switch (s) {
    case Status::SlotUnderflow:
    case Status::SlotOverflow:
        why = "Count mismatch (template placeholders do not match the number of provided integers or strings)";
        break;
    case Status::Malformed:
        why = "Malformed template syntax or unclosed brace";
        break;
    case Status::TooLarge:
        why = "Reconstructed message exceeds maximum size (16 MB)";
        break;
    default:
        why = std::string(razobill::describe(s));
        break;
    }
    std::string m = std::string("[razobill] What happened: ") + fn + " failed. " + "Why: " + why + ". " +
                    "What to do: Verify that {i} and {s} placeholders match provided values.";
    if (!tmpl.empty()) {
        std::string trunc(tmpl.substr(0, std::min<std::size_t>(tmpl.size(), 60)));
        m += " Offending template: '" + trunc + "'.";
    }
    duckdb_scalar_function_set_error(info, m.c_str());
}

/// vectorized SQL scalar implementation for RzTemplate(VARCHAR/BLOB)
/// inv: template matches RzSplit(line).template byte-for-byte
/// in: data chunk column 0 contains VARCHAR or BLOB log lines
/// out: out vector populated with structural templates; NULL inputs yield NULL
/// thread: thread-safe via thread-local scratch
/// err: sets scalar function error on parser failure
void RzTemplateImpl(duckdb_function_info info, duckdb_data_chunk in, duckdb_vector out) {
    auto& sc = scratch();
    const idx_t n = duckdb_data_chunk_get_size(in);
    auto* vec = duckdb_data_chunk_get_vector(in, 0);
    auto* data = static_cast<duckdb_string_t*>(duckdb_vector_get_data(vec));
    auto* vv = duckdb_vector_get_validity(vec);
    for (idx_t r = 0; r < n; ++r) {
        if (!isValid(vv, r)) {
            setNull(out, r);
            continue;
        }
        auto sv = viewOf(data[r]);
        if (sv.empty()) {
            put(out, r, "");
            continue;
        }
        if (auto s = core::parse(sv, sc.record, core::ScanMode::Semantic); s != Status::Ok) {
            fail(info, "RzTemplate", s, sv);
            break;
        }
        put(out, r, sc.record.templateStr);
    }
    sc.trim();
}

/// vectorized SQL scalar implementation for RzSplit(VARCHAR/BLOB)
/// inv: RzJoin(RzSplit(x)) == x
/// in: data chunk column 0 contains VARCHAR or BLOB log lines
/// out: STRUCT(template VARCHAR/BLOB, ints BIGINT[], strs VARCHAR[]/BLOB[])
/// thread: thread-safe via thread-local scratch
/// err: sets scalar function error on parser failure; NULL inputs yield NULL
void RzSplitImpl(duckdb_function_info info, duckdb_data_chunk in, duckdb_vector out) {
    auto& sc = scratch();
    const idx_t n = duckdb_data_chunk_get_size(in);
    auto* vec = duckdb_data_chunk_get_vector(in, 0);
    auto* data = static_cast<duckdb_string_t*>(duckdb_vector_get_data(vec));
    auto* vv = duckdb_vector_get_validity(vec);

    auto* tmplChild = duckdb_struct_vector_get_child(out, 0);
    auto* intsChild = duckdb_struct_vector_get_child(out, 1);
    auto* strsChild = duckdb_struct_vector_get_child(out, 2);

    auto* intsGrandchild = duckdb_list_vector_get_child(intsChild);
    auto* intsEntries = static_cast<duckdb_list_entry*>(duckdb_vector_get_data(intsChild));

    auto* strsGrandchild = duckdb_list_vector_get_child(strsChild);
    auto* strsEntries = static_cast<duckdb_list_entry*>(duckdb_vector_get_data(strsChild));

    idx_t totalInts = 0;
    idx_t totalStrs = 0;

    for (idx_t r = 0; r < n; ++r) {
        if (!isValid(vv, r)) {
            setNull(tmplChild, r);
            setNull(intsChild, r);
            setNull(strsChild, r);
            intsEntries[r] = {.offset = totalInts, .length = 0};
            strsEntries[r] = {.offset = totalStrs, .length = 0};
            setNull(out, r);
            continue;
        }
        auto sv = viewOf(data[r]);
        if (sv.empty()) {
            put(tmplChild, r, "");
            intsEntries[r] = {.offset = totalInts, .length = 0};
            strsEntries[r] = {.offset = totalStrs, .length = 0};
            continue;
        }
        if (auto s = core::parse(sv, sc.record, core::ScanMode::Semantic); s != Status::Ok) {
            fail(info, "RzSplit", s, sv);
            break;
        }
        put(tmplChild, r, sc.record.templateStr);

        const idx_t numInts = sc.record.ints.size();
        duckdb_list_vector_reserve(intsChild, totalInts + numInts);
        intsEntries[r] = {.offset = totalInts, .length = numInts};
        auto* dstInts = static_cast<std::int64_t*>(duckdb_vector_get_data(intsGrandchild));
        for (idx_t i = 0; i < numInts; ++i) {
            dstInts[totalInts + i] = sc.record.ints[i];
        }
        totalInts += numInts;

        const idx_t numStrs = sc.record.strs.size();
        duckdb_list_vector_reserve(strsChild, totalStrs + numStrs);
        strsEntries[r] = {.offset = totalStrs, .length = numStrs};
        for (idx_t i = 0; i < numStrs; ++i) {
            put(strsGrandchild, totalStrs + i, sc.record.strs[i]);
        }
        totalStrs += numStrs;
    }
    duckdb_list_vector_set_size(intsChild, totalInts);
    duckdb_list_vector_set_size(strsChild, totalStrs);
    sc.trim();
}

/// vectorized SQL scalar implementation for RzJoin(template, ints, strs, ...)
/// inv: RzJoin(*RzSplit(x)) == x
/// in: column 0 is template; subsequent columns are lists or varargs
/// out: reconstructed VARCHAR or BLOB vector matching template type
/// thread: thread-safe via thread-local scratch
/// err: sets error on placeholder count mismatch or malformed template
void RzJoinImpl(duckdb_function_info info, duckdb_data_chunk in, duckdb_vector out) {
    auto& sc = scratch();
    const idx_t n = duckdb_data_chunk_get_size(in);
    const idx_t numCols = duckdb_data_chunk_get_column_count(in);
    if (numCols < 1) return;

    auto* tmplVec = duckdb_data_chunk_get_vector(in, 0);
    auto* tmplData = static_cast<duckdb_string_t*>(duckdb_vector_get_data(tmplVec));
    auto* tmplVv = duckdb_vector_get_validity(tmplVec);

    sc.args.resize(numCols - 1);
    for (idx_t c = 1; c < numCols; ++c) {
        auto* vec = duckdb_data_chunk_get_vector(in, c);
        auto* ltype = duckdb_vector_get_column_type(vec);
        auto& a = sc.args[c - 1];
        a.kind = classify(ltype);
        duckdb_destroy_logical_type(&ltype);
        a.data = duckdb_vector_get_data(vec);
        a.validity = duckdb_vector_get_validity(vec);
        if (a.kind == Kind::IntList || a.kind == Kind::StrList) {
            auto* ch = duckdb_list_vector_get_child(vec);
            a.childData = duckdb_vector_get_data(ch);
            a.childValidity = duckdb_vector_get_validity(ch);
        }
    }

    const bool isStandardThreeArg =
        (numCols == 3 && sc.args[0].kind == Kind::IntList && sc.args[1].kind == Kind::StrList);

    for (idx_t r = 0; r < n; ++r) {
        if (!isValid(tmplVv, r)) {
            setNull(out, r);
            continue;
        }
        if (isStandardThreeArg && (!isValid(sc.args[0].validity, r) || !isValid(sc.args[1].validity, r))) {
            setNull(out, r);
            continue;
        }

        sc.ints.clear();
        sc.strs.clear();
        bool rowHasNullArg = false;

        for (const auto& a : sc.args) {
            if (!isValid(a.validity, r)) continue;
            switch (a.kind) {
            case Kind::I64:
                sc.ints.push_back(static_cast<const std::int64_t*>(a.data)[r]);
                break;
            case Kind::I32:
                sc.ints.push_back(static_cast<const std::int32_t*>(a.data)[r]);
                break;
            case Kind::I16:
                sc.ints.push_back(static_cast<const std::int16_t*>(a.data)[r]);
                break;
            case Kind::I8:
                sc.ints.push_back(static_cast<const std::int8_t*>(a.data)[r]);
                break;
            case Kind::U64:
                sc.ints.push_back(static_cast<std::int64_t>(static_cast<const std::uint64_t*>(a.data)[r]));
                break;
            case Kind::U32:
                sc.ints.push_back(static_cast<std::int64_t>(static_cast<const std::uint32_t*>(a.data)[r]));
                break;
            case Kind::U16:
                sc.ints.push_back(static_cast<std::int64_t>(static_cast<const std::uint16_t*>(a.data)[r]));
                break;
            case Kind::U8:
                sc.ints.push_back(static_cast<std::int64_t>(static_cast<const std::uint8_t*>(a.data)[r]));
                break;
            case Kind::Str:
                sc.strs.push_back(viewOf(static_cast<duckdb_string_t*>(a.data)[r]));
                break;
            case Kind::IntList: {
                const auto entry = static_cast<const duckdb_list_entry*>(a.data)[r];
                const auto* p = static_cast<const std::int64_t*>(a.childData);
                for (idx_t i = 0; i < entry.length; ++i) {
                    if (!isValid(a.childValidity, entry.offset + i)) {
                        rowHasNullArg = true;
                        break;
                    }
                    sc.ints.push_back(p[entry.offset + i]);
                }
                break;
            }
            case Kind::StrList: {
                const auto entry = static_cast<const duckdb_list_entry*>(a.data)[r];
                auto* p = static_cast<duckdb_string_t*>(a.childData);
                for (idx_t i = 0; i < entry.length; ++i) {
                    if (!isValid(a.childValidity, entry.offset + i)) {
                        rowHasNullArg = true;
                        break;
                    }
                    sc.strs.push_back(viewOf(p[entry.offset + i]));
                }
                break;
            }
            case Kind::Unsupported:
                break;
            }
            if (rowHasNullArg) break;
        }

        if (rowHasNullArg) {
            setNull(out, r);
            continue;
        }

        auto tmplSv = viewOf(tmplData[r]);
        if (tmplSv.empty() && sc.ints.empty() && sc.strs.empty()) {
            put(out, r, "");
            continue;
        }
        if (auto s = core::reconstruct(tmplSv, sc.ints, sc.strs, sc.text); s != Status::Ok) {
            fail(info, "RzJoin", s, tmplSv);
            break;
        }
        put(out, r, sc.text);
    }
    sc.trim();
}

/// vectorized SQL scalar implementation for RzHash(VARCHAR/BLOB)
/// inv: bit-identical to C++ RzHash, pyrazobill.RzHash, and ClickHouse xxHash64
/// in: column 0 contains VARCHAR or BLOB templates or messages
/// out: UBIGINT vector of 64-bit unsigned hashes (seed 0)
/// thread: thread-safe, pure calculation
void RzHashImpl(duckdb_function_info /*info*/, duckdb_data_chunk in, duckdb_vector out) {
    const idx_t n = duckdb_data_chunk_get_size(in);
    auto* vec = duckdb_data_chunk_get_vector(in, 0);
    auto* data = static_cast<duckdb_string_t*>(duckdb_vector_get_data(vec));
    auto* vv = duckdb_vector_get_validity(vec);
    auto* dst = static_cast<std::uint64_t*>(duckdb_vector_get_data(out));
    for (idx_t r = 0; r < n; ++r) {
        if (!isValid(vv, r)) {
            setNull(out, r);
            continue;
        }
        dst[r] = core::xxh64(viewOf(data[r]));
    }
}

/// exception boundary wrapper converting C++ exceptions to DuckDB error strings
template <void (*Fn)(duckdb_function_info, duckdb_data_chunk, duckdb_vector)>
void guarded(duckdb_function_info info, duckdb_data_chunk in, duckdb_vector out) {
    try {
        Fn(info, in, out);
    } catch (const std::exception& ex) {
        duckdb_scalar_function_set_error(info, ex.what());
    } catch (...) {
        duckdb_scalar_function_set_error(info, "razobill: unexpected exception");
    }
}

/// RAII wrapper managing duckdb_logical_type handle lifetime
class Type {
public:
    explicit Type(duckdb_type id) : t(duckdb_create_logical_type(id)) {}
    static Type listOf(const Type& elem) { return Type(duckdb_create_list_type(elem.t)); }
    static Type structOf(const std::vector<const char*>& names, const std::vector<duckdb_logical_type>& types) {
        return Type(duckdb_create_struct_type(const_cast<duckdb_logical_type*>(types.data()),
                                              const_cast<const char**>(names.data()), names.size()));
    }
    Type(const Type&) = delete;
    Type& operator=(const Type&) = delete;
    Type(Type&& o) noexcept : t(o.t) { o.t = nullptr; }
    Type& operator=(Type&&) = delete;
    ~Type() {
        if (t) duckdb_destroy_logical_type(&t);
    }
    duckdb_logical_type t;

private:
    explicit Type(duckdb_logical_type owned RZ_LIFETIMEBOUND) : t(owned) {}
};

/// RAII builder and registrar for DuckDB scalar function overloads
class FunctionSet {
public:
    explicit FunctionSet(const char* name RZ_LIFETIMEBOUND) : name_(name), set_(duckdb_create_scalar_function_set(name)) {}
    ~FunctionSet() {
        if (set_) duckdb_destroy_scalar_function_set(&set_);
    }
    FunctionSet(const FunctionSet&) = delete;
    FunctionSet& operator=(const FunctionSet&) = delete;
    FunctionSet(FunctionSet&&) = delete;
    FunctionSet& operator=(FunctionSet&&) = delete;

    void add(std::initializer_list<duckdb_logical_type> params, duckdb_logical_type ret,
             void (*fn)(duckdb_function_info, duckdb_data_chunk, duckdb_vector)) {
        auto* f = duckdb_create_scalar_function();
        duckdb_scalar_function_set_name(f, name_);
        for (auto* p : params)
            duckdb_scalar_function_add_parameter(f, p);
        duckdb_scalar_function_set_return_type(f, ret);
        duckdb_scalar_function_set_function(f, fn);
        duckdb_add_scalar_function_to_set(set_, f);
        duckdb_destroy_scalar_function(&f);
    }

    void addVarargs(duckdb_logical_type firstParam, duckdb_logical_type varargsType, duckdb_logical_type ret,
                    void (*fn)(duckdb_function_info, duckdb_data_chunk, duckdb_vector)) {
        auto* f = duckdb_create_scalar_function();
        duckdb_scalar_function_set_name(f, name_);
        if (firstParam) duckdb_scalar_function_add_parameter(f, firstParam);
        duckdb_scalar_function_set_varargs(f, varargsType);
        duckdb_scalar_function_set_special_handling(f);
        duckdb_scalar_function_set_return_type(f, ret);
        duckdb_scalar_function_set_function(f, fn);
        duckdb_add_scalar_function_to_set(set_, f);
        duckdb_destroy_scalar_function(&f);
    }

    [[nodiscard]] bool registerSet(duckdb_connection con) {
        return duckdb_register_scalar_function_set(con, set_) == DuckDBSuccess;
    }

private:
    const char* name_;
    duckdb_scalar_function_set set_;
};

} // namespace

/// primary DuckDB C extension entrypoint loaded via 'LOAD razobill;'
// NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast,cppcoreguidelines-avoid-non-const-global-variables,readability-named-parameter)
DUCKDB_EXTENSION_ENTRYPOINT(duckdb_connection connection, duckdb_extension_info, struct duckdb_extension_access*) {
    try {
        Type varchar(DUCKDB_TYPE_VARCHAR);
        Type blob(DUCKDB_TYPE_BLOB);
        Type bigint(DUCKDB_TYPE_BIGINT);
        Type ubigint(DUCKDB_TYPE_UBIGINT);
        Type any(DUCKDB_TYPE_ANY);
        Type bigintList = Type::listOf(bigint);
        Type varcharList = Type::listOf(varchar);
        Type blobList = Type::listOf(blob);

        std::vector<const char*> structNames = {"template", "ints", "strs"};
        std::vector<duckdb_logical_type> structTypesVarchar = {varchar.t, bigintList.t, varcharList.t};
        std::vector<duckdb_logical_type> structTypesBlob = {blob.t, bigintList.t, blobList.t};
        Type splitStructVarchar = Type::structOf(structNames, structTypesVarchar);
        Type splitStructBlob = Type::structOf(structNames, structTypesBlob);

        bool ok = true;

        // 1. RzSplit(line) -> STRUCT(template, ints, strs)
        FunctionSet splitSet("RzSplit");
        splitSet.add({varchar.t}, splitStructVarchar.t, guarded<RzSplitImpl>);
        splitSet.add({blob.t}, splitStructBlob.t, guarded<RzSplitImpl>);
        ok = ok && splitSet.registerSet(connection);

        // 2. RzJoin(template, ints, strs) -> VARCHAR / BLOB (supports varargs)
        FunctionSet joinSet("RzJoin");
        joinSet.add({varchar.t, bigintList.t, varcharList.t}, varchar.t, guarded<RzJoinImpl>);
        joinSet.add({blob.t, bigintList.t, blobList.t}, blob.t, guarded<RzJoinImpl>);
        joinSet.add({blob.t, bigintList.t, varcharList.t}, blob.t, guarded<RzJoinImpl>);
        joinSet.add({varchar.t, bigintList.t, blobList.t}, blob.t, guarded<RzJoinImpl>);
        joinSet.addVarargs(varchar.t, any.t, varchar.t, guarded<RzJoinImpl>);
        joinSet.addVarargs(blob.t, any.t, blob.t, guarded<RzJoinImpl>);
        ok = ok && joinSet.registerSet(connection);

        // 3. RzTemplate(line) -> VARCHAR / BLOB
        FunctionSet tmplSet("RzTemplate");
        tmplSet.add({varchar.t}, varchar.t, guarded<RzTemplateImpl>);
        tmplSet.add({blob.t}, blob.t, guarded<RzTemplateImpl>);
        ok = ok && tmplSet.registerSet(connection);

        // 4. RzHash(line) -> UBIGINT
        FunctionSet hashSet("RzHash");
        hashSet.add({varchar.t}, ubigint.t, guarded<RzHashImpl>);
        hashSet.add({blob.t}, ubigint.t, guarded<RzHashImpl>);
        ok = ok && hashSet.registerSet(connection);

        return ok;
    } catch (...) {
        return false;
    }
}

// dual-entrypoint alias supports both 'LOAD razobill;' and 'LOAD librazobill;'
// ~ allows both names so neither team argues
DUCKDB_EXTENSION_EXTERN_C_GUARD_OPEN
DUCKDB_CAPI_ENTRY_VISIBILITY DUCKDB_EXTENSION_API bool librazobill_init_c_api(duckdb_extension_info info,
                                                                              struct duckdb_extension_access* access) {
    try {
        return razobill_init_c_api(info, access);
    } catch (...) {
        return false;
    }
}
DUCKDB_EXTENSION_EXTERN_C_GUARD_CLOSE
