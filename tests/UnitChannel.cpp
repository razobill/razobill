// =============================================================================
// unit test suite for steganographic provenance channel (Theorem 8)
//
// verifies capacity bounds, message embedding, lossless decoding, Proposition 9
// key length, mantissa limits, circular S^1 boundary, and underflow rejection
// =============================================================================

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "doctest.h"
#include "librazobill/Channel.hpp"

using namespace razobill::channel;

// pins mean capacity and safe payload bounds for synonymous sequences
TEST_CASE("Channel capacity analysis for synonymous sequences") {
    // 100 choices, each with 2 equally likely synonymous forms (1 bit per step)
    std::vector<MeaningChoice> sequence;
    sequence.reserve(100);
    for (std::uint32_t i = 0; i < 100; ++i) {
        MeaningChoice choice;
        choice.meaning = i % 10;
        choice.options = {SynonymousOption{0, 0.5}, SynonymousOption{1, 0.5}};
        sequence.push_back(std::move(choice));
    }

    auto cap = analyzeCapacity(sequence);
    CHECK(cap.meanCapacityBits == doctest::Approx(100.0));
    CHECK(cap.variance == doctest::Approx(0.0).epsilon(1e-9));
    // safe payload k = floor(mu - 3*sigma - 7) = 100 - 7 = 93
    CHECK(cap.safePayloadBits == 93);

    // sequence with no latitude (deterministic 1 form per meaning)
    std::vector<MeaningChoice> zeroSeq;
    for (std::uint32_t i = 0; i < 50; ++i) {
        MeaningChoice choice;
        choice.meaning = i;
        choice.options = {SynonymousOption{0, 1.0}};
        zeroSeq.push_back(std::move(choice));
    }
    auto zeroCap = analyzeCapacity(zeroSeq);
    CHECK(zeroCap.meanCapacityBits == 0.0);
    CHECK(zeroCap.safePayloadBits == 0);
}

// pins Theorem 8 exact message recovery and wrong key decodability rejection
TEST_CASE("Channel embed and decode lossless message recovery") {
    // sequence of 16 synonymous choices, 4 options each (2 bits per choice)
    std::vector<MeaningChoice> sequence;
    for (std::uint32_t i = 0; i < 16; ++i) {
        MeaningChoice choice;
        choice.meaning = i;
        choice.options = {SynonymousOption{0, 0.25}, SynonymousOption{1, 0.25}, SynonymousOption{2, 0.25},
                          SynonymousOption{3, 0.25}};
        sequence.push_back(std::move(choice));
    }

    const std::uint64_t originalMessage = 0xABCDULL;
    constexpr std::size_t payloadBits = 16;     // within safe capacity bounds
    constexpr double sharedKey = 0.38196601125; // golden ratio fraction

    auto embedRes = embed(sequence, originalMessage, payloadBits, sharedKey, 0.5);
    REQUIRE(embedRes.success);
    CHECK(embedRes.payloadBits == payloadBits);
    CHECK(embedRes.chosenForms.size() == sequence.size());

    // Theorem 8: chosen forms must be valid indices within each option list
    for (std::size_t i = 0; i < sequence.size(); ++i) {
        CHECK(embedRes.chosenForms[i] < sequence[i].options.size());
    }

    // decode using the shared key
    auto decodeRes = decode(sequence, embedRes.chosenForms, payloadBits, sharedKey);
    REQUIRE(decodeRes.success);
    CHECK(decodeRes.payloadBits == payloadBits);
    CHECK(decodeRes.message == originalMessage);

    // decoding with a different key must not decode the original message
    constexpr double wrongKey = 0.71828182845;
    auto wrongDecode = decode(sequence, embedRes.chosenForms, payloadBits, wrongKey);
    if (wrongDecode.success) {
        CHECK(wrongDecode.message != originalMessage);
    }
}

