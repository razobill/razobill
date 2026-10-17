// =============================================================================
//  file : src/Stats.cpp
//  role : mathematical ledger, entropy estimation, and consequence accounting
//  inv  : Fréchet polytope projection preserves exact ledger conservation laws
// =============================================================================

#include "librazobill/Stats.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace razobill::stats {

namespace {

// -- neumaier compensated summation and hashers -------------------------------

// compensated summation tracking rounding error to preserve double precision
// ~ compensated summation because adding millions of tiny probabilities drifts
class Sum {
public:
    void add(double x) noexcept {
        const double t = s_ + x;
        c_ += std::abs(s_) >= std::abs(x) ? (s_ - t) + x : (x - t) + s_;
        s_ = t;
    }
    [[nodiscard]] double value() const noexcept { return s_ + c_; }

private:
    double s_ = 0.0, c_ = 0.0;
};

// 64-bit pair hash helper for high-performance unordered maps
struct PairHash64 {
    std::size_t operator()(const std::pair<std::uint64_t, std::uint32_t>& p) const noexcept {
        std::uint64_t h = p.first ^ (static_cast<std::uint64_t>(p.second) * 0x9E3779B97F4A7C15ULL);
        h ^= h >> 33U;
        h *= 0xFF51AFD7ED558CCDULL;
        h ^= h >> 33U;
        return static_cast<std::size_t>(h);
    }
};

struct PairHash32 {
    std::size_t operator()(const std::pair<std::uint32_t, std::uint32_t>& p) const noexcept {
        const std::uint64_t combined =
            (static_cast<std::uint64_t>(p.first) << 32U) | static_cast<std::uint64_t>(p.second);
        std::uint64_t h = combined * 0x9E3779B97F4A7C15ULL;
        h ^= h >> 32U;
        return static_cast<std::size_t>(h);
    }
};

struct TripleHash {
    std::size_t operator()(const std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>& t) const noexcept {
        std::uint64_t h = std::get<0>(t);
        h ^= (static_cast<std::uint64_t>(std::get<1>(t)) * 0x9E3779B97F4A7C15ULL) + (h << 6U) + (h >> 2U);
        h ^= (static_cast<std::uint64_t>(std::get<2>(t)) * 0xC6A4A7935BD1E995ULL) + (h << 6U) + (h >> 2U);
        return static_cast<std::size_t>(h);
    }
};

struct PairHashU64 {
    std::size_t operator()(const std::pair<std::uint64_t, std::uint64_t>& p) const noexcept {
        std::uint64_t h = p.first ^ (p.second * 0x9E3779B97F4A7C15ULL);
        h ^= h >> 33U;
        h *= 0xFF51AFD7ED558CCDULL;
        h ^= h >> 33U;
        return static_cast<std::size_t>(h);
    }
};

struct TripleHashU64 {
    std::size_t operator()(const std::tuple<std::uint64_t, std::uint32_t, std::uint64_t>& t) const noexcept {
        std::uint64_t h = std::get<0>(t);
        h ^= (static_cast<std::uint64_t>(std::get<1>(t)) * 0x9E3779B97F4A7C15ULL) + (h << 6U) + (h >> 2U);
        h ^= (std::get<2>(t) * 0xC6A4A7935BD1E995ULL) + (h << 6U) + (h >> 2U);
        return static_cast<std::size_t>(h);
    }
};

/// compute Shannon entropy across values of map using compensated sum
template <class Map> double entropyOfMap(const Map& m) {
    Sum total;
    for (const auto& kv : m)
        total.add(kv.second);
    const double t = total.value();
    if (!(t > 0.0)) return 0.0;
    Sum h;
    for (const auto& kv : m) {
        if (kv.second > 0.0) {
            const double p = kv.second / t;
            h.add(-p * std::log2(p));
        }
    }
    return std::max(h.value(), 0.0);
}

} // namespace

// -- shannon entropy estimation -----------------------------------------------

/// compute Shannon entropy H(X) in bits with Neumaier summation compensation
/// in: counts non-negative frequency weights
/// out: entropy in bits (base 2); 0.0 if empty or uniform single point
/// thread: pure function, noexcept
double entropyOfCounts(std::span<const double> counts) noexcept {
    Sum total;
    for (double c : counts)
        total.add(c);
    const double t = total.value();
    if (!(t > 0.0)) return 0.0;
    Sum h;
    for (double c : counts) {
        if (c > 0.0) {
            const double p = c / t;
            h.add(-p * std::log2(p));
        }
    }
    return std::max(h.value(), 0.0);
}

// -- semantic ledger computation ----------------------------------------------

