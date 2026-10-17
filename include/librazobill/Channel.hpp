// =============================================================================
//  file : include/librazobill/Channel.hpp
//  role : steganographic and provenance channel over synonymous log forms
//  inv  : decode(embed(m)) == m or reports clean failure; zero payload corruption
// =============================================================================

// Steganographic and provenance channel over synonymous template choices.
// Implementation of Theorem 8 (Capacity) and Proposition 9 (Finite-Precision Bound)
// from "Latitude and Doubt" (M. M. Atta, 2026; see docs/explanation/architecture.md).
//
// In an unambiguous semantic source (Delta = 0), Latitude Lambda = H(X | W) operates
// as an information channel: the exact number of bits per symbol that can be embedded
// into form selection without altering the underlying meaning or token statistics.
//
// OPERATIONAL REGIME & IMPLEMENTATION LIMITS:
// Implements an arithmetic cumulative interval prototype over IEEE 754 64-bit float
// (53 bits of mantissa). Proposition 9 proves distortion-free decoding across long
// sequences requires precision P = ceil(sum_i max_x (-log2 Q(x | w_i))) + s + 1 bits.
// When P exceeds mantissa precision, interval arithmetic underflow is safely caught,
// returning success = false rather than producing corrupted data.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace razobill::channel {

/// candidate synonymous form choice for a given semantic meaning
struct SynonymousOption {
    std::size_t formIndex; // identifier of candidate form
    double probability;    // conditional probability P(form | meaning) > 0
};

/// stratum of synonymous template forms available for one semantic step
/// inv: options sorted by formIndex; sum of probabilities equals 1.0
struct MeaningChoice {
    std::uint32_t meaning;
    std::vector<SynonymousOption> options;
};

/// channel metrics for a meaning sequence under Theorem 8
struct ChannelCapacity {
    double meanCapacityBits = 0.0;   // mu = sum_i H(X | w_i)
    double variance = 0.0;           // sigma^2 = sum_i Var[-log2 P(X | w_i)]
    std::size_t safePayloadBits = 0; // k = floor(mu - 3*sigma - 7)
};

/// result container for message embedding into synonymous choices
struct EmbedResult {
    bool success = false;
    std::size_t payloadBits = 0;
    std::uint64_t embeddedMessage = 0;
    std::vector<std::size_t> chosenForms; // chosen form index per sequence step
};

/// result container for message decoding from observed form indices
struct DecodeResult {
    bool success = false;
    std::size_t payloadBits = 0;
    std::uint64_t message = 0;
};

/// calculates mean capacity mu, variance sigma^2, and safe payload k in bits
/// in: sequence of MeaningChoice strata
/// out: ChannelCapacity struct
/// thread: pure, reentrant, noexcept
[[nodiscard]] ChannelCapacity analyzeCapacity(std::span<const MeaningChoice> sequence) noexcept;

/// calculates finite-precision key length bound P in bits under Proposition 9
/// in: sequence strata, security parameter s (default 64)
/// out: required key length P in bits
/// thread: pure, reentrant, noexcept
[[nodiscard]] std::size_t requiredKeyBits(std::span<const MeaningChoice> sequence, std::size_t s = 64) noexcept;

/// calculates Chebyshev upper bound on failure probability for given payload and safety margin
/// in: capacity metrics, payloadBits, security margin s
/// out: failure probability bound delta
/// thread: pure, reentrant, noexcept
[[nodiscard]] double failureProbabilityBound(const ChannelCapacity& capacity, std::size_t payloadBits,
                                             double s = 64.0) noexcept;

/// calculates exact failure probability via distribution convolution across positions
/// in: sequence strata, payloadBits
/// out: exact analytical failure probability
/// thread: pure, reentrant, noexcept
[[nodiscard]] double exactFailureProbability(std::span<const MeaningChoice> sequence, std::size_t payloadBits) noexcept;

/// embeds integer message into synonymous form selections via shared key interval arithmetic
/// inv: preserves underlying token probability distribution exactly
/// in: sequence strata, message integer, payloadBits <= 64, sharedKey in [0.0, 1.0), dither
/// out: EmbedResult containing chosen form indices
/// err: returns success = false if interval precision is insufficient
[[nodiscard]] EmbedResult embed(std::span<const MeaningChoice> sequence, std::uint64_t message, std::size_t payloadBits,
                                double sharedKey, double dither = 0.5);

/// extracts embedded message from observed synonymous form sequence
/// inv: exact message restoration on success; returns success = false on boundary crossing
/// in: sequence strata, observedForms indices, payloadBits, sharedKey
/// out: DecodeResult with decoded integer payload
/// err: returns success = false on ambiguity; never returns corrupted message
[[nodiscard]] DecodeResult decode(std::span<const MeaningChoice> sequence, std::span<const std::size_t> observedForms,
                                  std::size_t payloadBits, double sharedKey);

} // namespace razobill::channel
