// WP10 Tests J and L — the estimand and warning W6, and every warning every
// time. Spec sections 10.0, 10.4, and 10.5.

#include <doctest/doctest.h>

#include "core/utc.h"
#include "experiment/aggregator.h"
#include "experiment/quality_checks.h"
#include "experiment/visit_plan.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <set>
#include <string>
#include <vector>

using namespace rtlangle;

namespace {

Config warning_config() {
  Config c;
  c.center_hz = 118350000;
  c.angles_deg = {0.0, 45.0, 90.0};
  c.rounds = 4;
  return c;
}

SessionRecord base_record(const Config& cfg) {
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

struct Cap {
  std::size_t angle_index = 0;
  int         round = 1;
  double      channel_db = 18.0;
  std::optional<double> audio_db = 14.0;
  double      yield = 6.0;
  double      floor_dbfs = -62.0;
};

AttemptRecord& add(SessionRecord& rec, const Cap& c) {
  AttemptRecord a;
  a.visit_id = make_visit_id(c.round, c.angle_index);
  a.round = c.round;
  a.angle_index = c.angle_index;
  a.planned_deg = rec.plan.angles_deg[c.angle_index];
  a.actual_deg = a.planned_deg;
  a.segment_id = "seg1";
  a.attempt = 1;
  a.disposition = Disposition::Accepted;
  a.status = AttemptStatus::Ok;
  a.started_utc = utc_now();
  a.applied_gain_tenth_db = 496;
  a.applied_sample_rate_hz = 1024000;
  a.applied_center_hz = 118600000;
  a.valid_event_count = 6;
  a.valid_audio_event_count = c.audio_db.has_value() ? 6 : 0;
  a.audio_insufficient = !c.audio_db.has_value();
  a.events_total = 6;
  a.events_retained = 6;
  a.events_per_minute = c.yield;
  a.detected_fraction = 0.1;
  a.capture_score_channel_db = c.channel_db;
  a.capture_score_audio_db = c.audio_db;
  a.noise_floor_dbfs = c.floor_dbfs;
  rec.attempts.push_back(std::move(a));
  return rec.attempts.back();
}

SessionSummary summarise(const SessionRecord& rec, const Config& cfg) {
  SessionSummary s = aggregate(rec);
  evaluate_warnings(s, rec, cfg);
  return s;
}

bool fired(const SessionSummary& s, std::string_view id) {
  for (const Warning& w : s.warnings) {
    if (w.id == id) return true;
  }
  return false;
}

const Warning* warning_of(const SessionSummary& s, std::string_view id) {
  for (const Warning& w : s.warnings) {
    if (w.id == id) return &w;
  }
  return nullptr;
}

// A session with four rounds at every angle, comfortably clear of every
// warning, so a single deliberate change is the only reason one fires.
SessionRecord clean_session(const Config& cfg) {
  SessionRecord rec = base_record(cfg);
  for (int round = 1; round <= 4; ++round) {
    add(rec, {0, round, 22.0, 18.0, 6.0, -62.0});
    add(rec, {1, round, 16.0, 12.0, 6.0, -62.0});
    add(rec, {2, round, 12.0, 9.0, 6.0, -62.0});
  }
  return rec;
}

}  // namespace

TEST_SUITE("quality_checks") {

// ---------------------------------------------------------------------------
// Test J — the estimand, constructed directly.
// ---------------------------------------------------------------------------
TEST_CASE("Test J: the better angle scores LOWER, yields more, and W6 says so") {
  // Angle A sees a population of transmissions of which only the strong half
  // clears the squelch, so its median is taken over the loud ones alone. Angle
  // B sees the same population 3 dB stronger, so the weak half clears it too -
  // and those extra weak events pull B's median down.
  Config cfg = warning_config();
  cfg.angles_deg = {0.0, 45.0};
  SessionRecord rec = base_record(cfg);

  for (int round = 1; round <= 4; ++round) {
    // A: only the strong half detected. Median of {18, 20} = 19 dB, 4 per minute.
    add(rec, {0, round, 19.0, 15.0, 4.0, -62.0});
    // B: the whole population detected. Median of {9, 11, 21, 23} = 16 dB, 8 per minute.
    add(rec, {1, round, 16.0, 12.0, 8.0, -62.0});
  }

  const SessionSummary s = summarise(rec, cfg);
  const double score_a = *s.angles[0].score_channel_db;
  const double score_b = *s.angles[1].score_channel_db;
  const double yield_a = *s.angles[0].yield_events_per_min;
  const double yield_b = *s.angles[1].yield_events_per_min;

  CHECK(score_b < score_a);   // the BETTER angle scores LOWER
  CHECK(yield_b > yield_a);
  CHECK(s.ranking_channel[0] == doctest::Approx(0.0));

  REQUIRE(fired(s, "W6"));
  const Warning* w6 = warning_of(s, "W6");
  REQUIRE(w6 != nullptr);
  // The message carries both yields and the ratio.
  CHECK(w6->message.find("4.00") != std::string::npos);
  CHECK(w6->message.find("8.00") != std::string::npos);
  CHECK(w6->message.find("0.50") != std::string::npos);

  // Both rankings are still printed in full: nothing was withheld.
  CHECK(s.ranking_channel.size() == 2);
  CHECK(s.ranking_audio.size() == 2);
}

TEST_CASE("Test J, concordant fixture: W6 stays silent") {
  Config cfg = warning_config();
  cfg.angles_deg = {0.0, 45.0};
  SessionRecord rec = base_record(cfg);
  for (int round = 1; round <= 4; ++round) {
    add(rec, {0, round, 20.0, 16.0, 9.0, -62.0});   // higher score AND higher yield
    add(rec, {1, round, 14.0, 10.0, 6.0, -62.0});
  }
  const SessionSummary s = summarise(rec, cfg);
  CHECK_FALSE(fired(s, "W6"));
}

// ---------------------------------------------------------------------------
// Test L — every warning, enumerated by id.
// ---------------------------------------------------------------------------
TEST_CASE("Test L: the suite exercises exactly the warning ids the spec defines") {
  // Named, because warning_ids() returns by value: taking begin() and end()
  // from two separate temporaries would give iterators into different objects.
  const std::vector<std::string_view> ids = warning_ids();
  const std::set<std::string> defined(ids.begin(), ids.end());
  const std::set<std::string> expected = {"W1", "W2", "W3", "W4", "W5", "W6", "W7", "W8"};
  CHECK(defined == expected);
}

TEST_CASE("W1 fires on a thin angle and is silent on a full one") {
  const Config cfg = warning_config();
  CHECK_FALSE(fired(summarise(clean_session(cfg), cfg), "W1"));

  SessionRecord thin = base_record(cfg);
  for (int round = 1; round <= 4; ++round) {
    add(thin, {0, round, 22.0, 18.0, 6.0, -62.0});
    add(thin, {1, round, 16.0, 12.0, 6.0, -62.0});
  }
  add(thin, {2, 1, 12.0, 9.0, 6.0, -62.0});   // one capture only
  const SessionSummary s = summarise(thin, cfg);
  REQUIRE(fired(s, "W1"));
  CHECK(warning_of(s, "W1")->message.find("90.0 deg") != std::string::npos);
}

TEST_CASE("W2 names both angles and both counts, and computes no test statistic") {
  Config cfg = warning_config();
  cfg.rounds = 3;
  cfg.angles_deg = {30.0, 45.0};
  SessionRecord rec = base_record(cfg);
  // Three rounds run; the two angles share only two of them.
  add(rec, {0, 1, 20.0, 16.0, 6.0, -62.0});
  add(rec, {0, 2, 20.0, 16.0, 6.0, -62.0});
  add(rec, {0, 3, 20.0, 16.0, 6.0, -62.0});
  add(rec, {1, 1, 14.0, 10.0, 6.0, -62.0});
  add(rec, {1, 2, 14.0, 10.0, 6.0, -62.0});
  // Round 3 at angle 45 failed, so it does not count as shared.
  AttemptRecord& failed = add(rec, {1, 3, 14.0, 10.0, 6.0, -62.0});
  failed.status = AttemptStatus::InsufficientData;
  failed.capture_score_channel_db.reset();
  failed.capture_score_audio_db.reset();

  const SessionSummary s = summarise(rec, cfg);
  REQUIRE(fired(s, "W2"));
  const Warning* w2 = warning_of(s, "W2");
  CHECK(w2->message.find("30.0 deg") != std::string::npos);
  CHECK(w2->message.find("45.0 deg") != std::string::npos);
  CHECK(w2->message.find("2 of the 3 rounds") != std::string::npos);
  // An observation, not a test: no p-value, no interval, no difference.
  CHECK(w2->message.find("p-value") == std::string::npos);
  CHECK(w2->message.find("interval") == std::string::npos);
}

TEST_CASE("W3 fires when the top two are closer than the acting threshold") {
  const Config cfg = warning_config();
  CHECK_FALSE(fired(summarise(clean_session(cfg), cfg), "W3"));

  SessionRecord close = base_record(cfg);
  for (int round = 1; round <= 4; ++round) {
    add(close, {0, round, 18.0, 14.0, 6.0, -62.0});
    add(close, {1, round, 17.6, 13.6, 6.0, -62.0});
    add(close, {2, round, 12.0, 9.0, 6.0, -62.0});
  }
  const SessionSummary s = summarise(close, cfg);
  REQUIRE(fired(s, "W3"));
  CHECK(warning_of(s, "W3")->message.find("0.40 dB") != std::string::npos);
}

TEST_CASE("W4 fires when the two rankings disagree on the top angle") {
  const Config cfg = warning_config();
  CHECK_FALSE(fired(summarise(clean_session(cfg), cfg), "W4"));

  SessionRecord disagree = base_record(cfg);
  for (int round = 1; round <= 4; ++round) {
    add(disagree, {0, round, 22.0, 6.0, 6.0, -62.0});    // best channel, worst audio
    add(disagree, {1, round, 16.0, 18.0, 6.0, -62.0});   // best audio
    add(disagree, {2, round, 12.0, 9.0, 6.0, -62.0});
  }
  const SessionSummary s = summarise(disagree, cfg);
  REQUIRE(fired(s, "W4"));
  const Warning* w4 = warning_of(s, "W4");
  CHECK(w4->message.find("0.0 deg") != std::string::npos);
  CHECK(w4->message.find("45.0 deg") != std::string::npos);
}

TEST_CASE("W5 fires when the noise environment moved during the session") {
  const Config cfg = warning_config();
  CHECK_FALSE(fired(summarise(clean_session(cfg), cfg), "W5"));

  SessionRecord drift = base_record(cfg);
  for (int round = 1; round <= 4; ++round) {
    add(drift, {0, round, 22.0, 18.0, 6.0, -66.0});
    add(drift, {1, round, 16.0, 12.0, 6.0, -62.0});
    add(drift, {2, round, 12.0, 9.0, 6.0, -58.0});   // an 8 dB span
  }
  const SessionSummary s = summarise(drift, cfg);
  REQUIRE(fired(s, "W5"));
  CHECK(warning_of(s, "W5")->message.find("8.00 dB") != std::string::npos);
}

TEST_CASE("W7 compares against the segment baseline, not against the request") {
  const Config cfg = warning_config();

  // A device that snapped the request is normal: every attempt matches its own
  // segment baseline, and W7 stays silent. Comparing against the request would
  // fire on every capture of every real device.
  SessionRecord snapped = clean_session(cfg);
  snapped.receiver_segments[0].baseline.requested_gain_tenth_db = 500;
  snapped.receiver_segments[0].baseline.applied_gain_tenth_db = 496;
  CHECK_FALSE(fired(summarise(snapped, cfg), "W7"));

  // A gain that MOVED within a segment is what W7 exists to catch.
  SessionRecord moved = clean_session(cfg);
  moved.attempts[5].applied_gain_tenth_db = 400;
  const SessionSummary s = summarise(moved, cfg);
  REQUIRE(fired(s, "W7"));
  const Warning* w7 = warning_of(s, "W7");
  CHECK(w7->message.find("496") != std::string::npos);
  CHECK(w7->message.find("400") != std::string::npos);
  CHECK(w7->message.find("seg1") != std::string::npos);

  // And AGC reported enabled anywhere fires it too.
  SessionRecord agc = clean_session(cfg);
  agc.attempts[2].agc_enabled = true;
  CHECK(fired(summarise(agc, cfg), "W7"));
}

TEST_CASE("W8 names the angle and the flag") {
  const Config cfg = warning_config();
  CHECK_FALSE(fired(summarise(clean_session(cfg), cfg), "W8"));

  SessionRecord flagged = clean_session(cfg);
  flagged.attempts[1].host_dropped_samples = 128;
  const SessionSummary s = summarise(flagged, cfg);
  REQUIRE(fired(s, "W8"));
  const Warning* w8 = warning_of(s, "W8");
  CHECK(w8->message.find("host_dropped_samples") != std::string::npos);
  CHECK(w8->message.find("45.0 deg") != std::string::npos);
  // The honest name, never a word meaning a device overrun.
  CHECK(w8->message.find("overrun") == std::string::npos);
}

TEST_CASE("a record engineered to trip three warnings reports all three") {
  // Spec section 10.4 forbids short-circuiting: a list of problems that stops
  // at the first problem is not a list of problems.
  Config cfg = warning_config();
  SessionRecord rec = base_record(cfg);
  // W1: one capture at one angle. W5: an 8 dB floor span. W8: a dropped sample.
  add(rec, {0, 1, 22.0, 18.0, 6.0, -66.0});
  for (int round = 1; round <= 4; ++round) {
    add(rec, {1, round, 16.0, 12.0, 6.0, -62.0});
    add(rec, {2, round, 12.0, 9.0, 6.0, -58.0});
  }
  rec.attempts.front().host_dropped_samples = 64;

  const SessionSummary s = summarise(rec, cfg);
  std::set<std::string> ids;
  for (const Warning& w : s.warnings) ids.insert(w.id);
  CHECK(ids.count("W1") == 1);
  CHECK(ids.count("W5") == 1);
  CHECK(ids.count("W8") == 1);
  CHECK(s.warnings.size() >= 3);
}

TEST_CASE("a clean session reports an empty warning list") {
  const Config cfg = warning_config();
  const SessionSummary s = summarise(clean_session(cfg), cfg);
  CHECK(s.warnings.empty());
  for (const AngleSummary& a : s.angles) CHECK(a.warning_ids.empty());
}

TEST_CASE("evaluate_warnings writes only warning_ids on an angle") {
  const Config cfg = warning_config();
  SessionRecord rec = base_record(cfg);
  add(rec, {0, 1, 22.0, 18.0, 6.0, -62.0});
  add(rec, {1, 1, 16.0, 12.0, 6.0, -62.0});

  SessionSummary before = aggregate(rec);
  SessionSummary after = before;
  evaluate_warnings(after, rec, cfg);

  REQUIRE(before.angles.size() == after.angles.size());
  for (std::size_t i = 0; i < before.angles.size(); ++i) {
    AngleSummary stripped = after.angles[i];
    stripped.warning_ids.clear();
    nlohmann::json a;
    nlohmann::json b;
    to_json(a, before.angles[i]);
    to_json(b, stripped);
    CHECK(a.dump() == b.dump());
  }
  // The rankings themselves are untouched.
  CHECK(before.ranking_channel == after.ranking_channel);
  CHECK(before.ranking_audio == after.ranking_audio);
  CHECK(before.ranking_yield == after.ranking_yield);
}

TEST_CASE("the section 10.5 comparability notes are observations, not warnings") {
  Config cfg = warning_config();
  cfg.rounds = 1;
  cfg.angles_deg = {0.0, 180.0};
  SessionRecord rec = base_record(cfg);
  add(rec, {0, 1, 24.0, 18.0, 6.0, -62.0});
  add(rec, {1, 1, 12.0, 9.0, 6.0, -62.0});
  rec.receiver_segments.push_back(ReceiverSegment{"seg2", utc_now(), SourceInfo{}});
  rec.durability_warnings.push_back("the directory fsync failed once");

  const SessionSummary s = summarise(rec, cfg);
  std::string all;
  for (const std::string& note : s.notes) all += note + "\n";

  CHECK(all.find("One round was run") != std::string::npos);
  CHECK(all.find("receiver segments") != std::string::npos);
  CHECK(all.find("power loss") != std::string::npos);
  // The (theta, theta+180) observation: an ideal dipole would receive
  // identically, so a difference is environmental.
  CHECK(all.find("180 degrees apart") != std::string::npos);
  CHECK(all.find("environment") != std::string::npos);
}

}  // TEST_SUITE
