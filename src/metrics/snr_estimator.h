#pragma once

#include "core/config.h"
#include "core/records.h"
#include "dsp/spectrum.h"
#include "experiment/capture_runner.h"
#include "metrics/event_detector.h"
#include "metrics/noise_floor.h"

#include <span>

namespace rtlangle::metrics {

// Fills the metric fields of an AttemptRecord. It never invents a value when a
// floor is not Reliable or when an event is invalid, and it returns without
// touching any metric field when the CHANNEL floor is unusable: there is no
// partial result.
//
// It also fills the spec section 9.5 yield fields, computed from the same event
// list and frame count:
//
//   events_total       = valid, non-truncated events (ALL of them)
//   events_retained    = valid events among the retained event objects
//   events_per_minute  = events_total / (duration_s / 60)
//   detected_fraction  = frames inside a valid event / frames_total
//
// Percentiles and the capture score are computed over ALL valid events; only
// the persisted `events` vector is truncated (spec section 11.3), so the
// retention cap can never become a measurement change.
//
// The two metrics qualify SEPARATELY (spec section 9.4.1). `channel_floor` and
// `audio_floor` are estimated independently and either may be unreliable:
//
//   valid_event_count       -> capture_score_channel_db  (needs >= min_valid_events)
//   valid_audio_event_count -> capture_score_audio_db    (needs >= min_valid_events)
//
// Whichever count falls short leaves ITS score absent; a short audio count
// additionally sets audio_insufficient. Attempt status describes the CHANNEL
// path only.
//
// `spectrum`, when non-null, supplies each event's carrier frequency offset - a
// comparability diagnostic only, which does not identify a transmitter.
void estimate_snr(const CaptureOutcome& capture, const NoiseFloor& channel_floor,
                  const NoiseFloor& audio_floor, std::span<const Event> events,
                  const Config& cfg, AttemptRecord& out,
                  dsp::Spectrum* spectrum = nullptr);

}  // namespace rtlangle::metrics
