// =============================================================================
//  file : include/librazobill/Stats.hpp
//  role : information-theoretic ledger, leakage, divergence, and consequences
//  inv  : exact conservation identities: E = I + Lambda and C = I + Delta
// =============================================================================

#pragma once

#include <cmath>
#include <cstdint>
#include <numbers>
#include <span>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace razobill::stats {

// -- joint mass and event structures ------------------------------------------

/// joint mass of form x and semantic meaning w: event count times p(w|x)
struct Cell {
    std::uint64_t form;
    std::uint32_t meaning;
    double mass;
};

/// event frequency with template form x and downstream purpose value y
struct XY {
    std::uint64_t form;
    std::uint32_t y;
    double n;
};

/// probabilistic mapping from template form to discrete meaning category
struct FormMeaning {
    std::uint64_t form;
    std::uint32_t meaning;
    double p;
};

// -- semantic ledger ----------------------------------------------------------

/// information ledger decomposing expression into content, conveyance,
/// latitude, and doubt
/// inv: satisfies conservation laws E = I + Lambda and C = I + Delta
struct Ledger {
    double coverage = 0.0;   // resolved events / all events in [0, 1]
    double content = 0.0;    // C = H(W): entropy of intended meanings
    double expression = 0.0; // E = H(X): entropy of observed surface forms
    double latitude = 0.0;   // Lambda = H(X|W): surface variation given fixed meaning
    double doubt = 0.0;      // Delta = H(W|X): residual uncertainty of meaning given form
    double conveyance = 0.0; // I = I(W;X): mutual information conveyed by surface form
    double distance = 0.0;   // d(W, X) = Lambda + Delta = H(W, X) - I(W; X)

    // Perplexity / effective vocabulary measures: 2^H
    double effectiveVocabulary = 0.0; // 2^E: effective number of surface forms
    double effectiveContent = 0.0;    // 2^C: effective number of meanings
    double effectiveLatitude = 0.0;   // 2^Lambda: effective ways a meaning is expressed
    double effectiveDoubt = 0.0;      // 2^Delta: effective interpretations per form

    // Stratified Miller-Madow bias-corrected estimators
    double contentMm = 0.0;    // H_MM(W)
    double expressionMm = 0.0; // H_MM(X)
    double latitudeMm = 0.0;   // stratified H_MM(X|W)
    double doubtMm = 0.0;      // stratified H_MM(W|X)
    double conveyanceMm = 0.0; // I_MM(W; X) = C_MM - Delta_MM

    // Certified confidence interval for small meaning alphabets
    double contentEpsilon = 0.0;
    double contentHalfWidth = 0.0;

    /// verify fundamental conservation identities within numerical tolerance
    /// in: absolute numerical tolerance tol
    /// out: true if E == I + Lambda, C == I + Delta, and non-negativity hold
    /// thread: pure const query, noexcept
    [[nodiscard]] bool satisfiesLedgerIdentities(double tol = 1e-9) const noexcept {
        const double diffE = std::abs(expression - (conveyance + latitude));
        const double diffC = std::abs(content - (conveyance + doubt));
        const double diffBal = std::abs((expression - content) - (latitude - doubt));
        return diffE <= tol && diffC <= tol && diffBal <= tol && latitude >= -tol && doubt >= -tol &&
               conveyance >= -tol;
    }

    /// verify vocabulary law bound: Delta - Lambda >= C - log2(N)
    /// in: nDistinctForms total unique surface forms observed
    /// out: true if doubt-latitude balance satisfies vocabulary bound
    /// thread: pure const query, noexcept
    [[nodiscard]] bool satisfiesVocabularyLaw(std::size_t nDistinctForms, double tol = 1e-9) const noexcept {
        if (nDistinctForms == 0) return true;
        const double maxE = std::log2(static_cast<double>(nDistinctForms));
        return (doubt - latitude) >= (content - maxE - tol);
    }
};

// -- consequence decomposition and pricing ------------------------------------

/// three-way decomposition of form entropy for downstream purpose Y
/// inv: E = I(X; Y) [consequence] + H(M | Y) [hedge] + H(X | M) [latitude]
struct ConsequenceDecomposition {
    double expression = 0.0;  // H(X): total entropy of surface forms
    double consequence = 0.0; // I(X; Y): mutual information about purpose Y
    double hedge = 0.0;       // H(M | Y): residual receiver entropy
    double latitude = 0.0;    // H(X | M): form variation irrelevant to purpose

