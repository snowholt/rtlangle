#pragma once

#include "core/config.h"
#include "dsp/am_demodulator.h"
#include "dsp/fir_decimator.h"
#include "dsp/offset_mixer.h"

#include <complex>
#include <cstdint>
#include <span>
#include <vector>

namespace rtlangle::dsp {

// Deterministic factorisation of sample_rate/channel_rate into stage factors.
//
// The rule: repeatedly take the largest divisor in [2, 8]; when none divides
// the remainder, take the largest divisor in [2, 16]. Returns an empty vector
// when no such factorisation exists, which spec section 7.1 makes a
// configuration error rather than a runtime surprise. 1024000 -> 32000 gives
// {8, 4}; 1024000 -> 64000 gives {8, 2}; a ratio of 17 gives {}.
std::vector<int> decimation_factors(std::uint32_t sample_rate_hz,
                                    std::uint32_t channel_rate_hz);

// The chain latency referred to the channel rate is a RATIONAL number (spec
// section 8.4). It is kept exact and the caller chooses how to round, because
// truncating it silently is what made an impulse-response test disagree with a
// formula.
struct Latency {
  std::int64_t num = 0;
  std::int64_t den = 1;          // exact latency in channel samples = num/den
  std::size_t  ceil_samples = 0; // ceil(num/den); the transient trim uses this
  double       residual = 0.0;   // ceil_samples - num/den, in [0,1)
};

// The receive chain of spec section 8, from raw normalised IQ at the device
// sample rate to channel-rate complex samples and a channel-rate audio band.
struct Chain {
  OffsetMixer               mixer{1, 0};
  std::vector<FirDecimator> stages;
  ChannelFilter             channel;
  AmDemodulator             audio;
  std::uint32_t             channel_rate_hz = 0;

  Latency     latency() const;
  std::size_t latency_samples() const { return latency().ceil_samples; }

  // The total FIR length of the chain expressed at the channel rate, which the
  // transient guard of spec section 6.3 rule 4 is twice.
  std::size_t total_taps_at_channel_rate() const;

  // Appends to both outputs; the caller clears them. Returns the number of
  // channel samples produced by this call.
  std::size_t process(std::span<const std::complex<float>> raw,
                      std::vector<std::complex<float>>& channel_out,
                      std::vector<float>& audio_out);

  void reset();
};

// Builds the chain from a validated Config. A configuration whose decimation
// ratio has no factorisation yields a chain with no stages, which validate()
// has already refused.
Chain build_chain(const Config&);

}  // namespace rtlangle::dsp