// pins Proposition 9 key length formula and exact failure probability bounds
TEST_CASE("Proposition 9: Minimum key length P and Theorem 8 failure probability") {
    // 100 choices, each with 2 equally likely synonymous forms
    std::vector<MeaningChoice> sequence;
    sequence.reserve(100);
    for (std::uint32_t i = 0; i < 100; ++i) {
        MeaningChoice choice;
        choice.meaning = i;
        choice.options = {SynonymousOption{0, 0.5}, SynonymousOption{1, 0.5}};
        sequence.push_back(std::move(choice));
    }

    // Proposition 9: P = ceil(sum max -log2 Q) + s + 1
    // sum max -log2 Q = 100.0
    CHECK(requiredKeyBits(sequence, 64) == 100 + 64 + 1);
    CHECK(requiredKeyBits(sequence, 32) == 100 + 32 + 1);

    auto cap = analyzeCapacity(sequence);
    CHECK(cap.meanCapacityBits == 100.0);

    // Chebyshev bound calculation
    const double bound = failureProbabilityBound(cap, 50, 10.0);
    CHECK(bound >= 0.0);
    CHECK(bound <= 1.0);

    // exact analytical failure probability on small toy sequence:
    // 2 choices of 2 equal options:
    std::vector<MeaningChoice> toySeq;
    toySeq.push_back(MeaningChoice{0, {SynonymousOption{0, 0.5}, SynonymousOption{1, 0.5}}});
    toySeq.push_back(MeaningChoice{1, {SynonymousOption{0, 0.5}, SynonymousOption{1, 0.5}}});

    // for k = 1: delta = sum_{x^2} Q min{1, 2^1 * Q} = 4 * 0.25 * 0.5 = 0.5
    CHECK(exactFailureProbability(toySeq, 1) == doctest::Approx(0.5).epsilon(1e-6));

    // for k = 0 bits: delta = sum_{x^2} Q^2 = 4 * 0.0625 = 0.25
    CHECK(exactFailureProbability(toySeq, 0) == doctest::Approx(0.25).epsilon(1e-6));

    // for k = 2 bits: delta = sum_{x^2} Q min{1, 4 * 0.25} = 1.0
    CHECK(exactFailureProbability(toySeq, 2) == doctest::Approx(1.0).epsilon(1e-6));
}

// pins 53-bit IEEE-754 mantissa guard and circular S1 torus wrap-around
TEST_CASE("Channel mantissa bounds (> 53 bits) and circular S1 boundary recovery") {
    std::vector<MeaningChoice> sequence;
    for (std::uint32_t i = 0; i < 30; ++i) {
        MeaningChoice choice;
        choice.meaning = i;
        choice.options = {SynonymousOption{0, 0.5}, SynonymousOption{1, 0.5}};
        sequence.push_back(std::move(choice));
    }

    constexpr double sharedKey = 0.6180339887;

    // 1. guard against payloadBits > 53 (IEEE-754 mantissa limits)
    auto embed54 = embed(sequence, 1, 54, sharedKey, 0.5);
    CHECK(!embed54.success);

    auto embed64 = embed(sequence, 1, 64, sharedKey, 0.5);
    CHECK(!embed64.success);

    std::vector<std::size_t> mockForms(sequence.size(), 0);
    auto decode54 = decode(sequence, mockForms, 54, sharedKey);
    CHECK(!decode54.success);

    auto decode64 = decode(sequence, mockForms, 64, sharedKey);
    CHECK(!decode64.success);

    // 2. circular boundary on S^1: message 0 with small dither
    constexpr std::size_t safeK = 8;
    const std::uint64_t msg0 = 0;
    auto embed0 = embed(sequence, msg0, safeK, sharedKey, 0.001);
    if (embed0.success) {
        auto decode0 = decode(sequence, embed0.chosenForms, safeK, sharedKey);
        REQUIRE(decode0.success);
        CHECK(decode0.message == msg0);
    }

    // 3. circular boundary on S^1: message 2^k - 1 with large dither
    const std::uint64_t msgMax = (1ULL << safeK) - 1;
    auto embedMax = embed(sequence, msgMax, safeK, sharedKey, 0.999);
    if (embedMax.success) {
        auto decodeMax = decode(sequence, embedMax.chosenForms, safeK, sharedKey);
        REQUIRE(decodeMax.success);
        CHECK(decodeMax.message == msgMax);
    }
}

