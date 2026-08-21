#include "experiment/controller.h"

#include "core/angle_math.h"
#include "core/utc.h"
#include "dsp/framer.h"
#include "metrics/event_detector.h"
#include "metrics/noise_floor.h"
#include "metrics/snr_estimator.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <sstream>
#include <thread>

namespace rtlangle {
namespace {

std::string degrees(double v) {
  std::ostringstream os;
  os.precision(3);
  os << std::defaultfloat << v;
  return os.str();
}

int count_incomplete_visits(const SessionRecord& rec) {
  int n = 0;
  for (const Visit& v : rec.plan.visits) {
    if (!visit_complete(rec, v.visit_id)) ++n;
  }
  return n;
}

bool record_holds_attempt(const SessionRecord& rec, const std::string& visit_id, int attempt) {
  for (const AttemptRecord& a : rec.attempts) {
    if (a.visit_id == visit_id && a.attempt == attempt) return true;
  }
  return false;
}

}  // namespace

ExperimentController::ExperimentController(ISampleSource& source, IAngleProvider& provider,
                                           ui::ITerminalUi& terminal, ISessionStore& store,
                                           dsp::Chain& chain, const Config& cfg,
                                           std::string segment_id)
    : source_(source),
      provider_(provider),
      terminal_(terminal),
      store_(store),
      chain_(chain),
      cfg_(cfg),
      segment_id_(std::move(segment_id)),
      spectrum_(1024, 16) {}

AttemptStatus ExperimentController::map_status(const CaptureOutcome& capture,
                                               const metrics::NoiseFloor& floor,
                                               int valid_events) const {
  switch (capture.status) {
    case CaptureOutcome::Status::Timeout: return AttemptStatus::Timeout;
    case CaptureOutcome::Status::SourceError: return AttemptStatus::SourceError;
    case CaptureOutcome::Status::Clipped: return AttemptStatus::Clipped;
    case CaptureOutcome::Status::InsufficientSamples: return AttemptStatus::InsufficientSamples;
    case CaptureOutcome::Status::Cancelled: return AttemptStatus::Cancelled;
    case CaptureOutcome::Status::Ok:
      break;
  }
  if (floor.status == metrics::FloorStatus::Unreliable) return AttemptStatus::NoiseFloorUnreliable;
  if (floor.status == metrics::FloorStatus::Unidentifiable) {
    return AttemptStatus::NoiseFloorUnidentifiable;
  }
  if (valid_events < cfg_.min_valid_events) return AttemptStatus::InsufficientData;
  return AttemptStatus::Ok;
}

bool ExperimentController::retry_offered() const {
  // Capacity for one attempt per remaining visit is reserved and can never be
  // spent on a retry, so the session can always reach its last visit.
  return retry_available(static_cast<int>(store_.record().attempts.size()),
                         count_incomplete_visits(store_.record()), kAttemptCeiling);
}

ExperimentController::VisitStep ExperimentController::apply_commit(const VisitCommit& commit,
                                                                   const CommitResult& result) {
  switch (result.outcome) {
    case CommitOutcome::Committed:
      return VisitStep::Advance;

    case CommitOutcome::CommittedNotDurable:
      // NOT a failure. The commit is in the record and every later read sees
      // it; retrying would duplicate the attempt.
      terminal_.warn(result.detail);
      return VisitStep::Advance;

    case CommitOutcome::NotCommitted:
      // Nothing was written, so there is no half state to repair. The run ends
      // here without advancing and without re-issuing the commit: re-issuing
      // could only duplicate the attempt in the case where the outcome was
      // misclassified.
      failure_detail_ = "a visit commit reported that nothing was written: " + result.detail;
      terminal_.error(failure_detail_);
      return VisitStep::Failed;

    case CommitOutcome::Indeterminate: {
      // The reconciliation of spec section 11.1.2, before anything else.
      terminal_.warn(result.detail);
      const CommitResult reload = store_.reload_from_disk();
      if (reload.outcome != CommitOutcome::Committed) {
        failure_detail_ =
            "a visit commit returned an unclassifiable error and the record could not be "
            "re-read to reconcile it: " + reload.detail;
        terminal_.error(failure_detail_);
        return VisitStep::Failed;
      }
      if (!invariant_i1(store_.record()) || !invariant_i2(store_.record())) {
        failure_detail_ =
            "the reloaded record violates the attempt invariants, so the session cannot be "
            "continued safely.";
        terminal_.error(failure_detail_);
        return VisitStep::Failed;
      }
      if (record_holds_attempt(store_.record(), commit.attempt.visit_id, commit.attempt.attempt)) {
        terminal_.info(
            "The commit did take place; the record on disk holds the attempt and the session "
            "continues.");
        return VisitStep::Advance;
      }
      // Absent: a NotCommitted in every respect that matters to the caller.
      failure_detail_ =
          "a visit commit returned an unclassifiable error and the attempt is absent from the "
          "record, so nothing was written.";
      terminal_.error(failure_detail_);
      return VisitStep::Failed;
    }
  }
  return VisitStep::Failed;
}

ExperimentController::VisitStep ExperimentController::run_visit(const Visit& visit, int attempt) {
  VisitCommit commit;
  commit.attempt.visit_id = visit.visit_id;
  commit.attempt.round = visit.round;
  commit.attempt.angle_index = visit.angle_index;
  commit.attempt.planned_deg = visit.planned_deg;
  commit.attempt.segment_id = segment_id_;
  commit.attempt.attempt = attempt;
  commit.attempt.duration_s = cfg_.duration_s;

  // 1. Position. A pre-capture Retry re-positions at the same attempt number:
  //    nothing has been captured, so there is nothing to supersede.
  AngleOutcome positioned = provider_.request(visit.planned_deg, attempt);
  while (positioned.cmd == AngleOutcome::Cmd::Retry) {
    positioned = provider_.request(visit.planned_deg, attempt);
  }
  if (positioned.note.has_value()) commit.attempt.note = positioned.note;

  if (positioned.cmd == AngleOutcome::Cmd::Skip) {
    commit.attempt.status = AttemptStatus::Skipped;
    commit.attempt.actual_deg = positioned.actual_deg;
    commit.attempt.started_utc = utc_now();
    commit.disposition = Disposition::Abandoned;
    commit.pending_retry.reset();
    return apply_commit(commit, store_.commit_visit(commit));
  }
  if (positioned.cmd == AngleOutcome::Cmd::Quit) {
    commit.attempt.status = AttemptStatus::Cancelled;
    commit.attempt.actual_deg = positioned.actual_deg;
    commit.attempt.started_utc = utc_now();
    commit.disposition = Disposition::Abandoned;
    commit.pending_retry.reset();
    const VisitStep step = apply_commit(commit, store_.commit_visit(commit));
    return step == VisitStep::Advance ? VisitStep::Quit : step;
  }

  commit.attempt.actual_deg = positioned.actual_deg;
  commit.attempt.angle_deviation_deg =
      circular_distance_deg(positioned.actual_deg, visit.planned_deg);

  // 2. Settling is the CONTROLLER's job, so the timing is identical for every
  //    provider and a future servo provider cannot change the measurement by
  //    changing it. The flush follows the settle and precedes the first read.
  if (cfg_.settle_s > 0.0) {
    terminal_.info("Settling for " + degrees(cfg_.settle_s) + " s.");
    std::this_thread::sleep_for(
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(cfg_.settle_s)));
  }
  chain_.reset();
  source_.flush();

