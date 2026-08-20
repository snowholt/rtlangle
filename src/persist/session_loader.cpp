#include "persist/session_loader.h"

#include "core/utc.h"
#include "persist/csv.h"
#include "persist/json_session_store.h"
#include "persist/path_safety.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstring>

namespace rtlangle {
namespace {

using J = nlohmann::json;

std::string field_value(const J& config, const std::string& key) {
  if (!config.contains(key)) return "(absent)";
  const J& node = config.at(key);
  if (node.is_string()) return node.get<std::string>();
  return node.dump();
}

}  // namespace

bool migrate_record(J& document, std::string& error) {
  error.clear();
  int version = 0;
  if (document.contains("schema_version") && document.at("schema_version").is_number_integer()) {
    version = document.at("schema_version").get<int>();
  }
  if (version > kSchemaVersion) {
    error = "this session was written with schema_version " + std::to_string(version) +
            ", which is newer than the version this build supports (" +
            std::to_string(kSchemaVersion) + "). Use a newer rtlangle to read it.";
    return false;
  }
  if (version == kSchemaVersion) return true;

  // Version 0 -> 1. Version 0 predates the durability_warnings list, the
  // receiver segments, and the abort state, so the migration supplies the
  // fields their absence would otherwise leave undefined.
  if (version == 0) {
    document["schema_version"] = 1;
    if (!document.contains("durability_warnings")) document["durability_warnings"] = J::array();
    if (!document.contains("abort_reason")) document["abort_reason"] = nullptr;
    if (!document.contains("summary_partial")) document["summary_partial"] = false;
    if (!document.contains("pending_retry")) document["pending_retry"] = nullptr;

    // Version 0 predates receiver segments, so a migrated record has attempts
    // that name a segment nothing describes. A migration must produce a record
    // satisfying the CURRENT invariants, so one segment is synthesised to carry
    // them. Its baseline is empty and honestly so: version 0 never recorded the
    // applied settings warning W7 would compare against, and inventing values
    // would be worse than an empty baseline the report can see is empty.
    const bool no_segments = !document.contains("receiver_segments") ||
                             !document.at("receiver_segments").is_array() ||
                             document.at("receiver_segments").empty();
    if (no_segments) {
      J segment = J::object();
      segment["segment_id"] = "seg1";
      segment["opened_utc"] = document.value("started_utc", std::string{});
      segment["baseline"] = J::object();
      document["receiver_segments"] = J::array({segment});
    }
    const std::string first_segment =
        document.at("receiver_segments")[0].value("segment_id", std::string("seg1"));
    if (document.contains("attempts") && document.at("attempts").is_array()) {
      for (J& attempt : document.at("attempts")) {
        if (!attempt.contains("segment_id") || attempt.at("segment_id").is_null() ||
            attempt.at("segment_id").get<std::string>().empty()) {
          attempt["segment_id"] = first_segment;
        }
      }
    }
    return true;
  }

  error = "no migration exists from schema_version " + std::to_string(version) + " to " +
          std::to_string(kSchemaVersion) + ".";
  return false;
}

namespace {

// Everything both entry points do: open the directory, read and migrate the
// document, and check the invariants that hold after every possible crash
// point. A violation is an error rather than a repair, because the record
// cannot be continued - or honestly re-rendered - once it holds a state the
// completion rule cannot represent.
LoadResult read_and_check(const std::filesystem::path& path) {
  LoadResult result;

  std::string error;
  auto dir = SessionDir::open_existing(path, error);
  if (!dir.has_value()) {
    result.error = error;
    return result;
  }
  result.dir = std::move(*dir);

  const auto text =
      read_at_limited(result.dir.fd(), kSessionFileName, kMaxSessionJsonBytes, error);
  if (!text.has_value()) {
    result.error = error;
    return result;
  }

  J document;
  try {
    document = J::parse(*text);
  } catch (const J::exception& e) {
    result.error = std::string("cannot parse ") + kSessionFileName + ": " + e.what();
    return result;
  }

  std::string migration_error;
  if (!migrate_record(document, migration_error)) {
    result.error = migration_error;
    return result;
  }

  try {
    from_json(document, result.record);
  } catch (const J::exception& e) {
    result.error = std::string("cannot read ") + kSessionFileName + ": " + e.what();
    return result;
  }

  // The invariants that hold after every possible crash point.
  if (!invariant_i1(result.record)) {
    result.error =
        "this session violates invariant I1: a visit has more than one attempt whose "
        "disposition is accepted or abandoned, which the completion rule cannot represent.";
    return result;
  }
  if (!invariant_i2(result.record)) {
    result.error =
        "this session violates invariant I2: pending_retry does not match the disposition of "
        "the named visit's latest attempt.";
    return result;
  }

  // Every attempt names the receiver segment it was taken in, and warning W7
  // compares its applied settings against that segment's baseline. An attempt
  // naming a segment that does not exist would leave W7 with nothing to
  // examine, so it is a load error rather than something to work around.
  for (const AttemptRecord& a : result.record.attempts) {
    bool found = false;
    for (const ReceiverSegment& s : result.record.receiver_segments) {
      if (s.segment_id == a.segment_id) found = true;
    }
    if (!found) {
      result.error = "attempt " + std::to_string(a.attempt) + " of visit " + a.visit_id +
                     " names receiver segment \"" + a.segment_id +
                     "\", which this record does not contain. Warning W7 compares an "
                     "attempt's applied settings against its segment's baseline, and there "
                     "is no baseline to compare against.";
      return result;
    }
  }

  return result;
}

}  // namespace

LoadResult load_session_read_only(const std::filesystem::path& path) {
  return read_and_check(path);
}

LoadResult load_session(const std::filesystem::path& path) {
  LoadResult result = read_and_check(path);
  if (result.error.has_value()) return result;

  // Step 1: terminal states are refused. Both are statements about the session,
  // and reopening either would invalidate that statement silently.
  if (result.record.state == SessionState::Completed) {
    result.error = "this session was completed at " +
                   result.record.finished_utc.value_or("an unrecorded time") +
                   ". Use `rtlangle report " + path.string() +
                   "` to re-render it, or start a new session to take more data.";
    return result;
  }
  if (result.record.state == SessionState::Aborted) {
    result.error = "this session was aborted at " +
                   result.record.finished_utc.value_or("an unrecorded time") + ": " +
                   result.record.abort_reason.value_or("no reason was recorded") +
                   ". Use `rtlangle report " + path.string() +
                   "` to re-render what it holds, or start a new session.";
    return result;
  }

  if (result.record.state == SessionState::Running) {
    result.repairs.push_back(
        "this session was not shut down cleanly; every measurement committed before the "
        "interruption is present.");
  } else if (result.record.state == SessionState::Paused) {
    // The partial report described a subset that is about to change.
    result.record.state = SessionState::Running;
    result.record.summary.reset();
    result.record.summary_partial = false;
    result.record.finished_utc.reset();
  }

  // Step 3: stray temporaries from an interrupted write.
  const int removed = remove_stale_temporaries(result.dir.fd(), kSessionFileName);
  if (removed > 0) {
    result.repairs.push_back("removed " + std::to_string(removed) +
                             " stale session.json temporary file(s) left by an interrupted "
                             "write.");
  }

  // Step 4: the derived CSV, regenerated whenever it disagrees with the JSON or
  // is missing, unreadable, or malformed. Corrupting it cannot lose data.
  std::string csv_error;
  const auto existing =
      read_at_limited(result.dir.fd(), kMeasurementsFileName, kMaxSessionJsonBytes, csv_error);
  const int expected = static_cast<int>(result.record.attempts.size());
  const int found = existing.has_value() ? csv_row_count(*existing) : -1;
  if (found != expected) {
    const CommitResult r = write_atomic_at(result.dir.fd(), kMeasurementsFileName,
                                           render_csv(result.record));
    if (r.outcome == CommitOutcome::Committed ||
        r.outcome == CommitOutcome::CommittedNotDurable) {
      result.repairs.push_back("regenerated measurements.csv from session.json.");
    } else {
      result.repairs.push_back("measurements.csv could not be regenerated: " + r.detail);
    }
  }

  // report.txt is a pure render and is never read back by anything, so a
  // missing or truncated one is noted and re-rendered at the next write.
  std::string report_error;
  const auto report =
      read_at_limited(result.dir.fd(), kReportFileName, kMaxSessionJsonBytes, report_error);
  if (!report.has_value() || report->empty()) {
    result.repairs.push_back(
        "report.txt is missing or empty; it is a pure render of session.json and is "
        "reproduced by `rtlangle report`.");
  }

  return result;
}

std::optional<SessionDir> create_session_dir(const std::filesystem::path& session_root,
                                             std::string_view label, std::string& session_id,
                                             std::string& error) {
  error.clear();
  session_id.clear();

  std::error_code ec;
  std::filesystem::create_directories(session_root, ec);
  if (ec && !std::filesystem::is_directory(session_root)) {
    error = "cannot create the session root \"" + session_root.string() + "\": " + ec.message();
    return std::nullopt;
  }

  const std::string stamp = session_stamp(now());
  const std::string slug = slugify_label(label);

  for (int suffix = 1; suffix <= 64; ++suffix) {
    std::string name = stamp;
    if (!slug.empty()) name += "-" + slug;
    if (suffix > 1) name += "-" + std::to_string(suffix);

    // The invariant sanitisation exists to satisfy, checked before mkdir.
    if (!valid_directory_component(name)) {
      error = "the assembled session directory name \"" + name + "\" is not a valid single "
              "path component.";
      return std::nullopt;
    }

    std::string create_error;
    auto dir = SessionDir::create(session_root / name, create_error);
    if (dir.has_value()) {
      session_id = name;
      return dir;
    }
    if (create_error.find("already exists") == std::string::npos) {
      error = create_error;
      return std::nullopt;
    }
  }
  error = "cannot find an unused session directory name under \"" + session_root.string() +
          "\" after 64 attempts.";
  return std::nullopt;
}

std::vector<ValidationError> resume_conflicts(const Config& stored, const Config& cli,
                                              const std::set<std::string>& explicitly_set) {
  std::vector<ValidationError> out;

  J stored_json;
  to_json(stored_json, stored);
  J cli_json;
  to_json(cli_json, cli);

  for (const FieldSpec& f : field_registry()) {
    const std::string key(f.key);
    switch (resume_class(key)) {
      case FieldClass::ExperimentDefining: {
        if (explicitly_set.count(key) == 0) continue;
        if (stored_json.contains(key) && cli_json.contains(key) &&
            stored_json.at(key) == cli_json.at(key)) {
          continue;
        }
        out.push_back(
            {key, "cannot change " + std::string(f.flag) + " on a resumed session: the stored "
                      "value is " + field_value(stored_json, key) + " and " +
                      std::string(f.flag) + " was given as " + field_value(cli_json, key) +
                      ". A resumed session whose experiment parameters differed would "
                      "silently mix two experiments."});
        break;
      }
      case FieldClass::ReportOrOperational:
        // Accepted: these are applied when the report is rendered or govern how
        // the run is operated, never what is measured.
        break;
      case FieldClass::PathDetermined:
        // Ignored: the resume path decides.
        break;
    }
  }
  return out;
}

}  // namespace rtlangle
