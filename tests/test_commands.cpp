// WP11 — orchestration. One integration test per command path, and the
// assertion that the APP LAYER, not the controller, writes every terminal or
// paused state. Spec sections 11.1.1, 11.5, and 12.1.

#include <doctest/doctest.h>

#include "app/run_command.h"
#include "core/utc.h"
#include "persist/json_session_store.h"
#include "persist/session_loader.h"
#include "source/synthetic_source.h"
#include "tests/support/fault_store.h"
#include "tests/support/temp_dir.h"
#include "ui/report_renderer.h"
#include "ui/scripted_terminal_ui.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace rtlangle;
using namespace rtlangle::app;
using namespace rtlangle::test;
using rtlangle::ui::ScriptedTerminalUi;

namespace {

namespace fs = std::filesystem;

Config synthetic_run(const TempDir& temp) {
  Config c;
  c.center_hz = 118350000;
  c.angles_deg = {0.0, 45.0};
  c.rounds = 1;
  c.duration_s = 1.2;
  c.settle_s = 0.0;
  c.source_spec = "synthetic";
  c.non_interactive = true;
  c.synthetic_snr_db = 25.0;
  c.synthetic_duty = 0.3;
  c.min_valid_events = 1;
  c.min_event_ms = 100.0;
  c.session_root = temp.child("sessions").string();
  c.label = "orchestration";
  return c;
}

fs::path only_session_under(const fs::path& root) {
  for (const auto& entry : fs::directory_iterator(root)) {
    if (entry.is_directory()) return entry.path();
  }
  return {};
}

SessionState state_of(const fs::path& dir) {
  SessionRecord rec;
  from_json(nlohmann::json::parse(read_whole(dir / kSessionFileName)), rec);
  return rec.state;
}

// A store decorator that also records the state the moment the controller
// returns, so the app layer's exclusive ownership of the terminal states can be
// asserted from this side too.
struct RecordingStore final : ISessionStore {
  std::unique_ptr<JsonSessionStore> inner;
  std::vector<std::string>*         log = nullptr;
  std::optional<CommitOutcome>      commit_outcome;
  std::optional<CommitOutcome>      pause_outcome;
  std::optional<CommitOutcome>      finalize_outcome;
  std::optional<CommitOutcome>      abort_outcome;
  int pause_calls = 0;
  int finalize_calls = 0;
  int abort_calls = 0;

  const SessionRecord& record() const override { return inner->record(); }
  std::string begin_receiver_segment(const SourceInfo& a) override {
    if (log != nullptr) log->push_back("store.begin_receiver_segment");
    return inner->begin_receiver_segment(a);
  }
  CommitResult commit_visit(const VisitCommit& v) override {
    if (commit_outcome.has_value()) {
      CommitResult r;
      r.outcome = *commit_outcome;
      r.detail = "injected";
      return r;
    }
    return inner->commit_visit(v);
  }
  CommitResult reload_from_disk() override { return inner->reload_from_disk(); }
  CommitResult pause(const SessionSummary& s) override {
    ++pause_calls;
    if (log != nullptr) log->push_back("store.pause");
    if (pause_outcome.has_value()) {
      CommitResult r;
      r.outcome = *pause_outcome;
      r.detail = "injected";
      return r;
    }
    return inner->pause(s);
  }
  CommitResult finalize(const SessionSummary& s) override {
    ++finalize_calls;
    if (log != nullptr) log->push_back("store.finalize");
    if (finalize_outcome.has_value()) {
      CommitResult r;
      r.outcome = *finalize_outcome;
      r.detail = "injected";
      return r;
    }
    return inner->finalize(s);
  }
  CommitResult abort(const SessionSummary& s, std::string_view reason) override {
    ++abort_calls;
    if (log != nullptr) log->push_back("store.abort");
    if (abort_outcome.has_value()) {
      CommitResult r;
      r.outcome = *abort_outcome;
      r.detail = "injected";
      return r;
    }
    return inner->abort(s, reason);
  }
  void flush_exports() override { inner->flush_exports(); }
};

// Builds hooks that record the orchestration order and hand back a decorated
// store the test keeps a pointer to.
RunHooks recording_hooks(std::vector<std::string>& log, RecordingStore*& store_out) {
  RunHooks hooks;
  hooks.observe = [&log](std::string_view name) { log.emplace_back(name); };
  hooks.make_source = [&log](const Config& cfg, std::string& error) {
    log.emplace_back("source.open");
    error.clear();
    SyntheticParams p;
    p.sample_rate_hz = cfg.sample_rate_hz;
    p.center_hz = cfg.center_hz.value_or(118350000);
    p.offset_hz = cfg.offset_tune_hz;
    p.snr_db = cfg.synthetic_snr_db;
    p.talk_s = 0.15;
    p.duty = 0.3;
    return std::unique_ptr<ISampleSource>(new SyntheticSource(p));
  };
  hooks.make_store = [&log, &store_out](SessionDir dir, SessionRecord record) {
    auto decorated = std::make_unique<RecordingStore>();
    decorated->inner = std::make_unique<JsonSessionStore>(std::move(dir), std::move(record));
    decorated->inner->set_report_renderer(
        [](const SessionRecord& r, const SessionSummary& s) {
          return ui::render_report(r, s, false);
        });
    decorated->log = &log;
    store_out = decorated.get();
    return std::unique_ptr<ISessionStore>(std::move(decorated));
  };
  return hooks;
}

}  // namespace