/// compute exact information-theoretic ledger from joint cell frequency mass
/// inv: Fréchet polytope bounds projection guarantees non-negative quantities
/// in: cells joint observations, totalEvents total count, confidenceAlpha
/// out: Ledger struct with exact Shannon and Miller-Madow estimators
/// thread: thread-safe
Ledger ledger(std::span<const Cell> cells, double totalEvents, double confidenceAlpha) {
    std::unordered_map<std::pair<std::uint64_t, std::uint32_t>, double, PairHash64> joint;
    std::unordered_map<std::uint32_t, double> w;
    std::unordered_map<std::uint64_t, double> x;
    std::unordered_map<std::uint32_t, std::vector<double>> xGivenW;
    std::unordered_map<std::uint64_t, std::vector<double>> wGivenX;

    Sum resolved;
    for (const auto& c : cells) {
        if (c.mass <= 0.0) continue;
        joint[{c.form, c.meaning}] += c.mass;
        w[c.meaning] += c.mass;
        x[c.form] += c.mass;
        resolved.add(c.mass);
    }

    for (const auto& [fm, mass] : joint) {
        if (mass <= 0.0) continue;
        xGivenW[fm.second].push_back(mass);
        wGivenX[fm.first].push_back(mass);
    }

    const double nTot = resolved.value();
    const double C = entropyOfMap(w);
    const double E = entropyOfMap(x);
    const double J = entropyOfMap(joint);

    // project empirical joint entropy into Fréchet polytope [max(C, E), C + E]
    // ~ bounds projection stops floating-point imprecision violating non-negativity
    const double J_bound = std::max({C, E, std::min(J, C + E)});
    const double Lambda = J_bound - C;
    const double Delta = J_bound - E;
    const double I = C + E - J_bound;
    const double distance = Lambda + Delta;

    // Perplexity / Effective vocabulary metrics: 2^H
    const double effectiveVocab = std::pow(2.0, E);
    const double effectiveContent = std::pow(2.0, C);
    const double effectiveLambda = std::pow(2.0, Lambda);
    const double effectiveDelta = std::pow(2.0, Delta);

    // Stratified Miller-Madow bias corrections for conditional distributions
    constexpr double twoLn2 = 1.3862943611198906;
    double lambdaMm = 0.0;
    if (nTot > 0.0) {
        Sum sumStratifiedLambda;
        for (const auto& [meaningId, massList] : xGivenW) {
            const double wMass = w[meaningId];
            if (wMass > 0.0) {
                const double rawCondH = entropyOfCounts(massList);
                const auto kObs = massList.size();
                const double mmCondH =
                    (kObs > 1) ? rawCondH + (static_cast<double>(kObs - 1) / (wMass * twoLn2)) : rawCondH;
                sumStratifiedLambda.add((wMass / nTot) * mmCondH);
            }
        }
        lambdaMm = std::max(sumStratifiedLambda.value(), 0.0);
    }

    double deltaMm = 0.0;
    if (nTot > 0.0) {
        Sum sumStratifiedDelta;
        for (const auto& [formId, massList] : wGivenX) {
            const double xMass = x[formId];
            if (xMass > 0.0) {
                const double rawCondH = entropyOfCounts(massList);
                const auto kObs = massList.size();
                const double mmCondH =
                    (kObs > 1) ? rawCondH + (static_cast<double>(kObs - 1) / (xMass * twoLn2)) : rawCondH;
                sumStratifiedDelta.add((xMass / nTot) * mmCondH);
            }
        }
        deltaMm = std::max(sumStratifiedDelta.value(), 0.0);
    }

    const double contentMm = millerMadow(C, w.size(), nTot);
    const double expressionMm = millerMadow(E, x.size(), nTot);
    const double conveyanceMm = std::max(contentMm - deltaMm, 0.0);

    const auto ci = certifiedInterval(w.size(), nTot, confidenceAlpha);

    Ledger out;
    out.coverage = totalEvents > 0.0 ? nTot / totalEvents : 0.0;
    out.content = C;
    out.expression = E;
    out.latitude = Lambda;
    out.doubt = Delta;
    out.conveyance = I;
    out.distance = distance;
    out.effectiveVocabulary = effectiveVocab;
    out.effectiveContent = effectiveContent;
    out.effectiveLatitude = effectiveLambda;
    out.effectiveDoubt = effectiveDelta;
    out.contentMm = contentMm;
    out.expressionMm = expressionMm;
    out.latitudeMm = lambdaMm;
    out.doubtMm = deltaMm;
    out.conveyanceMm = conveyanceMm;
    out.contentEpsilon = ci.epsilon;
    out.contentHalfWidth = ci.halfWidth;

    return out;
}

