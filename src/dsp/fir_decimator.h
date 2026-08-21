#pragma once

#include <complex>
#include <cstddef>
#include <span>
#include <vector>

namespace rtlangle::dsp {

// A decimating FIR. State is carried between calls, and the output is
// bit-identical whether the input arrives in one block or in arbitrary smaller
// chunks - a normative property (spec section 8.2), because the capture loop
// delivers whatever the device buffer yields.
class FirDecimator {
 public:
  FirDecimator() = default;
  FirDecimator(std::vector<float> taps, int factor);

  // Appends this call's output to `out`; the caller clears it.
  std::size_t process(std::span<const std::complex<float>> in,
                      std::vector<std::complex<float>>& out);

  // The EXACT delay in samples at this filter's own INPUT rate. It is always an
  // integer because the tap count is odd.
  //
  // There is deliberately no group delay expressed in output samples: the old
  // (taps-1)/(2*factor) in integer arithmetic silently truncated - 131 taps at
  // factor 8 is 8.125 output samples, not 8. Referring stage delays to a common
  // rate is Chain's job, and Chain keeps the result rational (spec section 8.4).
  std::size_t group_delay_input_samples() const;

  int factor() const { return factor_; }
  std::size_t taps() const { return taps_.size(); }
  void reset();

 private:
  std::vector<float>               taps_;
  int                              factor_ = 1;
  std::vector<std::complex<float>> ring_;
  std::size_t                      pos_ = 0;
  int                              phase_ = 0;
};

// The channel-rate low-pass. Same shape, no decimation.
class ChannelFilter {
 public:
  ChannelFilter() = default;
  explicit ChannelFilter(std::vector<float> taps) : fir_(std::move(taps), 1) {}

  std::size_t process(std::span<const std::complex<float>> in,
                      std::vector<std::complex<float>>& out) {
    return fir_.process(in, out);
  }
  std::size_t group_delay_input_samples() const { return fir_.group_delay_input_samples(); }
  std::size_t taps() const { return fir_.taps(); }
  void reset() { fir_.reset(); }

 private:
  FirDecimator fir_;
};

}  // namespace rtlangle::dsp
