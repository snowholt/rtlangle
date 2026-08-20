#include "dsp/fir_decimator.h"

namespace rtlangle::dsp {

FirDecimator::FirDecimator(std::vector<float> taps, int factor)
    : taps_(std::move(taps)), factor_(factor < 1 ? 1 : factor) {
  if (taps_.empty()) taps_.push_back(1.0F);
  ring_.assign(taps_.size(), std::complex<float>(0.0F, 0.0F));
  // Output k is the filtered input at index k*factor, the standard decimation
  // convention. Emitting at index k*factor + (factor-1) instead would shift the
  // whole output grid by (factor-1) input samples, and the chain latency of
  // spec section 8.4 - which sums filter delays only - would then disagree with
  // the measured impulse peak by nearly one channel sample per cascade.
  phase_ = factor_ - 1;
}

void FirDecimator::reset() {
  std::fill(ring_.begin(), ring_.end(), std::complex<float>(0.0F, 0.0F));
  pos_ = 0;
  phase_ = factor_ - 1;
}

std::size_t FirDecimator::group_delay_input_samples() const {
  return (taps_.size() - 1) / 2;
}

std::size_t FirDecimator::process(std::span<const std::complex<float>> in,
                                  std::vector<std::complex<float>>& out) {
  const std::size_t n = taps_.size();
  std::size_t produced = 0;

  for (const std::complex<float>& x : in) {
    ring_[pos_] = x;
    pos_ = (pos_ + 1 == n) ? 0 : pos_ + 1;

    if (++phase_ < factor_) continue;
    phase_ = 0;

    // The newest sample sits one before pos_; taps_[0] multiplies it.
    float re = 0.0F;
    float im = 0.0F;
    std::size_t idx = (pos_ == 0) ? n - 1 : pos_ - 1;
    for (std::size_t k = 0; k < n; ++k) {
      const float t = taps_[k];
      re += t * ring_[idx].real();
      im += t * ring_[idx].imag();
      idx = (idx == 0) ? n - 1 : idx - 1;
    }
    out.emplace_back(re, im);
    ++produced;
  }
  return produced;
}

}  // namespace rtlangle::dsp