    /// verify three-way decomposition identity within numerical tolerance
    /// out: true if expression == consequence + hedge + latitude
    /// thread: pure const query, noexcept
    [[nodiscard]] bool satisfiesIdentity(double tol = 1e-9) const noexcept {
        return std::abs(expression - (consequence + hedge + latitude)) <= tol;
    }
};

/// prefix-code transmission cost decomposition: n(E + Delta) = nC + nLambda
/// inv: saying forms instead of meanings incurs an exact premium of
///      n * Lambda bits
struct PriceOfSaying {
    double expectedMeaningBits = 0.0;       // n * C: cost to send meaning directly
    double expectedExpressionBits = 0.0;    // n * E: cost to send form
    double expectedClarificationBits = 0.0; // n * Delta: clarification cost
    double totalSayingBits = 0.0;           // n * (E + Delta) == n * (C + Lambda)
    double sayingCostBits = 0.0;            // n * Lambda: premium of surface forms
    double lowerBoundBits = 0.0;            // n * (E + Delta)
    double upperBoundBits = 0.0;            // n * (E + Delta) + 2

    /// verify price-of-saying conservation identities within numerical
    /// tolerance
    /// out: true if total transmission matches both dual formulations
    /// thread: pure const query, noexcept
    [[nodiscard]] bool satisfiesIdentity(double tol = 1e-9) const noexcept {
        const double diff1 = std::abs(totalSayingBits - (expectedMeaningBits + sayingCostBits));
        const double diff2 = std::abs(totalSayingBits - (expectedExpressionBits + expectedClarificationBits));
        return diff1 <= tol && diff2 <= tol;
    }
};

/// compute prefix-code expected length bounds and wording premium
/// in: computed ledger and message count nMessages
/// out: PriceOfSaying struct populated with exact transmission bit budgets
/// thread: pure function, noexcept
[[nodiscard]] PriceOfSaying priceOfSaying(const Ledger& ledger, double nMessages) noexcept;

// -- hierarchical depth decomposition -----------------------------------------

/// hierarchical projection step across successive semantic abstraction depths
struct HierarchicalStep {
    std::uint64_t form = 0;            // W_0 = X: observed surface form
    std::vector<std::uint32_t> levels; // W_1, W_2, ..., W_L: nested abstractions
    std::uint32_t y = 0;               // downstream purpose Y
    double mass = 0.0;
};

/// multi-level depth ledger decomposing entropy and relevance across hierarchy
/// inv: total entropy and relevance telescope across intermediate levels
struct DepthLedger {
    std::vector<double> entropyLevels;   // H(W_k-1 | W_k) for k in [1..L]
    std::vector<double> relevanceLevels; // I(W_k-1; Y | W_k) for k in [1..L]
    double totalEntropy = 0.0;           // H(X)
    double totalRelevance = 0.0;         // I(X; Y)
    double coarsestEntropy = 0.0;        // H(W_L)
    double coarsestRelevance = 0.0;      // I(W_L; Y)

    /// verify telescopic identities across hierarchical levels
    /// out: true if total entropy and relevance equal sum of steps plus
    ///      coarsest
    /// thread: pure const query, noexcept
    [[nodiscard]] bool satisfiesIdentity(double tol = 1e-9) const noexcept {
        double sumH = coarsestEntropy;
        for (double h : entropyLevels)
            sumH += h;
        double sumI = coarsestRelevance;
        for (double r : relevanceLevels)
            sumI += r;
        return std::abs(totalEntropy - sumH) <= tol && std::abs(totalRelevance - sumI) <= tol;
    }
};

/// compute multi-level depth ledger across successive functional projections
/// in: steps span of hierarchical event records
/// out: DepthLedger containing decomposed entropy and relevance vectors
/// thread: thread-safe, allocates memory for internal projection maps
[[nodiscard]] DepthLedger computeDepthLedger(std::span<const HierarchicalStep> steps);

// -- information-theoretic estimation functions -------------------------------

/// compute Shannon entropy in bits (log base 2) with Neumaier summation
/// in: counts non-negative frequency weights
/// out: entropy in bits; 0.0 if empty or single point mass
/// thread: pure function, noexcept
[[nodiscard]] double entropyOfCounts(std::span<const double> counts) noexcept;

