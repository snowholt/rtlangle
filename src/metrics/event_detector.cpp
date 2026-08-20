#include "metrics/event_detector.h"

#include "core/db.h"
#include "dsp/framer.h"

#include <cmath>

namespace rtlangle::metrics {

double frame_start_s(std::size_t frame, std::uint32_t channel_rate_hz) {
  if (channel_rate_hz == 0) return 0.0;
  return static_cast<double>(frame * dsp::kFrameHop) / static_cast<double>(channel_rate_hz);
}

double event_duration_s(const Event& e, std::uint32_t channel_rate_hz) {
  if (channel_rate_hz == 0) return 0.0;
  const std::size_t span = (e.last_frame - e.first_frame) * dsp::kFrameHop + dsp::kFrameLength;
  return static_cast<double>(span) / static_cast<double>(channel_rate_hz);
}

std::vector<Event> detect_events(std::span<const double> frame_powers, const NoiseFloor& nf,
                                 const Config& cfg) {
  std::vector<Event> events;
  if (nf.status != FloorStatus::Reliable) return events;
  if (frame_powers.empty() || !(nf.power > 0.0)) return events;

  const double open_threshold = nf.power * from_db(cfg.open_db);
  const double close_threshold = nf.power * from_db(cfg.close_db);

  // Pass one: hysteresis. Opening needs the higher threshold and closing the
  // lower one, so a burst hovering at the boundary does not fragment.
  std::vector<Event> candidates;
  bool open = false;
  Event current;
  for (std::size_t k = 0; k < frame_powers.size(); ++k) {
    const double p = frame_powers[k];
    if (!open) {
      if (p > open_threshold) {
        open = true;
        current = Event{};
        current.first_frame = k;
        current.last_frame = k;
        if (k == 0) current.truncated = true;   // already open at the first frame
      }
      continue;
    }
    if (p < close_threshold) {
      open = false;
      candidates.push_back(current);
      continue;
    }
    current.last_frame = k;
  }
  if (open) {
    current.truncated = true;   // still open at the last frame
    current.last_frame = frame_powers.size() - 1;
    candidates.push_back(current);
  }
  if (candidates.empty()) return events;

  // Pass two: merge before discarding.
  const double gap_limit_s = cfg.merge_gap_ms / 1000.0;
  std::vector<Event> merged;
  merged.push_back(candidates.front());
  for (std::size_t i = 1; i < candidates.size(); ++i) {
    Event& prev = merged.back();
    const Event& next = candidates[i];
    const double prev_end_s =
        frame_start_s(prev.last_frame, cfg.channel_rate_hz) +
        static_cast<double>(dsp::kFrameLength) / static_cast<double>(cfg.channel_rate_hz);
    const double next_start_s = frame_start_s(next.first_frame, cfg.channel_rate_hz);
    if (next_start_s - prev_end_s < gap_limit_s) {
      prev.last_frame = next.last_frame;
      prev.truncated = prev.truncated || next.truncated;
    } else {
      merged.push_back(next);
    }
  }

  // Pass three: discard the short ones, and mark validity.
  const double min_duration_s = cfg.min_event_ms / 1000.0;
  for (Event& e : merged) {
    if (event_duration_s(e, cfg.channel_rate_hz) < min_duration_s) continue;
    e.valid = !e.truncated;
    events.push_back(e);
  }
  return events;
}

}  // namespace rtlangle::metrics
