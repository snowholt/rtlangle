#include "app/run_command.h"

#include "angle/fixed_angle_provider.h"
#include "angle/manual_angle_provider.h"
#include "app/device_check.h"
#include "core/utc.h"
#include "core/version.h"
#include "dsp/chain.h"
#include "experiment/aggregator.h"
#include "experiment/controller.h"
#include "experiment/quality_checks.h"
#include "experiment/visit_plan.h"
#include "persist/json_session_store.h"
#include "persist/session_loader.h"
#include "source/rtlsdr_source.h"
#include "source/source_factory.h"
#include "ui/report_renderer.h"

#include <random>
#include <sstream>

namespace rtlangle::app {
namespace {

void step(const RunHooks& hooks, std::string_view name) {
  if (hooks.observe) hooks.observe(name);
}

// The seed is drawn once when the operator did not choose one, used, and
// RECORDED, so the realised random order is reproducible after the fact.
void resolve_seed(Config& cfg) {
  if (cfg.seed != 0) return;
  std::random_device device;
  cfg.seed = (static_cast<std::uint64_t>(device()) << 32) ^ device();
  if (cfg.seed == 0) cfg.seed = 1;
}

SessionRecord new_record(const Config& cfg, const std::string& session_id,
                         const IAngleProvider& provider) {
  SessionRecord rec;
  rec.session_id = session_id;
  rec.tool_version = kToolVersion;
  rec.started_utc = utc_now();
  rec.config = cfg;
  rec.angle_provider.name = std::string(provider.name());
  rec.angle_provider.automated = provider.is_automated();
  rec.plan.angles_deg = resolved_angles(cfg);
  rec.plan.rounds = cfg.rounds;
  rec.plan.order = cfg.order;
  rec.plan.seed = cfg.seed;
  rec.plan.visits = build_visit_plan(cfg);
  return rec;
}

std::unique_ptr<IAngleProvider> build_provider(const Config& cfg, ui::ITerminalUi& terminal,
                                               const RunHooks& hooks) {
  if (hooks.make_provider) return hooks.make_provider(cfg, terminal);
  if (cfg.non_interactive || !terminal.interactive()) {
    return std::make_unique<FixedAngleProvider>();
  }
  return std::make_unique<ManualAngleProvider>(terminal, cfg);
}

}  // namespace

int device_check_command(const Config& cfg, ui::ITerminalUi& terminal) {
  const DeviceReport report = describe_devices(cfg);
  terminal.heading("Devices");
  for (const std::string& line : report.lines) terminal.info(line);
  return report.any_device ? 0 : 3;
}

int report_command(const std::filesystem::path& session_dir, const Config& cfg,
                   ui::ITerminalUi& terminal) {
  LoadResult loaded = load_session_read_only(session_dir);
  if (loaded.error.has_value()) {
    terminal.error(*loaded.error);
    return 1;
  }

  // The reporting thresholds of spec section 7.4 may be overridden at render
  // time: they are applied when the report is rendered, never when a capture is
  // taken, so re-rendering with a stricter threshold changes only which
  // warnings appear and cannot alter a stored number.
  SessionRecord record = loaded.record;
  record.config.report_metric = cfg.report_metric;
  record.config.min_captures_advisory = cfg.min_captures_advisory;
  record.config.min_effect_db = cfg.min_effect_db;
  record.config.yield_concordance_ratio = cfg.yield_concordance_ratio;
  record.config.noise_drift_warn_db = cfg.noise_drift_warn_db;

  SessionSummary summary = aggregate(record);
  evaluate_warnings(summary, record, record.config);
  summary.report_metric = cfg.report_metric;
  summary.partial = record.state != SessionState::Completed;
  ui::choose_report_metric(summary, terminal);

  const std::string text = ui::render_report(record, summary, !cfg.no_color);
  terminal.info(text);
  return 0;
}

int run_command(const Config& config, ui::ITerminalUi& terminal, const RunHooks& hooks,
                const std::set<std::string>& explicitly_set) {
  Config cfg = config;

  // ---- open the source ---------------------------------------------------
  step(hooks, "open_source");
  std::string error;
  std::unique_ptr<ISampleSource> source =
      hooks.make_source ? hooks.make_source(cfg, error) : make_source(cfg, error);
  if (source == nullptr) {
    terminal.error(error);
    return 3;   // device not found, busy, or refused
  }
  for (const std::string& note : take_source_notices(*source)) terminal.info(note);
  for (const std::string& note : applied_setting_notices(source->info())) terminal.info(note);

  // ---- the session record: new, or reopened ------------------------------
  SessionDir    dir;
  SessionRecord record;
  bool          resumed = false;

  if (!cfg.resume_dir.empty()) {
    step(hooks, "load_session");
    LoadResult loaded = load_session(cfg.resume_dir);
    if (loaded.error.has_value()) {
      terminal.error(*loaded.error);
      return 2;
    }
    for (const std::string& repair : loaded.repairs) terminal.warn(repair);

    const auto conflicts = resume_conflicts(loaded.record.config, cfg, explicitly_set);
    if (!conflicts.empty()) {
      for (const ValidationError& e : conflicts) terminal.error(e.message);
      return 2;
    }
    // A manual and an automated session are not the same experiment.
    const auto provider_probe = build_provider(cfg, terminal, hooks);
    if (loaded.record.angle_provider.automated != provider_probe->is_automated()) {
      terminal.error(
          "this session was recorded with an " +
          std::string(loaded.record.angle_provider.automated ? "automated" : "operator-driven") +
          " angle provider, and this invocation would use an " +
          std::string(provider_probe->is_automated() ? "automated" : "operator-driven") +
          " one. They are not the same experiment.");
      return 2;
    }

    // The stored configuration is authoritative for everything the experiment
    // is defined by; the operational and reporting fields come from this
    // invocation.
    record = std::move(loaded.record);
    dir = std::move(loaded.dir);
    cfg = record.config;
    cfg.report_metric = config.report_metric;
    cfg.min_captures_advisory = config.min_captures_advisory;
    cfg.min_effect_db = config.min_effect_db;
    cfg.yield_concordance_ratio = config.yield_concordance_ratio;
    cfg.noise_drift_warn_db = config.noise_drift_warn_db;
    cfg.settle_s = config.settle_s;
    cfg.no_color = config.no_color;
    cfg.non_interactive = config.non_interactive;
    cfg.max_retained_events = config.max_retained_events;
    cfg.read_timeout_factor = config.read_timeout_factor;
    cfg.read_timeout_slack_s = config.read_timeout_slack_s;
    cfg.setup_note = config.setup_note;
    record.config = cfg;
    resumed = true;
  } else {
    step(hooks, "create_session");
    resolve_seed(cfg);
    std::string session_id;
    auto created = create_session_dir(cfg.session_root, cfg.label, session_id, error);
    if (!created.has_value()) {
      terminal.error(error);
      return 1;
    }
    dir = std::move(*created);
    const auto provider_probe = build_provider(cfg, terminal, hooks);
    record = new_record(cfg, session_id, *provider_probe);
    terminal.info("Session directory: " + dir.path().string());
  }

  for (const std::string& note : config_notices(cfg)) terminal.info(note);

  // ---- the store, then the receiver segment ------------------------------
  step(hooks, "make_store");
  std::unique_ptr<ISessionStore> store =
      hooks.make_store ? hooks.make_store(std::move(dir), record)
                       : std::make_unique<JsonSessionStore>(std::move(dir), record);

  // The report renderer is wired unconditionally: a pause or a finalize that
  // silently wrote no report.txt would be a missing deliverable nobody noticed.
  if (auto* json_store = dynamic_cast<JsonSessionStore*>(store.get())) {
    json_store->set_report_renderer(
        [](const SessionRecord& r, const SessionSummary& s) {
          return ui::render_report(r, s, false);
        });
  }

  step(hooks, "begin_receiver_segment");
  const std::string segment = store->begin_receiver_segment(source->info());

  // ---- the provider, then the controller ---------------------------------
  step(hooks, "build_provider");
  auto provider = build_provider(cfg, terminal, hooks);

  step(hooks, "build_controller");
  dsp::Chain chain = dsp::build_chain(cfg);
  ExperimentController controller(*source, *provider, terminal, *store, chain, cfg, segment);

  step(hooks, "controller_run");
  const ExperimentController::Result result = controller.run();

  // ---- aggregation, which the controller is forbidden to run -------------
  step(hooks, "aggregate");
  SessionSummary summary = aggregate(store->record());
  step(hooks, "evaluate_warnings");
  evaluate_warnings(summary, store->record(), cfg);
  summary.report_metric = cfg.report_metric;
  summary.report_metric_source = "default";

  step(hooks, "choose_report_metric");
  ui::choose_report_metric(summary, terminal);

  step(hooks, "render");
  summary.partial = result != ExperimentController::Result::Completed;
  const std::string report_text = ui::render_report(store->record(), summary, !cfg.no_color);

  // ---- the terminal or paused state, written HERE and nowhere else -------
  switch (result) {
    case ExperimentController::Result::Completed: {
      step(hooks, "finalize");
      const CommitResult commit = store->finalize(summary);
      if (commit.outcome == CommitOutcome::NotCommitted) {
        // No abort is attempted. Everything this was about to summarise is
        // already committed and the record still says `running`, which resume
        // reopens; writing `aborted` here would make terminal a session that a
        // later invocation could still finish.
        terminal.error("the session summary could not be written: " + commit.detail +
                       "  Every measurement committed before this point is intact; reopen "
                       "the directory with --resume to finish the session.");
        return 1;
      }
      terminal.info(report_text);
      return 0;
    }
    case ExperimentController::Result::QuitRequested: {
      step(hooks, "pause");
      const CommitResult commit = store->pause(summary);
      if (commit.outcome == CommitOutcome::NotCommitted) {
        terminal.error("the partial summary could not be written: " + commit.detail +
                       "  Every measurement committed before this point is intact; reopen "
                       "the directory with --resume to continue.");
        return 1;
      }
      terminal.info(report_text);
      return 0;
    }
    case ExperimentController::Result::Failed: {
      step(hooks, "abort");
      const std::string reason = controller.failure_detail().empty()
                                     ? std::string("the session could not be continued safely.")
                                     : controller.failure_detail();
      const CommitResult commit = store->abort(summary, reason);
      if (commit.outcome == CommitOutcome::NotCommitted) {
        // The terminus. The record is left in its last committed state:
        // unchanged, finished_utc not stamped, no abort_reason, and every
        // measurement committed before the fault still present. A store that
        // cannot write cannot record that it could not write.
        terminal.error(
            "the session failed and the failure itself could not be recorded: " +
            commit.detail +
            "  The record is unchanged at its last committed state and holds every "
            "measurement taken before the fault.");
        return 1;
      }
      terminal.info(report_text);
      return 1;
    }
  }
  (void)resumed;
  return 1;
}

}  // namespace rtlangle::app
