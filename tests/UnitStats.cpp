// =============================================================================
// unit test suite for information-theoretic semantic accounting
//
// verifies xxHash64 test vectors, entropy, semantic ledger identities, merge
// cost closed form, consequence decomposition, and Frechet polytope bounds
// =============================================================================

#include <algorithm>
#include <cmath>
#include <random>
#include <unordered_map>

#include "doctest.h"
#include "librazobill/Hash.hpp"
#include "librazobill/Stats.hpp"

using namespace razobill;
using namespace razobill::stats;

// pins xxHash64 test vectors ("" -> 0xEF46DB3751D8E999ULL) and collision checks
TEST_CASE("xxh64 matches the reference test vectors") {
    CHECK(core::xxh64("") == 0xEF46DB3751D8E999ULL);
    CHECK(core::xxh64("a") == 0xD24EC4F1A98C6E5BULL);
    CHECK(core::xxh64("abc") == 0x44BC2CF5AD770999ULL);
    std::string s;
    std::unordered_map<std::uint64_t, int> seen;
    for (int n = 0; n < 200; ++n) {
        CHECK(++seen[core::xxh64(s)] == 1);
        s.push_back(static_cast<char>('a' + n % 26));
    }
}

// pins Shannon entropy calculation on uniform, degenerate, and empty counts
TEST_CASE("entropy basics") {
    const double u[] = {1, 1, 1, 1};
    CHECK(entropyOfCounts(u) == doctest::Approx(2.0));
    const double d[] = {5, 0, 0};
    CHECK(entropyOfCounts(d) == 0.0);
    CHECK(entropyOfCounts({}) == 0.0);
}

// pins canonical polysemy/synonymy toy lexicon values from Atta (2026)
TEST_CASE("the paper's Figure 1 lexicon") {
    // exact verification of the canonical polysemy / synonymy toy example from
    // "Latitude and Doubt: An exact ledger of meaning in messages" (Atta, 2026)
    // meanings: river edge(0), aircraft tilt(1), financial institution(2)
    // forms:    shore(0), bank(1), lender(2)
    // polysemous word 'bank' creates doubt; synonyms create latitude
    const std::vector<Cell> cells{{0, 0, 0.25}, {1, 0, 0.25}, {1, 1, 0.125}, {1, 2, 0.25}, {2, 2, 0.125}};
    const auto l = ledger(cells, 1.0);
    CHECK(l.content == doctest::Approx(1.406).epsilon(1e-3));
    CHECK(l.expression == doctest::Approx(1.299).epsilon(1e-3));
    CHECK(l.latitude == doctest::Approx(0.844).epsilon(1e-3));
    CHECK(l.doubt == doctest::Approx(0.951).epsilon(1e-3));
    CHECK(l.conveyance == doctest::Approx(0.454).epsilon(2e-3));
    CHECK(l.coverage == doctest::Approx(1.0));
}

// pins Theorem 1 identities |E - (I + L)| < 1e-11 across 20,000 random laws
TEST_CASE("ledger identities hold on random joint laws") {
    // Monte Carlo check of Theorem 1 identities across 20,000 random laws
    // asserting |E - (I + Lambda)| < 1e-11 and |C - (I + Delta)| < 1e-11
    std::mt19937_64 rng(42);
    std::exponential_distribution<double> e(1.0);
    double worst = 0;
    for (int it = 0; it < 20000; ++it) {
        const unsigned K = 2 + rng() % 8, N = 2 + rng() % 8;
        std::vector<Cell> cells;
        double tot = 0;
        for (unsigned w = 0; w < K; ++w)
            for (unsigned x = 0; x < N; ++x)
                if (rng() % 3) {
                    const double m = std::pow(e(rng), static_cast<double>(1 + rng() % 4));
                    cells.push_back({x, w, m});
                    tot += m;
                }
        if (cells.empty()) continue;
        for (auto& c : cells)
            c.mass /= tot;
        const auto l = ledger(cells, 1.0);
        for (double d :
             {std::abs(l.expression - (l.conveyance + l.latitude)), std::abs(l.content - (l.conveyance + l.doubt)),
              std::abs((l.expression - l.content) - (l.latitude - l.doubt))})
            worst = std::max(worst, d);
        REQUIRE(l.latitude >= -1e-12);
        REQUIRE(l.doubt >= -1e-12);
        REQUIRE(l.conveyance >= -1e-12);
    }
    CHECK(worst < 1e-12);
}