TEST_SUITE("commands") {

TEST_CASE("a new run follows the orchestration sequence in order") {
  TempDir temp;
  const Config cfg = synthetic_run(temp);
  ScriptedTerminalUi terminal(false);

  std::vector<std::string> log;
  RecordingStore* store = nullptr;
  RunHooks hooks = recording_hooks(log, store);

  CHECK(run_command(cfg, terminal, hooks) == 0);

  auto position = [&log](std::string_view name) {
    const auto it = std::find(log.begin(), log.end(), name);
    REQUIRE(it != log.end());
    return static_cast<std::size_t>(std::distance(log.begin(), it));
  };

  // The order of the first steps is not cosmetic: begin_receiver_segment is a
  // method on the store, so the store must exist first, and it needs
  // source.info(), so the source must be open before that.
  CHECK(position("open_source") < position("create_session"));
  CHECK(position("create_session") < position("make_store"));
  CHECK(position("make_store") < position("store.begin_receiver_segment"));
  CHECK(position("store.begin_receiver_segment") < position("build_controller"));
  CHECK(position("build_controller") < position("controller_run"));
  CHECK(position("controller_run") < position("aggregate"));
  CHECK(position("aggregate") < position("evaluate_warnings"));
  CHECK(position("evaluate_warnings") < position("choose_report_metric"));
  CHECK(position("choose_report_metric") < position("render"));
  CHECK(position("render") < position("store.finalize"));

  // There is no decision step anywhere between aggregation and rendering.
  for (const std::string& entry : log) {
    CHECK(entry.find("decide") == std::string::npos);
    CHECK(entry.find("resolve") == std::string::npos);
  }
}

TEST_CASE("Completed maps to finalize and a completed state, exit 0") {
  TempDir temp;
  const Config cfg = synthetic_run(temp);
  ScriptedTerminalUi terminal(false);
  std::vector<std::string> log;
  RecordingStore* store = nullptr;
  RunHooks hooks = recording_hooks(log, store);

  CHECK(run_command(cfg, terminal, hooks) == 0);
  REQUIRE(store != nullptr);
  CHECK(store->finalize_calls == 1);
  CHECK(store->pause_calls == 0);
  CHECK(store->abort_calls == 0);

  const fs::path dir = only_session_under(cfg.session_root);
  REQUIRE(!dir.empty());
  CHECK(state_of(dir) == SessionState::Completed);
  CHECK(fs::exists(dir / kSessionFileName));
  CHECK(fs::exists(dir / kMeasurementsFileName));
  CHECK(fs::exists(dir / kReportFileName));

  // The report the store wrote carries the one headline.
  const std::string report = read_whole(dir / kReportFileName);
  CHECK(report.find(ui::kResultHeadline) != std::string::npos);
}

TEST_CASE("QuitRequested maps to pause and a paused state, exit 0") {
  TempDir temp;
  const Config cfg = synthetic_run(temp);
  ScriptedTerminalUi terminal(false);
  std::vector<std::string> log;
  RecordingStore* store = nullptr;
  RunHooks hooks = recording_hooks(log, store);
  hooks.make_provider = [](const Config&, ui::ITerminalUi&) {
    struct QuitAfterOne final : IAngleProvider {
      int calls = 0;
      std::string_view name() const override { return "scripted"; }
      bool is_automated() const override { return true; }
      AngleOutcome request(double planned_deg, int) override {
        AngleOutcome out;
        out.actual_deg = planned_deg;
        out.cmd = ++calls > 1 ? AngleOutcome::Cmd::Quit : AngleOutcome::Cmd::Proceed;
        return out;
      }
    };
    return std::unique_ptr<IAngleProvider>(new QuitAfterOne());
  };

  CHECK(run_command(cfg, terminal, hooks) == 0);
  REQUIRE(store != nullptr);
  CHECK(store->pause_calls == 1);
  CHECK(store->finalize_calls == 0);
  CHECK(store->abort_calls == 0);

  const fs::path dir = only_session_under(cfg.session_root);
  CHECK(state_of(dir) == SessionState::Paused);
  SessionRecord rec;
  from_json(nlohmann::json::parse(read_whole(dir / kSessionFileName)), rec);
  CHECK_FALSE(rec.finished_utc.has_value());
  CHECK(rec.summary_partial);
  CHECK(read_whole(dir / kReportFileName).rfind("PARTIAL", 0) == 0);
}

TEST_CASE("Failed maps to abort, an aborted state with a reason, exit 1") {
  TempDir temp;
  const Config cfg = synthetic_run(temp);
  ScriptedTerminalUi terminal(false);
  std::vector<std::string> log;
  RecordingStore* store = nullptr;
  RunHooks hooks = recording_hooks(log, store);
  RecordingStore* captured = nullptr;
  hooks.make_store = [&captured](SessionDir dir, SessionRecord record) {
    auto decorated = std::make_unique<RecordingStore>();
    decorated->inner = std::make_unique<JsonSessionStore>(std::move(dir), std::move(record));
    decorated->inner->set_report_renderer(
        [](const SessionRecord& r, const SessionSummary& s) {
          return ui::render_report(r, s, false);
        });
    decorated->commit_outcome = CommitOutcome::NotCommitted;
    captured = decorated.get();
    return std::unique_ptr<ISessionStore>(std::move(decorated));
  };

  CHECK(run_command(cfg, terminal, hooks) == 1);
  REQUIRE(captured != nullptr);
  CHECK(captured->abort_calls == 1);
  CHECK(captured->finalize_calls == 0);
  CHECK(captured->pause_calls == 0);

  const fs::path dir = only_session_under(cfg.session_root);
  SessionRecord rec;
  from_json(nlohmann::json::parse(read_whole(dir / kSessionFileName)), rec);
  CHECK(rec.state == SessionState::Aborted);
  REQUIRE(rec.abort_reason.has_value());
  CHECK(rec.finished_utc.has_value());
  CHECK(read_whole(dir / kReportFileName).rfind("ABORTED - PARTIAL", 0) == 0);
}

TEST_CASE("a store that cannot record its own failure leaves the record untouched") {
  // The terminus of the NotCommitted ladder: exit 1, the record in its last
  // committed state, finished_utc NOT stamped, no abort_reason, and every
  // measurement committed before the fault still present.
  TempDir temp;
  const Config cfg = synthetic_run(temp);
  ScriptedTerminalUi terminal(false);
  std::vector<std::string> log;
  RecordingStore* store = nullptr;
  RunHooks hooks = recording_hooks(log, store);
  RecordingStore* captured = nullptr;
  hooks.make_store = [&captured](SessionDir dir, SessionRecord record) {
    auto decorated = std::make_unique<RecordingStore>();
    decorated->inner = std::make_unique<JsonSessionStore>(std::move(dir), std::move(record));
    decorated->commit_outcome = CommitOutcome::NotCommitted;
    decorated->abort_outcome = CommitOutcome::NotCommitted;
    captured = decorated.get();
    return std::unique_ptr<ISessionStore>(std::move(decorated));
  };

  CHECK(run_command(cfg, terminal, hooks) == 1);
  REQUIRE(captured != nullptr);
  CHECK(captured->abort_calls == 1);

  const fs::path dir = only_session_under(cfg.session_root);
  // Nothing was ever committed, so there is no session.json at all: the record
  // is byte-for-byte what it was, which for a session that never wrote one is
  // its absence.
  CHECK_FALSE(fs::exists(dir / kSessionFileName));
}

TEST_CASE("a NotCommitted from finalize does not abort and the directory still resumes") {
  TempDir temp;
  const Config cfg = synthetic_run(temp);
  ScriptedTerminalUi terminal(false);
  std::vector<std::string> log;
  RecordingStore* store = nullptr;
  RunHooks hooks = recording_hooks(log, store);
  RecordingStore* captured = nullptr;
  hooks.make_store = [&captured](SessionDir dir, SessionRecord record) {
    auto decorated = std::make_unique<RecordingStore>();
    decorated->inner = std::make_unique<JsonSessionStore>(std::move(dir), std::move(record));
    decorated->finalize_outcome = CommitOutcome::NotCommitted;
    captured = decorated.get();
    return std::unique_ptr<ISessionStore>(std::move(decorated));
  };

  CHECK(run_command(cfg, terminal, hooks) == 1);
  REQUIRE(captured != nullptr);
  CHECK(captured->finalize_calls == 1);
  // No abort is attempted: aborting here would make terminal a session that a
  // later invocation could still finish.
  CHECK(captured->abort_calls == 0);

  const fs::path dir = only_session_under(cfg.session_root);
  CHECK(state_of(dir) == SessionState::Running);
  // And the directory reopens.
  const LoadResult loaded = load_session(dir);
  CHECK_FALSE(loaded.error.has_value());
}

TEST_CASE("a NotCommitted from pause behaves the same way") {
  TempDir temp;
  const Config cfg = synthetic_run(temp);
  ScriptedTerminalUi terminal(false);
  std::vector<std::string> log;
  RecordingStore* store = nullptr;
  RunHooks hooks = recording_hooks(log, store);
  RecordingStore* captured = nullptr;
  hooks.make_store = [&captured](SessionDir dir, SessionRecord record) {
    auto decorated = std::make_unique<RecordingStore>();
    decorated->inner = std::make_unique<JsonSessionStore>(std::move(dir), std::move(record));
    decorated->pause_outcome = CommitOutcome::NotCommitted;
    captured = decorated.get();
    return std::unique_ptr<ISessionStore>(std::move(decorated));
  };
  hooks.make_provider = [](const Config&, ui::ITerminalUi&) {
    struct AlwaysQuit final : IAngleProvider {
      std::string_view name() const override { return "scripted"; }
      bool is_automated() const override { return true; }
      AngleOutcome request(double planned_deg, int) override {
        AngleOutcome out;
        out.cmd = AngleOutcome::Cmd::Quit;
        out.actual_deg = planned_deg;
        return out;
      }
    };
    return std::unique_ptr<IAngleProvider>(new AlwaysQuit());
  };

  CHECK(run_command(cfg, terminal, hooks) == 1);
  REQUIRE(captured != nullptr);
  CHECK(captured->pause_calls == 1);
  CHECK(captured->abort_calls == 0);

  const fs::path dir = only_session_under(cfg.session_root);
  CHECK(state_of(dir) == SessionState::Running);
  CHECK_FALSE(load_session(dir).error.has_value());
}

TEST_CASE("a device that cannot be opened is exit 3 and creates no session") {
  TempDir temp;
  Config cfg = synthetic_run(temp);
  ScriptedTerminalUi terminal(false);
  RunHooks hooks;
  hooks.make_source = [](const Config&, std::string& error) {
    error = "RTL-SDR device 0 is busy - close the SDR application that is using it.";
    return std::unique_ptr<ISampleSource>();
  };

  CHECK(run_command(cfg, terminal, hooks) == 3);
  CHECK_FALSE(fs::exists(cfg.session_root));
}

TEST_CASE("resume completes the session, and resuming a completed one is refused") {
  TempDir temp;
  const Config cfg = synthetic_run(temp);
  ScriptedTerminalUi terminal(false);

  // Quit after the first visit.
  {
    std::vector<std::string> log;
    RecordingStore* store = nullptr;
    RunHooks hooks = recording_hooks(log, store);
    hooks.make_provider = [](const Config&, ui::ITerminalUi&) {
      struct QuitAfterOne final : IAngleProvider {
        int calls = 0;
        std::string_view name() const override { return "scripted"; }
        bool is_automated() const override { return true; }
        AngleOutcome request(double planned_deg, int) override {
          AngleOutcome out;
          out.actual_deg = planned_deg;
          out.cmd = ++calls > 1 ? AngleOutcome::Cmd::Quit : AngleOutcome::Cmd::Proceed;
          return out;
        }
      };
      return std::unique_ptr<IAngleProvider>(new QuitAfterOne());
    };
    CHECK(run_command(cfg, terminal, hooks) == 0);
  }

  const fs::path dir = only_session_under(cfg.session_root);
  REQUIRE(state_of(dir) == SessionState::Paused);

  // Resume and finish.
  {
    Config resume_cfg = cfg;
    resume_cfg.resume_dir = dir.string();
    std::vector<std::string> log;
    RecordingStore* store = nullptr;
    RunHooks hooks = recording_hooks(log, store);
    CHECK(run_command(resume_cfg, terminal, hooks) == 0);
    CHECK(std::count(log.begin(), log.end(), "load_session") == 1);
  }

  SessionRecord rec;
  from_json(nlohmann::json::parse(read_whole(dir / kSessionFileName)), rec);
  CHECK(rec.state == SessionState::Completed);
  CHECK(rec.receiver_segments.size() == 2);
  // Every visit exactly once.
  std::set<std::string> visits;
  for (const AttemptRecord& a : rec.attempts) visits.insert(a.visit_id);
  CHECK(visits.size() == rec.plan.visits.size());

  // A further resume of the now-completed directory is refused.
  {
    Config again = cfg;
    again.resume_dir = dir.string();
    ScriptedTerminalUi refused(false);
    std::vector<std::string> log;
    RecordingStore* store = nullptr;
    RunHooks hooks = recording_hooks(log, store);
    CHECK(run_command(again, refused, hooks) == 2);
    bool named = false;
    for (const std::string& line : refused.emitted()) {
      if (line.find("completed") != std::string::npos) named = true;
    }
    CHECK(named);
  }
}

TEST_CASE("a resume that would change an experiment-defining field is refused") {
  TempDir temp;
  const Config cfg = synthetic_run(temp);
  ScriptedTerminalUi terminal(false);
  {
    std::vector<std::string> log;
    RecordingStore* store = nullptr;
    RunHooks hooks = recording_hooks(log, store);
    hooks.make_provider = [](const Config&, ui::ITerminalUi&) {
      struct AlwaysQuit final : IAngleProvider {
        std::string_view name() const override { return "scripted"; }
        bool is_automated() const override { return true; }
        AngleOutcome request(double planned_deg, int) override {
          AngleOutcome out;
          out.cmd = AngleOutcome::Cmd::Quit;
          out.actual_deg = planned_deg;
          return out;
        }
      };
      return std::unique_ptr<IAngleProvider>(new AlwaysQuit());
    };
    CHECK(run_command(cfg, terminal, hooks) == 0);
  }

  const fs::path dir = only_session_under(cfg.session_root);
  Config changed = cfg;
  changed.resume_dir = dir.string();
  changed.duration_s = 30.0;   // experiment-defining

  ScriptedTerminalUi refused(false);
  std::vector<std::string> log;
  RecordingStore* store = nullptr;
  RunHooks hooks = recording_hooks(log, store);
  CHECK(run_command(changed, refused, hooks, {"duration_s"}) == 2);
  bool named = false;
  for (const std::string& line : refused.emitted()) {
    if (line.find("--duration") != std::string::npos) named = true;
  }
  CHECK(named);
}

TEST_CASE("report re-renders a stored session and changes nothing it recorded") {
  TempDir temp;
  const Config cfg = synthetic_run(temp);
  {
    ScriptedTerminalUi terminal(false);
    std::vector<std::string> log;
    RecordingStore* store = nullptr;
    RunHooks hooks = recording_hooks(log, store);
    CHECK(run_command(cfg, terminal, hooks) == 0);
  }
  const fs::path dir = only_session_under(cfg.session_root);
  const std::string before = read_whole(dir / kSessionFileName);

  ScriptedTerminalUi terminal(false);
  CHECK(report_command(dir, cfg, terminal) == 0);
  CHECK(read_whole(dir / kSessionFileName) == before);

  bool headline = false;
  for (const std::string& line : terminal.emitted()) {
    if (line.find("Exploratory ranking only") != std::string::npos) headline = true;
  }
  CHECK(headline);
}

TEST_CASE("a stricter reporting threshold at report time adds a warning and moves no number") {
  TempDir temp;
  Config cfg = synthetic_run(temp);
  cfg.rounds = 2;                  // two captures per angle
  cfg.min_captures_advisory = 2;   // which is exactly enough
  {
    ScriptedTerminalUi terminal(false);
    std::vector<std::string> log;
    RecordingStore* store = nullptr;
    RunHooks hooks = recording_hooks(log, store);
    CHECK(run_command(cfg, terminal, hooks) == 0);
  }
  const fs::path dir = only_session_under(cfg.session_root);
  const std::string stored = read_whole(dir / kSessionFileName);

  ScriptedTerminalUi lenient(false);
  CHECK(report_command(dir, cfg, lenient) == 0);

  Config strict = cfg;
  strict.min_captures_advisory = 100;   // every angle is now thin
  ScriptedTerminalUi harsh(false);
  CHECK(report_command(dir, strict, harsh) == 0);

  auto contains = [](const ScriptedTerminalUi& ui, std::string_view needle) {
    for (const std::string& line : ui.emitted()) {
      if (line.find(needle) != std::string::npos) return true;
    }
    return false;
  };
  CHECK_FALSE(contains(lenient, "W1  Fewer than"));
  CHECK(contains(harsh, "W1  Fewer than"));

  // And the stored record is byte-identical: a reporting threshold is applied
  // when the report is rendered, never when a capture is taken.
  CHECK(read_whole(dir / kSessionFileName) == stored);
}

TEST_CASE("the device check reports without a device and never suggests a broad kill") {
  Config cfg;
  ScriptedTerminalUi terminal(false);
  const int code = device_check_command(cfg, terminal);
  CHECK((code == 0 || code == 3));
  CHECK_FALSE(terminal.emitted().empty());
  for (const std::string& line : terminal.emitted()) {
    CHECK(line.find("pkill") == std::string::npos);
  }
}

}  // TEST_SUITE
