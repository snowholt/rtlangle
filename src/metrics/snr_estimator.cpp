#include "metrics/snr_estimator.h"

#include "core/db.h"
#include "core/statistics.h"
#include "dsp/framer.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <vector>

namespace rtlangle::metrics {
namespace {

std::string format_utc(std::chrono::system_clock::time_point tp) {
  const std::time_t t = std::chrono::system_clock::to_time_t(tp);
  std::tm tm{};
  ::gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

Percentiles percentiles_of(const std::vector<double>& xs) {
  Percentiles p;
  p.median = median(xs).value;
  p.p10 = percentile(xs, 10.0).value;
  p.p25 = percentile(xs, 25.0).value;
  p.p75 = percentile(xs, 75.0).value;
  p.p90 = percentile(xs, 90.0).value;
  return p;
}

// The median frame power over an inclusive frame range.
std::optional<double> median_power(std::span<const double> powers, std::size_t first,
                                   std::size_t last) {
  if (powers.empty() || first > last || last >= powers.size()) return std::nullopt;
  std::vector<double> v(powers.begin() + static_cast<std::ptrdiff_t>(first),
                        powers.begin() + static_cast<std::ptrdiff_t>(last) + 1);
  const Stat m = median(v);
  if (!m.valid) return std::nullopt;
  return m.value;
}

}  // namespace

void estimate_snr(const CaptureOutcome& capture, const NoiseFloor& channel_floor,
                  const NoiseFloor& audio_floor, std::span<const Event> events,
                  const Config& cfg, AttemptRecord& out, dsp::Spectrum* spectrum) {
  const auto channel_powers =
      dsp::frame_powers(capture.channel, dsp::kFrameLength, dsp::kFrameHop);
  const auto audio_powers = dsp::frame_powers(capture.audio, dsp::kFrameLength, dsp::kFrameHop);

  out.frames_total = static_cast<int>(channel_powers.size());
  out.active_probe_fraction = channel_floor.active_probe_fraction;
  out.dynamic_range_db = channel_floor.dynamic_range_db;
  out.clipped_fraction = capture.clipped_fraction;
  out.host_dropped_samples = capture.host_dropped_samples;

  // There is no partial result: when the channel floor is unusable, no metric
  // field is written, so a missing measurement can never be read as a
  // measurement of zero.
  if (channel_floor.status != FloorStatus::Reliable) {
    out.status_detail = channel_floor.detail;
    return;
  }

  out.noise_floor_dbfs = to_db(channel_floor.power);
  if (audio_floor.status == FloorStatus::Reliable) {
    out.audio_noise_floor_dbfs = to_db(audio_floor.power);
  }

  const double n_channel = channel_floor.power;
  const double n_audio = audio_floor.power;
  const bool audio_floor_usable = audio_floor.status == FloorStatus::Reliable && n_audio > 0.0;

  const std::size_t guard_samples = static_cast<std::size_t>(
      std::llround(cfg.audio_guard_ms / 1000.0 * static_cast<double>(cfg.channel_rate_hz)));
  const std::size_t min_audio_samples = static_cast<std::size_t>(
      std::llround(cfg.min_audio_window_ms / 1000.0 * static_cast<double>(cfg.channel_rate_hz)));

  std::vector<double> channel_snrs;
  std::vector<double> audio_snrs;
  std::vector<EventRecord> records;
  std::size_t frames_in_valid_events = 0;
  int truncated_count = 0;

  const double bin_hz = spectrum != nullptr && spectrum->fft_size() > 0
                            ? static_cast<double>(cfg.channel_rate_hz) /
                                  static_cast<double>(spectrum->fft_size())
                            : 0.0;

  for (const Event& e : events) {
    EventRecord r;
    r.start_s = frame_start_s(e.first_frame, cfg.channel_rate_hz);
    r.duration_s = event_duration_s(e, cfg.channel_rate_hz);
    r.utc = format_utc(capture.started_utc +
                       std::chrono::duration_cast<std::chrono::system_clock::duration>(
                           std::chrono::duration<double>(r.start_s)));
    r.truncated = e.truncated;

    if (e.truncated) {
      ++truncated_count;
      r.valid = false;
      records.push_back(std::move(r));
      continue;
    }

    const auto p_event = median_power(channel_powers, e.first_frame, e.last_frame);
    if (!p_event.has_value()) {
      r.valid = false;
      records.push_back(std::move(r));
      continue;
    }
    r.signal_power_dbfs = to_db(*p_event);

    // (P - N) / N in the LINEAR power domain, then convert to decibels. P / N
    // is SNR + 1 - the total-channel-power quantity that is explicitly not SNR
    // and must never be called one. Events at or below the floor are discarded
    // rather than reported as a negative or infinite number.
    if (!(*p_event > n_channel)) {
      r.valid = false;
      records.push_back(std::move(r));
      continue;
    }
    const double snr_lin = (*p_event - n_channel) / n_channel;
    const double snr_db = 10.0 * std::log10(snr_lin);
    r.channel_snr_db = snr_db;
    r.valid = true;
    channel_snrs.push_back(snr_db);
    frames_in_valid_events += e.last_frame - e.first_frame + 1;

    // The carrier offset: a comparability diagnostic only. A large change
    // between two angles indicates that different transmitters were heard; it
    // does NOT identify a transmitter or an aircraft.
    if (spectrum != nullptr && bin_hz > 0.0) {
      const std::size_t first_sample = e.first_frame * dsp::kFrameHop;
      const std::size_t last_sample =
          std::min(capture.channel.size(),
                   e.last_frame * dsp::kFrameHop + dsp::kFrameLength);
      if (last_sample > first_sample) {
        const auto psd = spectrum->welch_psd(
            std::span<const std::complex<float>>(capture.channel)
                .subspan(first_sample, last_sample - first_sample));
        if (!psd.empty()) {
          r.carrier_offset_hz = spectrum->carrier_offset_hz(psd, bin_hz, 2000.0);
        }
      }
    }

    // The audio window: shifted by the audio path's own delay and guarded at
    // both ends, so the uncompensated single-pole sections' residual is
    // absorbed rather than compensated.
    if (audio_floor_usable) {
      const std::size_t event_start = e.first_frame * dsp::kFrameHop;
      const std::size_t event_end = e.last_frame * dsp::kFrameHop + dsp::kFrameLength;
      const std::size_t audio_start = event_start + capture.audio_latency_samples + guard_samples;
      const std::size_t audio_end_raw = event_end + capture.audio_latency_samples;
      const std::size_t audio_end = audio_end_raw > guard_samples ? audio_end_raw - guard_samples : 0;

      if (audio_end > audio_start && audio_end - audio_start >= min_audio_samples) {
        const std::size_t first_af = audio_start / dsp::kFrameHop;
        const std::size_t last_af =
            audio_end >= dsp::kFrameLength ? (audio_end - dsp::kFrameLength) / dsp::kFrameHop : 0;
        const auto s_a = median_power(audio_powers, first_af,
                                      std::min(last_af, audio_powers.empty()
                                                            ? 0
                                                            : audio_powers.size() - 1));
        if (s_a.has_value() && *s_a > n_audio) {
          const double alin = (*s_a - n_audio) / n_audio;
          const double adb = 10.0 * std::log10(alin);
          r.audio_snr_db = adb;
          r.audio_snr_valid = true;
          audio_snrs.push_back(adb);
        }
      }
    }
    records.push_back(std::move(r));
  }

  out.valid_event_count = static_cast<int>(channel_snrs.size());
  out.valid_audio_event_count = static_cast<int>(audio_snrs.size());
  out.truncated_event_count = truncated_count;
  out.frames_active = static_cast<int>(frames_in_valid_events);

  out.events_total = out.valid_event_count;
  out.events_per_minute =
      cfg.duration_s > 0.0 ? static_cast<double>(out.events_total) / (cfg.duration_s / 60.0) : 0.0;
  out.detected_fraction =
      channel_powers.empty()
          ? 0.0
          : static_cast<double>(frames_in_valid_events) / static_cast<double>(channel_powers.size());

  // Every metric above is computed over ALL valid events. Only the persisted
  // event objects are capped, and the retained VALID count is recorded beside
  // the total so a truncated list is visible rather than looking like a quiet
  // capture.
  const std::size_t cap = static_cast<std::size_t>(std::max(cfg.max_retained_events, 0));
  if (records.size() > cap) records.resize(cap);
  int retained_valid = 0;
  for (const EventRecord& r : records) {
    if (r.valid) ++retained_valid;
  }
  out.events_retained = retained_valid;
  out.events = std::move(records);

  if (out.valid_event_count >= cfg.min_valid_events) {
    out.capture_score_channel_db = median(channel_snrs).value;
    out.channel_snr = percentiles_of(channel_snrs);
  }

  if (out.valid_audio_event_count >= cfg.min_valid_events) {
    out.capture_score_audio_db = median(audio_snrs).value;
    out.audio_snr = percentiles_of(audio_snrs);
    out.audio_insufficient = false;
  } else {
    // A capture with a channel score and no audio score is still `ok`. Attempt
    // status describes the channel path and the channel floor; audio
    // eligibility is a per-metric property recorded in a flag, not a status.
    out.audio_insufficient = true;
    if (!audio_floor_usable) {
      out.status_detail = "the audio noise floor was " +
                          std::string(to_string(audio_floor.status)) + ": " + audio_floor.detail;
    }
  }
}

}  // namespace rtlangle::metrics
