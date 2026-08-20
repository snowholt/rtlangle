#pragma once

#include "core/config.h"
#include "core/statistics.h"
#include "core/version.h"

#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rtlangle {

// ---------------------------------------------------------------------------
// SourceInfo — what a source reports about the configuration it is actually
// running at. It lives in core rather than beside ISampleSource because it is
// persisted: spec section 11.2 stores it as a receiver segment's baseline, and
// spec section 10.4 warning W7 compares every attempt's applied settings
// against that baseline.
//
// requested_* versus applied_* exists because librtlsdr snaps sample rate,
// frequency, and gain to what the hardware supports. Snapping is legitimate;
// what must not happen is the applied values CHANGING part-way through a
// session, which is what W7 detects.
// ---------------------------------------------------------------------------
struct SourceInfo {
  std::string   driver;        // "librtlsdr 0.6.0-128-g240b" | "synthetic" | "iq-file"
  std::string   device_name;
  std::string   serial;
  std::uint32_t requested_sample_rate_hz = 0;
  std::uint32_t applied_sample_rate_hz = 0;
  std::uint32_t requested_center_hz = 0;   // fc + applied_offset_hz
  std::uint32_t applied_center_hz = 0;
  int           requested_gain_tenth_db = 0;
  int           applied_gain_tenth_db = 0;
  bool          agc_enabled = false;       // read-back evidence that AGC was off
  int           ppm = 0;
  std::int64_t  applied_offset_hz = 0;     // integer NCO offset actually used
};

// ---------------------------------------------------------------------------
// Attempts
// ---------------------------------------------------------------------------

enum class Disposition {
  Accepted,     // this attempt is the visit's final result; ranks only if status is Ok
  Superseded,   // a later attempt replaced it; retained for audit, never ranked
  Abandoned,    // the operator skipped or quit without a result for this visit
};
std::optional<Disposition> parse_disposition(std::string_view);
std::string_view to_string(Disposition);

// Spec section 11.3. Each value has exactly one meaning, and only Ok can rank
// in the channel ranking. Audio ranking eligibility is decided separately by
// spec section 9.4.1 and is not a function of status.
enum class AttemptStatus {
  Ok,
  InsufficientData,
  NoiseFloorUnreliable,
  NoiseFloorUnidentifiable,
  Clipped,
  Timeout,
  SourceError,
  InsufficientSamples,
  Skipped,
  Cancelled,
};
std::optional<AttemptStatus> parse_attempt_status(std::string_view);
std::string_view to_string(AttemptStatus);

// The per-capture event distribution, reported as a diagnostic beside the
// capture score (spec section 10.1).
struct Percentiles {
  double median = 0.0;
  double p10 = 0.0;
  double p25 = 0.0;
  double p75 = 0.0;
  double p90 = 0.0;
};

struct EventRecord {
  double      start_s = 0.0;
  double      duration_s = 0.0;
  std::string utc;
  // A comparability diagnostic only. It does NOT identify a transmitter or an
  // aircraft: several aircraft share a channel and offsets drift with
  // temperature and Doppler (spec section 9.2).
  double carrier_offset_hz = 0.0;
  double signal_power_dbfs = 0.0;
  std::optional<double> channel_snr_db;
  std::optional<double> audio_snr_db;
  bool valid = false;
  bool truncated = false;
  bool audio_snr_valid = false;
};

struct AttemptRecord {
  std::string   visit_id;      // "r1-i003": round, then the angle INDEX, 3 digits
  int           round = 0;
  std::size_t   angle_index = 0;
  double        planned_deg = 0.0;   // full precision, never re-derived from the id
  std::string   segment_id;
  int           attempt = 1;
  Disposition   disposition = Disposition::Accepted;
  AttemptStatus status = AttemptStatus::Ok;
  std::optional<std::string> status_detail;

  double      actual_deg = 0.0;
  double      angle_deviation_deg = 0.0;   // circular
  std::string started_utc;
  double      duration_s = 0.0;
  double      wall_duration_s = 0.0;

  // Applied receiver settings at this capture's start, copied from the capture
  // outcome. Warning W7 compares these against the segment baseline.
  std::uint32_t applied_sample_rate_hz = 0;
  std::uint32_t applied_center_hz = 0;
  int           applied_gain_tenth_db = 0;
  bool          agc_enabled = false;