  // 3. Capture.
  CaptureRunner runner(source_, chain_, cfg_);
  std::ostringstream label;
  label << "Capturing at " << degrees(visit.planned_deg) << " deg, round " << visit.round
        << ", attempt " << attempt;
  const std::string progress_label = label.str();
  const CaptureOutcome capture =
      runner.run([&](double fraction) { terminal_.progress(progress_label, fraction); });

  commit.attempt.started_utc = format_utc(capture.started_utc);
  commit.attempt.wall_duration_s = capture.wall_duration_s;
  commit.attempt.host_dropped_samples = capture.host_dropped_samples;
  commit.attempt.clipped_fraction = capture.clipped_fraction;
  // The applied receiver settings, from the snapshot taken at capture start.
  // Warning W7 compares exactly these against the segment baseline; without
  // them it would have nothing to examine and the three applied_* columns would
  // ship as zeros.
  commit.attempt.applied_sample_rate_hz = capture.applied.applied_sample_rate_hz;
  commit.attempt.applied_center_hz = capture.applied.applied_center_hz;
  commit.attempt.applied_gain_tenth_db = capture.applied.applied_gain_tenth_db;
  commit.attempt.agc_enabled = capture.applied.agc_enabled;

  // 4. Measure, but only on a capture that produced samples.
  metrics::NoiseFloor channel_floor;
  if (capture.status == CaptureOutcome::Status::Ok) {
    const auto channel_powers =
        dsp::frame_powers(capture.channel, dsp::kFrameLength, dsp::kFrameHop);
    const auto audio_powers = dsp::frame_powers(capture.audio, dsp::kFrameLength, dsp::kFrameHop);
    channel_floor = metrics::estimate_noise_floor(channel_powers, capture.channel, cfg_, spectrum_);
    const metrics::NoiseFloor audio_floor =
        metrics::estimate_noise_floor(audio_powers, capture.audio, cfg_, spectrum_);
    const auto events = metrics::detect_events(channel_powers, channel_floor, cfg_);
    metrics::estimate_snr(capture, channel_floor, audio_floor, events, cfg_, commit.attempt,
                          &spectrum_);
  } else {
    channel_floor.status = metrics::FloorStatus::Unreliable;
    if (!capture.detail.empty()) commit.attempt.status_detail = capture.detail;
  }

