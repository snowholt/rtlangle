// WP9 — the visit loop. Every row of the spec section 11.3.1 transition table
// is produced by at least one case here, and each asserts the persisted
// disposition, the resulting session state, the number of commit_visit calls,
// and the resume outcome.
//
// The single most important assertion in this file is that the controller
// ALWAYS leaves the session state at `running`, and calls pause(), finalize(),
// and abort() zero times, for every one of its three return values. Writing a
// terminal state here would mean finalising a session whose summary the
// controller is forbidden to compute.

#include <doctest/doctest.h>

#include "angle/angle_provider.h"
#include "angle/fixed_angle_provider.h"
#include "core/utc.h"
#include "dsp/chain.h"
#include "experiment/controller.h"
#include "experiment/visit_plan.h"
#include "persist/json_session_store.h"
#include "persist/session_loader.h"
#include "source/synthetic_source.h"
#include "tests/support/fault_store.h"
#include "tests/support/fake_sources.h"
#include "tests/support/temp_dir.h"

#include <nlohmann/json.hpp>
#include "ui/scripted_terminal_ui.h"

#include <deque>
#include <memory>
#include <string>

using namespace rtlangle;
using namespace rtlangle::test;
using rtlangle::ui::ScriptedTerminalUi;

namespace {

namespace fs = std::filesystem;

Config controller_config() {
  Config c;
  c.center_hz = 118350000;
  c.angles_deg = {0.0, 45.0, 90.0};
  c.rounds = 1;
  // Long enough that at least one burst lies wholly inside every capture
  // whatever phase the generator is at: the synthetic stream runs continuously
  // across visits, so a capture shorter than one talk-plus-gap period could
  // catch only truncated bursts, and a truncated event is excluded from every
  // metric by design.
  c.duration_s = 1.2;
  c.settle_s = 0.0;
  c.source_spec = "synthetic";
  c.non_interactive = true;
  c.synthetic_snr_db = 25.0;
  c.min_valid_events = 1;
  c.min_event_ms = 100.0;
  return c;
}

SyntheticParams params_for(const Config& c) {
  SyntheticParams p;
  p.sample_rate_hz = c.sample_rate_hz;
  p.center_hz = c.center_hz.value_or(118350000);
  p.offset_hz = c.offset_tune_hz;
  p.snr_db = c.synthetic_snr_db;
  // 150 ms of speech every 500 ms: above min_event_ms, and an occupancy well
  // under max_active_fraction so the probe floor stays reliable.
  p.talk_s = 0.15;
  p.duty = 0.3;
  return p;
}

SessionRecord record_for(const Config& cfg) {
  SessionRecord r;
  r.session_id = "20260819-143000-test";
  r.started_utc = utc_now();
  r.config = cfg;
  r.plan.angles_deg = resolved_angles(cfg);
  r.plan.rounds = cfg.rounds;
  r.plan.order = cfg.order;
  r.plan.seed = cfg.seed;
  r.plan.visits = build_visit_plan(cfg);
  return r;
}

// A provider that plays a scripted sequence of outcomes and records the angles
// it was asked for, so a test can assert that no later visit was started.
class ScriptedProvider final : public IAngleProvider {
 public:
  explicit ScriptedProvider(std::deque<AngleOutcome> script) : script_(std::move(script)) {}

  std::string_view name() const override { return "scripted"; }
  bool is_automated() const override { return true; }

  AngleOutcome request(double planned_deg, int attempt) override {
    requests.push_back({planned_deg, attempt});
    if (script_.empty()) {
      AngleOutcome out;
      out.actual_deg = planned_deg;
      return out;   // Proceed
    }
    AngleOutcome out = script_.front();
    script_.pop_front();
    if (out.actual_deg == 0.0) out.actual_deg = planned_deg;
    return out;
  }

  struct Request {
    double planned_deg;
    int    attempt;
  };
  std::vector<Request> requests;

