#pragma once

#include "persist/atomic_write.h"
#include "persist/session_store.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rtlangle {

inline constexpr const char* kSessionFileName = "session.json";
inline constexpr const char* kMeasurementsFileName = "measurements.csv";
inline constexpr const char* kReportFileName = "report.txt";

// The production store. session.json is the single source of truth;
// measurements.csv and report.txt are DERIVED artifacts that can be regenerated
// from it at any time, and neither is ever read back as an authority.
class JsonSessionStore final : public ISessionStore {
 public:
  // Takes ownership of an already-open session directory descriptor. `record`
  // is the starting state, which for a new session is a fresh record and for a
  // resume is what the loader returned.
  JsonSessionStore(SessionDir dir, SessionRecord record, FileOps ops = default_file_ops());

  // report.txt is a pure render of (record, summary). Rendering lives above
  // persistence, so the app layer injects it. With no renderer set the store
  // writes no report rather than writing a placeholder one.
  void set_report_renderer(
      std::function<std::string(const SessionRecord&, const SessionSummary&)> renderer);

  const SessionRecord& record() const override { return record_; }
  const SessionDir&    dir() const { return dir_; }

  std::string begin_receiver_segment(const SourceInfo& applied) override;

  [[nodiscard]] CommitResult commit_visit(const VisitCommit&) override;
  [[nodiscard]] CommitResult reload_from_disk() override;
  [[nodiscard]] CommitResult pause(const SessionSummary& partial) override;
  [[nodiscard]] CommitResult finalize(const SessionSummary& summary) override;
  [[nodiscard]] CommitResult abort(const SessionSummary& partial,
                                   std::string_view reason) override;
  void flush_exports() override;

  // Repair notes accumulated since construction, such as a CSV regeneration
  // that failed and will be retried on the next commit.
  const std::vector<std::string>& repairs() const { return repairs_; }

 private:
  // Serialises `candidate`, writes it atomically, and swaps it into place only
  // once the commit has happened. A failure anywhere in the write therefore
  // leaves the live record untouched, which is what makes the transaction
  // all-or-nothing.
  CommitResult write_record(SessionRecord candidate);

  SessionDir    dir_;
  SessionRecord record_;
  FileOps       ops_;
  int           segment_counter_ = 0;
  std::vector<std::string> repairs_;
  std::function<std::string(const SessionRecord&, const SessionSummary&)> renderer_;
};

}  // namespace rtlangle
