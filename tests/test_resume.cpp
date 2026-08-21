// WP6 — resume, migration, the resume-conflict classification, and the derived
// artifacts' recoverability. Spec sections 11.1.3 and 11.5.

#include <doctest/doctest.h>

#include "core/records.h"
#include "core/utc.h"
#include "persist/csv.h"
#include "persist/json_session_store.h"
#include "persist/session_loader.h"
#include "tests/support/temp_dir.h"

#include <nlohmann/json.hpp>

#include <set>
#include <string>

using namespace rtlangle;
using namespace rtlangle::test;
using nlohmann::json;

namespace {

namespace fs = std::filesystem;

SessionRecord planned_record(int visits) {
  SessionRecord r;
  r.session_id = "20260819-143000-airband";
  r.started_utc = utc_now();
  r.config.center_hz = 118350000;
  r.plan.angles_deg = {0.0, 45.0, 90.0};
  r.plan.rounds = 1;
  for (int i = 0; i < visits; ++i) {
    Visit v;
    v.visit_id = make_visit_id(1, static_cast<std::size_t>(i));
    v.round = 1;
    v.angle_index = static_cast<std::size_t>(i);
    v.planned_deg = r.plan.angles_deg[static_cast<std::size_t>(i % 3)];
    r.plan.visits.push_back(v);
  }
  return r;
}

VisitCommit commit_for(const std::string& visit_id, int attempt, Disposition d,
                       AttemptStatus s) {
  VisitCommit c;
  c.attempt.visit_id = visit_id;
  c.attempt.attempt = attempt;
  c.attempt.segment_id = "seg1";
  c.attempt.status = s;
  c.attempt.started_utc = utc_now();
  c.disposition = d;
  return c;
}

// Builds a directory holding a session at a chosen state.
struct Session {
  TempDir temp;
  fs::path dir;

  Session() { dir = temp.child("session"); }

  std::unique_ptr<JsonSessionStore> open_new(int visits = 3) {
    std::string error;
    auto session_dir = SessionDir::create(dir, error);
    REQUIRE_MESSAGE(session_dir.has_value(), error);
    auto store = std::make_unique<JsonSessionStore>(std::move(*session_dir),
                                                    planned_record(visits));
    store->set_report_renderer(
        [](const SessionRecord&, const SessionSummary& s) {
          return std::string(s.partial ? "PARTIAL\n" : "COMPLETE\n");
        });
    return store;
  }
};

}  // namespace

