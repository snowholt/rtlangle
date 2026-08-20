#include "metrics/noise_floor.h"

#include "core/db.h"
#include "core/statistics.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace rtlangle::metrics {
namespace {

// Segments per analysis window for the persistent-carrier test. Averaging is
// required rather than optional: an unaveraged periodogram over 1024 bins
// already has a peak-to-median ratio of about 10 dB on pure noise, which is the
// same order as the threshold that has to tell a carrier from noise.
constexpr std::size_t kCarrierSegments = 16;
constexpr std::size_t kCarrierFftSize = 1024;

// The fraction of analysis windows carrying a prominent peak within the channel
// bandwidth.
double carrier_presence(std::span<const std::complex<float>> stream, const Config& cfg,
                        dsp::Spectrum& spectrum) {
  if (stream.empty()) return 0.0;
  const std::size_t fft = spectrum.fft_size();
  if (fft < 4) return 0.0;
  const std::size_t hop = fft / 2;
  const std::size_t window_samples = fft + (kCarrierSegments - 1) * hop;
  if (stream.size() < window_samples) return 0.0;

  const double bin_hz = static_cast<double>(cfg.channel_rate_hz) / static_cast<double>(fft);
  const double half_band_bins =
      (bin_hz > 0.0) ? (static_cast<double>(cfg.channel_bw_hz) / 2.0) / bin_hz : 0.0;

  std::size_t windows = 0;
  std::size_t present = 0;
  for (std::size_t base = 0; base + window_samples <= stream.size(); base += window_samples) {
    const auto psd = spectrum.welch_psd(stream.subspan(base, window_samples));
    if (psd.empty()) continue;
    ++windows;
    if (spectrum.peak_prominence_db(psd, half_band_bins) > cfg.carrier_prominence_db) {
      ++present;
    }
  }
  if (windows == 0) return 0.0;
  return static_cast<double>(present) / static_cast<double>(windows);
}

NoiseFloor estimate_impl(std::span<const double> frame_powers,
                         std::span<const std::complex<float>> stream, const Config& cfg,
                         dsp::Spectrum& spectrum) {
  NoiseFloor nf;
  if (frame_powers.empty()) {
    nf.status = FloorStatus::Unreliable;
    nf.detail = "the capture produced no analysis frames.";
    return nf;
  }

  // Stage one: the probe floor, at a percentile low enough to sit under the
  // traffic, used only to judge occupancy.
  const Stat probe = percentile(frame_powers, cfg.probe_percentile);
  if (!probe.valid || !(probe.value > 0.0)) {
    nf.status = FloorStatus::Unreliable;
    nf.detail = "the probe percentile of the frame powers is not a positive value.";
    return nf;
  }
  nf.probe_power = probe.value;

  const double open_linear = from_db(cfg.open_db);
  std::size_t active = 0;
  for (double p : frame_powers) {
    if (p > probe.value * open_linear) ++active;
  }
  nf.active_probe_fraction =
      static_cast<double>(active) / static_cast<double>(frame_powers.size());

  const Stat p95 = percentile(frame_powers, 95.0);
  nf.dynamic_range_db = (p95.valid && p95.value > 0.0) ? to_db(p95.value / probe.value) : 0.0;

  // R1.
  if (nf.active_probe_fraction > cfg.max_active_fraction) {
    nf.status = FloorStatus::Unreliable;
    nf.detail = "the probe occupancy is " + std::to_string(nf.active_probe_fraction) +
                ", above the limit of " + std::to_string(cfg.max_active_fraction) +
                ", so no part of the capture can serve as a quiet reference.";
    return nf;
  }

  // R2. Both conditions are required: a flat power series alone is what pure
  // noise looks like, and pure noise is a perfectly good floor.
  nf.carrier_persistence = carrier_presence(stream, cfg, spectrum);
  if (nf.dynamic_range_db < cfg.open_db && nf.carrier_persistence >= cfg.carrier_persistence) {
    nf.status = FloorStatus::Unidentifiable;
    nf.detail = "the channel carries a persistent carrier - present in " +
                std::to_string(nf.carrier_persistence) +
                " of the analysis windows - with a dynamic range of only " +
                std::to_string(nf.dynamic_range_db) +
                " dB, so there is no quiet reference anywhere in the capture.";
    return nf;
  }

  // R3.
  const Stat floor = percentile(frame_powers, cfg.noise_percentile);
  if (!floor.valid || !(floor.value > 0.0)) {
    nf.status = FloorStatus::Unreliable;
    nf.detail = "the noise percentile of the frame powers is not a positive value.";
    return nf;
  }
  nf.power = floor.value;
  nf.status = FloorStatus::Reliable;
  return nf;
}

}  // namespace

std::string_view to_string(FloorStatus s) {
  switch (s) {
    case FloorStatus::Reliable: return "reliable";
    case FloorStatus::Unreliable: return "unreliable";
    case FloorStatus::Unidentifiable: return "unidentifiable";
  }
  return "reliable";
}

NoiseFloor estimate_noise_floor(std::span<const double> frame_powers,
                                std::span<const std::complex<float>> stream,
                                const Config& cfg, dsp::Spectrum& spectrum) {
  return estimate_impl(frame_powers, stream, cfg, spectrum);
}

NoiseFloor estimate_noise_floor(std::span<const double> frame_powers,
                                std::span<const float> stream, const Config& cfg,
                                dsp::Spectrum& spectrum) {
  // The audio floor obeys the same two-stage check, so the real stream is
  // presented to the identical code path rather than to a parallel one that
  // could drift from it.
  std::vector<std::complex<float>> complex_stream;
  complex_stream.reserve(stream.size());
  for (float v : stream) complex_stream.emplace_back(v, 0.0F);
  return estimate_impl(frame_powers, complex_stream, cfg, spectrum);
}

// The carrier FFT geometry, exposed so callers construct a Spectrum that
// matches what the estimator expects.
static_assert(kCarrierFftSize == 1024, "the analysis window geometry is fixed");

}  // namespace rtlangle::metrics
