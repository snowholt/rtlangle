// WP2 — record schemas: visit identity, dispositions, the two crash invariants,
// remaining-visit computation, and JSON round-trip. Spec sections 6.6, 11.2,
// 11.3, and 11.4.

#include <doctest/doctest.h>

#include "core/records.h"

#include <nlohmann/json.hpp>

#include <set>
#include <string>

using namespace rtlangle;
using nlohmann::json;

namespace {

AttemptRecord attempt(std::string visit_id, int number, Disposition d, AttemptStatus s) {
  AttemptRecord a;
  a.visit_id = std::move(visit_id);
  a.attempt = number;
  a.disposition = d;
  a.status = s;
  a.segment_id = "seg1";
  return a;
}

SessionRecord three_visit_record() {
  SessionRecord r;
  r.plan.angles_deg = {0.0, 45.0, 90.0};
  r.plan.rounds = 1;
  for (std::size_t i = 0; i < 3; ++i) {
    Visit v;
    v.visit_id = make_visit_id(1, i);
    v.round = 1;
    v.angle_index = i;
    v.planned_deg = r.plan.angles_deg[i];
    r.plan.visits.push_back(v);
  }
  return r;
}

}  // namespace

TEST_SUITE("records") {

TEST_CASE("visit identifiers are index-based and three digits wide") {
  CHECK(make_visit_id(1, 0) == "r1-i000");
  CHECK(make_visit_id(1, 3) == "r1-i003");
  CHECK(make_visit_id(12, 179) == "r12-i179");
  // Two angles that de-duplication keeps apart cannot share an identifier,
  // which an angle-formatted id could not guarantee.
  CHECK(make_visit_id(1, 0) != make_visit_id(1, 1));
}

TEST_CASE("a visit is complete with exactly one accepted or abandoned attempt") {
  SessionRecord r = three_visit_record();
  CHECK_FALSE(visit_complete(r, "r1-i000"));

  r.attempts.push_back(attempt("r1-i000", 1, Disposition::Superseded,
                               AttemptStatus::InsufficientData));
  CHECK_FALSE(visit_complete(r, "r1-i000"));

  r.attempts.push_back(attempt("r1-i000", 2, Disposition::Accepted, AttemptStatus::Ok));
  CHECK(visit_complete(r, "r1-i000"));

  // An accepted attempt with a non-ok status still completes the visit: the
  // record is honest and the angle simply has one fewer contributing capture.
  r.attempts.push_back(attempt("r1-i001", 1, Disposition::Accepted,
                               AttemptStatus::InsufficientData));
  CHECK(visit_complete(r, "r1-i001"));

  // A skipped visit is abandoned, and complete.
  r.attempts.push_back(attempt("r1-i002", 1, Disposition::Abandoned, AttemptStatus::Skipped));
  CHECK(visit_complete(r, "r1-i002"));
}

TEST_CASE("invariant I1: at most one terminal attempt per visit") {
  SessionRecord r = three_visit_record();
  CHECK(invariant_i1(r));

  r.attempts.push_back(attempt("r1-i000", 1, Disposition::Superseded, AttemptStatus::Timeout));
  r.pending_retry = PendingRetry{"r1-i000", 2};
  CHECK(invariant_i1(r));

  r.attempts.push_back(attempt("r1-i000", 2, Disposition::Accepted, AttemptStatus::Ok));
  r.pending_retry.reset();
  CHECK(invariant_i1(r));

  // Two accepted attempts for one visit is the state spec section 11.3's
  // completion rule cannot represent.
  r.attempts.push_back(attempt("r1-i000", 3, Disposition::Accepted, AttemptStatus::Ok));
  CHECK_FALSE(invariant_i1(r));
}

TEST_CASE("invariant I2: pending_retry exists exactly when the latest attempt is superseded") {
  SessionRecord r = three_visit_record();
  CHECK(invariant_i2(r));

  // A superseded attempt with no pending retry would leave the visit
  // unreachable on resume.
  r.attempts.push_back(attempt("r1-i000", 1, Disposition::Superseded, AttemptStatus::Timeout));
  CHECK_FALSE(invariant_i2(r));

  r.pending_retry = PendingRetry{"r1-i000", 2};
  CHECK(invariant_i2(r));

  // A pending retry naming a visit whose latest attempt is not superseded is
  // equally invalid.
  r.attempts.push_back(attempt("r1-i000", 2, Disposition::Accepted, AttemptStatus::Ok));
  CHECK_FALSE(invariant_i2(r));

  r.pending_retry.reset();
  CHECK(invariant_i2(r));
}

TEST_CASE("remaining visits skip complete ones and honour a pending retry") {
  SessionRecord r = three_visit_record();
  auto rem = remaining_visits(r);
  REQUIRE(rem.size() == 3);
  CHECK(rem[0].visit_id == "r1-i000");
  CHECK(rem[0].next_attempt == 1);

  r.attempts.push_back(attempt("r1-i000", 1, Disposition::Accepted, AttemptStatus::Ok));
  rem = remaining_visits(r);
  REQUIRE(rem.size() == 2);
  CHECK(rem[0].visit_id == "r1-i001");

  // A retry commit leaves the visit incomplete and names the attempt to resume
  // at, which survives a crash because both landed in one commit.
  r.attempts.push_back(attempt("r1-i001", 1, Disposition::Superseded, AttemptStatus::Timeout));
  r.pending_retry = PendingRetry{"r1-i001", 2};
  rem = remaining_visits(r);
  REQUIRE(rem.size() == 2);
  CHECK(rem[0].visit_id == "r1-i001");
  CHECK(rem[0].next_attempt == 2);
  CHECK(rem[1].visit_id == "r1-i002");
  CHECK(rem[1].next_attempt == 1);
}

TEST_CASE("enumerations round-trip through their wire names") {
  for (auto d : {Disposition::Accepted, Disposition::Superseded, Disposition::Abandoned}) {
    CHECK(parse_disposition(to_string(d)).value() == d);
  }
  for (auto s : {AttemptStatus::Ok, AttemptStatus::InsufficientData,
                 AttemptStatus::NoiseFloorUnreliable, AttemptStatus::NoiseFloorUnidentifiable,
                 AttemptStatus::Clipped, AttemptStatus::Timeout, AttemptStatus::SourceError,
                 AttemptStatus::InsufficientSamples, AttemptStatus::Skipped,
                 AttemptStatus::Cancelled}) {
    CHECK(parse_attempt_status(to_string(s)).value() == s);
  }
  for (auto s : {SessionState::Running, SessionState::Paused, SessionState::Completed,
                 SessionState::Aborted}) {
    CHECK(parse_session_state(to_string(s)).value() == s);
  }
  CHECK_FALSE(parse_attempt_status("almost_ok").has_value());
  CHECK_FALSE(parse_session_state("finished").has_value());

  CHECK(to_string(CommitOutcome::Committed) == "committed");
  CHECK(to_string(CommitOutcome::CommittedNotDurable) == "committed_not_durable");
  CHECK(to_string(CommitOutcome::NotCommitted) == "not_committed");
  CHECK(to_string(CommitOutcome::Indeterminate) == "indeterminate");
}

TEST_CASE("an attempt round-trips every field, and absent scores stay null") {
  AttemptRecord a = attempt("r2-i005", 2, Disposition::Superseded, AttemptStatus::InsufficientData);
  a.round = 2;
  a.angle_index = 5;
  a.planned_deg = 45.001;
  a.status_detail = "the audio noise floor was unreliable";
  a.actual_deg = 44.0;
  a.angle_deviation_deg = 1.001;
  a.started_utc = "2026-08-19T14:36:07Z";
  a.duration_s = 60.0;
  a.wall_duration_s = 60.4;
  a.applied_sample_rate_hz = 1024000;
  a.applied_center_hz = 118600000;
  a.applied_gain_tenth_db = 496;
  a.frames_total = 3745;
  a.frames_active = 412;
  a.active_probe_fraction = 0.118;
  a.dynamic_range_db = 21.4;
  a.noise_floor_dbfs = -62.41;
  a.audio_noise_floor_dbfs = -58.9;
  a.host_dropped_samples = 7;
  a.clipped_fraction = 1e-6;
  a.valid_event_count = 7;
  a.valid_audio_event_count = 6;
  a.truncated_event_count = 1;
  a.audio_insufficient = true;
  a.events_total = 9;
  a.events_retained = 4;
  a.events_per_minute = 9.0;
  a.detected_fraction = 0.11;
  a.capture_score_channel_db = 18.2;
  // The audio score is absent while the channel score is present: exactly the
  // shape spec section 9.4.1 requires, and the reason both are optional.
  a.capture_score_audio_db.reset();
  a.channel_snr = Percentiles{18.2, 12.9, 15.4, 21.0, 23.8};
  a.note = "cable routed along the balcony rail";

  EventRecord e;
  e.start_s = 3.42;
  e.duration_s = 2.15;
  e.utc = "2026-08-19T14:36:10Z";
  e.carrier_offset_hz = -213.4;
  e.signal_power_dbfs = -44.0;
  e.channel_snr_db = 18.4;
  e.audio_snr_db.reset();
  e.valid = true;
  e.truncated = false;
  e.audio_snr_valid = false;
  a.events.push_back(e);

  json j1;
  to_json(j1, a);
  CHECK(j1.at("capture_score_audio_db").is_null());
  CHECK(j1.at("audio_snr_db").is_null());
  CHECK(j1.at("events").at(0).at("audio_snr_db").is_null());
  CHECK(j1.at("disposition").get<std::string>() == "superseded");

  AttemptRecord back;
  from_json(j1, back);
  json j2;
  to_json(j2, back);
  CHECK(j1.dump() == j2.dump());
  CHECK_FALSE(back.capture_score_audio_db.has_value());
  CHECK(back.capture_score_channel_db.value() == doctest::Approx(18.2));
  CHECK(back.planned_deg == doctest::Approx(45.001));
  // The retention cap is visible in the record rather than looking like a quiet
  // capture.
  CHECK(back.events_total == 9);
  CHECK(back.events_retained == 4);
}

TEST_CASE("a session record round-trips") {
  SessionRecord r = three_visit_record();
  r.session_id = "20260819-143000-airband";
  r.started_utc = "2026-08-19T14:30:00Z";
  r.config.center_hz = 118600000;
  r.durability_warnings.push_back("directory fsync failed at attempt 3");
  ReceiverSegment seg;
  seg.segment_id = "seg1";
  seg.opened_utc = "2026-08-19T14:30:02Z";
  seg.baseline.driver = "synthetic";
  seg.baseline.applied_gain_tenth_db = 496;
  r.receiver_segments.push_back(seg);
  r.attempts.push_back(attempt("r1-i000", 1, Disposition::Accepted, AttemptStatus::Ok));
  r.pending_retry.reset();

  json j1;
  to_json(j1, r);
  CHECK(j1.at("state").get<std::string>() == "running");
  CHECK(j1.at("finished_utc").is_null());
  CHECK(j1.at("summary").is_null());
  CHECK(j1.at("pending_retry").is_null());
  CHECK(j1.at("schema_version").get<int>() == 1);

  SessionRecord back;
  from_json(j1, back);
  json j2;
  to_json(j2, back);
  CHECK(j1.dump() == j2.dump());
}

TEST_CASE("SessionSummary carries no resolution field and no exploration flag") {
  // Structural, over the serialisation rather than by review: spec decision Q1
  // removed every construct that existed to justify a decision.
  SessionSummary s;
  AngleSummary a;
  a.planned_deg = 45.0;
  a.score_channel_db = 18.0;
  a.yield_events_per_min = 7.0;
  s.angles.push_back(a);
  s.ranking_channel = {45.0};
  s.ranking_audio = {45.0};
  s.ranking_yield = {45.0};
  s.warnings.push_back(Warning{"W1", "one capture only", {45.0}});

  json j;
  to_json(j, s);
  const std::string dumped = j.dump();
  for (const char* forbidden : {"resolved", "resolution", "winner"}) {
    CHECK_MESSAGE(dumped.find(forbidden) == std::string::npos, forbidden);
  }

  std::set<std::string> keys;
  for (auto it = j.begin(); it != j.end(); ++it) keys.insert(it.key());
  for (const auto& k : keys) {
    CHECK(k.find("resolved") == std::string::npos);
    CHECK(k.find("resolution") == std::string::npos);
    CHECK(k.find("winner") == std::string::npos);
  }

  SessionSummary back;
  from_json(j, back);
  json j2;
  to_json(j2, back);
  CHECK(j.dump() == j2.dump());
}

TEST_CASE("an unknown enumeration value in a stored record is rejected") {
  SessionRecord r = three_visit_record();
  json j;
  to_json(j, r);
  j["state"] = "half-finished";
  SessionRecord back;
  CHECK_THROWS_AS(from_json(j, back), json::exception);
}

}  // TEST_SUITE
