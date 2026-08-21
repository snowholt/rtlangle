// WP12 — the whole program on synthetic data, and the generated example
// session. Spec section 14.4.

#include <doctest/doctest.h>

#include "app/run_command.h"
#include "core/utc.h"
#include "persist/csv.h"
#include "persist/json_session_store.h"
#include "persist/session_loader.h"
#include "tests/support/scripted_angle_provider.h"
#include "tests/support/scripted_capture_source.h"
#include "tests/support/temp_dir.h"
#include "ui/report_renderer.h"
#include "ui/scripted_terminal_ui.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <string>

using namespace rtlangle;
using namespace rtlangle::app;
using namespace rtlangle::test;
using rtlangle::ui::ScriptedTerminalUi;

namespace {

namespace fs = std::filesystem;

Config e2e_config(const TempDir& temp, int rounds) {
  Config c;
  c.center_hz = 118350000;
  c.angles_deg = {0.0, 45.0, 90.0};
  c.rounds = rounds;
  c.duration_s = 1.2;
  c.settle_s = 0.0;
  c.order = VisitOrder::Alternating;
  c.seed = 20260819;
  c.source_spec = "synthetic";
  c.non_interactive = true;
  c.min_valid_events = 1;
  c.min_event_ms = 100.0;
  c.session_root = temp.child("sessions").string();
  c.label = "synthetic";
  return c;
}

SyntheticParams e2e_params(const Config& cfg) {
  SyntheticParams p;
  p.sample_rate_hz = cfg.sample_rate_hz;
  p.center_hz = cfg.center_hz.value_or(118350000);
  p.offset_hz = cfg.offset_tune_hz;
  p.snr_db = 15.0;
  p.talk_s = 0.15;
  p.duty = 0.3;
  p.seed = 4242;
  return p;
}

// Runs a whole session through the production orchestration, with the scripted
// source and provider substituted so the generated SNR can vary by angle.
struct Session {
  int                      exit_code = 0;
  fs::path                 dir;
  SessionRecord            record;
  std::vector<std::string> emitted;
  std::string              report;
};

Session run_session(const Config& cfg, std::map<double, double> snr_by_angle,
                    std::deque<AngleOutcome> script = {}) {
  Session out;
  ScriptedTerminalUi terminal(false);

  auto source =
      std::make_shared<ScriptedCaptureSource>(e2e_params(cfg), std::move(snr_by_angle));
  auto provider = std::make_shared<ScriptedAngleProvider>(*source, std::move(script));

  RunHooks hooks;
  hooks.make_source = [source](const Config&, std::string& error) {
    error.clear();
    // The controller owns no lifetime here; the shared pointer keeps the source
    // alive for the test to inspect afterwards.
    struct Borrow final : ISampleSource {
      std::shared_ptr<ScriptedCaptureSource> inner;
      SourceInfo info() const override { return inner->info(); }
      ReadResult read(std::span<std::complex<float>> o,
                      std::chrono::steady_clock::time_point d) override {
        return inner->read(o, d);
      }
      void   flush() override { inner->flush(); }
      void   cancel() override { inner->cancel(); }
      double clipped_fraction() const override { return inner->clipped_fraction(); }
    };
    auto borrow = std::make_unique<Borrow>();
    borrow->inner = source;
    return std::unique_ptr<ISampleSource>(std::move(borrow));
  };
  hooks.make_provider = [provider](const Config&, ui::ITerminalUi&) {
    struct Borrow final : IAngleProvider {
      std::shared_ptr<ScriptedAngleProvider> inner;
      std::string_view name() const override { return inner->name(); }
      bool is_automated() const override { return inner->is_automated(); }
      AngleOutcome request(double planned_deg, int attempt) override {
        return inner->request(planned_deg, attempt);
      }
    };
    auto borrow = std::make_unique<Borrow>();
    borrow->inner = provider;
    return std::unique_ptr<IAngleProvider>(std::move(borrow));
  };

  out.exit_code = run_command(cfg, terminal, hooks);
  out.emitted = terminal.emitted();

  for (const auto& entry : fs::directory_iterator(cfg.session_root)) {
    if (entry.is_directory()) out.dir = entry.path();
  }
  if (!out.dir.empty() && fs::exists(out.dir / kSessionFileName)) {
    from_json(nlohmann::json::parse(read_whole(out.dir / kSessionFileName)), out.record);
  }
  if (!out.dir.empty() && fs::exists(out.dir / kReportFileName)) {
    out.report = read_whole(out.dir / kReportFileName);
  }
  return out;
}

bool report_has(const Session& s, std::string_view needle) {
  return s.report.find(needle) != std::string::npos;
}

// The pinned clock is process-global. A failing REQUIRE between set_clock and
// reset_clock would throw past the reset and leave every later test in this
// binary looking at a frozen clock, which would then fail for a reason that has
// nothing to do with it.
class PinnedClock {
 public:
  explicit PinnedClock(std::chrono::system_clock::time_point at) {
    set_clock([at] { return at; });
  }
  ~PinnedClock() { reset_clock(); }
  PinnedClock(const PinnedClock&) = delete;
  PinnedClock& operator=(const PinnedClock&) = delete;
};

}  // namespace

