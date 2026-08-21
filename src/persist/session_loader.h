#pragma once

#include "core/config.h"
#include "core/records.h"
#include "persist/atomic_write.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace rtlangle {

struct LoadResult {
  SessionRecord            record;
  SessionDir               dir;
  std::vector<std::string> repairs;   // "removed stale tmp", "regenerated csv", ...
  std::optional<std::string> error;   // set when the session must not be reopened
};

// Loads a session directory for resumption (spec section 11.5).
//
// It refuses a `completed` or an `aborted` session, naming finished_utc and any
// abort_reason: both are terminal, because a completed summary is a statement
// about a finished experiment and an aborted one is a statement that the record
// could not be trusted to continue. Reopening either would invalidate that
// statement silently.
//
// A `paused` session reopens: state returns to running, the summary is cleared,
// and summary_partial returns to false, because the partial report described a
// subset that is about to change. A `running` session - the record a crash
// leaves - also reopens, and the loader reports that the session was not shut
// down cleanly.
//
// It opens no device and creates no receiver segment: that is the app layer's
// job on both the new-session and the resume path.
LoadResult load_session(const std::filesystem::path& dir);

// The read-only entry point, for `rtlangle report <dir>`.
//
// It parses and migrates exactly as load_session does and applies the same
// invariant checks, but it does NOT refuse a completed or an aborted session,
// does not turn a paused one back into a running one, and regenerates nothing:
// re-rendering a finished session's report is legitimate and must not change
// anything the session recorded. load_session refuses terminal states precisely
// so that this is the only way to reach them.
LoadResult load_session_read_only(const std::filesystem::path& dir);

// Creates a new session directory under `session_root`, named
// YYYYMMDD-HHMMSS[-label] with a numeric suffix on collision. Because the
// directory is created with mkdir, a collision is detected by EEXIST rather
// than by a check that could race.
std::optional<SessionDir> create_session_dir(const std::filesystem::path& session_root,
                                             std::string_view label, std::string& session_id,
                                             std::string& error);

// Spec section 11.5 step 6. An ExperimentDefining field that the resumed
// invocation would change is a conflict naming the field and both values; a
// ReportOrOperational one is accepted; a PathDetermined one is taken from the
// resume path and the CLI value ignored.
std::vector<ValidationError> resume_conflicts(const Config& stored, const Config& cli,
                                              const std::set<std::string>& explicitly_set);

// The migration mechanism of spec section 11.5 step 2. Version 1 is the first,
// so the table is currently a single step from the synthetic version-0 shape;
// the mechanism exists and is exercised rather than being asserted to work.
bool migrate_record(nlohmann::json& document, std::string& error);

}  // namespace rtlangle
