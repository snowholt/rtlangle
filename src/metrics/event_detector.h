#pragma once

#include "core/config.h"
#include "metrics/noise_floor.h"

#include <cstddef>
#include <span>
#include <vector>

namespace rtlangle::metrics {

struct Event {
  std::size_t first_frame = 0;
  std::size_t last_frame = 0;   // inclusive
  bool        truncated = false;
  bool        valid = false;
};

// Hysteresis squelch on the frame powers, with thresholds relative to the
// reported noise floor:
//
//   open  when P[k] > N * from_db(open_db)
//   close when P[k] < N * from_db(close_db)
//
// Candidates separated by less than merge_gap_ms are MERGED FIRST; candidates
// shorter than min_event_ms are DISCARDED AFTER merging. The order matters: it
// is what stops a single transmission with a brief pause from being discarded
// as two short fragments.
//
// An event already open at the first frame, or still open at the last, is
// `truncated`: its true extent is unknown and its median would be taken over a
// partial transmission, so it is recorded for audit with valid == false and
// excluded from every metric.
//
// Returns an empty list when the floor is not Reliable: there is nothing to
// measure events against.
std::vector<Event> detect_events(std::span<const double> frame_powers, const NoiseFloor&,
                                 const Config&);

// Frame timing at the channel rate. Frame k covers samples [k*hop, k*hop+len).
double frame_start_s(std::size_t frame, std::uint32_t channel_rate_hz);
double event_duration_s(const Event&, std::uint32_t channel_rate_hz);

}  // namespace rtlangle::metrics