TEST_SUITE("resume") {

TEST_CASE("a paused session reopens, clears the partial summary, and returns to running") {
  Session s;
  {
    auto store = s.open_new();
    const std::string segment = store->begin_receiver_segment(SourceInfo{});
    REQUIRE(store->commit_visit(commit_for("r1-i000", 1, Disposition::Accepted,
                                           AttemptStatus::Ok))
                .outcome == CommitOutcome::Committed);
    SessionSummary partial;
    REQUIRE(store->pause(partial).outcome == CommitOutcome::Committed);
    (void)segment;
  }

  LoadResult loaded = load_session(s.dir);
  REQUIRE_MESSAGE(!loaded.error.has_value(), loaded.error.value_or(""));
  CHECK(loaded.record.state == SessionState::Running);
  CHECK_FALSE(loaded.record.summary.has_value());
  CHECK_FALSE(loaded.record.summary_partial);
  CHECK_FALSE(loaded.record.finished_utc.has_value());

  const auto remaining = remaining_visits(loaded.record);
  REQUIRE(remaining.size() == 2);
  CHECK(remaining[0].visit_id == "r1-i001");
}

TEST_CASE("a completed session is refused, naming finished_utc") {
  Session s;
  {
    auto store = s.open_new();
    (void)store->begin_receiver_segment(SourceInfo{});
    SessionSummary summary;
    REQUIRE(store->finalize(summary).outcome == CommitOutcome::Committed);
  }
  const LoadResult loaded = load_session(s.dir);
  REQUIRE(loaded.error.has_value());
  CHECK(loaded.error->find("completed") != std::string::npos);
  CHECK(loaded.error->find("20") != std::string::npos);   // the timestamp
  CHECK(loaded.error->find("rtlangle report") != std::string::npos);
}

TEST_CASE("an aborted session is refused, naming the reason") {
  Session s;
  {
    auto store = s.open_new();
    (void)store->begin_receiver_segment(SourceInfo{});
    SessionSummary partial;
    REQUIRE(store->abort(partial, "the record could not be trusted to continue").outcome ==
            CommitOutcome::Committed);
  }
  const LoadResult loaded = load_session(s.dir);
  REQUIRE(loaded.error.has_value());
  CHECK(loaded.error->find("aborted") != std::string::npos);
  CHECK(loaded.error->find("could not be trusted") != std::string::npos);
}

TEST_CASE("a running record left by a crash reopens and reports the unclean shutdown") {
  Session s;
  {
    auto store = s.open_new();
    (void)store->begin_receiver_segment(SourceInfo{});
    REQUIRE(store->commit_visit(commit_for("r1-i000", 1, Disposition::Accepted,
                                           AttemptStatus::Ok))
                .outcome == CommitOutcome::Committed);
    // No pause, no finalize: the process simply ends.
  }
  const LoadResult loaded = load_session(s.dir);
  REQUIRE_MESSAGE(!loaded.error.has_value(), loaded.error.value_or(""));
  CHECK(loaded.record.state == SessionState::Running);
  bool reported = false;
  for (const auto& r : loaded.repairs) {
    if (r.find("not shut down cleanly") != std::string::npos) reported = true;
  }
  CHECK(reported);
  CHECK(loaded.record.attempts.size() == 1);
}

TEST_CASE("a retry survives a simulated crash and resumes at the right attempt") {
  Session s;
  {
    auto store = s.open_new();
    (void)store->begin_receiver_segment(SourceInfo{});
    VisitCommit retry = commit_for("r1-i000", 1, Disposition::Superseded,
                                   AttemptStatus::InsufficientData);
    retry.pending_retry = PendingRetry{"r1-i000", 2};
    REQUIRE(store->commit_visit(retry).outcome == CommitOutcome::Committed);
    // The store is discarded without any further write: a crash.
  }
  const LoadResult loaded = load_session(s.dir);
  REQUIRE_MESSAGE(!loaded.error.has_value(), loaded.error.value_or(""));
  REQUIRE(loaded.record.pending_retry.has_value());
  CHECK(loaded.record.pending_retry->visit_id == "r1-i000");

  const auto remaining = remaining_visits(loaded.record);
  REQUIRE(remaining.size() == 3);
  CHECK(remaining[0].visit_id == "r1-i000");
  CHECK(remaining[0].next_attempt == 2);
}

TEST_CASE("a skipped visit counts as complete for resume") {
  Session s;
  {
    auto store = s.open_new();
    (void)store->begin_receiver_segment(SourceInfo{});
    REQUIRE(store->commit_visit(commit_for("r1-i000", 1, Disposition::Abandoned,
                                           AttemptStatus::Skipped))
                .outcome == CommitOutcome::Committed);
  }
  const LoadResult loaded = load_session(s.dir);
  REQUIRE(!loaded.error.has_value());
  CHECK(visit_complete(loaded.record, "r1-i000"));
  const auto remaining = remaining_visits(loaded.record);
  for (const auto& v : remaining) CHECK(v.visit_id != "r1-i000");
}

TEST_CASE("a record violating an invariant is an error, not a repair") {
  Session s;
  {
    auto store = s.open_new();
    (void)store->begin_receiver_segment(SourceInfo{});
    REQUIRE(store->commit_visit(commit_for("r1-i000", 1, Disposition::Accepted,
                                           AttemptStatus::Ok))
                .outcome == CommitOutcome::Committed);
  }
  // Hand-edit the record into a state the completion rule cannot represent.
  json doc = json::parse(read_whole(s.dir / kSessionFileName));
  doc["attempts"].push_back(doc["attempts"][0]);
  write_whole(s.dir / kSessionFileName, doc.dump(2));

  const LoadResult loaded = load_session(s.dir);
  REQUIRE(loaded.error.has_value());
  CHECK(loaded.error->find("I1") != std::string::npos);
}

TEST_CASE("a stale, truncated, or missing CSV is regenerated from the JSON") {
  auto attempts_committed = [](Session& s, int n) {
    auto store = s.open_new();
    (void)store->begin_receiver_segment(SourceInfo{});
    for (int i = 0; i < n; ++i) {
      REQUIRE(store->commit_visit(commit_for(make_visit_id(1, static_cast<std::size_t>(i)),
                                             1, Disposition::Accepted, AttemptStatus::Ok))
                  .outcome == CommitOutcome::Committed);
    }
  };

  SUBCASE("truncated") {
    Session s;
    attempts_committed(s, 3);
    write_whole(s.dir / kMeasurementsFileName, csv_header() + "partial,row");
    const LoadResult loaded = load_session(s.dir);
    REQUIRE(!loaded.error.has_value());
    CHECK(csv_row_count(read_whole(s.dir / kMeasurementsFileName)) == 3);
  }
  SUBCASE("deleted") {
    Session s;
    attempts_committed(s, 3);
    fs::remove(s.dir / kMeasurementsFileName);
    const LoadResult loaded = load_session(s.dir);
    REQUIRE(!loaded.error.has_value());
    CHECK(fs::exists(s.dir / kMeasurementsFileName));
    CHECK(csv_row_count(read_whole(s.dir / kMeasurementsFileName)) == 3);
  }
  SUBCASE("malformed") {
    Session s;
    attempts_committed(s, 3);
    write_whole(s.dir / kMeasurementsFileName, std::string("\xff\xfe not a csv at all"));
    const LoadResult loaded = load_session(s.dir);
    REQUIRE(!loaded.error.has_value());
    CHECK(csv_row_count(read_whole(s.dir / kMeasurementsFileName)) == 3);
    bool noted = false;
    for (const auto& r : loaded.repairs) {
      if (r.find("regenerated measurements.csv") != std::string::npos) noted = true;
    }
    CHECK(noted);
  }
  SUBCASE("agreeing, so it is left alone") {
    Session s;
    attempts_committed(s, 3);
    const std::string before = read_whole(s.dir / kMeasurementsFileName);
    const LoadResult loaded = load_session(s.dir);
    REQUIRE(!loaded.error.has_value());
    CHECK(read_whole(s.dir / kMeasurementsFileName) == before);
  }
}

TEST_CASE("a missing report.txt is a repair note, never an error, and is never read back") {
  Session s;
  {
    auto store = s.open_new();
    (void)store->begin_receiver_segment(SourceInfo{});
    REQUIRE(store->commit_visit(commit_for("r1-i000", 1, Disposition::Accepted,
                                           AttemptStatus::Ok))
                .outcome == CommitOutcome::Committed);
    SessionSummary partial;
    REQUIRE(store->pause(partial).outcome == CommitOutcome::Committed);
  }
  REQUIRE(fs::exists(s.dir / kReportFileName));
  fs::remove(s.dir / kReportFileName);

  const LoadResult loaded = load_session(s.dir);
  CHECK_FALSE(loaded.error.has_value());
  bool noted = false;
  for (const auto& r : loaded.repairs) {
    if (r.find("report.txt") != std::string::npos) noted = true;
  }
  CHECK(noted);
  // Nothing in the reloaded record came from report.txt.
  CHECK(loaded.record.attempts.size() == 1);
}

TEST_CASE("a schema version newer than this build is rejected naming both versions") {
  Session s;
  {
    auto store = s.open_new();
    (void)store->begin_receiver_segment(SourceInfo{});
    REQUIRE(store->commit_visit(commit_for("r1-i000", 1, Disposition::Accepted,
                                           AttemptStatus::Ok))
                .outcome == CommitOutcome::Committed);
  }
  json doc = json::parse(read_whole(s.dir / kSessionFileName));
  doc["schema_version"] = 2;
  write_whole(s.dir / kSessionFileName, doc.dump(2));

  const LoadResult loaded = load_session(s.dir);
  REQUIRE(loaded.error.has_value());
  CHECK(loaded.error->find("2") != std::string::npos);
  CHECK(loaded.error->find("1") != std::string::npos);
}

TEST_CASE("a version-zero fixture migrates and is then loadable") {
  Session s;
  {
    auto store = s.open_new();
    (void)store->begin_receiver_segment(SourceInfo{});
    REQUIRE(store->commit_visit(commit_for("r1-i000", 1, Disposition::Accepted,
                                           AttemptStatus::Ok))
                .outcome == CommitOutcome::Committed);
  }
  // The synthetic version-0 shape: no schema_version, and none of the fields
  // that version 1 added.
  json doc = json::parse(read_whole(s.dir / kSessionFileName));
  doc.erase("schema_version");
  doc.erase("durability_warnings");
  doc.erase("receiver_segments");
  doc.erase("abort_reason");
  doc.erase("summary_partial");
  doc.erase("pending_retry");
  write_whole(s.dir / kSessionFileName, doc.dump(2));

  // The mechanism, exercised directly.
  json standalone = doc;
  std::string error;
  CHECK(migrate_record(standalone, error));
  CHECK(standalone.at("schema_version").get<int>() == kSchemaVersion);
  CHECK(standalone.at("durability_warnings").is_array());
  // The migration produces a record satisfying the CURRENT invariants: version 0
  // predates receiver segments, so one is synthesised to carry the attempts that
  // name it.
  REQUIRE(standalone.at("receiver_segments").size() == 1);
  CHECK(standalone.at("receiver_segments")[0].at("segment_id").get<std::string>() == "seg1");

  const LoadResult loaded = load_session(s.dir);
  REQUIRE_MESSAGE(!loaded.error.has_value(), loaded.error.value_or(""));
  CHECK(loaded.record.schema_version == kSchemaVersion);
  CHECK(loaded.record.attempts.size() == 1);
}

TEST_CASE("resume conflicts: every field class behaves as spec section 11.5 requires") {
  Config stored;
  stored.center_hz = 118350000;

  // Nothing overridden: no conflict.
  CHECK(resume_conflicts(stored, stored, {}).empty());

  // Every ExperimentDefining field produces a conflict when overridden.
  int defining = 0;
  for (const FieldSpec& f : field_registry()) {
    if (resume_class(f.key) != FieldClass::ExperimentDefining) continue;
    ++defining;
    Config cli = stored;
    // A change that is visible in the serialisation, whatever the type.
    json j;
    to_json(j, cli);
    if (j.at(std::string(f.key)).is_boolean()) {
      j[std::string(f.key)] = !j.at(std::string(f.key)).get<bool>();
    } else if (j.at(std::string(f.key)).is_number()) {
      j[std::string(f.key)] = j.at(std::string(f.key)).get<double>() + 1.0;
    } else if (j.at(std::string(f.key)).is_string()) {
      j[std::string(f.key)] = "changed";
    } else if (j.at(std::string(f.key)).is_array()) {
      j[std::string(f.key)] = json::array({1.0, 2.0});
    } else if (j.at(std::string(f.key)).is_null()) {
      j[std::string(f.key)] = 200000000;
    }
    // Some fields have a constrained vocabulary; give them a legal alternative.
    if (f.key == "order") j["order"] = "forward";
    if (f.key == "report_metric") j["report_metric"] = "audio";
    if (f.key == "gain_tenth_db") j["gain_tenth_db"] = 300;
    if (f.key == "source_spec") j["source_spec"] = "synthetic";

    Config changed;
    from_json(j, changed);
    const auto conflicts = resume_conflicts(stored, changed, {std::string(f.key)});
    CHECK_MESSAGE(conflicts.size() == 1, f.key);
    if (!conflicts.empty()) {
      CHECK_MESSAGE(conflicts[0].field == f.key, f.key);
      CHECK_MESSAGE(conflicts[0].message.find(std::string(f.flag)) != std::string::npos, f.key);
    }
  }
  CHECK(defining > 30);

  // Every ReportOrOperational and PathDetermined field does not.
  for (const FieldSpec& f : field_registry()) {
    const FieldClass c = resume_class(f.key);
    if (c == FieldClass::ExperimentDefining) continue;
    Config cli = stored;
    cli.min_captures_advisory = 8;
    cli.settle_s = 30.0;
    cli.no_color = true;
    cli.setup_note = "moved the mount";
    cli.session_root = "elsewhere";
    cli.label = "different";
    cli.resume_dir = "somewhere";
    cli.scan_passes = 9;
    cli.max_retained_events = 100;
    const auto conflicts = resume_conflicts(stored, cli, {std::string(f.key)});
    CHECK_MESSAGE(conflicts.empty(), f.key);
  }
}

TEST_CASE("every section 7.4 field is overridable on a resume, asserted as a set") {
  const std::set<std::string> section_74 = {"report_metric", "min_captures_advisory",
                                            "min_effect_db", "yield_concordance_ratio",
                                            "noise_drift_warn_db"};
  Config stored;
  stored.center_hz = 118350000;
  Config cli = stored;
  cli.report_metric = Metric::Audio;
  cli.min_captures_advisory = 10;
  cli.min_effect_db = 3.0;
  cli.yield_concordance_ratio = 0.5;
  cli.noise_drift_warn_db = 10.0;

  const auto conflicts = resume_conflicts(stored, cli, section_74);
  CHECK(conflicts.empty());

  // And the classification itself, as a set equality.
  std::set<std::string> operational;
  for (const auto& key : section_74) {
    if (resume_class(key) == FieldClass::ReportOrOperational) operational.insert(key);
  }
  CHECK(operational == section_74);
}

TEST_CASE("a new session directory is created with a timestamp and a sanitised label") {
  TempDir temp;
  std::string session_id;
  std::string error;

  auto first = create_session_dir(temp.child("sessions"), "../../etc/passwd", session_id, error);
  REQUIRE_MESSAGE(first.has_value(), error);
  CHECK(session_id.find("etc-passwd") != std::string::npos);
  CHECK(session_id.find('/') == std::string::npos);
  CHECK(session_id.find("..") == std::string::npos);

  // A collision within the same second gets a numeric suffix rather than
  // reusing an existing directory.
  std::string second_id;
  auto second = create_session_dir(temp.child("sessions"), "../../etc/passwd", second_id, error);
  REQUIRE_MESSAGE(second.has_value(), error);
  CHECK(second_id != session_id);

  // A label that sanitises to nothing is simply dropped.
  std::string third_id;
  auto third = create_session_dir(temp.child("sessions"), "..", third_id, error);
  REQUIRE_MESSAGE(third.has_value(), error);
  CHECK(third_id.find("..") == std::string::npos);
}

TEST_CASE("a resumed store continues the segment numbering") {
  Session s;
  {
    auto store = s.open_new();
    CHECK(store->begin_receiver_segment(SourceInfo{}) == "seg1");
    REQUIRE(store->commit_visit(commit_for("r1-i000", 1, Disposition::Accepted,
                                           AttemptStatus::Ok))
                .outcome == CommitOutcome::Committed);
    SessionSummary partial;
    REQUIRE(store->pause(partial).outcome == CommitOutcome::Committed);
  }
  LoadResult loaded = load_session(s.dir);
  REQUIRE(!loaded.error.has_value());
  JsonSessionStore resumed(std::move(loaded.dir), loaded.record);
  CHECK(resumed.begin_receiver_segment(SourceInfo{}) == "seg2");
  REQUIRE(resumed.commit_visit(commit_for("r1-i001", 1, Disposition::Accepted,
                                          AttemptStatus::Ok))
              .outcome == CommitOutcome::Committed);
  CHECK(resumed.record().receiver_segments.size() == 2);
  CHECK(resumed.record().attempts.back().segment_id == "seg1");   // as committed
}

}  // TEST_SUITE

