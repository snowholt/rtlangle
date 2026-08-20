// WP6 — canonical persistence: atomicity, the four commit outcomes at every
// boundary, EINTR-only retry, reconciliation, the derived artifacts, receiver
// segments, the session lifecycle, label sanitisation, and the filesystem
// hardening. Spec sections 6.6, 11, and 13.

#include <doctest/doctest.h>

#include "core/records.h"
#include "core/utc.h"
#include "persist/atomic_write.h"
#include "persist/csv.h"
#include "persist/json_session_store.h"
#include "persist/path_safety.h"
#include "persist/session_loader.h"
#include "tests/support/fault_store.h"
#include "tests/support/temp_dir.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <random>
#include <set>
#include <string>
#include <sys/stat.h>

using namespace rtlangle;
using namespace rtlangle::test;
using nlohmann::json;

namespace {

namespace fs = std::filesystem;

SessionRecord fresh_record(int visits = 3) {
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
    v.planned_deg = r.plan.angles_deg[static_cast<std::size_t>(i)];
    r.plan.visits.push_back(v);
  }
  return r;
}

VisitCommit accepted(const std::string& visit_id, int attempt, const std::string& segment) {
  VisitCommit c;
  c.attempt.visit_id = visit_id;
  c.attempt.attempt = attempt;
  c.attempt.segment_id = segment;
  c.attempt.status = AttemptStatus::Ok;
  c.attempt.started_utc = utc_now();
  c.attempt.capture_score_channel_db = 18.0;
  c.disposition = Disposition::Accepted;
  return c;
}

VisitCommit retried(const std::string& visit_id, int attempt, const std::string& segment) {
  VisitCommit c = accepted(visit_id, attempt, segment);
  c.attempt.status = AttemptStatus::InsufficientData;
  c.attempt.capture_score_channel_db.reset();
  c.disposition = Disposition::Superseded;
  c.pending_retry = PendingRetry{visit_id, attempt + 1};
  return c;
}

SourceInfo baseline() {
  SourceInfo i;
  i.driver = "synthetic";
  i.device_name = "synthetic AM airband generator";
  i.applied_sample_rate_hz = 1024000;
  i.applied_center_hz = 118600000;
  i.applied_gain_tenth_db = 496;
  return i;
}

// A store over a freshly created session directory.
struct Fixture {
  TempDir temp;
  std::unique_ptr<JsonSessionStore> store;
  fs::path dir;

  explicit Fixture(FileOps ops = default_file_ops(), int visits = 3) {
    std::string error;
    dir = temp.child("session");
    auto session_dir = SessionDir::create(dir, error);
    REQUIRE_MESSAGE(session_dir.has_value(), error);
    store = std::make_unique<JsonSessionStore>(std::move(*session_dir), fresh_record(visits),
                                               std::move(ops));
  }

  json on_disk() const { return json::parse(read_whole(dir / kSessionFileName)); }
  bool has_session_file() const { return fs::exists(dir / kSessionFileName); }
  int  temp_files() const {
    int n = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
      if (e.path().filename().string().find(".tmp.") != std::string::npos) ++n;
    }
    return n;
  }
};

}  // namespace