  commit.attempt.status = map_status(capture, channel_floor, commit.attempt.valid_event_count);
  if (!capture.detail.empty() && !commit.attempt.status_detail.has_value()) {
    commit.attempt.status_detail = capture.detail;
  }

  // 5. The operator's choice, known BEFORE the commit, so the attempt is
  //    written once with its final disposition.
  if (commit.attempt.status == AttemptStatus::Ok) {
    std::ostringstream summary;
    summary << "Captured " << commit.attempt.valid_event_count << " valid events, "
            << commit.attempt.events_per_minute << " per minute";
    if (commit.attempt.capture_score_channel_db.has_value()) {
      summary << ", median channel SNR " << degrees(*commit.attempt.capture_score_channel_db)
              << " dB";
    }
    summary << ".";
    terminal_.info(summary.str());
    commit.disposition = Disposition::Accepted;
    commit.pending_retry.reset();
    return apply_commit(commit, store_.commit_visit(commit));
  }

  terminal_.warn("This capture ended as " + std::string(to_string(commit.attempt.status)) +
                 (commit.attempt.status_detail.has_value()
                      ? ": " + *commit.attempt.status_detail
                      : std::string{}));

  if (!terminal_.interactive()) {
    // There is nobody to ask. The attempt is accepted as it stands: it keeps
    // its non-ok status, so it does not rank, and the visit is complete. That
    // is the only choice of the four that neither discards a record nor
    // repeats a capture nobody asked for - and a run that stopped at the first
    // imperfect capture would leave every later angle unmeasured.
    terminal_.info(
        "No operator is attached, so this attempt is kept as it stands. It is recorded with "
        "its status and does not contribute to a ranking.");
    commit.disposition = Disposition::Accepted;
    commit.pending_retry.reset();
    return apply_commit(commit, store_.commit_visit(commit));
  }

  std::vector<ui::MenuItem> items;
  const bool retry = retry_offered();
  if (retry) {
    items.push_back({"Retry this angle", "capture again at this angle"});
  } else {
    terminal_.info(
        "The retry budget is exhausted: capacity for one attempt at each of the " +
        std::to_string(count_incomplete_visits(store_.record())) +
        " remaining visits is reserved, so this session can still reach its last visit.");
  }
  items.push_back({"Accept as-is", "keep this attempt; it will not contribute to a ranking"});
  items.push_back({"Skip this angle", "record no result for this visit"});
  items.push_back({"Quit and save", "stop here and write a partial report"});

  const int choice = terminal_.menu("What next?", items, 0);
  const int offset = retry ? 0 : 1;
  const int decision = choice < 0 ? 3 : choice + offset;

  switch (decision) {
    case 0:
      // The retry: this attempt is superseded and the intent that names its
      // replacement is recorded IN THE SAME COMMIT, so a crash between them is
      // impossible.
      commit.disposition = Disposition::Superseded;
      commit.pending_retry = PendingRetry{visit.visit_id, attempt + 1};
      return apply_commit(commit, store_.commit_visit(commit));
    case 1:
      commit.disposition = Disposition::Accepted;
      commit.pending_retry.reset();
      return apply_commit(commit, store_.commit_visit(commit));
    case 2:
      commit.disposition = Disposition::Abandoned;
      commit.pending_retry.reset();
      return apply_commit(commit, store_.commit_visit(commit));
    case 3:
    default: {
      commit.disposition = Disposition::Abandoned;
      commit.pending_retry.reset();
      const VisitStep step = apply_commit(commit, store_.commit_visit(commit));
      return step == VisitStep::Advance ? VisitStep::Quit : step;
    }
  }
}

ExperimentController::Result ExperimentController::run() {
  for (;;) {
    const auto remaining = remaining_visits(store_.record());
    if (remaining.empty()) return Result::Completed;

    const VisitKey& key = remaining.front();
    const Visit* visit = nullptr;
    for (const Visit& v : store_.record().plan.visits) {
      if (v.visit_id == key.visit_id) visit = &v;
    }
    if (visit == nullptr) {
      failure_detail_ = "the record names a remaining visit \"" + key.visit_id +
                        "\" that is not in the plan.";
      return Result::Failed;
    }

    std::ostringstream heading;
    heading << "Visit " << (store_.record().plan.visits.size() - remaining.size() + 1) << " of "
            << store_.record().plan.visits.size() << "  -  " << degrees(visit->planned_deg)
            << " deg, round " << visit->round << ", attempt " << key.next_attempt;
    terminal_.heading(heading.str());

    // The attempt is committed BEFORE the next visit begins.
    switch (run_visit(*visit, key.next_attempt)) {
      case VisitStep::Advance:
        continue;
      case VisitStep::Quit:
        return Result::QuitRequested;
      case VisitStep::Failed:
        return Result::Failed;
    }
  }
}

}  // namespace rtlangle