// pins JSD merge cost equivalence to mutual info drop across 2,000 trials
TEST_CASE("merge cost equals I(X;Y) - I(W;Y) (closed form of the gate)") {
    std::mt19937_64 rng(7);
    for (int it = 0; it < 2000; ++it) {
        const unsigned F = 3 + rng() % 8, Y = 2 + rng() % 4;
        std::vector<XY> xy;
        double N = 0;
        for (unsigned f = 0; f < F; ++f)
            for (unsigned y = 0; y < Y; ++y) {
                const double n = static_cast<double>(1 + rng() % 50);
                xy.push_back({f, y, n});
                N += n;
            }
        std::unordered_map<std::uint64_t, bool> merged;
        std::vector<XY> collapsed;
        for (unsigned f = 0; f < F; ++f)
            if (rng() % 2 || f < 2) merged[f] = true;
        for (const auto& r : xy)
            collapsed.push_back({merged.contains(r.form) ? std::uint64_t{1000} : r.form, r.y, r.n});
        const auto c = mergeCost(xy, merged, N);
        const double direct = mutualInformation(xy) - mutualInformation(collapsed);
        REQUIRE(c.share * c.jsd == doctest::Approx(direct).epsilon(1e-9));
        REQUIRE(c.jsd >= 0);
        REQUIRE(c.jsd <= std::log2(static_cast<double>(merged.size())) + 1e-9);
    }
}

// pins zero cost for identical distributions and 1 bit for disjoint pairs
TEST_CASE("merge cost: identical distributions cost nothing, disjoint ones cost one bit per equal-volume pair") {
    const std::vector<XY> same{{0, 0, 10}, {0, 1, 30}, {1, 0, 5}, {1, 1, 15}};
    CHECK(mergeCost(same, {{0, true}, {1, true}}, 60).jsd == doctest::Approx(0.0).epsilon(1e-12));
    const std::vector<XY> disjoint{{0, 0, 10}, {1, 1, 10}};
    CHECK(mergeCost(disjoint, {{0, true}, {1, true}}, 20).jsd == doctest::Approx(1.0));
    CHECK(mergeCost(disjoint, {{0, true}}, 20).jsd == 0.0);
}

// pins zero leakage under purpose-preserving maps and positive otherwise
TEST_CASE("leakage is zero for a map that preserves the purpose, positive otherwise") {
    const std::vector<XY> xy{{0, 0, 10}, {1, 0, 12}, {2, 1, 9}, {3, 1, 11}};
    const std::vector<FormMeaning> good{{0, 0, 1}, {1, 0, 1}, {2, 1, 1}, {3, 1, 1}};
    CHECK(leakage(xy, good) == doctest::Approx(0.0).epsilon(1e-12));
    const std::vector<FormMeaning> bad{{0, 0, 1}, {1, 0, 1}, {2, 0, 1}, {3, 0, 1}};
    CHECK(leakage(xy, bad) == doctest::Approx(mutualInformation(xy)).epsilon(1e-9));
}

// pins Miller-Madow bias correction and Proposition 10 confidence intervals
TEST_CASE("estimation: Miller-Madow correction and certified interval") {
    // Miller-Madow correction adds (K - 1) / (2n ln 2) bits
    const double pluginH = 1.5;
    const double corrected = millerMadow(pluginH, 4, 1000.0);
    CHECK(corrected > pluginH);
    CHECK(corrected == doctest::Approx(pluginH + 3.0 / (2000.0 * 0.6931471805599453)));

    // Proposition 10 certified interval
    const auto ci = certifiedInterval(25, 10000.0, 0.05);
    CHECK(ci.epsilon > 0.0);
    CHECK(ci.halfWidth > 0.0);
    CHECK(ci.halfWidth < std::log2(25.0));
}