TEST_SUITE("persist") {

TEST_CASE("after every commit the record parses and no temporary file remains") {
  Fixture f;
  const std::string segment = f.store->begin_receiver_segment(baseline());

  for (int i = 0; i < 50; ++i) {
    const auto r = f.store->commit_visit(accepted("r1-i000", i + 1, segment));
    REQUIRE(r.outcome == CommitOutcome::Committed);
    const json parsed = f.on_disk();
    CHECK(parsed.at("attempts").size() == static_cast<std::size_t>(i + 1));
    CHECK(f.temp_files() == 0);
  }
}

TEST_CASE("the commit-outcome matrix, one case per boundary") {
  struct Row {
    FaultPoint point;
    CommitOutcome expected;
    const char* name;
  };
  const Row rows[] = {
      {FaultPoint::BeforeTempWrite, CommitOutcome::NotCommitted, "before temp write"},
      {FaultPoint::AfterTempBeforeSync, CommitOutcome::NotCommitted,
       "after temp write, before fsync"},
      {FaultPoint::AtFileSync, CommitOutcome::NotCommitted, "at fsync(file)"},
      {FaultPoint::AtRename, CommitOutcome::NotCommitted, "at renameat, classifiable"},
      {FaultPoint::AtDirSync, CommitOutcome::CommittedNotDurable, "at fsync(dirfd)"},
      {FaultPoint::AtRenameUnclassified, CommitOutcome::Indeterminate,
       "at renameat, unclassifiable"},
  };

  for (const Row& row : rows) {
    CAPTURE(row.name);
    Fixture f;
    const std::string segment = f.store->begin_receiver_segment(baseline());
    // One clean commit first, so there is a canonical record to compare against.
    REQUIRE(f.store->commit_visit(accepted("r1-i000", 1, segment)).outcome ==
            CommitOutcome::Committed);
    const std::string before = read_whole(f.dir / kSessionFileName);

    // Rebuild the store over the same directory with the fault armed.
    std::string error;
    auto reopened = SessionDir::open_existing(f.dir, error);
    REQUIRE(reopened.has_value());
    JsonSessionStore faulty(std::move(*reopened), f.store->record(),
                            faulty_ops(row.point, EACCES));
    const CommitResult result = faulty.commit_visit(accepted("r1-i001", 1, segment));

    CHECK_MESSAGE(result.outcome == row.expected,
                  row.name << " gave " << to_string(result.outcome));

    const std::string after = read_whole(f.dir / kSessionFileName);
    if (row.expected == CommitOutcome::NotCommitted) {
      // The canonical record is byte-for-byte what it was: nothing was written,
      // so no measurement is lost.
      CHECK_MESSAGE(after == before, row.name);
    }
    if (row.expected == CommitOutcome::CommittedNotDurable) {
      // NOT a failed commit: the attempt IS readable afterwards, and a
      // durability warning is recorded so the report can say so.
      const json parsed = json::parse(after);
      CHECK(parsed.at("attempts").size() == 2);
      CHECK(faulty.record().attempts.size() == 2);
      CHECK_FALSE(faulty.record().durability_warnings.empty());
    }

    // Invariants I1 and I2 hold after every injection.
    SessionRecord reloaded;
    from_json(json::parse(after), reloaded);
    CHECK_MESSAGE(invariant_i1(reloaded), row.name);
    CHECK_MESSAGE(invariant_i2(reloaded), row.name);
  }
}

TEST_CASE("a failure regenerating the derived CSV leaves the commit durable") {
  Fixture f;
  const std::string segment = f.store->begin_receiver_segment(baseline());
  REQUIRE(f.store->commit_visit(accepted("r1-i000", 1, segment)).outcome ==
          CommitOutcome::Committed);

  std::string error;
  auto reopened = SessionDir::open_existing(f.dir, error);
  REQUIRE(reopened.has_value());
  JsonSessionStore faulty(std::move(*reopened), f.store->record(),
                          faulty_ops(FaultPoint::BeforeTempWrite, EACCES, kMeasurementsFileName));
  const CommitResult result = faulty.commit_visit(accepted("r1-i001", 1, segment));

  // Everything after the commit point is bookkeeping on a commit that already
  // happened.
  CHECK(result.outcome == CommitOutcome::Committed);
  CHECK(f.on_disk().at("attempts").size() == 2);
  REQUIRE(faulty.repairs().size() == 1);
  CHECK(faulty.repairs()[0].find("measurements.csv") != std::string::npos);
}

TEST_CASE("an unclassifiable rename errno is Indeterminate and EIO is one") {
  CHECK(renameat_errno_is_classifiable(EACCES));
  CHECK(renameat_errno_is_classifiable(ENOENT));
  CHECK(renameat_errno_is_classifiable(ENOSPC));
  CHECK(renameat_errno_is_classifiable(EXDEV));
  // EIO can be returned after the directory entry has been written, so it does
  // not distinguish "did not happen" from "happened".
  CHECK_FALSE(renameat_errno_is_classifiable(EIO));
}

TEST_CASE("EINTR is retried inside the wrappers and never becomes an outcome") {
  Fixture f(eintr_once_ops());
  const std::string segment = f.store->begin_receiver_segment(baseline());
  const CommitResult r = f.store->commit_visit(accepted("r1-i000", 1, segment));
  CHECK(r.outcome == CommitOutcome::Committed);
  CHECK(f.on_disk().at("attempts").size() == 1);
}

TEST_CASE("EIO from fsync is not retried and classifies by the matrix") {
  auto calls = std::make_shared<int>(0);
  Fixture f(counting_fsync_ops(calls, EIO));
  const std::string segment = f.store->begin_receiver_segment(baseline());
  const CommitResult r = f.store->commit_visit(accepted("r1-i000", 1, segment));

  CHECK(r.outcome == CommitOutcome::NotCommitted);
  // Exactly one call: a failed fsync may already have discarded the error state
  // that a second call would then report as success.
  CHECK(*calls == 1);
  CHECK_FALSE(f.has_session_file());
}

TEST_CASE("Indeterminate reconciliation, three cases") {
  SUBCASE("the attempt is present on disk, so the commit happened") {
    Fixture f;
    const std::string segment = f.store->begin_receiver_segment(baseline());
    REQUIRE(f.store->commit_visit(accepted("r1-i000", 1, segment)).outcome ==
            CommitOutcome::Committed);

    // Simulate a rename that reported an unclassifiable error but did take
    // place, by committing normally and then reloading.
    REQUIRE(f.store->commit_visit(accepted("r1-i001", 1, segment)).outcome ==
            CommitOutcome::Committed);
    const CommitResult reload = f.store->reload_from_disk();
    CHECK(reload.outcome == CommitOutcome::Committed);
    bool found = false;
    for (const auto& a : f.store->record().attempts) {
      if (a.visit_id == "r1-i001") found = true;
    }
    CHECK(found);
  }

  SUBCASE("the attempt is absent, which is a NotCommitted in every way that matters") {
    Fixture f;
    const std::string segment = f.store->begin_receiver_segment(baseline());
    REQUIRE(f.store->commit_visit(accepted("r1-i000", 1, segment)).outcome ==
            CommitOutcome::Committed);

    std::string error;
    auto reopened = SessionDir::open_existing(f.dir, error);
    REQUIRE(reopened.has_value());
    JsonSessionStore faulty(std::move(*reopened), f.store->record(),
                            faulty_ops(FaultPoint::AtRenameUnclassified));
    const CommitResult result = faulty.commit_visit(accepted("r1-i001", 1, segment));
    REQUIRE(result.outcome == CommitOutcome::Indeterminate);

    const CommitResult reload = faulty.reload_from_disk();
    CHECK(reload.outcome == CommitOutcome::Committed);
    for (const auto& a : faulty.record().attempts) {
      CHECK(a.visit_id != "r1-i001");
    }
  }

  SUBCASE("reload itself fails, so the session cannot be continued safely") {
    Fixture f;
    const std::string segment = f.store->begin_receiver_segment(baseline());
    REQUIRE(f.store->commit_visit(accepted("r1-i000", 1, segment)).outcome ==
            CommitOutcome::Committed);
    fs::remove(f.dir / kSessionFileName);
    const CommitResult reload = f.store->reload_from_disk();
    CHECK(reload.outcome == CommitOutcome::NotCommitted);
    CHECK_FALSE(reload.detail.empty());
  }
}

TEST_CASE("a retry is one commit carrying the disposition and the retry intent together") {
  Fixture f;
  const std::string segment = f.store->begin_receiver_segment(baseline());
  const CommitResult r = f.store->commit_visit(retried("r1-i000", 1, segment));
  REQUIRE(r.outcome == CommitOutcome::Committed);

  const json parsed = f.on_disk();
  REQUIRE(parsed.at("attempts").size() == 1);
  CHECK(parsed.at("attempts")[0].at("disposition").get<std::string>() == "superseded");
  REQUIRE(!parsed.at("pending_retry").is_null());
  CHECK(parsed.at("pending_retry").at("visit_id").get<std::string>() == "r1-i000");
  CHECK(parsed.at("pending_retry").at("next_attempt").get<int>() == 2);

  SessionRecord reloaded;
  from_json(parsed, reloaded);
  CHECK(invariant_i1(reloaded));
  CHECK(invariant_i2(reloaded));

  // The replacement clears the retry intent in the same commit that records it.
  REQUIRE(f.store->commit_visit(accepted("r1-i000", 2, segment)).outcome ==
          CommitOutcome::Committed);
  const json after = f.on_disk();
  CHECK(after.at("pending_retry").is_null());
  SessionRecord final_record;
  from_json(after, final_record);
  CHECK(invariant_i1(final_record));
  CHECK(invariant_i2(final_record));
  CHECK(visit_complete(final_record, "r1-i000"));
}

TEST_CASE("a fault anywhere inside a retry commit leaves one whole state or the other") {
  for (FaultPoint point :
       {FaultPoint::BeforeTempWrite, FaultPoint::AfterTempBeforeSync, FaultPoint::AtFileSync,
        FaultPoint::AtRename, FaultPoint::AtDirSync, FaultPoint::AtRenameUnclassified}) {
    Fixture f;
    const std::string segment = f.store->begin_receiver_segment(baseline());
    REQUIRE(f.store->commit_visit(accepted("r1-i000", 1, segment)).outcome ==
            CommitOutcome::Committed);
    const std::string before = read_whole(f.dir / kSessionFileName);

    std::string error;
    auto reopened = SessionDir::open_existing(f.dir, error);
    REQUIRE(reopened.has_value());
    JsonSessionStore faulty(std::move(*reopened), f.store->record(), faulty_ops(point));
    (void)faulty.commit_visit(retried("r1-i001", 1, segment));

    const std::string after = read_whole(f.dir / kSessionFileName);
    SessionRecord state;
    from_json(json::parse(after), state);
    // Either the pre-retry record or the full post-retry record, never a half
    // state: no visit has two terminal attempts, and pending_retry is set if
    // and only if the prior attempt is superseded.
    CHECK(invariant_i1(state));
    CHECK(invariant_i2(state));
    const bool unchanged = (after == before);
    const bool fully_applied =
        state.pending_retry.has_value() && state.pending_retry->visit_id == "r1-i001";
    CHECK((unchanged || fully_applied));
  }
}

TEST_CASE("receiver segments are appended and stamped on later attempts") {
  Fixture f;
  const std::string first = f.store->begin_receiver_segment(baseline());
  CHECK(first == "seg1");
  REQUIRE(f.store->commit_visit(accepted("r1-i000", 1, first)).outcome ==
          CommitOutcome::Committed);

  SourceInfo second_baseline = baseline();
  second_baseline.applied_gain_tenth_db = 400;
  const std::string second = f.store->begin_receiver_segment(second_baseline);
  CHECK(second == "seg2");
  REQUIRE(f.store->commit_visit(accepted("r1-i001", 1, second)).outcome ==
          CommitOutcome::Committed);

  const json parsed = f.on_disk();
  CHECK(parsed.at("receiver_segments").size() == 2);
  CHECK(parsed.at("attempts")[0].at("segment_id").get<std::string>() == "seg1");
  CHECK(parsed.at("attempts")[1].at("segment_id").get<std::string>() == "seg2");
  CHECK(parsed.at("receiver_segments")[1].at("baseline").at("applied_gain_tenth_db").get<int>() ==
        400);
}

TEST_CASE("the session lifecycle writes four states") {
  SUBCASE("pause") {
    Fixture f;
    f.store->set_report_renderer([](const SessionRecord&, const SessionSummary& s) {
      return std::string(s.partial ? "PARTIAL\n" : "COMPLETE\n") + "body\n";
    });
    const std::string segment = f.store->begin_receiver_segment(baseline());
    REQUIRE(f.store->commit_visit(accepted("r1-i000", 1, segment)).outcome ==
            CommitOutcome::Committed);

    SessionSummary partial;
    REQUIRE(f.store->pause(partial).outcome == CommitOutcome::Committed);
    const json parsed = f.on_disk();
    CHECK(parsed.at("state").get<std::string>() == "paused");
    CHECK(parsed.at("summary_partial").get<bool>() == true);
    CHECK(parsed.at("finished_utc").is_null());
    CHECK_FALSE(parsed.at("summary").is_null());
    const std::string report = read_whole(f.dir / kReportFileName);
    CHECK(report.rfind("PARTIAL", 0) == 0);
  }

  SUBCASE("finalize") {
    Fixture f;
    f.store->set_report_renderer(
        [](const SessionRecord&, const SessionSummary&) { return std::string("done\n"); });
    const std::string segment = f.store->begin_receiver_segment(baseline());
    REQUIRE(f.store->commit_visit(accepted("r1-i000", 1, segment)).outcome ==
            CommitOutcome::Committed);
    SessionSummary summary;
    REQUIRE(f.store->finalize(summary).outcome == CommitOutcome::Committed);
    const json parsed = f.on_disk();
    CHECK(parsed.at("state").get<std::string>() == "completed");
    CHECK(parsed.at("summary_partial").get<bool>() == false);
    CHECK_FALSE(parsed.at("finished_utc").is_null());
  }

  SUBCASE("abort") {
    Fixture f;
    f.store->set_report_renderer([](const SessionRecord&, const SessionSummary&) {
      return std::string("ABORTED - PARTIAL\n");
    });
    const std::string segment = f.store->begin_receiver_segment(baseline());
    REQUIRE(f.store->commit_visit(accepted("r1-i000", 1, segment)).outcome ==
            CommitOutcome::Committed);
    SessionSummary partial;
    REQUIRE(f.store->abort(partial, "the record could not be trusted to continue").outcome ==
            CommitOutcome::Committed);
    const json parsed = f.on_disk();
    CHECK(parsed.at("state").get<std::string>() == "aborted");
    CHECK(parsed.at("summary_partial").get<bool>() == true);
    CHECK_FALSE(parsed.at("finished_utc").is_null());
    CHECK(parsed.at("abort_reason").get<std::string>().find("could not be trusted") !=
          std::string::npos);
    CHECK(read_whole(f.dir / kReportFileName).rfind("ABORTED", 0) == 0);
  }
}

TEST_CASE("the attempt ceiling is enforced without corrupting the record") {
  Fixture f;
  const std::string segment = f.store->begin_receiver_segment(baseline());
  for (int i = 0; i < kAttemptCeiling; ++i) {
    REQUIRE(f.store->commit_visit(accepted("r1-i000", i + 1, segment)).outcome ==
            CommitOutcome::Committed);
  }
  const CommitResult refused = f.store->commit_visit(accepted("r1-i000", 251, segment));
  CHECK(refused.outcome == CommitOutcome::NotCommitted);
  CHECK(refused.detail.find("250") != std::string::npos);
  // The committed attempts still load.
  CHECK(f.on_disk().at("attempts").size() == static_cast<std::size_t>(kAttemptCeiling));
}

TEST_CASE("the derived CSV carries the honest column names and escapes correctly") {
  const std::string header = csv_header();
  for (const char* required :
       {"host_dropped_samples", "valid_audio_events", "audio_insufficient",
        "events_per_minute", "detected_fraction", "segment_id", "angle_index",
        "applied_gain_tenth_db", "applied_sample_rate_hz", "applied_center_hz"}) {
    CHECK_MESSAGE(header.find(required) != std::string::npos, required);
  }
  CHECK(header.find("sample_overruns") == std::string::npos);

  CHECK(csv_escape("plain") == "plain");
  CHECK(csv_escape("a,b") == "\"a,b\"");
  CHECK(csv_escape("he said \"hi\"") == "\"he said \"\"hi\"\"\"");
  CHECK(csv_escape("line\nbreak") == "\"line\nbreak\"");
}

TEST_CASE("a note with commas, quotes, and newlines survives both formats verbatim") {
  Fixture f;
  const std::string segment = f.store->begin_receiver_segment(baseline());
  VisitCommit c = accepted("r1-i000", 1, segment);
  const std::string note = "a,b and he said \"hi\"\nsecond line";
  c.attempt.note = note;
  REQUIRE(f.store->commit_visit(c).outcome == CommitOutcome::Committed);

  // Stored verbatim in JSON: control-character neutralisation is a render-time
  // concern and never mutates the canonical record.
  CHECK(f.on_disk().at("attempts")[0].at("note").get<std::string>() == note);

  const std::string csv = read_whole(f.dir / kMeasurementsFileName);
  CHECK(csv.find("\"a,b and he said \"\"hi\"\"\nsecond line\"") != std::string::npos);
  CHECK(csv_row_count(csv) == 1);
}

TEST_CASE("label sanitisation, every worked example plus the no-dot-dot property") {
  const std::pair<std::string, std::string> table[] = {
      {"../../etc/passwd", "etc-passwd"},
      {"a..b", "a.b"},
      {"..", ""},
      {".", ""},
      {"x/../y", "x-.-y"},
      {std::string(100, 'a'), std::string(32, 'a')},
      {"my label!", "my-label"},
      {"airband", "airband"},
      {"", ""},
  };
  for (const auto& [in, expected] : table) {
    CHECK_MESSAGE(slugify_label(in) == expected, "input \"" << in << "\"");
  }

  // The property, over a corpus that includes the dot-heavy cases the old rule
  // let through.
  std::vector<std::string> corpus = {"a..b", "...", "a...b", std::string(40, '.'),
                                     "..a..b..", "./../.", "a.-.b"};
  std::mt19937_64 rng(20260819);
  std::uniform_int_distribution<int> byte(1, 255);
  std::uniform_int_distribution<int> length(0, 48);
  for (int i = 0; i < 10000; ++i) {
    std::string s;
    const int n = length(rng);
    for (int k = 0; k < n; ++k) s.push_back(static_cast<char>(byte(rng)));
    corpus.push_back(std::move(s));
  }
  for (const std::string& s : corpus) {
    const std::string out = slugify_label(s);
    CHECK_MESSAGE(out.find("..") == std::string::npos, "input \"" << s << "\"");
    CHECK(out.find('/') == std::string::npos);
    CHECK(out != ".");
    CHECK(out != "..");
    CHECK(out.size() <= 32);
  }

  // The invariant the sanitisation exists to satisfy.
  CHECK_FALSE(valid_directory_component("a/b"));
  CHECK_FALSE(valid_directory_component("."));
  CHECK_FALSE(valid_directory_component(".."));
  CHECK_FALSE(valid_directory_component(""));
  CHECK(valid_directory_component("20260819-143000-airband"));
}

TEST_CASE("a session directory is created, never reused, and is mode 0700") {
  TempDir temp;
  std::string error;

  const fs::path target = temp.child("session");
  auto dir = SessionDir::create(target, error);
  REQUIRE_MESSAGE(dir.has_value(), error);

  struct stat st {};
  REQUIRE(::stat(target.c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0700);

  // A second create over the same path fails with EEXIST, and nothing is
  // written through it.
  auto again = SessionDir::create(target, error);
  CHECK_FALSE(again.has_value());
  CHECK(error.find("already exists") != std::string::npos);

  // Including when the path is a symlink.
  const fs::path elsewhere = temp.child("elsewhere");
  fs::create_directories(elsewhere);
  const fs::path link = temp.child("link");
  fs::create_symlink(elsewhere, link);
  auto through_link = SessionDir::create(link, error);
  CHECK_FALSE(through_link.has_value());
  CHECK(fs::is_empty(elsewhere));
}

TEST_CASE("session files are mode 0600") {
  Fixture f;
  const std::string segment = f.store->begin_receiver_segment(baseline());
  REQUIRE(f.store->commit_visit(accepted("r1-i000", 1, segment)).outcome ==
          CommitOutcome::Committed);

  for (const char* name : {kSessionFileName, kMeasurementsFileName}) {
    struct stat st {};
    REQUIRE(::stat((f.dir / name).c_str(), &st) == 0);
    CHECK_MESSAGE((st.st_mode & 0777) == 0600, name);
  }
}

TEST_CASE("a held directory descriptor cannot be redirected by replacing a parent") {
  // This is the property O_NOFOLLOW alone cannot deliver: it guards the final
  // component only, and a symlink at an intermediate directory redirects
  // everything underneath.
  TempDir temp;
  const fs::path parent = temp.child("parent");
  const fs::path session = parent / "session";
  fs::create_directories(parent);

  std::string error;
  auto dir = SessionDir::create(session, error);
  REQUIRE_MESSAGE(dir.has_value(), error);

  struct stat before {};
  REQUIRE(::stat(session.c_str(), &before) == 0);
  const ino_t original_inode = before.st_ino;

  // Swap the intermediate directory for a different one mid-session.
  const fs::path decoy = temp.child("decoy");
  fs::create_directories(decoy / "session");
  fs::rename(parent, temp.child("parent-moved"));
  fs::rename(decoy, parent);

  REQUIRE(write_atomic_at(dir->fd(), "session.json", "{\"marker\":1}").outcome ==
          CommitOutcome::Committed);

  // The bytes landed in the ORIGINAL inode, not in the directory that now
  // occupies the path.
  const fs::path original = temp.child("parent-moved") / "session" / "session.json";
  REQUIRE(fs::exists(original));
  CHECK(read_whole(original) == "{\"marker\":1}");
  CHECK_FALSE(fs::exists(parent / "session" / "session.json"));

  struct stat after {};
  REQUIRE(::stat(original.c_str(), &after) == 0);
  struct stat dir_stat {};
  REQUIRE(::stat((temp.child("parent-moved") / "session").c_str(), &dir_stat) == 0);
  CHECK(dir_stat.st_ino == original_inode);
}

TEST_CASE("a session.json above the read limit is refused") {
  TempDir temp;
  std::string error;
  auto dir = SessionDir::create(temp.child("session"), error);
  REQUIRE(dir.has_value());
  write_whole(temp.child("session") / kSessionFileName,
              std::string(kMaxSessionJsonBytes + 16, 'x'));

  const auto text = read_at_limited(dir->fd(), kSessionFileName, kMaxSessionJsonBytes, error);
  CHECK_FALSE(text.has_value());
  CHECK(error.find("above the limit") != std::string::npos);
}

TEST_CASE("a symlink at the final component is refused for a read") {
  TempDir temp;
  std::string error;
  auto dir = SessionDir::create(temp.child("session"), error);
  REQUIRE(dir.has_value());
  write_whole(temp.child("secret.txt"), "secret");
  fs::create_symlink(temp.child("secret.txt"), temp.child("session") / "session.json");

  const auto text = read_at_limited(dir->fd(), kSessionFileName, kMaxSessionJsonBytes, error);
  CHECK_FALSE(text.has_value());
  CHECK(error.find("symbolic link") != std::string::npos);
}

TEST_CASE("stale temporary files are found and removed") {
  TempDir temp;
  std::string error;
  auto dir = SessionDir::create(temp.child("session"), error);
  REQUIRE(dir.has_value());
  write_whole(temp.child("session") / "session.json.tmp.111", "partial");
  write_whole(temp.child("session") / "session.json.tmp.222", "partial");
  write_whole(temp.child("session") / "session.json", "{}");

  CHECK(remove_stale_temporaries(dir->fd(), kSessionFileName) == 2);
  CHECK(fs::exists(temp.child("session") / "session.json"));
  CHECK_FALSE(fs::exists(temp.child("session") / "session.json.tmp.111"));
}

TEST_CASE("superseded attempts are inert to every consumer") {
  Fixture f;
  const std::string segment = f.store->begin_receiver_segment(baseline());

  VisitCommit strong = retried("r1-i000", 1, segment);
  strong.attempt.capture_score_channel_db = 30.0;
  strong.attempt.status = AttemptStatus::Ok;
  REQUIRE(f.store->commit_visit(strong).outcome == CommitOutcome::Committed);

  VisitCommit weak = accepted("r1-i000", 2, segment);
  weak.attempt.capture_score_channel_db = 10.0;
  REQUIRE(f.store->commit_visit(weak).outcome == CommitOutcome::Committed);

  const SessionRecord& rec = f.store->record();
  CHECK(visit_complete(rec, "r1-i000"));
  // The visit is complete and no longer remaining, and the accepted attempt is
  // the 10 dB one; the 30 dB attempt stays for audit and is never ranked.
  const auto remaining = remaining_visits(rec);
  for (const auto& v : remaining) CHECK(v.visit_id != "r1-i000");

  int accepted_count = 0;
  for (const AttemptRecord& a : rec.attempts) {
    if (a.disposition == Disposition::Accepted) {
      ++accepted_count;
      CHECK(a.capture_score_channel_db.value() == doctest::Approx(10.0));
    }
  }
  CHECK(accepted_count == 1);

  // And the CSV shows both rows with their true dispositions, so the audit
  // trail is complete without the superseded row being mistakable for data.
  const std::string csv = read_whole(f.dir / kMeasurementsFileName);
  CHECK(csv.find(",superseded,") != std::string::npos);
  CHECK(csv.find(",accepted,") != std::string::npos);
  CHECK(csv_row_count(csv) == 2);
}

TEST_CASE("event retention round-trips with the truncation visible") {
  Fixture f;
  const std::string segment = f.store->begin_receiver_segment(baseline());
  VisitCommit c = accepted("r1-i000", 1, segment);
  c.attempt.events_total = 9;
  c.attempt.events_retained = 4;
  for (int i = 0; i < 4; ++i) {
    EventRecord e;
    e.valid = true;
    e.start_s = i;
    c.attempt.events.push_back(e);
  }
  REQUIRE(f.store->commit_visit(c).outcome == CommitOutcome::Committed);

  SessionRecord reloaded;
  from_json(f.on_disk(), reloaded);
  REQUIRE(reloaded.attempts.size() == 1);
  CHECK(reloaded.attempts[0].events_total == 9);
  CHECK(reloaded.attempts[0].events_retained == 4);
  // The loader never reconstructs the missing events and never treats
  // events_retained as the true count.
  CHECK(reloaded.attempts[0].events.size() == 4);
}

}  // TEST_SUITE
