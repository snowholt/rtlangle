#include "dsp/spectrum.h"

#include "core/statistics.h"

#include <algorithm>
#include <cmath>
#include <fftw3.h>
#include <numbers>

namespace rtlangle::dsp {

Spectrum::Spectrum(std::size_t fft_size, std::size_t segments)
    : fft_size_(fft_size == 0 ? 1 : fft_size), segments_(segments == 0 ? 1 : segments) {
  in_.assign(fft_size_, std::complex<float>(0.0F, 0.0F));
  out_.assign(fft_size_, std::complex<float>(0.0F, 0.0F));

  // Hann window. Its coherent power gain normalises the periodogram so that a
  // full-scale tone reads the same power as it would unwindowed.
  window_.resize(fft_size_);
  double p = 0.0;
  for (std::size_t n = 0; n < fft_size_; ++n) {
    const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi_v<double> *
                                          static_cast<double>(n) /
                                          static_cast<double>(fft_size_));
    window_[n] = static_cast<float>(w);
    p += w * w;
  }
  window_power_ = p / static_cast<double>(fft_size_);
  if (!(window_power_ > 0.0)) window_power_ = 1.0;

  plan_ = reinterpret_cast<fftwf_plan_s*>(
      fftwf_plan_dft_1d(static_cast<int>(fft_size_),
                        reinterpret_cast<fftwf_complex*>(in_.data()),
                        reinterpret_cast<fftwf_complex*>(out_.data()), FFTW_FORWARD,
                        FFTW_ESTIMATE));
}

Spectrum::~Spectrum() {
  if (plan_ != nullptr) fftwf_destroy_plan(reinterpret_cast<fftwf_plan>(plan_));
}

std::vector<double> Spectrum::welch_psd(std::span<const std::complex<float>> x) {
  std::vector<double> psd;
  if (plan_ == nullptr || x.size() < fft_size_) return psd;

  // Fifty percent overlap, as many segments as the caller asked for or as the
  // input allows, whichever is smaller.
  const std::size_t hop = std::max<std::size_t>(1, fft_size_ / 2);
  const std::size_t available = 1 + (x.size() - fft_size_) / hop;
  const std::size_t used = std::min(available, segments_);

  std::vector<double> acc(fft_size_, 0.0);
  for (std::size_t s = 0; s < used; ++s) {
    const std::size_t base = s * hop;
    for (std::size_t n = 0; n < fft_size_; ++n) {
      in_[n] = x[base + n] * window_[n];
    }
    fftwf_execute(reinterpret_cast<fftwf_plan>(plan_));
    const double norm = 1.0 / (static_cast<double>(fft_size_) * static_cast<double>(fft_size_) *
                               window_power_);
    for (std::size_t k = 0; k < fft_size_; ++k) {
      const double re = out_[k].real();
      const double im = out_[k].imag();
      acc[k] += (re * re + im * im) * norm;
    }
  }
  const double inv = 1.0 / static_cast<double>(used);
  for (double& v : acc) v *= inv;

  // Shift so bin 0 is -fs/2 and bin fft_size/2 is DC.
  psd.resize(fft_size_);
  const std::size_t half = fft_size_ / 2;
  for (std::size_t k = 0; k < fft_size_; ++k) {
    psd[(k + half) % fft_size_] = acc[k];
  }
  return psd;
}

double Spectrum::peak_prominence_db(std::span<const double> psd, double half_band_bins) const {
  if (psd.empty()) return 0.0;
  const std::size_t dc = psd.size() / 2;
  const std::size_t half = static_cast<std::size_t>(
      std::clamp(half_band_bins, 0.0, static_cast<double>(psd.size())));
  const std::size_t lo = (half >= dc) ? 0 : dc - half;
  const std::size_t hi = std::min(psd.size() - 1, dc + half);

  double peak = 0.0;
  for (std::size_t k = lo; k <= hi; ++k) peak = std::max(peak, psd[k]);

  const Stat med = median(psd);
  if (!med.valid || !(med.value > 0.0) || !(peak > 0.0)) return 0.0;
  return 10.0 * std::log10(peak / med.value);
}

double Spectrum::carrier_offset_hz(std::span<const double> psd, double bin_hz,
                                   double search_half_bw_hz) const {
  if (psd.size() < 3) return 0.0;
  const std::size_t dc = psd.size() / 2;

  std::size_t lo = 1;
  std::size_t hi = psd.size() - 2;
  if (search_half_bw_hz > 0.0 && bin_hz > 0.0) {
    const auto half = static_cast<std::size_t>(std::floor(search_half_bw_hz / bin_hz));
    lo = std::max<std::size_t>(1, dc > half ? dc - half : 1);
    hi = std::min<std::size_t>(psd.size() - 2, dc + half);
  }
  if (lo > hi) return 0.0;

  std::size_t peak = lo;
  for (std::size_t k = lo; k <= hi; ++k) {
    if (psd[k] > psd[peak]) peak = k;
  }

  // Parabolic interpolation on the log magnitudes, which is what makes the
  // estimate accurate to a fraction of a bin.
  const double ym1 = std::log(std::max(psd[peak - 1], 1e-300));
  const double y0 = std::log(std::max(psd[peak], 1e-300));
  const double yp1 = std::log(std::max(psd[peak + 1], 1e-300));
  const double denom = ym1 - 2.0 * y0 + yp1;
  double delta = 0.0;
  if (std::fabs(denom) > 1e-300) delta = 0.5 * (ym1 - yp1) / denom;
  delta = std::clamp(delta, -0.5, 0.5);

  return (static_cast<double>(peak) + delta - static_cast<double>(dc)) * bin_hz;
}

}  // namespace rtlangle::dsp