// pins Theorem 4c four-term entropy decomposition
TEST_CASE("consequence decomposition: Theorem 4c E = I(X;Y) + H(M|Y) + H(X|M)") {
    // Purpose Y in {0, 1}, Meanings M in {0, 1}, Forms X in {0, 1, 2, 3}
    // forms 0, 1 convey M = 0; forms 2, 3 convey M = 1 (latitude > 0)
    const std::vector<XY> xy{{0, 0, 40.0}, {1, 0, 60.0}, {2, 1, 30.0}, {3, 1, 70.0}};
    const std::vector<FormMeaning> map{{0, 0, 1.0}, {1, 0, 1.0}, {2, 1, 1.0}, {3, 1, 1.0}};

    auto decomp = consequenceDecomposition(xy, map);
    CHECK(decomp.satisfiesIdentity(1e-9));
    CHECK(decomp.expression > 0.0);
    CHECK(decomp.consequence > 0.0);
    CHECK(decomp.latitude > 0.0); // synonyms {0, 1} and {2, 3} give positive latitude
    CHECK(decomp.hedge >= 0.0);
}

// pins metric distance d(W,X) = Lambda + Delta and vocabulary law lower bound
TEST_CASE("Proposition 3 metric distance and Corollary 2 vocabulary law") {
    // exact verification using Figure 1 lexicon (3 forms: shore, bank, lender)
    const std::vector<Cell> cells{{0, 0, 0.25}, {1, 0, 0.25}, {1, 1, 0.125}, {1, 2, 0.25}, {2, 2, 0.125}};
    const auto l = ledger(cells, 1000.0);

    // ledger identities:
    CHECK(l.satisfiesLedgerIdentities(1e-6));

    // Proposition 3 metric distance d(W, X) = Lambda + Delta
    CHECK(l.distance == doctest::Approx(l.latitude + l.doubt).epsilon(1e-12));
    CHECK(l.distance >= 0.0);

    // Corollary 2 vocabulary law: Delta - Lambda >= C - log2(N) for N = 3 forms
    CHECK(l.satisfiesVocabularyLaw(3, 1e-5));
    CHECK(l.effectiveVocabulary == doctest::Approx(std::pow(2.0, l.expression)).epsilon(1e-6));
    CHECK(l.effectiveContent == doctest::Approx(std::pow(2.0, l.content)).epsilon(1e-6));
    CHECK(l.effectiveLatitude == doctest::Approx(std::pow(2.0, l.latitude)).epsilon(1e-6));
    CHECK(l.effectiveDoubt == doctest::Approx(std::pow(2.0, l.doubt)).epsilon(1e-6));

    // stratified Miller-Madow estimators >= plugin estimators on finite counts
    CHECK(l.contentMm >= l.content);
    CHECK(l.expressionMm >= l.expression);
    CHECK(l.latitudeMm >= l.latitude);
    CHECK(l.doubtMm >= l.doubt);
}

// pins Theorem 7 asymptotic price of saying identity and 2-bit coding bounds
TEST_CASE("Theorem 7: The price of saying n(E + Delta) = nC + nLambda") {
    // canonical Figure 1 lexicon from Atta (2026)
    const std::vector<Cell> cells{{0, 0, 0.25}, {1, 0, 0.25}, {1, 1, 0.125}, {1, 2, 0.25}, {2, 2, 0.125}};
    const auto l = ledger(cells, 1.0);
    REQUIRE(l.satisfiesLedgerIdentities(1e-9));

    for (double nMessages : {1.0, 10.0, 100.0, 5000.0}) {
        const auto p = priceOfSaying(l, nMessages);
        CHECK(p.satisfiesIdentity(1e-9));
        CHECK(p.expectedMeaningBits == doctest::Approx(nMessages * l.content));
        CHECK(p.expectedExpressionBits == doctest::Approx(nMessages * l.expression));
        CHECK(p.expectedClarificationBits == doctest::Approx(nMessages * l.doubt));
        CHECK(p.sayingCostBits == doctest::Approx(nMessages * l.latitude));
        CHECK(p.totalSayingBits == doctest::Approx(p.expectedMeaningBits + p.sayingCostBits));
        CHECK(p.totalSayingBits == doctest::Approx(p.expectedExpressionBits + p.expectedClarificationBits));
        CHECK(p.lowerBoundBits == p.totalSayingBits);
        CHECK(p.upperBoundBits == p.totalSayingBits + 2.0);
    }
}