// -- consequence decomposition and information loss ---------------------------

/// compute three-way consequence decomposition: E = I(X;Y) + H(M|Y) + H(X|M)
ConsequenceDecomposition consequenceDecomposition(std::span<const XY> xy, std::span<const FormMeaning> map) {
    std::unordered_map<std::uint64_t, std::vector<std::pair<std::uint32_t, double>>> pm;
    for (const auto& m : map)
        pm[m.form].emplace_back(m.meaning, m.p);

    std::unordered_map<std::uint64_t, double> fx;
    std::unordered_map<std::uint32_t, double> fy;
    std::unordered_map<std::pair<std::uint64_t, std::uint32_t>, double, PairHash64> fxy;

    std::unordered_map<std::uint32_t, double> fm;
    std::unordered_map<std::pair<std::uint32_t, std::uint32_t>, double, PairHash32> fmy;
    std::unordered_map<std::pair<std::uint64_t, std::uint32_t>, double, PairHash64> fxm;

    Sum total;
    for (const auto& r : xy) {
        if (r.n <= 0.0) continue;
        fx[r.form] += r.n;
        fy[r.y] += r.n;
        fxy[{r.form, r.y}] += r.n;
        total.add(r.n);

        const auto it = pm.find(r.form);
        if (it != pm.end()) {
            for (const auto& [meaning, p] : it->second) {
                const double mass = r.n * p;
                fm[meaning] += mass;
                fmy[{meaning, r.y}] += mass;
                fxm[{r.form, meaning}] += mass;
            }
        }
    }

    const double hX = entropyOfMap(fx);
    const double hY = entropyOfMap(fy);
    const double hXY = entropyOfMap(fxy);
    const double iXY = std::max(hX + hY - hXY, 0.0); // Consequence

    const double hM = entropyOfMap(fm);
    const double hMY = entropyOfMap(fmy);
    const double hMgivenY = std::max(hMY - hY, 0.0); // Hedge

    const double hXM = entropyOfMap(fxm);
    const double hXgivenM = std::max(hXM - hM, 0.0); // Latitude

    ConsequenceDecomposition decomp;
    decomp.expression = hX;
    decomp.consequence = iXY;
    decomp.hedge = hMgivenY;
    decomp.latitude = hXgivenM;
    return decomp;
}

/// compute volume-weighted Jensen-Shannon divergence for merging template forms
MergeCost mergeCost(std::span<const XY> xy, const std::unordered_map<std::uint64_t, bool>& merged, double totalEvents) {
    if (merged.size() < 2) return {.jsd = 0.0, .share = 0.0};
    std::unordered_map<std::uint64_t, double> nf;
    std::unordered_map<std::pair<std::uint64_t, std::uint32_t>, double, PairHash64> nfy;
    std::unordered_map<std::uint32_t, double> ny;
    Sum nF;

    for (const auto& r : xy) {
        if (!merged.contains(r.form)) continue;
        nf[r.form] += r.n;
        nfy[{r.form, r.y}] += r.n;
        ny[r.y] += r.n;
        nF.add(r.n);
    }
    const double N = nF.value();
    if (!(N > 0.0)) return {.jsd = 0.0, .share = 0.0};

    Sum jsd;
    for (const auto& [key, n] : nfy) {
        const double p_yf = n / nf[key.first];
        const double p_yF = ny[key.second] / N;
        if (p_yf > 0.0 && p_yF > 0.0) {
            jsd.add((n / N) * std::log2(p_yf / p_yF));
        }
    }
    return {.jsd = std::max(jsd.value(), 0.0), .share = totalEvents > 0.0 ? N / totalEvents : 0.0};
}

/// compute conditional mutual information leakage I(X; Y | W)
double leakage(std::span<const XY> xy, std::span<const FormMeaning> map) {
    std::unordered_map<std::uint64_t, std::vector<std::pair<std::uint32_t, double>>> pm;
    for (const auto& m : map)
        pm[m.form].emplace_back(m.meaning, m.p);

    std::unordered_map<std::pair<std::uint64_t, std::uint32_t>, double, PairHash64> xw;
    std::unordered_map<std::pair<std::uint32_t, std::uint32_t>, double, PairHash32> yw;
    std::unordered_map<std::uint32_t, double> w;
    std::unordered_map<std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>, double, TripleHash> xyw;

    for (const auto& r : xy) {
        const auto it = pm.find(r.form);
        if (it == pm.end()) continue;
        for (const auto& [meaning, p] : it->second) {
            const double m = r.n * p;
            xw[{r.form, meaning}] += m;
            yw[{r.y, meaning}] += m;
            w[meaning] += m;
            xyw[{r.form, r.y, meaning}] += m;
        }
    }
    return std::max(entropyOfMap(xw) + entropyOfMap(yw) - entropyOfMap(w) - entropyOfMap(xyw), 0.0);
}

