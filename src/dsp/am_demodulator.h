#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "dsp/fir_decimator.h"

namespace rtlangle::dsp {

// Envelope detection followed by the audio band shaping of spec section 8.2:
//
//   |x|  ->  25 Hz single-pole high-pass (removes the carrier DC term)
//        ->  3400 Hz FIR low-pass, 4000 Hz stopband, 60 dB
//        ->  300 Hz single-pole high-pass
//
// Only the FIR contributes to latency_samples(). The two single-pole sections
// are not group-delay compensated; their residual is absorbed by the guard
// interval of spec section 8.4, and a test asserts the residual is smaller than
// the guard.
class AmDemodulator {
 public:
  AmDemodulator() = default;
  explicit AmDemodulator(std::uint32_t channel_rate_hz);

  void process(std::span<const std::complex<float>> in, std::vector<float>& audio_out);

  // The audio low-pass FIR group delay, in channel samples. Exact: the filter
  // does not decimate and its tap count is odd.
  std::size_t latency_samples() const { return audio_fir_taps_ > 0 ? (audio_fir_taps_ - 1) / 2 : 0; }

  void reset();

 private:
  double dc_block_a_ = 0.0;   // 25 Hz
  double hp300_a_ = 0.0;      // 300 Hz
  double dc_x1_ = 0.0, dc_y1_ = 0.0;
  double hp_x1_ = 0.0, hp_y1_ = 0.0;

  std::vector<float> audio_taps_;
  std::size_t        audio_fir_taps_ = 0;
  std::vector<float> ring_;
  std::size_t        pos_ = 0;
};

}  // namespace rtlangle::dsp
