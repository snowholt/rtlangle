#include "persist/json_session_store.h"

#include "core/utc.h"
#include "persist/csv.h"
#include "persist/path_safety.h"

#include <nlohmann/json.hpp>

namespace rtlangle {
namespace {

using J = nlohmann::json;

std::string serialise(const SessionRecord& rec) {
  J j;
  to_json(j, rec);
  return j.dump(2) + "\n";
}

}  // namespace

JsonSessionStore::JsonSessionStore(SessionDir dir, SessionRecord record, FileOps ops)
    : dir_(std::move(dir)), record_(std::move(record)), ops_(std::move(ops)) {
  segment_counter_ = static_cast<int>(record_.receiver_segments.size());
}

void JsonSessionStore::set_report_renderer(
    std::function<std::string(const SessionRecord&, const SessionSummary&)> renderer) {
  renderer_ = std::move(renderer);
}

std::string JsonSessionStore::begin_receiver_segment(const SourceInfo& applied) {
  ReceiverSegment segment;
  segment.segment_id = "seg" + std::to_string(++segment_counter_);
  segment.opened_utc = utc_now();
  segment.baseline = applied;
  record_.receiver_segments.push_back(segment);
  // The segment is durable at the next commit_visit; a crash before then leaves
  // a record with one fewer segment, and the resume path opens a new one anyway.
  return segment.segment_id;
}

CommitResult JsonSessionStore::write_record(SessionRecord candidate) {
  const std::string bytes = serialise(candidate);
  CommitResult result = write_atomic_at(dir_.fd(), kSessionFileName, bytes, ops_);

  if (result.outcome == CommitOutcome::NotCommitted ||
      result.outcome == CommitOutcome::Indeterminate) {
    // The live record is untouched: nothing observable changed, and for
    // Indeterminate the caller must reconcile before doing anything else.
    return result;
  }

  if (result.outcome == CommitOutcome::CommittedNotDurable) {
    // NOT a failed commit. The commit is in the record and every later read
    // sees it; it may not survive a power cut. Retrying here would duplicate
    // the attempt, so the event is recorded and the caller continues.
    candidate.durability_warnings.push_back(result.detail);
  }

  record_ = std::move(candidate);

  // Step 5 of the write protocol. A failure here leaves a commit that is both
  // visible and durable, so the outcome stays Committed and the CSV is rebuilt
  // on the next commit or on reopen.
  const std::string csv = render_csv(record_);
  const CommitResult csv_result = write_atomic_at(dir_.fd(), kMeasurementsFileName, csv, ops_);
  if (csv_result.outcome != CommitOutcome::Committed &&
      csv_result.outcome != CommitOutcome::CommittedNotDurable) {
    const std::string note =
        "measurements.csv could not be regenerated and will be rebuilt from session.json: " +
        csv_result.detail;
    repairs_.push_back(note);
    if (result.detail.empty()) result.detail = note;
  }
  return result;
}

CommitResult JsonSessionStore::commit_visit(const VisitCommit& commit) {
  // The whole VisitCommit is applied to a COPY. The attempt is written once,
  // with its FINAL disposition: a retry commits the just-finished attempt as
  // superseded together with the pending_retry that names its replacement, so
  // there is no already-committed record to go back and change.
  SessionRecord candidate = record_;

  if (static_cast<int>(candidate.attempts.size()) >= kAttemptCeiling) {
    CommitResult refused;
    refused.outcome = CommitOutcome::NotCommitted;
    refused.detail = "the attempt ceiling of " + std::to_string(kAttemptCeiling) +
                     " has been reached; no further attempt can be committed. Every attempt "
                     "already committed remains in the record.";
    return refused;
  }

  AttemptRecord attempt = commit.attempt;
  attempt.disposition = commit.disposition;
  candidate.attempts.push_back(std::move(attempt));
  candidate.pending_retry = commit.pending_retry;

  return write_record(std::move(candidate));
}

CommitResult JsonSessionStore::reload_from_disk() {
  std::string error;
  const auto text = read_at_limited(dir_.fd(), kSessionFileName, kMaxSessionJsonBytes, error);
  CommitResult result;
  if (!text.has_value()) {
    result.outcome = CommitOutcome::NotCommitted;
    result.detail = "cannot re-read " + std::string(kSessionFileName) + ": " + error;
    return result;
  }
  try {
    const J j = J::parse(*text);
    SessionRecord reloaded;
    from_json(j, reloaded);
    record_ = std::move(reloaded);
  } catch (const J::exception& e) {
    result.outcome = CommitOutcome::NotCommitted;
    result.detail = "cannot parse " + std::string(kSessionFileName) + ": " + e.what();
    return result;
  }
  result.outcome = CommitOutcome::Committed;
  return result;
}

CommitResult JsonSessionStore::pause(const SessionSummary& partial) {
  SessionRecord candidate = record_;
  candidate.state = SessionState::Paused;
  candidate.summary = partial;
  candidate.summary.value().partial = true;
  candidate.summary_partial = true;
  candidate.finished_utc.reset();   // a paused session is not finished

  CommitResult result = write_record(std::move(candidate));
  if (result.outcome == CommitOutcome::Committed ||
      result.outcome == CommitOutcome::CommittedNotDurable) {
    if (renderer_ && record_.summary.has_value()) {
      const std::string text = renderer_(record_, *record_.summary);
      (void)write_atomic_at(dir_.fd(), kReportFileName, text, ops_);
    }
  }
  return result;
}

CommitResult JsonSessionStore::finalize(const SessionSummary& summary) {
  SessionRecord candidate = record_;
  candidate.state = SessionState::Completed;
  candidate.summary = summary;
  candidate.summary.value().partial = false;
  candidate.summary_partial = false;
  candidate.finished_utc = utc_now();

  CommitResult result = write_record(std::move(candidate));
  if (result.outcome == CommitOutcome::Committed ||
      result.outcome == CommitOutcome::CommittedNotDurable) {
    if (renderer_ && record_.summary.has_value()) {
      const std::string text = renderer_(record_, *record_.summary);
      (void)write_atomic_at(dir_.fd(), kReportFileName, text, ops_);
    }
  }
  return result;
}

CommitResult JsonSessionStore::abort(const SessionSummary& partial, std::string_view reason) {
  SessionRecord candidate = record_;
  candidate.state = SessionState::Aborted;
  candidate.summary = partial;
  candidate.summary.value().partial = true;
  candidate.summary_partial = true;
  candidate.abort_reason = std::string(reason);
  candidate.finished_utc = utc_now();

  CommitResult result = write_record(std::move(candidate));
  if (result.outcome == CommitOutcome::Committed ||
      result.outcome == CommitOutcome::CommittedNotDurable) {
    if (renderer_ && record_.summary.has_value()) {
      const std::string text = renderer_(record_, *record_.summary);
      (void)write_atomic_at(dir_.fd(), kReportFileName, text, ops_);
    }
  }
  return result;
}

void JsonSessionStore::flush_exports() {
  const std::string csv = render_csv(record_);
  const CommitResult r = write_atomic_at(dir_.fd(), kMeasurementsFileName, csv, ops_);
  if (r.outcome != CommitOutcome::Committed && r.outcome != CommitOutcome::CommittedNotDurable) {
    repairs_.push_back("measurements.csv could not be regenerated: " + r.detail);
  }
}

}  // namespace rtlangle