/// compute mutual information I(X; Y) from raw count distributions
double mutualInformation(std::span<const XY> xy) {
    std::unordered_map<std::uint64_t, double> fx;
    std::unordered_map<std::uint32_t, double> fy;
    std::unordered_map<std::pair<std::uint64_t, std::uint32_t>, double, PairHash64> fxy;
    Sum total;

    for (const auto& r : xy) {
        fx[r.form] += r.n;
        fy[r.y] += r.n;
        fxy[{r.form, r.y}] += r.n;
        total.add(r.n);
    }
    return std::max(entropyOfMap(fx) + entropyOfMap(fy) - entropyOfMap(fxy), 0.0);
}

// -- price of saying and depth ledger -----------------------------------------

/// compute prefix-code expected length bounds and wording premium
PriceOfSaying priceOfSaying(const Ledger& l, double nMessages) noexcept {
    PriceOfSaying p;
    if (nMessages <= 0.0) return p;
    p.expectedMeaningBits = nMessages * l.content;
    p.expectedExpressionBits = nMessages * l.expression;
    p.expectedClarificationBits = nMessages * l.doubt;
    p.totalSayingBits = nMessages * (l.expression + l.doubt);
    p.sayingCostBits = nMessages * l.latitude;
    p.lowerBoundBits = p.totalSayingBits;
    p.upperBoundBits = p.totalSayingBits + 2.0;
    return p;
}

/// compute multi-level depth ledger decomposing entropy across functional steps
DepthLedger computeDepthLedger(std::span<const HierarchicalStep> steps) {
    DepthLedger out;
    if (steps.empty()) return out;

    std::size_t L = 0;
    for (const auto& s : steps) {
        L = std::max(L, s.levels.size());
    }

    std::vector<std::unordered_map<std::uint64_t, double>> wMaps(L + 1);
    std::vector<std::unordered_map<std::pair<std::uint64_t, std::uint32_t>, double, PairHash64>> wyMaps(L + 1);
    std::unordered_map<std::uint32_t, double> yMap;

    std::vector<std::unordered_map<std::pair<std::uint64_t, std::uint64_t>, double, PairHashU64>> stepMaps(L + 1);
    std::vector<std::unordered_map<std::tuple<std::uint64_t, std::uint32_t, std::uint64_t>, double, TripleHashU64>>
        stepYMaps(L + 1);

    for (const auto& step : steps) {
        if (step.mass <= 0.0) continue;
        yMap[step.y] += step.mass;

        for (std::size_t k = 0; k <= L; ++k) {
            const std::uint64_t val =
                (k == 0) ? step.form
                         : (k - 1 < step.levels.size() ? static_cast<std::uint64_t>(step.levels[k - 1]) : 0ULL);
            wMaps[k][val] += step.mass;
            wyMaps[k][{val, step.y}] += step.mass;

            if (k >= 1) {
                const std::uint64_t prevVal =
                    (k == 1) ? step.form
                             : (k - 2 < step.levels.size() ? static_cast<std::uint64_t>(step.levels[k - 2]) : 0ULL);
                stepMaps[k][{prevVal, val}] += step.mass;
                stepYMaps[k][{prevVal, step.y, val}] += step.mass;
            }
        }
    }

    const double hY = entropyOfMap(yMap);
    std::vector<double> hW(L + 1);
    std::vector<double> iWY(L + 1);

    for (std::size_t k = 0; k <= L; ++k) {
        hW[k] = entropyOfMap(wMaps[k]);
        const double hWY = entropyOfMap(wyMaps[k]);
        iWY[k] = std::max(hW[k] + hY - hWY, 0.0);
    }

    out.totalEntropy = hW[0];
    out.totalRelevance = iWY[0];
    out.coarsestEntropy = hW[L];
    out.coarsestRelevance = iWY[L];

    out.entropyLevels.resize(L);
    out.relevanceLevels.resize(L);

    for (std::size_t k = 1; k <= L; ++k) {
        const double hStep = entropyOfMap(stepMaps[k]);
        const double hStepY = entropyOfMap(stepYMaps[k]);
        const double hWYk = entropyOfMap(wyMaps[k]);

        const double condH = std::max(hStep - hW[k], 0.0);
        const double condI = std::max(hStep + hWYk - hW[k] - hStepY, 0.0);

        out.entropyLevels[k - 1] = condH;
        out.relevanceLevels[k - 1] = condI;
    }

    return out;
}

} // namespace razobill::stats
