#include "source/synthetic_source.h"

#include "core/db.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace rtlangle {
namespace {

constexpr double kPi = std::numbers::pi_v<double>;

// A voice-like modulating envelope: three incommensurate tones inside the
// 300-3400 Hz audio band, so the demodulated result occupies the band the audio
// filter passes rather than being a single line the framing could resolve.
double voice(double t) {
  return (std::sin(2.0 * kPi * 430.0 * t) + 0.7 * std::sin(2.0 * kPi * 1130.0 * t) +
          0.4 * std::sin(2.0 * kPi * 2270.0 * t)) /
         2.1;
}

}  // namespace

SyntheticSource::SyntheticSource(SyntheticParams params)
    : params_(std::move(params)),
      snr_db_(params_.snr_db),
      pending_snr_db_(params_.snr_db),
      center_hz_(params_.center_hz),
      rng_(params_.seed) {
  if (params_.sample_rate_hz == 0) params_.sample_rate_hz = 1;
  if (params_.talk_s <= 0.0) params_.talk_s = 1.0;
  if (params_.duty > 0.0 && params_.duty < 1.0) {
    params_.gap_s = params_.talk_s * (1.0 / params_.duty - 1.0);
  }
  if (params_.gap_s < 0.0) params_.gap_s = 0.0;
}

void SyntheticSource::set_snr_db(double snr_db) { pending_snr_db_ = snr_db; }

SourceInfo SyntheticSource::info() const {
  SourceInfo i;
  i.driver = "synthetic";
  i.device_name = "synthetic AM airband generator";
  i.serial = "synthetic";
  i.requested_sample_rate_hz = params_.sample_rate_hz;
  i.applied_sample_rate_hz = params_.sample_rate_hz;
  const std::uint32_t tuned = static_cast<std::uint32_t>(
      static_cast<std::int64_t>(center_hz_) + params_.offset_hz);
  i.requested_center_hz = tuned;
  i.applied_center_hz = tuned;
  i.requested_gain_tenth_db = params_.gain_tenth_db;
  i.applied_gain_tenth_db = params_.gain_tenth_db;
  i.agc_enabled = false;
  i.ppm = params_.ppm;
  i.applied_offset_hz = params_.offset_hz;
  return i;
}

bool SyntheticSource::burst_active(double t) const {
  const double period = params_.talk_s + params_.gap_s;
  if (!(period > 0.0)) return true;
  return std::fmod(t, period) < params_.talk_s;
}

std::complex<float> SyntheticSource::generate_one() {
  const double fs = static_cast<double>(params_.sample_rate_hz);
  const double t = static_cast<double>(sample_index_) / fs;
  ++sample_index_;

  // Noise power is fixed; the signal amplitude carries the SNR, so changing the
  // SNR does not change the noise reference the estimator measures.
  constexpr double kNoiseSigma = 0.02;
  const double noise_power = 2.0 * kNoiseSigma * kNoiseSigma;   // I and Q
  double re = kNoiseSigma * noise_(rng_);
  double im = kNoiseSigma * noise_(rng_);

  const double signal_power = noise_power * from_db(snr_db_);
  const double amplitude = std::sqrt(signal_power);

  if (params_.band_transmitters.empty()) {
    if (burst_active(t)) {
      // AM: carrier plus sidebands, at baseband -offset so the mixer lifts it
      // to DC. Modulation depth 0.6.
      const double env = amplitude * (1.0 + 0.6 * voice(t));
      const double ph = -2.0 * kPi * static_cast<double>(params_.offset_hz) * t;
      re += env * std::cos(ph);
      im += env * std::sin(ph);
    }
  } else {
    for (const auto& [freq_hz, duty] : params_.band_transmitters) {
      const double period = params_.talk_s + params_.gap_s;
      const double on = (period > 0.0) ? period * std::clamp(duty, 0.0, 1.0) : 0.0;
      // Each transmitter keys on a phase of the shared cycle offset by its own
      // frequency, so they are not all active at once.
      const double phase_offset =
          period * static_cast<double>(freq_hz % 1000) / 1000.0;
      const double local = (period > 0.0) ? std::fmod(t + phase_offset, period) : 0.0;
      if (local >= on) continue;
      const double baseband =
          static_cast<double>(static_cast<std::int64_t>(freq_hz) -
                              (static_cast<std::int64_t>(center_hz_) + params_.offset_hz));
      if (std::fabs(baseband) > 0.5 * fs) continue;
      const double env = amplitude * (1.0 + 0.6 * voice(t));
      const double ph = 2.0 * kPi * baseband * t;
      re += env * std::cos(ph);
      im += env * std::sin(ph);
    }
  }
  return {static_cast<float>(re), static_cast<float>(im)};
}

ReadResult SyntheticSource::read(std::span<std::complex<float>> out,
                                 std::chrono::steady_clock::time_point deadline) {
  ReadResult r;
  if (cancelled_) {
    r.status = ReadStatus::Cancelled;
    return r;
  }
  if (std::chrono::steady_clock::now() >= deadline) {
    r.status = ReadStatus::Timeout;
    return r;
  }
  for (std::complex<float>& s : out) {
    s = generate_one();
    ++r.samples;
  }
  return r;
}

void SyntheticSource::flush() {
  // A generator has nothing buffered. The flush boundary is where a pending SNR
  // change takes effect, so a capture is never generated at two SNRs.
  snr_db_ = pending_snr_db_;
}

void SyntheticSource::cancel() { cancelled_ = true; }

double SyntheticSource::clipped_fraction() const {
  // There is no ADC and no rail to reach: the generator works in normalised
  // floating point throughout.
  return 0.0;
}

bool SyntheticSource::retune(std::uint32_t center_hz, std::string& error) {
  error.clear();
  center_hz_ = center_hz;
  flush();
  return true;
}

}  // namespace rtlangle