// pins rejection of empty inputs, zero payload bits, and precision underflow
TEST_CASE("Channel degenerate inputs and precision underflow rejection") {
    constexpr double sharedKey = 0.123456789;

    // 1. empty sequence rejection
    std::vector<MeaningChoice> emptySeq;
    auto resEmptyEmbed = embed(emptySeq, 1, 4, sharedKey, 0.5);
    CHECK(!resEmptyEmbed.success);

    std::vector<std::size_t> emptyForms;
    auto resEmptyDecode = decode(emptySeq, emptyForms, 4, sharedKey);
    CHECK(!resEmptyDecode.success);

    // 2. payloadBits == 0 rejection
    std::vector<MeaningChoice> validSeq;
    MeaningChoice mc;
    mc.meaning = 1;
    mc.options = {SynonymousOption{0, 0.5}, SynonymousOption{1, 0.5}};
    validSeq.push_back(mc);

    auto resZeroBitsEmbed = embed(validSeq, 0, 0, sharedKey, 0.5);
    CHECK(!resZeroBitsEmbed.success);

    std::vector<std::size_t> validForms = {0};
    auto resZeroBitsDecode = decode(validSeq, validForms, 0, sharedKey);
    CHECK(!resZeroBitsDecode.success);

    // 3. step with empty options rejection
    std::vector<MeaningChoice> emptyOptSeq;
    emptyOptSeq.push_back(MeaningChoice{1, {}});
    auto resEmptyOptEmbed = embed(emptyOptSeq, 1, 4, sharedKey, 0.5);
    CHECK(!resEmptyOptEmbed.success);

    // 4. step with all zero probabilities rejection
    std::vector<MeaningChoice> zeroProbSeq;
    zeroProbSeq.push_back(MeaningChoice{1, {SynonymousOption{0, 0.0}, SynonymousOption{1, 0.0}}});
    auto resZeroProbEmbed = embed(zeroProbSeq, 1, 4, sharedKey, 0.5);
    CHECK(!resZeroProbEmbed.success);

    auto resZeroProbDecode = decode(zeroProbSeq, validForms, 4, sharedKey);
    CHECK(!resZeroProbDecode.success);

    // 5. observed forms size mismatch rejection
    std::vector<std::size_t> mismatchedForms = {0, 1};
    auto resMismatchDecode = decode(validSeq, mismatchedForms, 4, sharedKey);
    CHECK(!resMismatchDecode.success);

    // 6. long sequence causing interval precision underflow (< 1e-15)
    // 60 choices of 2 equal options cuts interval by 2^-60 (~ 8.6e-19 < 1e-15)
    std::vector<MeaningChoice> longSeq;
    for (int i = 0; i < 60; ++i) {
        MeaningChoice choice;
        choice.meaning = static_cast<std::uint32_t>(i);
        choice.options = {SynonymousOption{0, 0.5}, SynonymousOption{1, 0.5}};
        longSeq.push_back(std::move(choice));
    }
    auto resLongEmbed = embed(longSeq, 42, 8, sharedKey, 0.5);
    CHECK(!resLongEmbed.success);

    std::vector<std::size_t> longForms(longSeq.size(), 0);
    auto resLongDecode = decode(longSeq, longForms, 8, sharedKey);
    CHECK(!resLongDecode.success);
}

TEST_CASE("Channel failureProbabilityBound handles NaN and negative s") {
    ChannelCapacity cap{.meanCapacityBits = 100.0, .variance = 10.0, .safePayloadBits = 50};
    CHECK(failureProbabilityBound(cap, 50, std::numeric_limits<double>::quiet_NaN()) == 1.0);
    CHECK(failureProbabilityBound(cap, 50, -1e12) == 1.0);
}

