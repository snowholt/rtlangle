#include "dsp/am_demodulator.h"

#include "dsp/fir_design.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace rtlangle::dsp {
namespace {

double single_pole_coefficient(double cutoff_hz, double sample_rate_hz) {
  if (sample_rate_hz <= 0.0) return 0.0;
  return std::exp(-2.0 * std::numbers::pi_v<double> * cutoff_hz / sample_rate_hz);
}

}  // namespace

AmDemodulator::AmDemodulator(std::uint32_t channel_rate_hz) {
  const double fs = static_cast<double>(channel_rate_hz);
  dc_block_a_ = single_pole_coefficient(25.0, fs);
  hp300_a_ = single_pole_coefficient(300.0, fs);
  audio_taps_ = design_lowpass(fs, 3400.0, 4000.0, 60.0);
  if (audio_taps_.empty()) audio_taps_.push_back(1.0F);
  audio_fir_taps_ = audio_taps_.size();
  ring_.assign(audio_fir_taps_, 0.0F);
}

void AmDemodulator::reset() {
  dc_x1_ = dc_y1_ = 0.0;
  hp_x1_ = hp_y1_ = 0.0;
  std::fill(ring_.begin(), ring_.end(), 0.0F);
  pos_ = 0;
}

void AmDemodulator::process(std::span<const std::complex<float>> in,
                            std::vector<float>& audio_out) {
  const std::size_t n = audio_fir_taps_;
  if (n == 0) return;

  for (const std::complex<float>& x : in) {
    // Envelope.
    const double env = std::hypot(static_cast<double>(x.real()), static_cast<double>(x.imag()));

    // 25 Hz single-pole high-pass: removes the carrier's DC term.
    const double dc_y = dc_block_a_ * (dc_y1_ + env - dc_x1_);
    dc_x1_ = env;
    dc_y1_ = dc_y;

    // 3400 Hz FIR low-pass.
    ring_[pos_] = static_cast<float>(dc_y);
    pos_ = (pos_ + 1 == n) ? 0 : pos_ + 1;
    float acc = 0.0F;
    std::size_t idx = (pos_ == 0) ? n - 1 : pos_ - 1;
    for (std::size_t k = 0; k < n; ++k) {
      acc += audio_taps_[k] * ring_[idx];
      idx = (idx == 0) ? n - 1 : idx - 1;
    }

    // 300 Hz single-pole high-pass.
    const double hp_y = hp300_a_ * (hp_y1_ + static_cast<double>(acc) - hp_x1_);
    hp_x1_ = static_cast<double>(acc);
    hp_y1_ = hp_y;

    audio_out.push_back(static_cast<float>(hp_y));
  }
}

}  // namespace rtlangle::dsp