  int    frames_total = 0;
  int    frames_active = 0;
  double active_probe_fraction = 0.0;
  double dynamic_range_db = 0.0;
  std::optional<double> noise_floor_dbfs;
  std::optional<double> audio_noise_floor_dbfs;
  std::uint64_t host_dropped_samples = 0;   // host-side, never a device overrun count
  double        clipped_fraction = 0.0;

  int  valid_event_count = 0;         // channel-eligible events    (spec 9.4.1)
  int  valid_audio_event_count = 0;   // of those, audio-eligible   (spec 9.4.1)
  int  truncated_event_count = 0;
  bool audio_insufficient = false;    // true when the audio score is absent

  int    events_total = 0;      // valid events found
  int    events_retained = 0;   // event objects kept below, capped by max_retained_events
  double events_per_minute = 0.0;   // spec section 9.5 yield
  double detected_fraction = 0.0;   // spec section 9.5

  // Both are optional because either may legitimately be absent while the other
  // is present. A sentinel of 0.0 here would silently become a measurement of
  // zero decibels.
  std::optional<double> capture_score_channel_db;
  std::optional<double> capture_score_audio_db;
  std::optional<Percentiles> channel_snr;
  std::optional<Percentiles> audio_snr;

  std::vector<EventRecord>   events;
  std::optional<std::string> note;
};

// ---------------------------------------------------------------------------
// Session structure
// ---------------------------------------------------------------------------

struct ReceiverSegment {
  std::string segment_id;
  std::string opened_utc;
  SourceInfo  baseline;
};

// One planned visit. The identifier is index-based rather than angle-formatted,
// so two angles that de-duplication keeps apart cannot share one id.
struct Visit {
  std::string visit_id;
  int         round = 0;
  std::size_t angle_index = 0;
  double      planned_deg = 0.0;
};

struct SessionPlan {
  std::vector<double> angles_deg;
  int                 rounds = 0;
  VisitOrder          order = VisitOrder::Alternating;
  std::uint64_t       seed = 0;
  std::vector<Visit>  visits;
};

struct AngleProviderInfo {
  std::string name = "manual";
  bool        automated = false;
};

enum class SessionState { Running, Paused, Completed, Aborted };
std::optional<SessionState> parse_session_state(std::string_view);
std::string_view to_string(SessionState);

struct PendingRetry {
  std::string visit_id;
  int         next_attempt = 2;
};

// Which visit the controller should enter next, and at which attempt number.
struct VisitKey {
  std::string visit_id;
  int         next_attempt = 1;
};

// ---------------------------------------------------------------------------
// The summary. Spec decision Q1: Phase 1 takes no decision, so there is no
// member whose name contains `resolved`, `resolution`, or `winner`, and no
// flag selecting between two headlines - there is only one headline.
// ---------------------------------------------------------------------------

struct AngleSummary {
  double planned_deg = 0.0;
  double actual_mean_deg = 0.0;     // circular, over 360 degrees
  double actual_spread_deg = 0.0;   // circular standard deviation; NaN when undefined
  int    n_captures = 0;            // captures with a CHANNEL score
  int    n_captures_audio = 0;      // captures with an AUDIO score; never assumed equal
  int    n_excluded = 0;
  int    n_valid_events = 0;
  std::optional<double> score_channel_db;
  std::optional<double> score_audio_db;
  DescriptiveSpread spread_channel;   // NOT a confidence interval
  DescriptiveSpread spread_audio;
  std::optional<double> yield_events_per_min;   // spec section 9.5
  std::optional<double> detected_fraction;
  std::optional<double> noise_floor_dbfs;
  double noise_floor_spread_db = 0.0;
  std::vector<std::string> flags;
  std::vector<std::string> warning_ids;   // which of W1..W8 name this angle
  std::string status;
};

// One entry per warning that FIRED. Warnings that did not fire are absent:
// there is no pass/fail pair, because nothing passes (spec section 10.4).
struct Warning {
  std::string id;        // "W1".."W8"
  std::string message;   // already formatted, with the numbers in it
  std::vector<double> angles_deg;
};

struct SessionSummary {
  std::vector<AngleSummary> angles;
  std::vector<double> ranking_channel;   // planned angles, highest score first
  std::vector<double> ranking_audio;
  std::vector<double> ranking_yield;     // spec section 9.5; feeds warning W6
  Metric      report_metric = Metric::Channel;   // which table is listed first
  std::string report_metric_source = "default";  // default | flag | interactive
  std::vector<Warning>     warnings;             // W1..W8, all that fired
  std::vector<std::string> notes;                // spec section 10.5 comparability notes
  bool partial = false;   // rendered from pause() or abort()
};

