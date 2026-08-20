// WP10 — aggregation. Spec sections 9.4.1, 9.5, 10.1, and 10.2.

#include <doctest/doctest.h>

#include "core/utc.h"
#include "experiment/aggregator.h"
#include "experiment/visit_plan.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <string>

using namespace rtlangle;

namespace {

Config summary_config() {
  Config c;
  c.center_hz = 118350000;
  c.angles_deg = {0.0, 45.0, 90.0};
  c.rounds = 4;
  return c;
}

SessionRecord empty_record(const Config& cfg) {
  SessionRecord r;
  r.config = cfg;
  r.started_utc = utc_now();
  r.plan.angles_deg = resolved_angles(cfg);
  r.plan.rounds = cfg.rounds;
  r.plan.visits = build_visit_plan(cfg);
  ReceiverSegment seg;
  seg.segment_id = "seg1";
  seg.baseline.applied_gain_tenth_db = 496;
  seg.baseline.applied_sample_rate_hz = 1024000;
  seg.baseline.applied_center_hz = 118600000;
  r.receiver_segments.push_back(seg);
  return r;
}

struct CaptureSpec {
  std::size_t angle_index = 0;
  int         round = 1;
  double      channel_db = 18.0;
  std::optional<double> audio_db = 14.0;
  double      events_per_minute = 6.0;
  int         valid_events = 6;
  double      noise_floor_dbfs = -62.0;
  double      actual_deg = -1.0;   // negative means "the planned angle"
};

void add_capture(SessionRecord& rec, const CaptureSpec& spec) {
  AttemptRecord a;
  a.visit_id = make_visit_id(spec.round, spec.angle_index);
  a.round = spec.round;
  a.angle_index = spec.angle_index;
  a.planned_deg = rec.plan.angles_deg[spec.angle_index];
  a.actual_deg = spec.actual_deg < 0.0 ? a.planned_deg : spec.actual_deg;
  a.segment_id = "seg1";
  a.attempt = 1;
  a.disposition = Disposition::Accepted;
  a.status = AttemptStatus::Ok;
  a.started_utc = utc_now();
  a.applied_gain_tenth_db = 496;
  a.applied_sample_rate_hz = 1024000;
  a.applied_center_hz = 118600000;
  a.valid_event_count = spec.valid_events;
  a.valid_audio_event_count = spec.audio_db.has_value() ? spec.valid_events : 0;
  a.audio_insufficient = !spec.audio_db.has_value();
  a.events_total = spec.valid_events;
  a.events_retained = spec.valid_events;
  a.events_per_minute = spec.events_per_minute;
  a.detected_fraction = 0.1;
  a.capture_score_channel_db = spec.channel_db;
  a.capture_score_audio_db = spec.audio_db;
  a.noise_floor_dbfs = spec.noise_floor_dbfs;
  for (int i = 0; i < spec.valid_events; ++i) {
    EventRecord e;
    e.valid = true;
    e.channel_snr_db = spec.channel_db;
    a.events.push_back(e);
  }
  rec.attempts.push_back(std::move(a));
}

const AngleSummary& angle_at(const SessionSummary& s, double planned_deg) {
  for (const AngleSummary& a : s.angles) {
    if (std::fabs(a.planned_deg - planned_deg) < 1e-9) return a;
  }
  FAIL("no angle summary for ", planned_deg);
  return s.angles.front();
}

}  // namespace

