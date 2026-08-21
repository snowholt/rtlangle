#pragma once

#include "core/records.h"

#include <string>

namespace rtlangle {

// Storage is an interface so that persistence faults can be injected without a
// filesystem, and so the controller depends on a contract rather than on JSON.
//
// The mutating surface is deliberately ONE call. Spec section 11.4 requires a
// retry to supersede the attempt and record the retry intent in one commit, and
// three separate calls cannot satisfy a one-commit requirement.
//
// Who calls what, so the seam has one owner:
//
//   begin_receiver_segment      the APP layer, after the source is opened and
//                               the record exists, before the controller is
//                               constructed
//   commit_visit, reload_from_disk
//                               the CONTROLLER; commit_visit exactly once per
//                               visit outcome, reload_from_disk only in
//                               response to Indeterminate
//   pause / finalize / abort    the APP layer, after the controller returns and
//                               after aggregation has produced the summary
//
// The controller cannot call pause, finalize, or abort, because all three take
// a SessionSummary and the controller is forbidden to compute one. THE
// CONTROLLER ALWAYS LEAVES THE SESSION `running`; the app layer decides which
// terminal or paused state to write.
class ISessionStore {
 public:
  virtual ~ISessionStore() = default;

  virtual const SessionRecord& record() const = 0;

  // Opens a receiver segment: a span of attempts taken through one continuous
  // device configuration. Called once at session start and once per resume. The
  // returned id is stamped on every attempt taken in the segment.
  virtual std::string begin_receiver_segment(const SourceInfo& applied) = 0;

  // The single mutator. Disposition, retry intent, and the attempt itself land
  // in the same rename() or none of them do.
  //
  // It RETURNS rather than throws: a failed commit is an expected operational
  // condition with four distinct meanings, and an exception can express only
  // one of them.
  [[nodiscard]] virtual CommitResult commit_visit(const VisitCommit&) = 0;

  // Re-reads session.json from disk and replaces the in-memory record with it.
  // The recovery step for CommitOutcome::Indeterminate.
  [[nodiscard]] virtual CommitResult reload_from_disk() = 0;

  // Quit: writes the summary and sets state to Paused. finished_utc stays null.
  [[nodiscard]] virtual CommitResult pause(const SessionSummary& partial) = 0;
  // Normal end: writes the summary, sets Completed, stamps finished_utc.
  [[nodiscard]] virtual CommitResult finalize(const SessionSummary& summary) = 0;
  // Unrecoverable end: writes whatever summary can be built, sets Aborted,
  // stamps finished_utc, and records the reason. Terminal, like Completed.
  [[nodiscard]] virtual CommitResult abort(const SessionSummary& partial,
                                           std::string_view reason) = 0;

  // Regenerate measurements.csv from the canonical JSON.
  virtual void flush_exports() = 0;
};

}  // namespace rtlangle