struct SessionRecord {
  int         schema_version = kSchemaVersion;
  std::string tool_version = kToolVersion;
  std::string session_id;
  SessionState state = SessionState::Running;
  std::optional<std::string> abort_reason;
  std::vector<std::string>   durability_warnings;   // one per CommittedNotDurable commit
  std::string started_utc;
  std::optional<std::string> finished_utc;
  Config      config;
  AngleProviderInfo angle_provider;
  std::vector<ReceiverSegment> receiver_segments;
  SessionPlan plan;
  std::optional<PendingRetry>  pending_retry;
  std::vector<AttemptRecord>   attempts;
  std::optional<SessionSummary> summary;
  bool summary_partial = false;
};

// ---------------------------------------------------------------------------
// The store's commit contract. Spec sections 6.6 and 11.1.1.
// ---------------------------------------------------------------------------

// Everything one visit outcome changes, committed atomically or not at all.
struct VisitCommit {
  AttemptRecord               attempt;       // the attempt just produced
  Disposition                 disposition = Disposition::Accepted;
  std::optional<PendingRetry> pending_retry; // set on retry; nullopt clears any existing
};

enum class CommitOutcome {
  // rename() succeeded and the directory fsync succeeded. The record on disk
  // contains the commit and will survive power loss.
  Committed,
  // rename() succeeded; the directory fsync did not. The commit IS in the
  // current filesystem state and every later read sees it, but it may not
  // survive power loss. The caller warns, records it, and continues. It is NOT
  // a failed commit, and retrying here would duplicate the attempt.
  CommittedNotDurable,
  // Failed at or before rename(). The canonical record is byte-for-byte what it
  // was. The caller may retry the commit or abort.
  NotCommitted,
  // rename() returned an error the platform does not let us classify. The
  // caller MUST run the reconciliation of spec section 11.1.2 before anything
  // else.
  Indeterminate,
};
std::string_view to_string(CommitOutcome);

struct CommitResult {
  CommitOutcome outcome = CommitOutcome::Committed;
  std::string   detail;
};

// ---------------------------------------------------------------------------
// Invariants that hold after every possible crash point (spec section 11.4).
// The loader asserts both on open.
// ---------------------------------------------------------------------------

// I1: no visit has more than one attempt whose disposition is accepted or
//     abandoned.
bool invariant_i1(const SessionRecord&);
// I2: pending_retry is non-null if and only if the named visit's latest attempt
//     has disposition superseded.
bool invariant_i2(const SessionRecord&);

// A visit is complete when it has exactly one attempt whose disposition is
// accepted or abandoned.
bool visit_complete(const SessionRecord&, std::string_view visit_id);

// The plan minus the complete visits, in the stored order, honouring
// pending_retry.
std::vector<VisitKey> remaining_visits(const SessionRecord&);

// The visit identifier format: r<round>-i<angle index padded to three digits>.
std::string make_visit_id(int round, std::size_t angle_index);

// ---------------------------------------------------------------------------
// Serialisation
// ---------------------------------------------------------------------------
void to_json(nlohmann::json&, const SourceInfo&);
void from_json(const nlohmann::json&, SourceInfo&);
void to_json(nlohmann::json&, const EventRecord&);
void from_json(const nlohmann::json&, EventRecord&);
void to_json(nlohmann::json&, const AttemptRecord&);
void from_json(const nlohmann::json&, AttemptRecord&);
void to_json(nlohmann::json&, const ReceiverSegment&);
void from_json(const nlohmann::json&, ReceiverSegment&);
void to_json(nlohmann::json&, const Visit&);
void from_json(const nlohmann::json&, Visit&);
void to_json(nlohmann::json&, const SessionPlan&);
void from_json(const nlohmann::json&, SessionPlan&);
void to_json(nlohmann::json&, const PendingRetry&);
void from_json(const nlohmann::json&, PendingRetry&);
void to_json(nlohmann::json&, const AngleSummary&);
void from_json(const nlohmann::json&, AngleSummary&);
void to_json(nlohmann::json&, const Warning&);
void from_json(const nlohmann::json&, Warning&);
void to_json(nlohmann::json&, const SessionSummary&);
void from_json(const nlohmann::json&, SessionSummary&);
void to_json(nlohmann::json&, const SessionRecord&);
void from_json(const nlohmann::json&, SessionRecord&);

}  // namespace rtlangle