TEST_SUITE("aggregation") {

TEST_CASE("the experimental unit is the capture, not the event") {
  const Config cfg = summary_config();
  SessionRecord rec = empty_record(cfg);

  // One capture of fifty events at 20 dB against four captures of six at 10 dB.
  add_capture(rec, {0, 1, 20.0, 16.0, 50.0, 50, -62.0, -1.0});
  for (int round = 1; round <= 4; ++round) {
    add_capture(rec, {1, round, 10.0, 8.0, 6.0, 6, -62.0, -1.0});
  }

  const SessionSummary summary = aggregate(rec);
  const AngleSummary& first = angle_at(summary, 0.0);
  const AngleSummary& second = angle_at(summary, 45.0);

  CHECK(first.score_channel_db.value() == doctest::Approx(20.0));
  CHECK(first.n_captures == 1);
  CHECK(second.score_channel_db.value() == doctest::Approx(10.0));
  CHECK(second.n_captures == 4);

  // Changing the NUMBER of events inside a capture does not change its vote.
  SessionRecord more_events = empty_record(cfg);
  add_capture(more_events, {0, 1, 20.0, 16.0, 50.0, 200, -62.0, -1.0});
  for (int round = 1; round <= 4; ++round) {
    add_capture(more_events, {1, round, 10.0, 8.0, 6.0, 6, -62.0, -1.0});
  }
  const SessionSummary other = aggregate(more_events);
  const AngleSummary& other_first = angle_at(other, 0.0);
  CHECK(other_first.score_channel_db.value() == doctest::Approx(*first.score_channel_db));
  CHECK(other_first.n_captures == 1);
}

TEST_CASE("superseded attempts are invisible to the summary") {
  const Config cfg = summary_config();
  SessionRecord rec = empty_record(cfg);
  add_capture(rec, {0, 1, 10.0, 8.0, 6.0, 6, -62.0, -1.0});

  // A 30 dB attempt that was superseded by the 10 dB one above.
  AttemptRecord superseded = rec.attempts.front();
  superseded.attempt = 0;
  superseded.disposition = Disposition::Superseded;
  superseded.capture_score_channel_db = 30.0;
  rec.attempts.insert(rec.attempts.begin(), superseded);

  const SessionSummary summary = aggregate(rec);
  const AngleSummary& a = angle_at(summary, 0.0);
  CHECK(a.n_captures == 1);
  CHECK(a.score_channel_db.value() == doctest::Approx(10.0));
}

TEST_CASE("per-metric eligibility flows to the angle level") {
  const Config cfg = summary_config();
  SessionRecord rec = empty_record(cfg);
  // Angle 0 has audio; angle 45 has none at all.
  for (int round = 1; round <= 4; ++round) {
    add_capture(rec, {0, round, 18.0, 14.0, 6.0, 6, -62.0, -1.0});
    add_capture(rec, {1, round, 17.0, std::nullopt, 6.0, 6, -62.0, -1.0});
  }

  const SessionSummary summary = aggregate(rec);
  const AngleSummary& audio_less = angle_at(summary, 45.0);
  CHECK(audio_less.n_captures == 4);
  CHECK(audio_less.n_captures_audio == 0);
  CHECK_FALSE(audio_less.score_audio_db.has_value());

  // Present in the channel ranking, absent from the audio one.
  CHECK(std::count(summary.ranking_channel.begin(), summary.ranking_channel.end(), 45.0) == 1);
  CHECK(std::count(summary.ranking_audio.begin(), summary.ranking_audio.end(), 45.0) == 0);

  for (const AngleSummary& a : summary.angles) {
    CHECK(a.n_captures_audio <= a.n_captures);
  }
}

TEST_CASE("yield aggregates like the score and moves independently of it") {
  const Config cfg = summary_config();
  SessionRecord rec = empty_record(cfg);
  for (int round = 1; round <= 4; ++round) {
    add_capture(rec, {0, round, 18.0, 14.0, 4.0, 4, -62.0, -1.0});
    add_capture(rec, {1, round, 18.0, 14.0, 12.0, 12, -62.0, -1.0});
  }

  const SessionSummary summary = aggregate(rec);
  CHECK(angle_at(summary, 0.0).yield_events_per_min.value() == doctest::Approx(4.0));
  CHECK(angle_at(summary, 45.0).yield_events_per_min.value() == doctest::Approx(12.0));
  // Equal scores, different yields.
  CHECK(angle_at(summary, 0.0).score_channel_db.value() ==
        doctest::Approx(*angle_at(summary, 45.0).score_channel_db));
  CHECK(summary.ranking_yield.front() == doctest::Approx(45.0));
}

TEST_CASE("the descriptive spread switches form at four captures") {
  const Config cfg = summary_config();

  SessionRecord three = empty_record(cfg);
  for (int round = 1; round <= 3; ++round) {
    add_capture(three, {0, round, 10.0 + round, 8.0, 6.0, 6, -62.0, -1.0});
  }
  const SessionSummary three_summary = aggregate(three);
  const AngleSummary& small = angle_at(three_summary, 0.0);
  CHECK(small.spread_channel.valid);
  CHECK_FALSE(small.spread_channel.is_iqr);
  CHECK(small.spread_channel.low == doctest::Approx(11.0));
  CHECK(small.spread_channel.high == doctest::Approx(13.0));

  SessionRecord four = empty_record(cfg);
  for (int round = 1; round <= 4; ++round) {
    add_capture(four, {0, round, 10.0 + round, 8.0, 6.0, 6, -62.0, -1.0});
  }
  const SessionSummary four_summary = aggregate(four);
  const AngleSummary& large = angle_at(four_summary, 0.0);
  CHECK(large.spread_channel.is_iqr);
  CHECK(large.n_captures == 4);
}

TEST_CASE("circular statistics are used for the actual angles, never folded modulo 180") {
  Config cfg = summary_config();
  cfg.angles_deg = {0.0, 180.0};
  SessionRecord rec = empty_record(cfg);
  add_capture(rec, {0, 1, 18.0, 14.0, 6.0, 6, -62.0, 350.0});
  add_capture(rec, {0, 2, 18.0, 14.0, 6.0, 6, -62.0, 10.0});
  add_capture(rec, {1, 1, 12.0, 10.0, 6.0, 6, -62.0, 180.0});

  const SessionSummary summary = aggregate(rec);
  // A mean of {350, 10} is near 0, not 180.
  CHECK(angle_at(summary, 0.0).actual_mean_deg == doctest::Approx(0.0).epsilon(1e-6));
  // And the two 180-apart angles remain two separate entries.
  CHECK(summary.angles.size() == 2);
  CHECK(angle_at(summary, 180.0).actual_mean_deg == doctest::Approx(180.0));
}

TEST_CASE("an angle with no eligible capture is present with its status and unordered") {
  const Config cfg = summary_config();
  SessionRecord rec = empty_record(cfg);
  add_capture(rec, {0, 1, 18.0, 14.0, 6.0, 6, -62.0, -1.0});

  // A capture that was accepted but did not produce a channel score.
  AttemptRecord failed;
  failed.visit_id = make_visit_id(1, 1);
  failed.round = 1;
  failed.angle_index = 1;
  failed.planned_deg = 45.0;
  failed.actual_deg = 45.0;
  failed.segment_id = "seg1";
  failed.disposition = Disposition::Accepted;
  failed.status = AttemptStatus::InsufficientData;
  rec.attempts.push_back(failed);

  const SessionSummary summary = aggregate(rec);
  const AngleSummary& excluded = angle_at(summary, 45.0);
  CHECK(excluded.n_captures == 0);
  CHECK(excluded.n_excluded == 1);
  CHECK(excluded.status == "no eligible capture");
  CHECK(std::count(summary.ranking_channel.begin(), summary.ranking_channel.end(), 45.0) == 0);

  const AngleSummary& untouched = angle_at(summary, 90.0);
  CHECK(untouched.status == "not measured");
}

TEST_CASE("the rankings order by score, highest first") {
  const Config cfg = summary_config();
  SessionRecord rec = empty_record(cfg);
  add_capture(rec, {0, 1, 12.0, 9.0, 6.0, 6, -62.0, -1.0});
  add_capture(rec, {1, 1, 20.0, 6.0, 6.0, 6, -62.0, -1.0});
  add_capture(rec, {2, 1, 16.0, 15.0, 6.0, 6, -62.0, -1.0});

  const SessionSummary summary = aggregate(rec);
  REQUIRE(summary.ranking_channel.size() == 3);
  CHECK(summary.ranking_channel[0] == doctest::Approx(45.0));
  CHECK(summary.ranking_channel[1] == doctest::Approx(90.0));
  CHECK(summary.ranking_channel[2] == doctest::Approx(0.0));

  // The audio ranking is ordered independently, and here it disagrees.
  REQUIRE(summary.ranking_audio.size() == 3);
  CHECK(summary.ranking_audio[0] == doctest::Approx(90.0));
}

TEST_CASE("the pooled noise floor and its spread come from the contributing captures") {
  const Config cfg = summary_config();
  SessionRecord rec = empty_record(cfg);
  add_capture(rec, {0, 1, 18.0, 14.0, 6.0, 6, -64.0, -1.0});
  add_capture(rec, {0, 2, 18.0, 14.0, 6.0, 6, -60.0, -1.0});

  const SessionSummary summary = aggregate(rec);
  const AngleSummary& a = angle_at(summary, 0.0);
  CHECK(a.noise_floor_dbfs.value() == doctest::Approx(-62.0));
  CHECK(a.noise_floor_spread_db == doctest::Approx(4.0));
}

TEST_CASE("the summary carries no decision") {
  const Config cfg = summary_config();
  SessionRecord rec = empty_record(cfg);
  add_capture(rec, {0, 1, 18.0, 14.0, 6.0, 6, -62.0, -1.0});
  const SessionSummary summary = aggregate(rec);

  nlohmann::json j;
  to_json(j, summary);
  const std::string dumped = j.dump();
  for (const char* forbidden : {"resolved", "resolution", "winner"}) {
    CHECK_MESSAGE(dumped.find(forbidden) == std::string::npos, forbidden);
  }
  CHECK(summary.report_metric_source == "default");
}

}  // TEST_SUITE