/// legacy snake_case alias for entropyOfCounts
[[nodiscard]] inline double entropy_of_counts(std::span<const double> counts) noexcept {
    return entropyOfCounts(counts);
}

/// compute complete semantic ledger from joint cell distributions
/// in: cells joint mass table, totalEvents count, confidenceAlpha significance
/// out: Ledger populated with Shannon and Miller-Madow estimators
/// thread: thread-safe, allocates intermediate frequency maps
[[nodiscard]] Ledger ledger(std::span<const Cell> cells, double totalEvents, double confidenceAlpha = 0.05);

/// compute three-way consequence decomposition for downstream purpose Y
/// in: xy joint form-purpose observations, map form-to-meaning distribution
/// out: ConsequenceDecomposition struct
/// thread: thread-safe
[[nodiscard]] ConsequenceDecomposition consequenceDecomposition(std::span<const XY> xy,
                                                                std::span<const FormMeaning> map);

/// volume-weighted Jensen-Shannon divergence of purpose distribution across
/// merged template forms
struct MergeCost {
    double jsd;
    double share;
};

/// compute information loss of merging template forms relative to purpose Y
/// in: xy joint events, merged set of forms to combine, totalEvents count
/// out: MergeCost struct containing Jensen-Shannon divergence and event share
/// thread: thread-safe
[[nodiscard]] MergeCost mergeCost(std::span<const XY> xy, const std::unordered_map<std::uint64_t, bool>& merged,
                                  double totalEvents);

/// legacy snake_case alias for mergeCost
[[nodiscard]] inline MergeCost merge_cost(std::span<const XY> xy, const std::unordered_map<std::uint64_t, bool>& merged,
                                          double totalEvents) {
    return mergeCost(xy, merged, totalEvents);
}

/// compute conditional mutual information I(X; Y | W)
/// inv: zero leakage indicates representation W is a sufficient statistic for Y
/// in: xy joint form-purpose events, map form-to-meaning distribution
/// out: leakage value in bits
/// thread: thread-safe
[[nodiscard]] double leakage(std::span<const XY> xy, std::span<const FormMeaning> map);

/// compute empirical mutual information I(X; Y) from raw count distributions
/// in: xy joint form-purpose frequency records
/// out: mutual information in bits
/// thread: thread-safe
[[nodiscard]] double mutualInformation(std::span<const XY> xy);

/// legacy snake_case alias for mutualInformation
[[nodiscard]] inline double mutual_information(std::span<const XY> xy) {
    return mutualInformation(xy);
}

// -- bias corrections and confidence intervals --------------------------------

/// apply Miller-Madow asymptotic bias correction to empirical plug-in entropy
/// in: pluginH raw plug-in entropy in bits, observedCategories count,
///     sampleCount sample size
/// out: bias-corrected entropy estimate H_MM in bits
/// thread: pure function, noexcept
[[nodiscard]] inline double millerMadow(double pluginH, std::size_t observedCategories, double sampleCount) noexcept {
    if (sampleCount <= 0 || observedCategories <= 1) return pluginH;
    constexpr double twoLn2 = 1.3862943611198906;
    return pluginH + static_cast<double>(observedCategories - 1) / (sampleCount * twoLn2);
}

/// certified confidence interval bounds for small discrete alphabets
struct CertifiedInterval {
    double epsilon;
    double halfWidth;
};

/// compute certified confidence interval for small alphabets at significance
/// in: kCategories alphabet size, sampleCount sample size, delta error
/// out: CertifiedInterval with epsilon radius and entropy half-width
/// thread: pure function, noexcept
[[nodiscard]] inline CertifiedInterval certifiedInterval(std::size_t kCategories, double sampleCount,
                                                         double delta = 0.05) noexcept {
    if (sampleCount <= 0 || kCategories < 2) return {0.0, 0.0};
    const double ln2 = std::numbers::ln2;
    const double eps =
        std::sqrt((2.0 / sampleCount) * (static_cast<double>(kCategories) * ln2 + std::log(1.0 / delta)));
    if (eps >= 0.5) {
        return {eps, std::log2(static_cast<double>(kCategories))};
    }
    const double hw = eps * std::log2(static_cast<double>(kCategories) / eps);
    return {eps, hw};
}

} // namespace razobill::stats
