// =============================================================================
//  file : include/librazobill/StringPool.hpp
//  role : string pool and dictionary with gc compaction and shared locking
//  inv  : dense 32-bit ids; deque preserves pointer stability on push_back
// =============================================================================

#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace razobill::core {

// -- compaction statistics ----------------------------------------------------

/// statistics from a string pool garbage collection and compaction pass
struct StringPoolStats {
    std::size_t totalBefore = 0;
    std::size_t activeAfter = 0;
    std::size_t purgedCount = 0;
};

// -- heterogeneous lookup functors --------------------------------------------

namespace detail {

/// heterogeneous string_view hash avoiding allocations on lookup
struct TransparentStringHash {
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
    [[nodiscard]] std::size_t operator()(const std::string& s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
};

/// heterogeneous string_view equality functor avoiding allocations on lookup
struct TransparentStringEqual {
    using is_transparent = void;
    [[nodiscard]] bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
    [[nodiscard]] bool operator()(const std::string& a, const std::string& b) const noexcept { return a == b; }
    [[nodiscard]] bool operator()(const std::string& a, std::string_view b) const noexcept { return a == b; }
    [[nodiscard]] bool operator()(std::string_view a, const std::string& b) const noexcept { return a == b; }
};

} // namespace detail

// -- thread-safe string pool --------------------------------------------------

/// thread-safe string pool assigning dense 32-bit ids to unique strings
/// inv: deque storage guarantees pointer stability across push_back (intern)
class StringPool {
public:
    StringPool() = default;

    /// intern string view into pool, returning dense 32-bit id
    /// inv: identical strings map to the same id; zero allocation on cache hit
    /// in: arbitrary byte sequence view s
    /// out: dense 32-bit integer id in range [0..size()-1]
    /// own: pool allocates and owns heap copy only on first insertion
    /// thread: thread-safe, double-checked locking with shared_mutex
    /// err: noexcept(false) on allocation failure
    std::uint32_t intern(std::string_view s) {
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            auto it = strToId_.find(s);
            if (it != strToId_.end()) {
                return it->second;
            }
        }

        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto it = strToId_.find(s);
        if (it != strToId_.end()) {
            return it->second;
        }

        std::uint32_t id = static_cast<std::uint32_t>(idToStr_.size());
        std::string strCopy(s);
        strToId_[strCopy] = id;
        idToStr_.push_back(std::move(strCopy));
        return id;
    }

    /// fast string_view accessor borrowing internal deque storage
    /// inv: pointers remain stable across intern() due to std::deque paging
    /// in: 32-bit interned string id
    /// out: borrowed string_view, or empty view if id is out of bounds
    /// own: borrows pool storage; invalidated if concurrent gc() or clear() runs
    /// thread: reads under shared_lock, but lock releases before returning view
    /// err: returns empty string_view on out-of-range id
    [[nodiscard]] std::string_view lookup(std::uint32_t id) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (id < idToStr_.size()) {
            return idToStr_[id];
        }
        return {};
    }

    /// thread-safe string accessor returning owned copy under read lock
    /// in: 32-bit interned string id
    /// out: owned std::string copy, or empty string if id is out of bounds
    /// own: caller owns returned copy
    /// thread: thread-safe under shared_lock
    /// err: returns empty string on out-of-range id
    [[nodiscard]] std::string lookupCopy(std::uint32_t id) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (id < idToStr_.size()) {
            return idToStr_[id];
        }
        return {};
    }

    /// thread-safe string accessor copying into destination buffer under read lock
    /// in: 32-bit interned string id, destination string reference
    /// out: true if found and copied, false if id is out of bounds
    /// own: copies bytes into caller-provided out buffer
    /// thread: thread-safe under shared_lock
    bool lookup(std::uint32_t id, std::string& out) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (id < idToStr_.size()) {
            out = idToStr_[id];
            return true;
        }
        return false;
    }

    /// scoped string inspection holding read lock across callback execution
    /// inv: zero heap allocations; view remains valid throughout callback
    /// in: 32-bit id, callback fn(std::string_view)
    /// out: true if id found and callback executed, false if id out of bounds
    /// own: callback receives borrowed view valid during execution
    /// thread: shared_lock held during callback; fn must not call intern/clear
    template <typename F> bool withString(std::uint32_t id, F&& fn) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (id < idToStr_.size()) {
            fn(std::string_view(idToStr_[id]));
            return true;
        }
        return false;
    }

    /// current number of unique strings interned in the pool
    /// out: total unique string count
    /// thread: thread-safe under shared_lock
    /// err: noexcept
    [[nodiscard]] std::size_t size() const noexcept {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return idToStr_.size();
    }

    /// clear all interned strings and reset dictionary mappings
    /// inv: invalidates all previously issued ids and borrowed string_views
    /// thread: thread-safe under unique_lock
    /// err: noexcept
    void clear() noexcept {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        strToId_.clear();
        idToStr_.clear();
    }

    /// purge strings not present in activeIds set and reindex ids compactly
    /// inv: active surviving strings receive contiguous ids in range [0..M-1]
    /// in: activeIds set supporting .find(id) != .end()
    /// out: StringPoolStats recording before, after, and purged string counts
    /// own: reorganizes internal maps in place; invalidates prior views
    /// thread: thread-safe under unique_lock
    /// err: noexcept(false) on allocation failure
    template <typename SetType> StringPoolStats gc(const SetType& activeIds) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        StringPoolStats stats;
        stats.totalBefore = idToStr_.size();

        std::deque<std::string> newIdToStr;
        std::unordered_map<std::string, std::uint32_t, detail::TransparentStringHash, detail::TransparentStringEqual>
            newStrToId;

        for (std::uint32_t id = 0; id < static_cast<std::uint32_t>(idToStr_.size()); ++id) {
            if (activeIds.find(id) != activeIds.end()) {
                std::uint32_t newId = static_cast<std::uint32_t>(newIdToStr.size());
                newStrToId[idToStr_[id]] = newId;
                newIdToStr.push_back(std::move(idToStr_[id]));
            }
        }

        stats.activeAfter = newIdToStr.size();
        stats.purgedCount = stats.totalBefore - stats.activeAfter;

        idToStr_ = std::move(newIdToStr);
        strToId_ = std::move(newStrToId);
        return stats;
    }

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::uint32_t, detail::TransparentStringHash, detail::TransparentStringEqual>
        strToId_;
    std::deque<std::string> idToStr_;
};

} // namespace razobill::core