// pins Proposition 6 telescoping entropy and relevance across hierarchy
TEST_CASE("Proposition 6: Multi-level depth ledger splits entropy and relevance across levels") {
    // hierarchical test setup:
    // W_0 = Form X in {0, 1, 2, 3}
    // W_1 = Synset / Lemma in {0, 1} (forms 0,1 -> W_1=0; forms 2,3 -> W_1=1)
    // W_2 = Coarse Domain in {0} (W_1=0,1 -> W_2=0)
    // purpose Y in {0, 1}
    std::vector<HierarchicalStep> steps{
        {0, {0, 0}, 0, 40.0}, {1, {0, 0}, 0, 60.0}, {2, {1, 0}, 1, 30.0}, {3, {1, 0}, 1, 70.0}};

    const auto dl = computeDepthLedger(steps);
    CHECK(dl.satisfiesIdentity(1e-9));
    CHECK(dl.entropyLevels.size() == 2);
    CHECK(dl.relevanceLevels.size() == 2);
    CHECK(dl.totalEntropy > 0.0);
    CHECK(dl.totalRelevance > 0.0);
    CHECK(dl.entropyLevels[0] > 0.0);
    CHECK(dl.coarsestEntropy == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(dl.coarsestRelevance == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(dl.entropyLevels[0] + dl.entropyLevels[1] + dl.coarsestEntropy ==
          doctest::Approx(dl.totalEntropy).epsilon(1e-9));
    CHECK(dl.relevanceLevels[0] + dl.relevanceLevels[1] + dl.coarsestRelevance ==
          doctest::Approx(dl.totalRelevance).epsilon(1e-9));
}

// pins independent/functional extrema and unaggregated cell summation
TEST_CASE("Theorem 1 Fréchet polytope bounding and Cell deduplication") {
    // 1. exact independent variable test: X independent of W
    std::vector<Cell> indepCells{{0, 0, 25.0}, {0, 1, 25.0}, {1, 0, 25.0}, {1, 1, 25.0}};
    const auto lIndep = ledger(indepCells, 100.0);
    CHECK(lIndep.conveyance == doctest::Approx(0.0).epsilon(1e-12));
    CHECK(std::abs(lIndep.expression - (lIndep.conveyance + lIndep.latitude)) < 1e-15);
    CHECK(std::abs(lIndep.content - (lIndep.conveyance + lIndep.doubt)) < 1e-15);
    CHECK(std::abs((lIndep.expression - lIndep.content) - (lIndep.latitude - lIndep.doubt)) < 1e-15);

    // 2. exact deterministic functional test: X == W
    std::vector<Cell> detCells{{0, 0, 40.0}, {1, 1, 60.0}};
    const auto lDet = ledger(detCells, 100.0);
    CHECK(lDet.latitude == doctest::Approx(0.0).epsilon(1e-12));
    CHECK(lDet.doubt == doctest::Approx(0.0).epsilon(1e-12));
    CHECK(lDet.conveyance == doctest::Approx(lDet.expression).epsilon(1e-15));
    CHECK(lDet.conveyance == doctest::Approx(lDet.content).epsilon(1e-15));
    CHECK(std::abs(lDet.expression - (lDet.conveyance + lDet.latitude)) < 1e-15);
    CHECK(std::abs(lDet.content - (lDet.conveyance + lDet.doubt)) < 1e-15);

    // 3. cell deduplication: unaggregated identical (form, meaning) rows
    std::vector<Cell> unaggCells{
        {0, 0, 10.0}, {0, 0, 10.0}, // combined: {0, 0, 20.0}
        {1, 0, 15.0}, {1, 0, 15.0}, // combined: {1, 0, 30.0}
        {2, 1, 25.0}, {2, 1, 25.0}  // combined: {2, 1, 50.0}
    };
    std::vector<Cell> aggCells{{0, 0, 20.0}, {1, 0, 30.0}, {2, 1, 50.0}};
    const auto lUnagg = ledger(unaggCells, 100.0);
    const auto lAgg = ledger(aggCells, 100.0);

    CHECK(lUnagg.expression == doctest::Approx(lAgg.expression).epsilon(1e-15));
    CHECK(lUnagg.content == doctest::Approx(lAgg.content).epsilon(1e-15));
    CHECK(lUnagg.latitude == doctest::Approx(lAgg.latitude).epsilon(1e-15));
    CHECK(lUnagg.doubt == doctest::Approx(lAgg.doubt).epsilon(1e-15));
    CHECK(lUnagg.conveyance == doctest::Approx(lAgg.conveyance).epsilon(1e-15));
    CHECK(lUnagg.distance == doctest::Approx(lAgg.distance).epsilon(1e-15));
    CHECK(lUnagg.effectiveVocabulary == doctest::Approx(lAgg.effectiveVocabulary).epsilon(1e-15));
}
