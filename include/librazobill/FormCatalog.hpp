// =============================================================================
//  file : include/librazobill/FormCatalog.hpp
//  role : thread-safe dictionary cache and collision-resistant template catalog
//  inv  : full string key comparison ensures zero loss under hash collisions
//  deps : "librazobill/Hash.hpp", <shared_mutex>
// =============================================================================

// Thread-safe dictionary cache and template registry.
// Full canonical key string comparison ensures that Birthday Bound collisions
// on 64-bit xxHash64 values never alias distinct templates.
// Dense 1-based sequential IDs ([1..N]) provide unambiguous integer handles.

#pragma once

#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "librazobill/Hash.hpp"

namespace razobill::core {

/// metadata container for a registered template form
struct FormEntry {
    std::uint32_t id = 0; // dense unique sequential identifier (1-based [1..N])
    std::uint64_t hash = 0;
    std::string key;
    std::string templateStr;
    std::uint32_t intSlots = 0;
    std::uint32_t strSlots = 0;
};

namespace detail {
struct FormKeyHash {
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
    [[nodiscard]] std::size_t operator()(const std::string& s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
};

struct FormKeyEqual {
    using is_transparent = void;
    [[nodiscard]] bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
    [[nodiscard]] bool operator()(const std::string& a, const std::string& b) const noexcept { return a == b; }
    [[nodiscard]] bool operator()(const std::string& a, std::string_view b) const noexcept { return a == b; }
    [[nodiscard]] bool operator()(std::string_view a, const std::string& b) const noexcept { return a == b; }
};
} // namespace detail

/// thread-safe concurrent registry of template forms with collision
/// disambiguation
class FormCatalog {
public:
    FormCatalog() = default;

    /// registers a form template and returns its 64-bit xxHash64 fingerprint
    /// in: key canonical form key, tmpl template string, numInts, numStrs
    /// out: 64-bit secondary hash fingerprint (not guaranteed unique under
    ///      Birthday Bound)
    /// thread: thread-safe via shared_mutex
    std::uint64_t registerForm(std::string_view key, std::string_view tmpl, std::uint32_t numInts,
                               std::uint32_t numStrs) {
        return registerFormWithHash(key, tmpl, numInts, numStrs, xxh64(key));
    }

    /// registers form with explicit hash parameter for testing collision
    /// disambiguation
    std::uint64_t registerFormWithHash(std::string_view key, std::string_view tmpl, std::uint32_t numInts,
                                       std::uint32_t numStrs, std::uint64_t explicitHash) {
        (void)registerFormWithHashGetId(key, tmpl, numInts, numStrs, explicitHash);
        return explicitHash;
    }

    /// registers form and returns 1-based dense sequential identifier
    /// out: unique uint32 dense ID guaranteed distinct even under hash
    ///      collisions
    /// thread: thread-safe via shared_mutex
    std::uint32_t registerFormGetId(std::string_view key, std::string_view tmpl, std::uint32_t numInts,
                                    std::uint32_t numStrs) {
        return registerFormWithHashGetId(key, tmpl, numInts, numStrs, xxh64(key));
    }

    /// registers form with explicit hash parameter and returns dense ID
    std::uint32_t registerFormWithHashGetId(std::string_view key, std::string_view tmpl, std::uint32_t numInts,
                                            std::uint32_t numStrs, std::uint64_t explicitHash) {
        // Shared lock read fast-path with zero dynamic allocation
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            auto it = formsByKey_.find(key);
            if (it != formsByKey_.end()) {
                return it->second.id;
            }
        }

        // Exclusive lock write path
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto it = formsByKey_.find(key);
        if (it != formsByKey_.end()) {
            return it->second.id;
        }

        auto hashIt = hashToKeys_.find(explicitHash);
        if (hashIt != hashToKeys_.end()) {
            // Hash collision: an existing entry has the same 64-bit hash
            // but a distinct key!
            collisionCount_++;
        }

        const std::string keyStr(key);

        FormEntry entry;
        entry.id = static_cast<std::uint32_t>(formsByKey_.size() + 1);
        entry.hash = explicitHash;
        entry.key = keyStr;
        entry.templateStr = std::string(tmpl);
        entry.intSlots = numInts;
        entry.strSlots = numStrs;

        // Strong exception safety: pre-reserve vector capacities before
        // container mutation (F-CONC-03)
        formsById_.reserve(formsById_.size() + 1);
        auto& keyVec = hashToKeys_[explicitHash];
        keyVec.reserve(keyVec.size() + 1);

        auto inserted = formsByKey_.emplace(keyStr, std::move(entry));
        keyVec.push_back(keyStr);
        // ISO C++ [unord.req]: Rehashing an std::unordered_map never
        // invalidates pointers or references to elements, only iterators.
        // Caching raw pointers to mapped values in formsById_ is guaranteed
        // stable for the lifetime of formsByKey_ entries.
        formsById_.push_back(&inserted.first->second);
        return inserted.first->second.id;
    }

    [[nodiscard]] bool contains(std::string_view key) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return formsByKey_.find(key) != formsByKey_.end();
    }

    [[nodiscard]] bool contains(std::uint64_t hash) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return hashToKeys_.find(hash) != hashToKeys_.end();
    }

    [[nodiscard]] bool findForm(std::string_view key, FormEntry& out) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = formsByKey_.find(key);
        if (it != formsByKey_.end()) {
            out = it->second;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool findFormById(std::uint32_t id, FormEntry& out) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (id > 0 && id <= formsById_.size()) {
            out = *formsById_[id - 1];
            return true;
        }
        return false;
    }

    [[nodiscard]] bool findKeysByHash(std::uint64_t hash, std::vector<std::string>& outKeys) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = hashToKeys_.find(hash);
        if (it != hashToKeys_.end()) {
            outKeys = it->second;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool isCollisionHash(std::uint64_t hash) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = hashToKeys_.find(hash);
        return it != hashToKeys_.end() && it->second.size() > 1;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return formsByKey_.size();
    }

    [[nodiscard]] std::size_t collisionCount() const noexcept {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return collisionCount_;
    }

    void clear() noexcept {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        formsByKey_.clear();
        hashToKeys_.clear();
        formsById_.clear();
        collisionCount_ = 0;
    }

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, FormEntry, detail::FormKeyHash, detail::FormKeyEqual> formsByKey_;
    std::unordered_map<std::uint64_t, std::vector<std::string>> hashToKeys_;
    std::vector<const FormEntry*> formsById_;
    std::size_t collisionCount_ = 0;
};

} // namespace razobill::core
