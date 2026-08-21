#pragma once

// Signal construction and reference measurement for the WP5 measurement tests.
//
// Every reference is derived BY MEASUREMENT from separated signal-only and
// noise-only streams rather than from the nominal input value. Asserting
// against the nominal value would put a test inside the estimator's own bias
// budget: the 20th-percentile floor reads about 0.12 dB low for 1024-sample
// frames, which biases the reported SNR high by the same amount before anything
// is actually wrong.

#include "core/config.h"

#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <random>
#include <span>
#include <vector>

namespace rtlangle::test {

inline constexpr double kPi = std::numbers::pi_v<double>;

template <typename T>
double mean_power_over(std::span<const T> x) {
  if (x.empty()) return 0.0;
  double sum = 0.0;
  for (const T& v : x) {
    if constexpr (std::is_same_v<T, std::complex<float>>) {
      sum += std::norm(v);
    } else {
      const double s = static_cast<double>(v);
      sum += s * s;
    }
  }
  return sum / static_cast<double>(x.size());
}

inline double mean_power(const std::vector<std::complex<float>>& x) {
  return mean_power_over<std::complex<float>>(x);
}
inline double mean_power(const std::vector<float>& x) { return mean_power_over<float>(x); }

// Complex additive white Gaussian noise of a known per-sample power.
inline std::vector<std::complex<float>> awgn(std::size_t n, double power, std::uint64_t seed) {
  const double sigma = std::sqrt(power / 2.0);   // split between I and Q
  std::mt19937_64 rng(seed);
  std::normal_distribution<double> g(0.0, sigma);
  std::vector<std::complex<float>> v(n);
  for (auto& x : v) x = {static_cast<float>(g(rng)), static_cast<float>(g(rng))};
  return v;
}

// A complex AM carrier at `carrier_hz`, tone-modulated at `tone_hz` to a known
// depth, active only inside the given sample ranges.
struct Burst {
  std::size_t first = 0;
  std::size_t last = 0;   // exclusive
};

inline std::vector<std::complex<float>> am_signal(std::size_t n, double sample_rate_hz,
                                                  double carrier_hz, double tone_hz,
                                                  double depth, double amplitude,
                                                  std::span<const Burst> bursts) {
  std::vector<std::complex<float>> v(n, std::complex<float>(0.0F, 0.0F));
  for (const Burst& b : bursts) {
    const std::size_t last = std::min(b.last, n);
    for (std::size_t k = b.first; k < last; ++k) {
      const double t = static_cast<double>(k) / sample_rate_hz;
      const double env = amplitude * (1.0 + depth * std::sin(2.0 * kPi * tone_hz * t));
      const double ph = 2.0 * kPi * carrier_hz * t;
      v[k] = {static_cast<float>(env * std::cos(ph)), static_cast<float>(env * std::sin(ph))};
    }
  }
  return v;
}

inline std::vector<std::complex<float>> add(const std::vector<std::complex<float>>& a,
                                            const std::vector<std::complex<float>>& b) {
  std::vector<std::complex<float>> out(std::max(a.size(), b.size()),
                                       std::complex<float>(0.0F, 0.0F));
  for (std::size_t i = 0; i < a.size(); ++i) out[i] += a[i];
  for (std::size_t i = 0; i < b.size(); ++i) out[i] += b[i];
  return out;
}

// The test-only squelch of spec section 14.2. At a true SNR of 0 dB the TOTAL
// in-channel power is only 10*log10(2) = 3.01 dB above the noise floor, which
// is below the production default open_db of 6.0, so no event is ever detected
// and there is no median to assert on. These thresholds are legal under the
// section 7.3 bounds and sit roughly 6.7 standard deviations above the mean
// noise frame power for 1024-sample frames, so false openings are negligible.
inline Config with_test_squelch(Config c) {
  c.open_db = 1.0;
  c.close_db = 0.5;
  return c;
}

}  // namespace rtlangle::test