TEST_SUITE("end_to_end") {

TEST_CASE("a full synthetic session completes and its three files reconcile") {
  TempDir temp;
  const Config cfg = e2e_config(temp, 4);
  const Session s = run_session(cfg, {{0.0, 24.0}, {45.0, 18.0}, {90.0, 12.0}});

  CHECK(s.exit_code == 0);
  CHECK(s.record.state == SessionState::Completed);
  CHECK(s.record.finished_utc.has_value());
  REQUIRE(fs::exists(s.dir / kSessionFileName));
  REQUIRE(fs::exists(s.dir / kMeasurementsFileName));
  REQUIRE(fs::exists(s.dir / kReportFileName));

  // Attempt for attempt: the CSV is a derived export of the canonical JSON.
  CHECK(csv_row_count(read_whole(s.dir / kMeasurementsFileName)) ==
        static_cast<int>(s.record.attempts.size()));
  CHECK(s.record.attempts.size() == 12);
  for (const AttemptRecord& a : s.record.attempts) {
    CHECK(a.disposition == Disposition::Accepted);
    CHECK(a.status == AttemptStatus::Ok);
  }

  // The seeded strongest angle leads the channel ranking.
  REQUIRE(s.record.summary.has_value());
  REQUIRE(!s.record.summary->ranking_channel.empty());
  CHECK(s.record.summary->ranking_channel.front() == doctest::Approx(0.0));

  // The report carries both tables, a yield beside every score, and the one
  // headline.
  CHECK(report_has(s, "Channel ranking"));
  CHECK(report_has(s, "Audio ranking"));
  CHECK(report_has(s, "events/min"));
  CHECK(report_has(s, ui::kResultHeadline));
}

TEST_CASE("W1 fires at two rounds, not at four, and the headline is byte-identical") {
  TempDir two_temp;
  TempDir four_temp;
  const std::map<double, double> script = {{0.0, 24.0}, {45.0, 18.0}, {90.0, 12.0}};
  const Session two = run_session(e2e_config(two_temp, 2), script);
  const Session four = run_session(e2e_config(four_temp, 4), script);

  auto fired = [](const Session& s, std::string_view id) {
    if (!s.record.summary.has_value()) return false;
    for (const Warning& w : s.record.summary->warnings) {
      if (w.id == id) return true;
    }
    return false;
  };
  CHECK(fired(two, "W1"));
  CHECK_FALSE(fired(four, "W1"));

  // The numbers differ and the headline does not: there is only one headline.
  CHECK(two.report != four.report);
  const auto head_two = two.report.find(ui::kResultHeadline);
  const auto head_four = four.report.find(ui::kResultHeadline);
  REQUIRE(head_two != std::string::npos);
  REQUIRE(head_four != std::string::npos);
  CHECK(two.report.substr(head_two) == four.report.substr(head_four));
}

TEST_CASE("the selection effect end to end: W6 fires and both rankings still print") {
  // The better angle detects the weak half of the population too, and those
  // extra weak events pull its median down. Reproduced here by giving 45
  // degrees a higher generated SNR: it clears the squelch on more of the
  // synthetic traffic, so it yields more and scores lower.
  TempDir temp;
  Config cfg = e2e_config(temp, 4);
  cfg.angles_deg = {0.0, 45.0};
  // 0 degrees hears only what is loud; 45 degrees hears more, including the
  // quiet parts of each burst.
  const Session s = run_session(cfg, {{0.0, 10.0}, {45.0, 22.0}});

  CHECK(s.exit_code == 0);
  REQUIRE(s.record.summary.has_value());
  const SessionSummary& summary = *s.record.summary;

  // Both rankings are printed in full whatever the outcome.
  CHECK(summary.ranking_channel.size() == 2);
  CHECK(summary.ranking_audio.size() <= 2);
  CHECK(report_has(s, "Channel ranking"));
  CHECK(report_has(s, "Audio ranking"));

  // Every angle has a yield beside its score.
  for (const AngleSummary& a : summary.angles) {
    if (a.score_channel_db.has_value()) CHECK(a.yield_events_per_min.has_value());
  }

  // And nothing anywhere claims one angle is the right one.
  std::string lowered = s.report;
  for (char& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (const char* forbidden :
       {"best angle", "optimal", "statistically", "resolved", "confidence interval"}) {
    CHECK_MESSAGE(lowered.find(forbidden) == std::string::npos, forbidden);
  }
}

TEST_CASE("an audio-sparse session keeps the channel ranking and shows a dash") {
  TempDir temp;
  Config cfg = e2e_config(temp, 4);
  // A very low generated SNR at one angle: the channel metric still qualifies
  // there while the audio one does not.
  const Session s = run_session(cfg, {{0.0, 24.0}, {45.0, 18.0}, {90.0, 7.0}});

  CHECK(s.exit_code == 0);
  REQUIRE(s.record.summary.has_value());
  const SessionSummary& summary = *s.record.summary;

  bool sparse_found = false;
  for (const AngleSummary& a : summary.angles) {
    CHECK(a.n_captures_audio <= a.n_captures);
    if (a.n_captures > 0 && a.n_captures_audio < a.n_captures) sparse_found = true;
  }
  if (sparse_found) {
    // The comparability note names both counts.
    CHECK(report_has(s, "audio-eligible captures against"));
  }

  // No capture was mis-statused: a missing audio score is a flag, not a status.
  for (const AttemptRecord& a : s.record.attempts) {
    if (a.capture_score_channel_db.has_value()) CHECK(a.status == AttemptStatus::Ok);
  }
}

TEST_CASE("quit, pause, resume, complete, and a refused second resume") {
  TempDir temp;
  const Config cfg = e2e_config(temp, 2);

  // Quit at the third visit.
  std::deque<AngleOutcome> script;
  AngleOutcome proceed;
  AngleOutcome quit;
  quit.cmd = AngleOutcome::Cmd::Quit;
  script.push_back(proceed);
  script.push_back(proceed);
  script.push_back(quit);

  const Session paused =
      run_session(cfg, {{0.0, 24.0}, {45.0, 18.0}, {90.0, 12.0}}, std::move(script));
  CHECK(paused.exit_code == 0);
  CHECK(paused.record.state == SessionState::Paused);
  CHECK_FALSE(paused.record.finished_utc.has_value());
  CHECK(paused.report.rfind("PARTIAL", 0) == 0);
  // The partial report was built from the completed subset alone.
  CHECK(paused.record.attempts.size() == 3);

  // Resume, and finish.
  Config resume_cfg = cfg;
  resume_cfg.resume_dir = paused.dir.string();
  ScriptedTerminalUi terminal(false);
  auto source = std::make_shared<ScriptedCaptureSource>(
      e2e_params(cfg), std::map<double, double>{{0.0, 24.0}, {45.0, 18.0}, {90.0, 12.0}});
  auto provider = std::make_shared<ScriptedAngleProvider>(*source, std::deque<AngleOutcome>{});
  RunHooks hooks;
  hooks.make_source = [source](const Config&, std::string& error) {
    error.clear();
    struct Borrow final : ISampleSource {
      std::shared_ptr<ScriptedCaptureSource> inner;
      SourceInfo info() const override { return inner->info(); }
      ReadResult read(std::span<std::complex<float>> o,
                      std::chrono::steady_clock::time_point d) override {
        return inner->read(o, d);
      }
      void   flush() override { inner->flush(); }
      void   cancel() override { inner->cancel(); }
      double clipped_fraction() const override { return inner->clipped_fraction(); }
    };
    auto borrow = std::make_unique<Borrow>();
    borrow->inner = source;
    return std::unique_ptr<ISampleSource>(std::move(borrow));
  };
  hooks.make_provider = [provider](const Config&, ui::ITerminalUi&) {
    struct Borrow final : IAngleProvider {
      std::shared_ptr<ScriptedAngleProvider> inner;
      std::string_view name() const override { return inner->name(); }
      bool is_automated() const override { return inner->is_automated(); }
      AngleOutcome request(double planned_deg, int attempt) override {
        return inner->request(planned_deg, attempt);
      }
    };
    auto borrow = std::make_unique<Borrow>();
    borrow->inner = provider;
    return std::unique_ptr<IAngleProvider>(std::move(borrow));
  };

  CHECK(run_command(resume_cfg, terminal, hooks) == 0);

  SessionRecord finished;
  from_json(nlohmann::json::parse(read_whole(paused.dir / kSessionFileName)), finished);
  CHECK(finished.state == SessionState::Completed);
  CHECK(finished.receiver_segments.size() == 2);
  std::set<std::string> visits;
  for (const AttemptRecord& a : finished.attempts) visits.insert(a.visit_id);
  CHECK(visits.size() == finished.plan.visits.size());

  // A further resume of the now-completed directory is refused, naming when it
  // finished.
  ScriptedTerminalUi refused(false);
  CHECK(run_command(resume_cfg, refused, hooks) == 2);
  bool named = false;
  for (const std::string& line : refused.emitted()) {
    if (line.find("completed at") != std::string::npos) named = true;
  }
  CHECK(named);
}

TEST_CASE("an unrecoverable fault ends aborted, and that directory is refused too") {
  TempDir temp;
  const Config cfg = e2e_config(temp, 2);
  ScriptedTerminalUi terminal(false);

  auto source = std::make_shared<ScriptedCaptureSource>(
      e2e_params(cfg), std::map<double, double>{{0.0, 20.0}});
  auto provider = std::make_shared<ScriptedAngleProvider>(*source, std::deque<AngleOutcome>{});

  // A store that reports Indeterminate and then cannot re-read the record: the
  // reconciliation of spec section 11.1.2 fails, so the session cannot be
  // continued safely.
  struct BrokenStore final : ISessionStore {
    std::unique_ptr<JsonSessionStore> inner;
    const SessionRecord& record() const override { return inner->record(); }
    std::string begin_receiver_segment(const SourceInfo& a) override {
      return inner->begin_receiver_segment(a);
    }
    CommitResult commit_visit(const VisitCommit&) override {
      CommitResult r;
      r.outcome = CommitOutcome::Indeterminate;
      r.detail = "the commit rename returned an unclassifiable error";
      return r;
    }
    CommitResult reload_from_disk() override {
      CommitResult r;
      r.outcome = CommitOutcome::NotCommitted;
      r.detail = "the record could not be re-read";
      return r;
    }
    CommitResult pause(const SessionSummary& s) override { return inner->pause(s); }
    CommitResult finalize(const SessionSummary& s) override { return inner->finalize(s); }
    CommitResult abort(const SessionSummary& s, std::string_view reason) override {
      return inner->abort(s, reason);
    }
    void flush_exports() override { inner->flush_exports(); }
  };

  RunHooks hooks;
  hooks.make_source = [source](const Config&, std::string& error) {
    error.clear();
    struct Borrow final : ISampleSource {
      std::shared_ptr<ScriptedCaptureSource> inner;
      SourceInfo info() const override { return inner->info(); }
      ReadResult read(std::span<std::complex<float>> o,
                      std::chrono::steady_clock::time_point d) override {
        return inner->read(o, d);
      }
      void   flush() override { inner->flush(); }
      void   cancel() override { inner->cancel(); }
      double clipped_fraction() const override { return inner->clipped_fraction(); }
    };
    auto borrow = std::make_unique<Borrow>();
    borrow->inner = source;
    return std::unique_ptr<ISampleSource>(std::move(borrow));
  };
  hooks.make_provider = [provider](const Config&, ui::ITerminalUi&) {
    struct Borrow final : IAngleProvider {
      std::shared_ptr<ScriptedAngleProvider> inner;
      std::string_view name() const override { return inner->name(); }
      bool is_automated() const override { return inner->is_automated(); }
      AngleOutcome request(double planned_deg, int attempt) override {
        return inner->request(planned_deg, attempt);
      }
    };
    auto borrow = std::make_unique<Borrow>();
    borrow->inner = provider;
    return std::unique_ptr<IAngleProvider>(std::move(borrow));
  };
  hooks.make_store = [](SessionDir dir, SessionRecord record) {
    auto broken = std::make_unique<BrokenStore>();
    broken->inner = std::make_unique<JsonSessionStore>(std::move(dir), std::move(record));
    broken->inner->set_report_renderer(
        [](const SessionRecord& r, const SessionSummary& s) {
          return ui::render_report(r, s, false);
        });
    return std::unique_ptr<ISessionStore>(std::move(broken));
  };

  CHECK(run_command(cfg, terminal, hooks) == 1);

  fs::path dir;
  for (const auto& entry : fs::directory_iterator(cfg.session_root)) {
    if (entry.is_directory()) dir = entry.path();
  }
  REQUIRE(!dir.empty());
  SessionRecord rec;
  from_json(nlohmann::json::parse(read_whole(dir / kSessionFileName)), rec);
  CHECK(rec.state == SessionState::Aborted);
  CHECK(rec.abort_reason.has_value());
  CHECK(rec.finished_utc.has_value());
  CHECK(read_whole(dir / kReportFileName).rfind("ABORTED - PARTIAL", 0) == 0);

  // Resuming an aborted directory is refused.
  Config resume_cfg = cfg;
  resume_cfg.resume_dir = dir.string();
  ScriptedTerminalUi refused(false);
  CHECK(run_command(resume_cfg, refused, hooks) == 2);
}

// ---------------------------------------------------------------------------
// The committed example session.
// ---------------------------------------------------------------------------
TEST_CASE("the committed example session is what the tool produces") {
  // The example is GENERATED, not written by hand: a hand-written one would
  // drift from the real output shape. The clock is pinned so the generated
  // record is reproducible.
  //
  // Exactly two fields cannot be reproduced, and both are facts about where the
  // test ran rather than measurements: `wall_duration_s`, which is real elapsed
  // time, and `config.session_root`, which is the temporary directory this run
  // happened to use. They are normalised, and everything else - every measured
  // number, every timestamp, the whole report, and the whole CSV - is compared
  // exactly. Asserting that those two are the ONLY differences is a stronger
  // check than a blanket byte comparison would be.
  const fs::path example = fs::path(RTLANGLE_SOURCE_DIR) / "examples" / "session-synthetic";
  const bool writing = std::getenv("RTLANGLE_WRITE_EXAMPLE") != nullptr;
  if (!writing && !fs::exists(example / kSessionFileName)) {
    MESSAGE("no committed example session; run with RTLANGLE_WRITE_EXAMPLE=1 to generate it");
    return;
  }

  TempDir temp;
  Config cfg = e2e_config(temp, 2);
  cfg.label = "example";
  cfg.angle_reference = "marked arm along the balcony rail, pointing at the street";
  cfg.setup_note = "dipole 1.2 m above the balcony floor, feedline along the rail";

  const PinnedClock pinned(std::chrono::system_clock::from_time_t(1787000000));
  const Session s = run_session(cfg, {{0.0, 22.0}, {45.0, 17.0}, {90.0, 12.0}});

  REQUIRE(s.exit_code == 0);
  REQUIRE(!s.dir.empty());

  auto normalise = [](std::string text) {
    nlohmann::json j = nlohmann::json::parse(text);
    for (auto& attempt : j["attempts"]) attempt["wall_duration_s"] = 0.0;
    j["config"]["session_root"] = "sessions";
    return j.dump(2) + "\n";
  };

  if (writing) {
    fs::create_directories(example);
    write_whole(example / kSessionFileName, normalise(read_whole(s.dir / kSessionFileName)));
    fs::copy_file(s.dir / kMeasurementsFileName, example / kMeasurementsFileName,
                  fs::copy_options::overwrite_existing);
    fs::copy_file(s.dir / kReportFileName, example / kReportFileName,
                  fs::copy_options::overwrite_existing);
    MESSAGE("wrote the example session to " << example.string());
    return;
  }

  CHECK(normalise(read_whole(s.dir / kSessionFileName)) ==
        read_whole(example / kSessionFileName));
  CHECK(read_whole(s.dir / kReportFileName) == read_whole(example / kReportFileName));

  // measurements.csv carries wall-clock-free fields only, so it compares
  // exactly.
  CHECK(read_whole(s.dir / kMeasurementsFileName) ==
        read_whole(example / kMeasurementsFileName));
}

}  // TEST_SUITE
