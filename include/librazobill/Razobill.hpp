// =============================================================================
//  file : include/librazobill/Razobill.hpp
//  role : control plane for forms, gated meaning, compaction, and dictionaries
//  inv  : exact lossless compaction; information gate prevents semantic leakage
// =============================================================================

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "librazobill/Backend.hpp"
#include "librazobill/Dialect.hpp"
#include "librazobill/Scanner.hpp"
#include "librazobill/Stats.hpp"

namespace razobill {

// -- engine configuration and control types -----------------------------------

/// control plane configuration parameters
struct Config {
    std::string table; // raw table name or compact table family prefix
    std::string messageColumn = "msg";
    std::string purpose; // target column meaning must remain adequate for
    double tau = 1.0;    // confidence threshold for meaning resolution
    double eps = 0.05;   // information gate loss threshold in bits per event
    core::ScanMode scanMode = core::ScanMode::Semantic;

    // Backward compatibility mappings
    std::string message_column;
    core::ScanMode scan_mode = core::ScanMode::Semantic;
};

/// exception raised when an information gate merge exceeds epsilon loss bound
struct GateRefused : std::runtime_error {
    GateRefused(const std::string& what, double jsd_, double share_)
        : std::runtime_error(what), jsd(jsd_), share(share_) {}
    double jsd = 0.0, share = 0.0;
};

/// outcome of defining a semantic meaning rule
struct Defined {
    int version = 0;
    std::size_t forms = 0;
    double lossPerMergedEvent = 0.0;
    double lossTableWide = 0.0;

    // Backward compatibility mappings
    double loss_per_merged_event = 0;
    double loss_table_wide = 0;
};

/// meaning resolution and uncertainty for a specific template form
struct MeaningOf {
    std::uint64_t formId = 0;
    std::optional<std::string> meaning; // nullopt if unresolved or ambiguous
    std::optional<double> doubt;        // nullopt if unresolved

    // Backward compatibility mappings
    std::uint64_t form_id = 0;
};

/// statistics from string dictionary garbage collection
struct DictionaryStats {
    std::size_t totalBefore = 0;
    std::size_t activeAfter = 0;
    std::size_t purgedCount = 0;

    // Backward compatibility mappings
    std::size_t total_before = 0;
    std::size_t active_after = 0;
    std::size_t purged_count = 0;
};

// -- control plane coordinator ------------------------------------------------

/// coordinates storage compaction, dictionaries, and semantic meaning rules
/// inv: bit-for-bit lossless invertibility; enforces information bounds
class RazobillEngine {
public:
    /// initialize engine coordinator with backend connection and configuration
    /// in: backend reference to database backend, config configuration struct
    RazobillEngine(Backend& backend, Config config);

    // ---- Storage ----

    /// refresh discovered template forms in additive mode
    /// out: count of newly discovered template forms registered
    /// thread: thread-safe, acquires internal mutex
    std::size_t refresh();

    /// compact raw source table into columnar compact storage schema
    /// in: src raw table name
    /// out: total count of log events compacted
    /// thread: thread-safe, acquires internal mutex
    std::uint64_t compact(const std::string& src);

    /// verify that compact reconstruction view matches source table byte for byte
    /// inv: 0 returned means 100% exact lossless reconstruction
    /// in: src source table name
    /// out: count of mismatched rows (0 on perfect reconstruction)
    /// thread: thread-safe, acquires internal mutex
    std::uint64_t verify(const std::string& src);

    // ---- Text Dictionary Lifecycle ----

    /// purge unreferenced string entries from string dictionary
    /// out: DictionaryStats recording before, after, and purged entry counts
    /// thread: thread-safe, acquires internal mutex
    DictionaryStats gcDictionary();

    /// compact string dictionary to dense contiguous IDs and remap references
    /// thread: thread-safe, acquires internal mutex
    void compactDictionary();

    /// legacy snake_case alias for gcDictionary
    DictionaryStats gc_dictionary() { return gcDictionary(); }

    /// legacy snake_case alias for compactDictionary
    void compact_dictionary() { compactDictionary(); }

    // ---- Semantic Meaning & Information Gate ----

    /// define or update semantic meaning rule with information gate validation
    /// in: meaning label, pattern regex, weight priority, force bypass flag
    /// out: Defined struct with assigned rule version and measured entropy loss
    /// thread: thread-safe, acquires internal mutex
    /// err: throws GateRefused if information loss exceeds epsilon and !force
    Defined define(const std::string& meaning, const std::string& pattern, double weight = 1.0, bool force = false);

    /// retire active semantic meaning rule by name
    /// in: meaning rule identifier to remove
    /// out: new rule catalog version after retirement
    /// thread: thread-safe, acquires internal mutex
    int retire(const std::string& meaning);

    /// pin rule catalog to specific historical version
    /// in: version target rule version number
    /// thread: thread-safe, acquires internal mutex
    void pin(int version);

    /// query currently active rule catalog version
    /// out: active version integer
    /// thread: thread-safe, acquires internal mutex
    [[nodiscard]] int version();

    /// compute complete information ledger across current table events
    /// out: Ledger struct populated with entropy, conveyance, latitude, doubt
    /// thread: thread-safe, acquires internal mutex
    [[nodiscard]] stats::Ledger ledger();

    /// compute residual semantic leakage I(X; Y | W) for purpose column
    /// inv: zero leakage proves meaning representation W is sufficient for Y
    /// out: leakage value in bits
    /// thread: thread-safe, acquires internal mutex
    [[nodiscard]] double leakage();

    /// resolve semantic meaning and doubt for arbitrary raw log line
    /// in: text raw input message string
    /// out: optional MeaningOf containing resolved label and doubt
    /// thread: thread-safe, acquires internal mutex
    [[nodiscard]] std::optional<MeaningOf> meaningOf(const std::string& text);

    /// legacy snake_case alias for meaningOf
    [[nodiscard]] std::optional<MeaningOf> meaning_of(const std::string& text) { return meaningOf(text); }

    /// return unresolved template forms ranked by observed event frequency
    /// in: n maximum number of candidate forms to return (default 50)
    /// out: vector of (form_pattern, frequency) pairs sorted descending
    /// thread: thread-safe, acquires internal mutex
    [[nodiscard]] std::vector<std::pair<std::string, std::int64_t>> todo(std::size_t n = 50);

private:
    struct Rule {
        std::string meaning, pattern;
        double weight;
    };
    struct FormP {
        std::uint64_t form;
        std::string meaning;
        double p;
    };

    void installLocked();
    void detectLocked();
    void requireReadyLocked() const;
    std::size_t registerFormsLocked(const std::string& selectForms);
    std::vector<Rule> activeRulesLocked(int version);
    std::vector<FormP> resolveLocked(const std::vector<Rule>& rules);
    void rebuildDerivedLocked();
    void createViewsLocked();
    std::vector<stats::XY> xyLocked(std::unordered_map<std::string, std::uint32_t>& yids);
    double totalEventsLocked();
    std::string_view physKeyLocked() const;

    Backend& db_;
    Config cfg_;
    std::unique_ptr<Dialect> dx_;
    std::mutex mu_;
    std::string phys_;
    std::string view_;
    bool compact_ = false;
    bool hasCollision_ = false;
    bool ready_ = false;
};

using Razobill = RazobillEngine;

} // namespace razobill