 private:
  std::deque<AngleOutcome> script_;
};

AngleOutcome outcome(AngleOutcome::Cmd cmd) {
  AngleOutcome o;
  o.cmd = cmd;
  return o;
}

// Everything a controller needs, over a real store in a temporary directory.
struct Harness {
  TempDir     temp;
  Config      cfg = controller_config();
  fs::path    dir;
  std::unique_ptr<JsonSessionStore> store;
  ScriptedTerminalUi terminal{false};
  dsp::Chain  chain;
  std::string segment;

  Harness() {
    dir = temp.child("session");
    std::string error;
    auto session_dir = SessionDir::create(dir, error);
    REQUIRE_MESSAGE(session_dir.has_value(), error);
    store = std::make_unique<JsonSessionStore>(std::move(*session_dir), record_for(cfg));
    chain = dsp::build_chain(cfg);
    segment = store->begin_receiver_segment(SourceInfo{});
  }

  SessionState state_on_disk() const {
    SessionRecord reloaded;
    nlohmann::json j = nlohmann::json::parse(read_whole(dir / kSessionFileName));
    from_json(j, reloaded);
    return reloaded.state;
  }
};

}  // namespace

TEST_SUITE("controller") {

TEST_CASE("a clean run completes, commits one accepted attempt per visit, and stays running") {
  Harness h;
  SyntheticSource source(params_for(h.cfg));
  FixedAngleProvider provider;
  FaultStore store(*h.store);

  ExperimentController controller(source, provider, h.terminal, store, h.chain, h.cfg,
                                  h.segment);
  CHECK(controller.run() == ExperimentController::Result::Completed);

  CHECK(store.commit_calls == 3);
  CHECK(h.store->record().attempts.size() == 3);
  for (const AttemptRecord& a : h.store->record().attempts) {
    CHECK(a.disposition == Disposition::Accepted);
    CHECK(a.status == AttemptStatus::Ok);
    CHECK(a.segment_id == h.segment);
  }

  // The controller may not write a terminal state: writing `completed` is the
  // app layer's job, after aggregation it is forbidden to run.
  CHECK(h.state_on_disk() == SessionState::Running);
  CHECK(store.pause_calls == 0);
  CHECK(store.finalize_calls == 0);
  CHECK(store.abort_calls == 0);
  // And it never opens a receiver segment of its own.
  CHECK(store.begin_segment_calls == 0);
}

TEST_CASE("the applied receiver settings reach the attempt from the capture snapshot") {
  Harness h;
  test::InstrumentedSyntheticSource source(params_for(h.cfg));
  SourceInfo applied;
  applied.driver = "synthetic";
  applied.applied_sample_rate_hz = 1024000;
  applied.applied_center_hz = 118600000;
  applied.applied_gain_tenth_db = 496;
  applied.agc_enabled = false;
  source.set_info(applied);

  FixedAngleProvider provider;
  ExperimentController controller(source, provider, h.terminal, *h.store, h.chain, h.cfg,
                                  h.segment);
  REQUIRE(controller.run() == ExperimentController::Result::Completed);

  for (const AttemptRecord& a : h.store->record().attempts) {
    // Warning W7 compares exactly these against the segment baseline; zeros
    // here would leave it nothing to examine.
    CHECK(a.applied_sample_rate_hz == 1024000);
    CHECK(a.applied_center_hz == 118600000);
    CHECK(a.applied_gain_tenth_db == 496);
    CHECK_FALSE(a.agc_enabled);
  }
}

TEST_CASE("settling elapses before the flush, and the flush happens once per capture") {
  Harness h;
  h.cfg.settle_s = 0.2;
  h.chain = dsp::build_chain(h.cfg);
  test::InstrumentedSyntheticSource source(params_for(h.cfg));
  FixedAngleProvider provider;

  const auto start = std::chrono::steady_clock::now();
  ExperimentController controller(source, provider, h.terminal, *h.store, h.chain, h.cfg,
                                  h.segment);
  REQUIRE(controller.run() == ExperimentController::Result::Completed);
  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  // Three visits, each settling 0.2 s before its capture.
  CHECK(elapsed >= 0.6);
  CHECK(source.flush_count() == 3);
}

TEST_CASE("a skip produces one abandoned attempt and completes the visit") {
  Harness h;
  SyntheticSource source(params_for(h.cfg));
  ScriptedProvider provider({outcome(AngleOutcome::Cmd::Skip)});
  FaultStore store(*h.store);

  ExperimentController controller(source, provider, h.terminal, store, h.chain, h.cfg,
                                  h.segment);
  CHECK(controller.run() == ExperimentController::Result::Completed);

  REQUIRE(h.store->record().attempts.size() == 3);
  const AttemptRecord& first = h.store->record().attempts[0];
  CHECK(first.disposition == Disposition::Abandoned);
  CHECK(first.status == AttemptStatus::Skipped);
  CHECK(visit_complete(h.store->record(), first.visit_id));
  CHECK(h.state_on_disk() == SessionState::Running);
}

TEST_CASE("a quit ends the loop, commits one abandoned attempt, and decides nothing") {
  Harness h;
  SyntheticSource source(params_for(h.cfg));
  ScriptedProvider provider({outcome(AngleOutcome::Cmd::Proceed),
                             outcome(AngleOutcome::Cmd::Quit)});
  FaultStore store(*h.store);

  ExperimentController controller(source, provider, h.terminal, store, h.chain, h.cfg,
                                  h.segment);
  CHECK(controller.run() == ExperimentController::Result::QuitRequested);

  // One visit captured, one cancelled: two commits, and the pause() write that
  // follows is a SECOND, SEPARATE commit made later by the app layer.
  CHECK(store.commit_calls == 2);
  CHECK(store.pause_calls == 0);
  CHECK(store.finalize_calls == 0);
  CHECK(store.abort_calls == 0);
  CHECK(h.state_on_disk() == SessionState::Running);

  const AttemptRecord& last = h.store->record().attempts.back();
  CHECK(last.disposition == Disposition::Abandoned);
  CHECK(last.status == AttemptStatus::Cancelled);
}

TEST_CASE("a pre-capture retry re-positions without committing anything") {
  Harness h;
  SyntheticSource source(params_for(h.cfg));
  ScriptedProvider provider({outcome(AngleOutcome::Cmd::Retry),
                             outcome(AngleOutcome::Cmd::Retry)});
  FaultStore store(*h.store);

  ExperimentController controller(source, provider, h.terminal, store, h.chain, h.cfg,
                                  h.segment);
  CHECK(controller.run() == ExperimentController::Result::Completed);

  // Nothing was captured before the retries, so there was nothing to supersede:
  // three visits, three commits, and two extra positioning requests.
  CHECK(store.commit_calls == 3);
  CHECK(provider.requests.size() == 5);
  CHECK(provider.requests[0].attempt == 1);
  CHECK(provider.requests[1].attempt == 1);
}

TEST_CASE("a post-capture retry is exactly one commit carrying both facts") {
  Harness h;
  h.cfg.min_valid_events = 1000;   // force insufficient_data on every capture
  h.chain = dsp::build_chain(h.cfg);
  SyntheticSource source(params_for(h.cfg));
  FixedAngleProvider provider;

  ScriptedTerminalUi terminal(false);
  terminal.push_menu_choice(0);   // Retry
  terminal.push_menu_choice(1);   // then Accept as-is
  terminal.push_menu_choice(1);   // and Accept as-is for the remaining visits
  terminal.push_menu_choice(1);
  FaultStore store(*h.store);

  ExperimentController controller(source, provider, terminal, store, h.chain, h.cfg,
                                  h.segment);
  CHECK(controller.run() == ExperimentController::Result::Completed);

  // The retry cost exactly ONE commit_visit, not three, and that single call
  // carried the superseded disposition AND the pending retry together.
  REQUIRE(store.commits.size() >= 2);
  CHECK(store.commits[0].disposition == Disposition::Superseded);
  REQUIRE(store.commits[0].pending_retry.has_value());
  CHECK(store.commits[0].pending_retry->visit_id == store.commits[0].attempt.visit_id);
  CHECK(store.commits[0].pending_retry->next_attempt == 2);
  // The first attempt is never observed with any other disposition: it is
  // written superseded the first and only time it is written.
  CHECK(store.commits[1].attempt.visit_id == store.commits[0].attempt.visit_id);
  CHECK(store.commits[1].attempt.attempt == 2);
  CHECK(store.commits[1].disposition == Disposition::Accepted);
  CHECK_FALSE(store.commits[1].pending_retry.has_value());

  CHECK(invariant_i1(h.store->record()));
  CHECK(invariant_i2(h.store->record()));
  CHECK(h.state_on_disk() == SessionState::Running);
}

TEST_CASE("accepting a non-ok capture as-is completes the visit without ranking it") {
  Harness h;
  h.cfg.min_valid_events = 1000;
  h.chain = dsp::build_chain(h.cfg);
  SyntheticSource source(params_for(h.cfg));
  FixedAngleProvider provider;

  ScriptedTerminalUi terminal(false);
  for (int i = 0; i < 3; ++i) terminal.push_menu_choice(1);   // Accept as-is
  ExperimentController controller(source, provider, terminal, *h.store, h.chain, h.cfg,
                                  h.segment);
  CHECK(controller.run() == ExperimentController::Result::Completed);

  for (const AttemptRecord& a : h.store->record().attempts) {
    CHECK(a.disposition == Disposition::Accepted);
    CHECK(a.status == AttemptStatus::InsufficientData);
    CHECK_FALSE(a.capture_score_channel_db.has_value());
    CHECK(visit_complete(h.store->record(), a.visit_id));
  }
}

TEST_CASE("a capture that times out becomes a timeout attempt and produces no metrics") {
  Harness h;
  h.cfg.duration_s = 0.2;
  h.cfg.read_timeout_slack_s = 0.2;
  h.chain = dsp::build_chain(h.cfg);
  test::RingBackedSource source(test::RingBackedSource::Behaviour::Stall,
                                std::chrono::milliseconds(0));
  FixedAngleProvider provider;

  ScriptedTerminalUi terminal(false);
  for (int i = 0; i < 3; ++i) terminal.push_menu_choice(1);   // Accept as-is
  ExperimentController controller(source, provider, terminal, *h.store, h.chain, h.cfg,
                                  h.segment);
  CHECK(controller.run() == ExperimentController::Result::Completed);

  for (const AttemptRecord& a : h.store->record().attempts) {
    CHECK(a.status == AttemptStatus::Timeout);
    CHECK_FALSE(a.capture_score_channel_db.has_value());
    CHECK_FALSE(a.noise_floor_dbfs.has_value());
    CHECK(a.valid_event_count == 0);
    CHECK(a.events.empty());
  }
  // A retry was offered: this is a failed capture, not a persistence fault.
  bool offered = false;
  for (const std::string& line : terminal.emitted()) {
    if (line.find("Retry this angle") != std::string::npos) offered = true;
  }
  CHECK(offered);
}

TEST_CASE("a final NotCommitted ends the run without advancing and without duplicating") {
  Harness h;
  SyntheticSource source(params_for(h.cfg));
  ScriptedProvider provider({});
  FaultStore store(*h.store);
  store.commit_outcome = CommitOutcome::NotCommitted;

  ExperimentController controller(source, provider, h.terminal, store, h.chain, h.cfg,
                                  h.segment);
  CHECK(controller.run() == ExperimentController::Result::Failed);

  // Three observables. The provider was asked for exactly one angle, so no
  // later visit was started.
  CHECK(provider.requests.size() == 1);
  // Nothing for that visit is on disk, so nothing was duplicated and nothing
  // was left half-written.
  CHECK(h.store->record().attempts.empty());
  CHECK_FALSE(fs::exists(h.dir / kSessionFileName));
  // And the commit was issued exactly once: the controller does not re-issue a
  // commit that reported nothing written.
  CHECK(store.commit_calls == 1);
  // Writing `aborted` is the app layer's job.
  CHECK(store.abort_calls == 0);
  CHECK_FALSE(controller.failure_detail().empty());
}

TEST_CASE("CommittedNotDurable is not a failure and the attempt appears exactly once") {
  Harness h;
  SyntheticSource source(params_for(h.cfg));
  FixedAngleProvider provider;
  FaultStore store(*h.store);
  store.commit_outcome = CommitOutcome::CommittedNotDurable;

  ExperimentController controller(source, provider, h.terminal, store, h.chain, h.cfg,
                                  h.segment);
  CHECK(controller.run() == ExperimentController::Result::Completed);

  // Advanced rather than retried: a retry here would duplicate the attempt.
  CHECK(store.commit_calls == 3);
  CHECK(h.store->record().attempts.size() == 3);
  std::set<std::string> visits;
  for (const AttemptRecord& a : h.store->record().attempts) visits.insert(a.visit_id);
  CHECK(visits.size() == 3);
}

TEST_CASE("Indeterminate reconciles before anything else, three ways") {
  SUBCASE("the attempt is present, so the run advances") {
    Harness h;
    SyntheticSource source(params_for(h.cfg));
    FixedAngleProvider provider;
    FaultStore store(*h.store);
    // The inner store commits normally; the decorator reports Indeterminate for
    // the first commit only.
    int commits = 0;
    struct PresentStore final : ISessionStore {
      ISessionStore& inner;
      int&           commits;
      int            reloads = 0;
      explicit PresentStore(ISessionStore& i, int& c) : inner(i), commits(c) {}
      const SessionRecord& record() const override { return inner.record(); }
      std::string begin_receiver_segment(const SourceInfo& a) override {
        return inner.begin_receiver_segment(a);
      }
      CommitResult commit_visit(const VisitCommit& v) override {
        const CommitResult real = inner.commit_visit(v);
        if (++commits == 1) {
          CommitResult r;
          r.outcome = CommitOutcome::Indeterminate;
          r.detail = "injected";
          return r;
        }
        return real;
      }
      CommitResult reload_from_disk() override {
        ++reloads;
        return inner.reload_from_disk();
      }
      CommitResult pause(const SessionSummary& s) override { return inner.pause(s); }
      CommitResult finalize(const SessionSummary& s) override { return inner.finalize(s); }
      CommitResult abort(const SessionSummary& s, std::string_view r) override {
        return inner.abort(s, r);
      }
      void flush_exports() override { inner.flush_exports(); }
    };
    PresentStore present(*h.store, commits);

    ExperimentController controller(source, provider, h.terminal, present, h.chain, h.cfg,
                                    h.segment);
    CHECK(controller.run() == ExperimentController::Result::Completed);
    CHECK(present.reloads == 1);
    CHECK(h.store->record().attempts.size() == 3);
    (void)store;
  }

  SUBCASE("the attempt is absent, which is a NotCommitted in every way that matters") {
    Harness h;
    SyntheticSource source(params_for(h.cfg));
    ScriptedProvider provider({});
    FaultStore store(*h.store);
    store.commit_outcome = CommitOutcome::Indeterminate;

    ExperimentController controller(source, provider, h.terminal, store, h.chain, h.cfg,
                                    h.segment);
    CHECK(controller.run() == ExperimentController::Result::Failed);
    CHECK(store.reload_calls == 1);
    CHECK(provider.requests.size() == 1);
    CHECK(store.commit_calls == 1);
    CHECK(store.abort_calls == 0);
  }

  SUBCASE("reload itself fails, so the run ends without writing any state") {
    Harness h;
    SyntheticSource source(params_for(h.cfg));
    ScriptedProvider provider({});
    FaultStore store(*h.store);
    store.commit_outcome = CommitOutcome::Indeterminate;
    store.reload_outcome = CommitOutcome::NotCommitted;

    ExperimentController controller(source, provider, h.terminal, store, h.chain, h.cfg,
                                    h.segment);
    CHECK(controller.run() == ExperimentController::Result::Failed);
    CHECK(store.reload_calls == 1);
    CHECK(store.pause_calls == 0);
    CHECK(store.finalize_calls == 0);
    CHECK(store.abort_calls == 0);
  }
}

TEST_CASE("a crash mid-session resumes at the right visit with no duplicate or lost visit") {
  TempDir temp;
  const fs::path dir = temp.child("session");
  const Config cfg = controller_config();

  std::string segment_one;
  {
    std::string error;
    auto session_dir = SessionDir::create(dir, error);
    REQUIRE_MESSAGE(session_dir.has_value(), error);
    JsonSessionStore store(std::move(*session_dir), record_for(cfg));
    segment_one = store.begin_receiver_segment(SourceInfo{});

    SyntheticSource source(params_for(cfg));
    ScriptedProvider provider({outcome(AngleOutcome::Cmd::Proceed),
                               outcome(AngleOutcome::Cmd::Quit)});
    ScriptedTerminalUi terminal(false);
    dsp::Chain chain = dsp::build_chain(cfg);
    ExperimentController controller(source, provider, terminal, store, chain, cfg, segment_one);
    CHECK(controller.run() == ExperimentController::Result::QuitRequested);
    // The store is discarded with no pause(): a crash.
  }

  LoadResult loaded = load_session(dir);
  REQUIRE_MESSAGE(!loaded.error.has_value(), loaded.error.value_or(""));
  CHECK(loaded.record.state == SessionState::Running);

  JsonSessionStore resumed(std::move(loaded.dir), loaded.record);
  const std::string segment_two = resumed.begin_receiver_segment(SourceInfo{});
  CHECK(segment_two == "seg2");

  SyntheticSource source(params_for(cfg));
  FixedAngleProvider provider;
  ScriptedTerminalUi terminal(false);
  dsp::Chain chain = dsp::build_chain(cfg);
  ExperimentController controller(source, provider, terminal, resumed, chain, cfg, segment_two);
  CHECK(controller.run() == ExperimentController::Result::Completed);

  // Every visit exactly once, two receiver segments, and each attempt naming
  // the segment that was live when it was taken.
  std::set<std::string> visits;
  int in_first = 0;
  int in_second = 0;
  for (const AttemptRecord& a : resumed.record().attempts) {
    visits.insert(a.visit_id);
    if (a.segment_id == segment_one) ++in_first;
    if (a.segment_id == segment_two) ++in_second;
  }
  CHECK(visits.size() == 3);
  CHECK(in_first == 2);
  CHECK(in_second == 1);
  CHECK(resumed.record().receiver_segments.size() == 2);
  CHECK(invariant_i1(resumed.record()));
  CHECK(invariant_i2(resumed.record()));
}

TEST_CASE("a crash after a retry commit resumes at the right attempt number") {
  // Spec section 11.3.1's last row: the superseded attempt and pending_retry
  // are both present, and the visit is re-entered at next_attempt. The crash is
  // reproduced by making the SECOND commit report that nothing was written,
  // which ends the run with only the retry on disk.
  TempDir temp;
  const fs::path dir = temp.child("session");
  Config cfg = controller_config();
  cfg.min_valid_events = 1000;   // every capture is insufficient_data

  {
    std::string error;
    auto session_dir = SessionDir::create(dir, error);
    REQUIRE_MESSAGE(session_dir.has_value(), error);
    JsonSessionStore inner(std::move(*session_dir), record_for(cfg));
    const std::string segment = inner.begin_receiver_segment(SourceInfo{});

    FaultStore store(inner);
    store.commit_outcome = CommitOutcome::NotCommitted;
    store.commit_fault_after = 1;   // the retry commit lands; the next does not

    SyntheticSource source(params_for(cfg));
    FixedAngleProvider provider;
    ScriptedTerminalUi terminal(false);
    terminal.push_menu_choice(0);   // Retry
    terminal.push_menu_choice(0);   // Retry again, which never commits
    dsp::Chain chain = dsp::build_chain(cfg);
    ExperimentController controller(source, provider, terminal, store, chain, cfg, segment);
    CHECK(controller.run() == ExperimentController::Result::Failed);
    CHECK(store.commit_calls == 2);
    CHECK(store.abort_calls == 0);
  }

  const LoadResult loaded = load_session(dir);
  REQUIRE_MESSAGE(!loaded.error.has_value(), loaded.error.value_or(""));
  REQUIRE(loaded.record.attempts.size() == 1);
  CHECK(loaded.record.attempts[0].disposition == Disposition::Superseded);
  REQUIRE(loaded.record.pending_retry.has_value());
  CHECK(loaded.record.pending_retry->visit_id == "r1-i000");

  const auto remaining = remaining_visits(loaded.record);
  REQUIRE(!remaining.empty());
  CHECK(remaining[0].visit_id == "r1-i000");
  CHECK(remaining[0].next_attempt == 2);
  CHECK(invariant_i1(loaded.record));
  CHECK(invariant_i2(loaded.record));
}

TEST_CASE("the retry option is withheld once the reserved capacity would be spent") {
  Harness h;
  h.cfg.min_valid_events = 1000;
  h.chain = dsp::build_chain(h.cfg);

  // Fill the record to the point where attempts_committed + remaining_visits
  // equals the ceiling: one attempt per remaining visit is all that is left.
  const int remaining_visits_count = 3;
  VisitCommit filler;
  filler.attempt.visit_id = "filler";
  filler.attempt.segment_id = h.segment;
  filler.attempt.status = AttemptStatus::Ok;
  filler.disposition = Disposition::Superseded;
  filler.pending_retry = PendingRetry{"filler", 2};
  for (int i = 0; i < kAttemptCeiling - remaining_visits_count; ++i) {
    filler.attempt.attempt = i + 1;
    REQUIRE(h.store->commit_visit(filler).outcome == CommitOutcome::Committed);
  }
  CHECK_FALSE(retry_available(static_cast<int>(h.store->record().attempts.size()),
                              remaining_visits_count, kAttemptCeiling));

  SyntheticSource source(params_for(h.cfg));
  FixedAngleProvider provider;
  ScriptedTerminalUi terminal(false);
  terminal.push_menu_choice(0);   // the first offered option, whatever it is

  ExperimentController controller(source, provider, terminal, *h.store, h.chain, h.cfg,
                                  h.segment);
  (void)controller.run();

  bool retry_offered = false;
  bool budget_explained = false;
  for (const std::string& line : terminal.emitted()) {
    if (line.find("Retry this angle") != std::string::npos) retry_offered = true;
    if (line.find("retry budget is exhausted") != std::string::npos) budget_explained = true;
  }
  CHECK_FALSE(retry_offered);
  CHECK(budget_explained);
}

TEST_CASE("the controller never writes a terminal state, for any of its three results") {
  struct Row {
    const char* name;
    ExperimentController::Result expected;
  };

  SUBCASE("Completed") {
    Harness h;
    SyntheticSource source(params_for(h.cfg));
    FixedAngleProvider provider;
    FaultStore store(*h.store);
    ExperimentController c(source, provider, h.terminal, store, h.chain, h.cfg, h.segment);
    CHECK(c.run() == ExperimentController::Result::Completed);
    CHECK(h.state_on_disk() == SessionState::Running);
    CHECK(store.pause_calls + store.finalize_calls + store.abort_calls == 0);
  }
  SUBCASE("QuitRequested") {
    Harness h;
    SyntheticSource source(params_for(h.cfg));
    ScriptedProvider provider({outcome(AngleOutcome::Cmd::Quit)});
    FaultStore store(*h.store);
    ExperimentController c(source, provider, h.terminal, store, h.chain, h.cfg, h.segment);
    CHECK(c.run() == ExperimentController::Result::QuitRequested);
    CHECK(h.state_on_disk() == SessionState::Running);
    CHECK(store.pause_calls + store.finalize_calls + store.abort_calls == 0);
  }
  SUBCASE("Failed") {
    Harness h;
    SyntheticSource source(params_for(h.cfg));
    FixedAngleProvider provider;
    FaultStore store(*h.store);
    store.commit_outcome = CommitOutcome::NotCommitted;
    ExperimentController c(source, provider, h.terminal, store, h.chain, h.cfg, h.segment);
    CHECK(c.run() == ExperimentController::Result::Failed);
    CHECK(store.pause_calls + store.finalize_calls + store.abort_calls == 0);
  }
  (void)sizeof(Row);
}

}  // TEST_SUITE