TEST_SUITE("resume") {

TEST_CASE("an attempt naming a segment the record does not hold is a load error") {
  Session s;
  {
    auto store = s.open_new();
    (void)store->begin_receiver_segment(SourceInfo{});
    REQUIRE(store->commit_visit(commit_for("r1-i000", 1, Disposition::Accepted,
                                           AttemptStatus::Ok))
                .outcome == CommitOutcome::Committed);
  }
  json doc = json::parse(read_whole(s.dir / kSessionFileName));
  doc["attempts"][0]["segment_id"] = "seg9";
  write_whole(s.dir / kSessionFileName, doc.dump(2));

  const LoadResult loaded = load_session(s.dir);
  REQUIRE(loaded.error.has_value());
  CHECK(loaded.error->find("seg9") != std::string::npos);
  CHECK(loaded.error->find("W7") != std::string::npos);
}

TEST_CASE("the read-only loader reaches the terminal sessions load_session refuses") {
  Session s;
  {
    auto store = s.open_new();
    (void)store->begin_receiver_segment(SourceInfo{});
    REQUIRE(store->commit_visit(commit_for("r1-i000", 1, Disposition::Accepted,
                                           AttemptStatus::Ok))
                .outcome == CommitOutcome::Committed);
    SessionSummary summary;
    REQUIRE(store->finalize(summary).outcome == CommitOutcome::Committed);
  }
  const std::string before = read_whole(s.dir / kSessionFileName);

  CHECK(load_session(s.dir).error.has_value());

  const LoadResult report = load_session_read_only(s.dir);
  REQUIRE_MESSAGE(!report.error.has_value(), report.error.value_or(""));
  // The state is preserved exactly: re-rendering a finished session must not
  // change anything it recorded.
  CHECK(report.record.state == SessionState::Completed);
  CHECK(report.record.finished_utc.has_value());
  CHECK(report.record.attempts.size() == 1);
  CHECK(report.repairs.empty());
  CHECK(read_whole(s.dir / kSessionFileName) == before);
}

TEST_CASE("the read-only loader still refuses a record that violates an invariant") {
  Session s;
  {
    auto store = s.open_new();
    (void)store->begin_receiver_segment(SourceInfo{});
    REQUIRE(store->commit_visit(commit_for("r1-i000", 1, Disposition::Accepted,
                                           AttemptStatus::Ok))
                .outcome == CommitOutcome::Committed);
  }
  json doc = json::parse(read_whole(s.dir / kSessionFileName));
  doc["attempts"].push_back(doc["attempts"][0]);
  write_whole(s.dir / kSessionFileName, doc.dump(2));

  const LoadResult loaded = load_session_read_only(s.dir);
  REQUIRE(loaded.error.has_value());
  CHECK(loaded.error->find("I1") != std::string::npos);
}

TEST_CASE("the clock is injectable, so a generated session can be reproduced byte for byte") {
  // reset_clock runs whatever happens below: the clock is process-global, and a
  // frozen one would make later tests in this binary fail for a reason that has
  // nothing to do with them.
  struct Restore {
    ~Restore() { reset_clock(); }
  } restore;

  const auto pinned = std::chrono::system_clock::from_time_t(1787000000);
  set_clock([pinned] { return pinned; });
  CHECK(utc_now() == format_utc(pinned));
  const std::string first = session_stamp(now());

  reset_clock();
  CHECK(utc_now() != format_utc(pinned));

  set_clock([pinned] { return pinned; });
  CHECK(session_stamp(now()) == first);
}

}  // TEST_SUITE
