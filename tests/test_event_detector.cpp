// WP5 Test H — event detection: exact counts and boundaries, merge before
// discard, hysteresis, and truncation. Spec section 9.2.

#include <doctest/doctest.h>

#include "core/config.h"
#include "dsp/framer.h"
#include "dsp/spectrum.h"
#include "metrics/event_detector.h"
#include "metrics/noise_floor.h"

#include <vector>

using namespace rtlangle;
using namespace rtlangle::metrics;

namespace {

Config detector_config() {
  Config c;
  c.center_hz = 118350000;
  c.min_event_ms = 300.0;
  c.merge_gap_ms = 200.0;
  return c;
}

// A reliable floor at unit power, so thresholds are exactly from_db(open_db).
NoiseFloor unit_floor() {
  NoiseFloor nf;
  nf.status = FloorStatus::Reliable;
  nf.power = 1.0;
  nf.probe_power = 1.0;
  return nf;
}

// One frame is 512 samples of hop at 32 kHz, so 16 ms per frame index.
std::size_t frames_for_ms(double ms, std::uint32_t rate) {
  return static_cast<std::size_t>(ms / 1000.0 * static_cast<double>(rate) /
                                  static_cast<double>(dsp::kFrameHop));
}

std::vector<double> quiet(std::size_t n) { return std::vector<double>(n, 1.0); }

void fill_active(std::vector<double>& p, std::size_t first, std::size_t count, double level) {
  for (std::size_t k = first; k < first + count && k < p.size(); ++k) p[k] = level;
}

}  // namespace

TEST_SUITE("event_detector") {

TEST_CASE("a single burst is found with exact boundaries") {
  const Config cfg = detector_config();
  auto powers = quiet(400);
  const std::size_t first = 50;
  const std::size_t count = frames_for_ms(1000.0, cfg.channel_rate_hz);
  fill_active(powers, first, count, 100.0);

  const auto events = detect_events(powers, unit_floor(), cfg);
  REQUIRE(events.size() == 1);
  CHECK(events[0].first_frame == first);
  CHECK(events[0].last_frame == first + count - 1);
  CHECK(events[0].valid);
  CHECK_FALSE(events[0].truncated);
  CHECK(event_duration_s(events[0], cfg.channel_rate_hz) > 0.3);
}

TEST_CASE("merging happens before discarding") {
  // 200 ms burst, a 100 ms gap, then another 200 ms burst. Merged first, the
  // pattern survives as one event of about 500 ms; discarded first, both
  // fragments would be thrown away as shorter than min_event_ms.
  const Config cfg = detector_config();
  auto powers = quiet(400);
  const std::size_t a_first = 40;
  const std::size_t a_len = frames_for_ms(200.0, cfg.channel_rate_hz);
  const std::size_t gap = frames_for_ms(100.0, cfg.channel_rate_hz);
  const std::size_t b_len = frames_for_ms(200.0, cfg.channel_rate_hz);
  fill_active(powers, a_first, a_len, 100.0);
  fill_active(powers, a_first + a_len + gap, b_len, 100.0);

  const auto events = detect_events(powers, unit_floor(), cfg);
  REQUIRE(events.size() == 1);
  CHECK(events[0].first_frame == a_first);
  CHECK(events[0].last_frame == a_first + a_len + gap + b_len - 1);
  const double duration = event_duration_s(events[0], cfg.channel_rate_hz);
  CHECK(duration > 0.45);
  CHECK(duration < 0.60);
}

TEST_CASE("a gap wider than merge_gap_ms leaves two events") {
  const Config cfg = detector_config();
  auto powers = quiet(600);
  const std::size_t len = frames_for_ms(400.0, cfg.channel_rate_hz);
  const std::size_t gap = frames_for_ms(500.0, cfg.channel_rate_hz);
  fill_active(powers, 40, len, 100.0);
  fill_active(powers, 40 + len + gap, len, 100.0);

  const auto events = detect_events(powers, unit_floor(), cfg);
  CHECK(events.size() == 2);
}

TEST_CASE("a burst shorter than min_event_ms is discarded after merging") {
  const Config cfg = detector_config();
  auto powers = quiet(400);
  fill_active(powers, 100, frames_for_ms(100.0, cfg.channel_rate_hz), 100.0);
  CHECK(detect_events(powers, unit_floor(), cfg).empty());
}

TEST_CASE("hysteresis prevents fragmentation at the threshold") {
  const Config cfg = detector_config();
  auto powers = quiet(400);
  const std::size_t first = 50;
  const std::size_t count = frames_for_ms(1000.0, cfg.channel_rate_hz);
  // The burst hovers between the close threshold (3 dB) and the open threshold
  // (6 dB) after its first frames. Without hysteresis this would open and close
  // repeatedly and be discarded as a string of short fragments.
  for (std::size_t k = first; k < first + count; ++k) {
    powers[k] = (k == first) ? 100.0 : 3.0;   // above close (1.995), below open (3.98)
  }
  const auto events = detect_events(powers, unit_floor(), cfg);
  REQUIRE(events.size() == 1);
  CHECK(events[0].last_frame == first + count - 1);
}

TEST_CASE("an event open at the first or last frame is truncated and invalid") {
  const Config cfg = detector_config();

  auto at_start = quiet(400);
  fill_active(at_start, 0, frames_for_ms(1000.0, cfg.channel_rate_hz), 100.0);
  const auto first_events = detect_events(at_start, unit_floor(), cfg);
  REQUIRE(first_events.size() == 1);
  CHECK(first_events[0].truncated);
  CHECK_FALSE(first_events[0].valid);

  auto at_end = quiet(400);
  fill_active(at_end, 300, 100, 100.0);
  const auto last_events = detect_events(at_end, unit_floor(), cfg);
  REQUIRE(last_events.size() == 1);
  CHECK(last_events[0].truncated);
  CHECK_FALSE(last_events[0].valid);
  CHECK(last_events[0].last_frame == at_end.size() - 1);
}

TEST_CASE("no events are reported against a floor that is not reliable") {
  const Config cfg = detector_config();
  auto powers = quiet(400);
  fill_active(powers, 50, 100, 100.0);

  NoiseFloor unreliable = unit_floor();
  unreliable.status = FloorStatus::Unreliable;
  CHECK(detect_events(powers, unreliable, cfg).empty());

  NoiseFloor unidentifiable = unit_floor();
  unidentifiable.status = FloorStatus::Unidentifiable;
  CHECK(detect_events(powers, unidentifiable, cfg).empty());
}

TEST_CASE("frame timing maps frames to seconds at the channel rate") {
  const Config cfg = detector_config();
  CHECK(frame_start_s(0, cfg.channel_rate_hz) == doctest::Approx(0.0));
  CHECK(frame_start_s(1, cfg.channel_rate_hz) == doctest::Approx(0.016));
  CHECK(frame_start_s(100, cfg.channel_rate_hz) == doctest::Approx(1.6));

  Event e;
  e.first_frame = 0;
  e.last_frame = 0;
  CHECK(event_duration_s(e, cfg.channel_rate_hz) == doctest::Approx(0.032));
}

}  // TEST_SUITE
