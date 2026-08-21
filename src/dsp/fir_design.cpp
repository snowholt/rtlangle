#include "dsp/fir_design.h"

#include <cmath>
#include <numbers>

namespace rtlangle::dsp {
namespace {

// Zeroth-order modified Bessel function of the first kind, by its series. The
// series converges quickly for the arguments a Kaiser window uses (beta is
// under 9 for 100 dB of attenuation).
double bessel_i0(double x) {
  double sum = 1.0;
  double term = 1.0;
  for (int k = 1; k < 64; ++k) {
    term *= (x / (2.0 * k)) * (x / (2.0 * k));
    sum += term;
    if (term < 1e-16 * sum) break;
  }
  return sum;
}

// The worst response anywhere in the stopband, in dB, by direct evaluation of
// the DTFT. Used to verify a design against its own specification.
double worst_stopband_db(const std::vector<double>& h, double sample_rate_hz,
                         double stopband_edge_hz) {
  constexpr int kPoints = 512;
  double worst = -1e30;
  for (int i = 0; i <= kPoints; ++i) {
    const double f = stopband_edge_hz +
                     (sample_rate_hz / 2.0 - stopband_edge_hz) * static_cast<double>(i) /
                         static_cast<double>(kPoints);
    double re = 0.0;
    double im = 0.0;
    for (std::size_t n = 0; n < h.size(); ++n) {
      const double w = -2.0 * std::numbers::pi_v<double> * f * static_cast<double>(n) /
                       sample_rate_hz;
      re += h[n] * std::cos(w);
      im += h[n] * std::sin(w);
    }
    const double db = 20.0 * std::log10(std::max(std::hypot(re, im), 1e-300));
    worst = std::max(worst, db);
  }
  return worst;
}

double sinc(double x) {
  if (std::fabs(x) < 1e-12) return 1.0;
  const double pix = std::numbers::pi_v<double> * x;
  return std::sin(pix) / pix;
}

}  // namespace

double kaiser_beta(double a) {
  if (a > 50.0) return 0.1102 * (a - 8.7);
  if (a >= 21.0) return 0.5842 * std::pow(a - 21.0, 0.4) + 0.07886 * (a - 21.0);
  return 0.0;
}

std::vector<float> design_lowpass(double sample_rate_hz, double passband_edge_hz,
                                  double stopband_edge_hz, double stopband_atten_db) {
  std::vector<float> taps;
  if (!std::isfinite(sample_rate_hz) || !std::isfinite(passband_edge_hz) ||
      !std::isfinite(stopband_edge_hz) || !std::isfinite(stopband_atten_db)) {
    return taps;
  }
  if (sample_rate_hz <= 0.0 || passband_edge_hz <= 0.0) return taps;
  if (stopband_edge_hz <= passband_edge_hz) return taps;
  if (stopband_edge_hz >= sample_rate_hz / 2.0) return taps;
  if (stopband_atten_db <= 0.0) return taps;

  const double transition = (stopband_edge_hz - passband_edge_hz) / sample_rate_hz;  // cycles/sample
  const double beta = kaiser_beta(stopband_atten_db);

  // Kaiser's order estimate is asymptotic, and at the very start of the
  // stopband it lands a fraction of a decibel short of the requested
  // attenuation: measured at -59.91 dB for a 60 dB request at the 4000/5000 Hz
  // channel edges and -59.86 dB at the 3400/4000 Hz audio edges. A design that
  // misses its own stated specification is discovered later as a failing
  // response test and invites loosening the assertion, so the length is grown
  // until the design MEETS the specification, verified against its own
  // response. The loop terminates because attenuation increases monotonically
  // with length at a fixed beta.
  const double order = (stopband_atten_db - 8.0) /
                       (2.285 * 2.0 * std::numbers::pi_v<double> * transition);
  std::size_t length = static_cast<std::size_t>(std::ceil(order)) + 3;
  if (length % 2 == 0) ++length;   // odd, for an integer group delay
  if (length < 3) length = 3;

  const double cutoff = (passband_edge_hz + stopband_edge_hz) / 2.0 / sample_rate_hz;
  const double denom = bessel_i0(beta);
  std::vector<double> h;

  for (int iteration = 0; iteration < 64; ++iteration) {
    if (length > 65535) return {};   // the transition width is unrealisably narrow

    const double m = static_cast<double>(length - 1);
    h.assign(length, 0.0);
    double sum = 0.0;
    for (std::size_t n = 0; n < length; ++n) {
      const double d = static_cast<double>(n) - m / 2.0;
      const double r = 2.0 * static_cast<double>(n) / m - 1.0;   // -1 .. +1
      const double w = bessel_i0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / denom;
      h[n] = 2.0 * cutoff * sinc(2.0 * cutoff * d) * w;
      sum += h[n];
    }
    if (!(std::fabs(sum) > 0.0)) return {};
    for (double& v : h) v /= sum;   // unity gain at DC

    if (worst_stopband_db(h, sample_rate_hz, stopband_edge_hz) <= -stopband_atten_db) break;
    length += 4;
  }

  taps.resize(h.size());
  for (std::size_t n = 0; n < h.size(); ++n) taps[n] = static_cast<float>(h[n]);
  return taps;
}

}  // namespace rtlangle::dsp
