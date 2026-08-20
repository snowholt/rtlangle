#include "core/records.h"

#include "core/version.h"

#include <algorithm>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <unordered_map>

namespace rtlangle {
namespace {

using J = nlohmann::json;

// An absent optional serialises as JSON null, never as a zero. A missing
// measurement must never be readable as a measurement of zero.
template <typename T>
void put_opt(J& j, const char* key, const std::optional<T>& v) {
  if (v.has_value()) {
    j[key] = *v;
  } else {
    j[key] = nullptr;
  }
}

template <typename T>
void get_opt(const J& j, const char* key, std::optional<T>& v) {
  v.reset();
  if (!j.contains(key)) return;
  const J& node = j.at(key);
  if (node.is_null()) return;
  v = node.get<T>();
}

template <typename T>
void get_to(const J& j, const char* key, T& dest) {
  if (j.contains(key) && !j.at(key).is_null()) j.at(key).get_to(dest);
}

J percentiles_to_json(const Percentiles& p) {
  return J{{"median", p.median}, {"p10", p.p10}, {"p25", p.p25},
           {"p75", p.p75},       {"p90", p.p90}};
}

Percentiles percentiles_from_json(const J& j) {
  Percentiles p;
  get_to(j, "median", p.median);
  get_to(j, "p10", p.p10);
  get_to(j, "p25", p.p25);
  get_to(j, "p75", p.p75);
  get_to(j, "p90", p.p90);
  return p;
}

template <typename E, typename F>
E parse_or_throw(const J& j, const char* key, F parser, const char* what, E fallback) {
  if (!j.contains(key) || j.at(key).is_null()) return fallback;
  const std::string s = j.at(key).get<std::string>();
  const auto parsed = parser(s);
  if (!parsed.has_value()) {
    throw J::other_error::create(501, std::string("unknown ") + what + " \"" + s + "\"",
                                 nullptr);
  }
  return *parsed;
}

}  // namespace

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------

std::optional<Disposition> parse_disposition(std::string_view s) {
  if (s == "accepted") return Disposition::Accepted;
  if (s == "superseded") return Disposition::Superseded;
  if (s == "abandoned") return Disposition::Abandoned;
  return std::nullopt;
}

std::string_view to_string(Disposition d) {
  switch (d) {
    case Disposition::Accepted: return "accepted";
    case Disposition::Superseded: return "superseded";
    case Disposition::Abandoned: return "abandoned";
  }
  return "accepted";
}

std::optional<AttemptStatus> parse_attempt_status(std::string_view s) {
  if (s == "ok") return AttemptStatus::Ok;
  if (s == "insufficient_data") return AttemptStatus::InsufficientData;
  if (s == "noise_floor_unreliable") return AttemptStatus::NoiseFloorUnreliable;
  if (s == "noise_floor_unidentifiable") return AttemptStatus::NoiseFloorUnidentifiable;
  if (s == "clipped") return AttemptStatus::Clipped;
  if (s == "timeout") return AttemptStatus::Timeout;
  if (s == "source_error") return AttemptStatus::SourceError;
  if (s == "insufficient_samples") return AttemptStatus::InsufficientSamples;
  if (s == "skipped") return AttemptStatus::Skipped;
  if (s == "cancelled") return AttemptStatus::Cancelled;
  return std::nullopt;
}

std::string_view to_string(AttemptStatus s) {
  switch (s) {
    case AttemptStatus::Ok: return "ok";
    case AttemptStatus::InsufficientData: return "insufficient_data";
    case AttemptStatus::NoiseFloorUnreliable: return "noise_floor_unreliable";
    case AttemptStatus::NoiseFloorUnidentifiable: return "noise_floor_unidentifiable";
    case AttemptStatus::Clipped: return "clipped";
    case AttemptStatus::Timeout: return "timeout";
    case AttemptStatus::SourceError: return "source_error";
    case AttemptStatus::InsufficientSamples: return "insufficient_samples";
    case AttemptStatus::Skipped: return "skipped";
    case AttemptStatus::Cancelled: return "cancelled";
  }
  return "ok";
}

std::optional<SessionState> parse_session_state(std::string_view s) {
  if (s == "running") return SessionState::Running;
  if (s == "paused") return SessionState::Paused;
  if (s == "completed") return SessionState::Completed;
  if (s == "aborted") return SessionState::Aborted;
  return std::nullopt;
}

std::string_view to_string(SessionState s) {
  switch (s) {
    case SessionState::Running: return "running";
    case SessionState::Paused: return "paused";
    case SessionState::Completed: return "completed";
    case SessionState::Aborted: return "aborted";
  }
  return "running";
}

std::string_view to_string(CommitOutcome o) {
  switch (o) {
    case CommitOutcome::Committed: return "committed";
    case CommitOutcome::CommittedNotDurable: return "committed_not_durable";
    case CommitOutcome::NotCommitted: return "not_committed";
    case CommitOutcome::Indeterminate: return "indeterminate";
  }
  return "committed";
}

// ---------------------------------------------------------------------------
// Visit identity and completion
// ---------------------------------------------------------------------------

std::string make_visit_id(int round, std::size_t angle_index) {
  std::ostringstream os;
  os << 'r' << round << "-i" << std::setw(3) << std::setfill('0') << angle_index;
  return os.str();
}

namespace {

// The attempts belonging to one visit, in commit order.
std::vector<const AttemptRecord*> attempts_for(const SessionRecord& rec,
                                               std::string_view visit_id) {
  std::vector<const AttemptRecord*> v;
  for (const AttemptRecord& a : rec.attempts) {
    if (a.visit_id == visit_id) v.push_back(&a);
  }
  return v;
}

}  // namespace

bool invariant_i1(const SessionRecord& rec) {
  std::unordered_map<std::string, int> terminal;
  for (const AttemptRecord& a : rec.attempts) {
    if (a.disposition == Disposition::Accepted || a.disposition == Disposition::Abandoned) {
      if (++terminal[a.visit_id] > 1) return false;
    }
  }
  return true;
}

bool invariant_i2(const SessionRecord& rec) {
  if (!rec.pending_retry.has_value()) {
    // No pending retry: no visit may be left with a superseded attempt as its
    // latest, because that visit would otherwise never be re-entered.
    std::unordered_map<std::string, Disposition> latest;
    for (const AttemptRecord& a : rec.attempts) latest[a.visit_id] = a.disposition;
    for (const auto& [visit_id, d] : latest) {
      (void)visit_id;
      if (d == Disposition::Superseded) return false;
    }
    return true;
  }
  const auto attempts = attempts_for(rec, rec.pending_retry->visit_id);
  if (attempts.empty()) return false;
  return attempts.back()->disposition == Disposition::Superseded;
}

bool visit_complete(const SessionRecord& rec, std::string_view visit_id) {
  int terminal = 0;
  for (const AttemptRecord& a : rec.attempts) {
    if (a.visit_id != visit_id) continue;
    if (a.disposition == Disposition::Accepted || a.disposition == Disposition::Abandoned) {
      ++terminal;
    }
  }
  return terminal == 1;
}

std::vector<VisitKey> remaining_visits(const SessionRecord& rec) {
  std::vector<VisitKey> out;
  for (const Visit& v : rec.plan.visits) {
    if (visit_complete(rec, v.visit_id)) continue;
    VisitKey key;
    key.visit_id = v.visit_id;
    key.next_attempt = 1;
    if (rec.pending_retry.has_value() && rec.pending_retry->visit_id == v.visit_id) {
      key.next_attempt = rec.pending_retry->next_attempt;
    } else {
      // A crash before the attempt committed leaves nothing written, so the
      // visit is re-entered at the same attempt number it was at.
      const auto attempts = attempts_for(rec, v.visit_id);
      key.next_attempt = static_cast<int>(attempts.size()) + 1;
    }
    out.push_back(std::move(key));
  }
  return out;
}

// ---------------------------------------------------------------------------
// Serialisation
// ---------------------------------------------------------------------------

void to_json(J& j, const SourceInfo& s) {
  j = J{{"driver", s.driver},
        {"device_name", s.device_name},
        {"serial", s.serial},
        {"requested_sample_rate_hz", s.requested_sample_rate_hz},
        {"applied_sample_rate_hz", s.applied_sample_rate_hz},
        {"requested_center_hz", s.requested_center_hz},
        {"applied_center_hz", s.applied_center_hz},
        {"requested_gain_tenth_db", s.requested_gain_tenth_db},
        {"applied_gain_tenth_db", s.applied_gain_tenth_db},
        {"agc_enabled", s.agc_enabled},
        {"ppm", s.ppm},
        {"applied_offset_hz", s.applied_offset_hz}};
}

void from_json(const J& j, SourceInfo& s) {
  get_to(j, "driver", s.driver);
  get_to(j, "device_name", s.device_name);
  get_to(j, "serial", s.serial);
  get_to(j, "requested_sample_rate_hz", s.requested_sample_rate_hz);
  get_to(j, "applied_sample_rate_hz", s.applied_sample_rate_hz);
  get_to(j, "requested_center_hz", s.requested_center_hz);
  get_to(j, "applied_center_hz", s.applied_center_hz);
  get_to(j, "requested_gain_tenth_db", s.requested_gain_tenth_db);
  get_to(j, "applied_gain_tenth_db", s.applied_gain_tenth_db);
  get_to(j, "agc_enabled", s.agc_enabled);
  get_to(j, "ppm", s.ppm);
  get_to(j, "applied_offset_hz", s.applied_offset_hz);
}

void to_json(J& j, const EventRecord& e) {
  j = J::object();
  j["start_s"] = e.start_s;
  j["duration_s"] = e.duration_s;
  j["utc"] = e.utc;
  j["carrier_offset_hz"] = e.carrier_offset_hz;
  j["signal_power_dbfs"] = e.signal_power_dbfs;
  put_opt(j, "channel_snr_db", e.channel_snr_db);
  put_opt(j, "audio_snr_db", e.audio_snr_db);
  j["valid"] = e.valid;
  j["truncated"] = e.truncated;
  j["audio_snr_valid"] = e.audio_snr_valid;
}

void from_json(const J& j, EventRecord& e) {
  get_to(j, "start_s", e.start_s);
  get_to(j, "duration_s", e.duration_s);
  get_to(j, "utc", e.utc);
  get_to(j, "carrier_offset_hz", e.carrier_offset_hz);
  get_to(j, "signal_power_dbfs", e.signal_power_dbfs);
  get_opt(j, "channel_snr_db", e.channel_snr_db);
  get_opt(j, "audio_snr_db", e.audio_snr_db);
  get_to(j, "valid", e.valid);
  get_to(j, "truncated", e.truncated);
  get_to(j, "audio_snr_valid", e.audio_snr_valid);
}

void to_json(J& j, const AttemptRecord& a) {
  j = J::object();
  j["visit_id"] = a.visit_id;
  j["round"] = a.round;
  j["angle_index"] = a.angle_index;
  j["planned_deg"] = a.planned_deg;
  j["segment_id"] = a.segment_id;
  j["attempt"] = a.attempt;
  j["disposition"] = to_string(a.disposition);
  j["status"] = to_string(a.status);
  put_opt(j, "status_detail", a.status_detail);

  j["actual_deg"] = a.actual_deg;
  j["angle_deviation_deg"] = a.angle_deviation_deg;
  j["started_utc"] = a.started_utc;
  j["duration_s"] = a.duration_s;
  j["wall_duration_s"] = a.wall_duration_s;

  j["applied_sample_rate_hz"] = a.applied_sample_rate_hz;
  j["applied_center_hz"] = a.applied_center_hz;
  j["applied_gain_tenth_db"] = a.applied_gain_tenth_db;
  j["agc_enabled"] = a.agc_enabled;

  j["frames_total"] = a.frames_total;
  j["frames_active"] = a.frames_active;
  j["active_probe_fraction"] = a.active_probe_fraction;
  j["dynamic_range_db"] = a.dynamic_range_db;
  put_opt(j, "noise_floor_dbfs", a.noise_floor_dbfs);
  put_opt(j, "audio_noise_floor_dbfs", a.audio_noise_floor_dbfs);
  j["host_dropped_samples"] = a.host_dropped_samples;
  j["clipped_fraction"] = a.clipped_fraction;

  j["valid_event_count"] = a.valid_event_count;
  j["valid_audio_event_count"] = a.valid_audio_event_count;
  j["truncated_event_count"] = a.truncated_event_count;
  j["audio_insufficient"] = a.audio_insufficient;
  j["events_total"] = a.events_total;
  j["events_retained"] = a.events_retained;
  j["events_per_minute"] = a.events_per_minute;
  j["detected_fraction"] = a.detected_fraction;

  put_opt(j, "capture_score_channel_db", a.capture_score_channel_db);
  put_opt(j, "capture_score_audio_db", a.capture_score_audio_db);
  j["channel_snr_db"] = a.channel_snr.has_value() ? percentiles_to_json(*a.channel_snr) : J(nullptr);
  j["audio_snr_db"] = a.audio_snr.has_value() ? percentiles_to_json(*a.audio_snr) : J(nullptr);

  j["events"] = J::array();
  for (const EventRecord& e : a.events) {
    J ej;
    to_json(ej, e);
    j["events"].push_back(std::move(ej));
  }
  put_opt(j, "note", a.note);
}

void from_json(const J& j, AttemptRecord& a) {
  get_to(j, "visit_id", a.visit_id);
  get_to(j, "round", a.round);
  get_to(j, "angle_index", a.angle_index);
  get_to(j, "planned_deg", a.planned_deg);
  get_to(j, "segment_id", a.segment_id);
  get_to(j, "attempt", a.attempt);
  a.disposition = parse_or_throw<Disposition>(j, "disposition", parse_disposition,
                                              "disposition", Disposition::Accepted);
  a.status = parse_or_throw<AttemptStatus>(j, "status", parse_attempt_status,
                                           "attempt status", AttemptStatus::Ok);
  get_opt(j, "status_detail", a.status_detail);

  get_to(j, "actual_deg", a.actual_deg);
  get_to(j, "angle_deviation_deg", a.angle_deviation_deg);
  get_to(j, "started_utc", a.started_utc);
  get_to(j, "duration_s", a.duration_s);
  get_to(j, "wall_duration_s", a.wall_duration_s);

  get_to(j, "applied_sample_rate_hz", a.applied_sample_rate_hz);
  get_to(j, "applied_center_hz", a.applied_center_hz);
  get_to(j, "applied_gain_tenth_db", a.applied_gain_tenth_db);
  get_to(j, "agc_enabled", a.agc_enabled);

  get_to(j, "frames_total", a.frames_total);
  get_to(j, "frames_active", a.frames_active);
  get_to(j, "active_probe_fraction", a.active_probe_fraction);
  get_to(j, "dynamic_range_db", a.dynamic_range_db);
  get_opt(j, "noise_floor_dbfs", a.noise_floor_dbfs);
  get_opt(j, "audio_noise_floor_dbfs", a.audio_noise_floor_dbfs);
  get_to(j, "host_dropped_samples", a.host_dropped_samples);
  get_to(j, "clipped_fraction", a.clipped_fraction);

  get_to(j, "valid_event_count", a.valid_event_count);
  get_to(j, "valid_audio_event_count", a.valid_audio_event_count);
  get_to(j, "truncated_event_count", a.truncated_event_count);
  get_to(j, "audio_insufficient", a.audio_insufficient);
  get_to(j, "events_total", a.events_total);
  get_to(j, "events_retained", a.events_retained);
  get_to(j, "events_per_minute", a.events_per_minute);
  get_to(j, "detected_fraction", a.detected_fraction);

  get_opt(j, "capture_score_channel_db", a.capture_score_channel_db);
  get_opt(j, "capture_score_audio_db", a.capture_score_audio_db);
  a.channel_snr.reset();
  if (j.contains("channel_snr_db") && !j.at("channel_snr_db").is_null()) {
    a.channel_snr = percentiles_from_json(j.at("channel_snr_db"));
  }
  a.audio_snr.reset();
  if (j.contains("audio_snr_db") && !j.at("audio_snr_db").is_null()) {
    a.audio_snr = percentiles_from_json(j.at("audio_snr_db"));
  }

  a.events.clear();
  if (j.contains("events") && j.at("events").is_array()) {
    for (const J& ej : j.at("events")) {
      EventRecord e;
      from_json(ej, e);
      a.events.push_back(std::move(e));
    }
  }
  get_opt(j, "note", a.note);
}

void to_json(J& j, const ReceiverSegment& s) {
  J baseline;
  to_json(baseline, s.baseline);
  j = J{{"segment_id", s.segment_id}, {"opened_utc", s.opened_utc}, {"baseline", baseline}};
}

void from_json(const J& j, ReceiverSegment& s) {
  get_to(j, "segment_id", s.segment_id);
  get_to(j, "opened_utc", s.opened_utc);
  if (j.contains("baseline")) from_json(j.at("baseline"), s.baseline);
}

void to_json(J& j, const Visit& v) {
  j = J{{"visit_id", v.visit_id},
        {"round", v.round},
        {"angle_index", v.angle_index},
        {"planned_deg", v.planned_deg}};
}

void from_json(const J& j, Visit& v) {
  get_to(j, "visit_id", v.visit_id);
  get_to(j, "round", v.round);
  get_to(j, "angle_index", v.angle_index);
  get_to(j, "planned_deg", v.planned_deg);
}

void to_json(J& j, const SessionPlan& p) {
  j = J::object();
  j["angles_deg"] = p.angles_deg;
  j["rounds"] = p.rounds;
  j["order"] = to_string(p.order);
  j["seed"] = p.seed;
  j["visits"] = J::array();
  for (const Visit& v : p.visits) {
    J vj;
    to_json(vj, v);
    j["visits"].push_back(std::move(vj));
  }
}

void from_json(const J& j, SessionPlan& p) {
  get_to(j, "angles_deg", p.angles_deg);
  get_to(j, "rounds", p.rounds);
  p.order = parse_or_throw<VisitOrder>(j, "order", parse_visit_order, "visit order",
                                       VisitOrder::Alternating);
  get_to(j, "seed", p.seed);
  p.visits.clear();
  if (j.contains("visits") && j.at("visits").is_array()) {
    for (const J& vj : j.at("visits")) {
      Visit v;
      from_json(vj, v);
      p.visits.push_back(std::move(v));
    }
  }
}

void to_json(J& j, const PendingRetry& r) {
  j = J{{"visit_id", r.visit_id}, {"next_attempt", r.next_attempt}};
}

void from_json(const J& j, PendingRetry& r) {
  get_to(j, "visit_id", r.visit_id);
  get_to(j, "next_attempt", r.next_attempt);
}

namespace {

J spread_to_json(const DescriptiveSpread& s) {
  return J{{"low", s.low}, {"high", s.high}, {"n", s.n},
           {"is_iqr", s.is_iqr}, {"valid", s.valid}};
}

DescriptiveSpread spread_from_json(const J& j) {
  DescriptiveSpread s;
  get_to(j, "low", s.low);
  get_to(j, "high", s.high);
  get_to(j, "n", s.n);
  get_to(j, "is_iqr", s.is_iqr);
  get_to(j, "valid", s.valid);
  return s;
}

}  // namespace

void to_json(J& j, const AngleSummary& a) {
  j = J::object();
  j["planned_deg"] = a.planned_deg;
  j["actual_mean_deg"] = a.actual_mean_deg;
  j["actual_spread_deg"] = a.actual_spread_deg;
  j["n_captures"] = a.n_captures;
  j["n_captures_audio"] = a.n_captures_audio;
  j["n_excluded"] = a.n_excluded;
  j["n_valid_events"] = a.n_valid_events;
  put_opt(j, "score_channel_db", a.score_channel_db);
  put_opt(j, "score_audio_db", a.score_audio_db);
  j["spread_channel"] = spread_to_json(a.spread_channel);
  j["spread_audio"] = spread_to_json(a.spread_audio);
  put_opt(j, "yield_events_per_min", a.yield_events_per_min);
  put_opt(j, "detected_fraction", a.detected_fraction);
  put_opt(j, "noise_floor_dbfs", a.noise_floor_dbfs);
  j["noise_floor_spread_db"] = a.noise_floor_spread_db;
  j["flags"] = a.flags;
  j["warning_ids"] = a.warning_ids;
  j["status"] = a.status;
}

void from_json(const J& j, AngleSummary& a) {
  get_to(j, "planned_deg", a.planned_deg);
  get_to(j, "actual_mean_deg", a.actual_mean_deg);
  get_to(j, "actual_spread_deg", a.actual_spread_deg);
  get_to(j, "n_captures", a.n_captures);
  get_to(j, "n_captures_audio", a.n_captures_audio);
  get_to(j, "n_excluded", a.n_excluded);
  get_to(j, "n_valid_events", a.n_valid_events);
  get_opt(j, "score_channel_db", a.score_channel_db);
  get_opt(j, "score_audio_db", a.score_audio_db);
  if (j.contains("spread_channel")) a.spread_channel = spread_from_json(j.at("spread_channel"));
  if (j.contains("spread_audio")) a.spread_audio = spread_from_json(j.at("spread_audio"));
  get_opt(j, "yield_events_per_min", a.yield_events_per_min);
  get_opt(j, "detected_fraction", a.detected_fraction);
  get_opt(j, "noise_floor_dbfs", a.noise_floor_dbfs);
  get_to(j, "noise_floor_spread_db", a.noise_floor_spread_db);
  get_to(j, "flags", a.flags);
  get_to(j, "warning_ids", a.warning_ids);
  get_to(j, "status", a.status);
}

void to_json(J& j, const Warning& w) {
  j = J{{"id", w.id}, {"message", w.message}, {"angles_deg", w.angles_deg}};
}

void from_json(const J& j, Warning& w) {
  get_to(j, "id", w.id);
  get_to(j, "message", w.message);
  get_to(j, "angles_deg", w.angles_deg);
}

void to_json(J& j, const SessionSummary& s) {
  j = J::object();
  j["angles"] = J::array();
  for (const AngleSummary& a : s.angles) {
    J aj;
    to_json(aj, a);
    j["angles"].push_back(std::move(aj));
  }
  j["ranking_channel"] = s.ranking_channel;
  j["ranking_audio"] = s.ranking_audio;
  j["ranking_yield"] = s.ranking_yield;
  j["report_metric"] = to_string(s.report_metric);
  j["report_metric_source"] = s.report_metric_source;
  j["warnings"] = J::array();
  for (const Warning& w : s.warnings) {
    J wj;
    to_json(wj, w);
    j["warnings"].push_back(std::move(wj));
  }
  j["notes"] = s.notes;
  j["partial"] = s.partial;
}

void from_json(const J& j, SessionSummary& s) {
  s.angles.clear();
  if (j.contains("angles") && j.at("angles").is_array()) {
    for (const J& aj : j.at("angles")) {
      AngleSummary a;
      from_json(aj, a);
      s.angles.push_back(std::move(a));
    }
  }
  get_to(j, "ranking_channel", s.ranking_channel);
  get_to(j, "ranking_audio", s.ranking_audio);
  get_to(j, "ranking_yield", s.ranking_yield);
  s.report_metric = parse_or_throw<Metric>(j, "report_metric", parse_metric,
                                           "report metric", Metric::Channel);
  get_to(j, "report_metric_source", s.report_metric_source);
  s.warnings.clear();
  if (j.contains("warnings") && j.at("warnings").is_array()) {
    for (const J& wj : j.at("warnings")) {
      Warning w;
      from_json(wj, w);
      s.warnings.push_back(std::move(w));
    }
  }
  get_to(j, "notes", s.notes);
  get_to(j, "partial", s.partial);
}

void to_json(J& j, const SessionRecord& r) {
  j = J::object();
  j["schema_version"] = r.schema_version;
  j["tool_version"] = r.tool_version;
  j["session_id"] = r.session_id;
  j["state"] = to_string(r.state);
  put_opt(j, "abort_reason", r.abort_reason);
  j["durability_warnings"] = r.durability_warnings;
  j["started_utc"] = r.started_utc;
  put_opt(j, "finished_utc", r.finished_utc);

  J cfg;
  to_json(cfg, r.config);
  j["config"] = std::move(cfg);

  j["angle_provider"] = J{{"name", r.angle_provider.name},
                          {"automated", r.angle_provider.automated}};

  j["receiver_segments"] = J::array();
  for (const ReceiverSegment& s : r.receiver_segments) {
    J sj;
    to_json(sj, s);
    j["receiver_segments"].push_back(std::move(sj));
  }

  J plan;
  to_json(plan, r.plan);
  j["plan"] = std::move(plan);

  if (r.pending_retry.has_value()) {
    J pr;
    to_json(pr, *r.pending_retry);
    j["pending_retry"] = std::move(pr);
  } else {
    j["pending_retry"] = nullptr;
  }

  j["attempts"] = J::array();
  for (const AttemptRecord& a : r.attempts) {
    J aj;
    to_json(aj, a);
    j["attempts"].push_back(std::move(aj));
  }

  if (r.summary.has_value()) {
    J sj;
    to_json(sj, *r.summary);
    j["summary"] = std::move(sj);
  } else {
    j["summary"] = nullptr;
  }
  j["summary_partial"] = r.summary_partial;
}

void from_json(const J& j, SessionRecord& r) {
  get_to(j, "schema_version", r.schema_version);
  get_to(j, "tool_version", r.tool_version);
  get_to(j, "session_id", r.session_id);
  r.state = parse_or_throw<SessionState>(j, "state", parse_session_state, "session state",
                                         SessionState::Running);
  get_opt(j, "abort_reason", r.abort_reason);
  get_to(j, "durability_warnings", r.durability_warnings);
  get_to(j, "started_utc", r.started_utc);
  get_opt(j, "finished_utc", r.finished_utc);

  if (j.contains("config")) from_json(j.at("config"), r.config);

  if (j.contains("angle_provider") && j.at("angle_provider").is_object()) {
    get_to(j.at("angle_provider"), "name", r.angle_provider.name);
    get_to(j.at("angle_provider"), "automated", r.angle_provider.automated);
  }

  r.receiver_segments.clear();
  if (j.contains("receiver_segments") && j.at("receiver_segments").is_array()) {
    for (const J& sj : j.at("receiver_segments")) {
      ReceiverSegment s;
      from_json(sj, s);
      r.receiver_segments.push_back(std::move(s));
    }
  }

  if (j.contains("plan")) from_json(j.at("plan"), r.plan);

  r.pending_retry.reset();
  if (j.contains("pending_retry") && !j.at("pending_retry").is_null()) {
    PendingRetry pr;
    from_json(j.at("pending_retry"), pr);
    r.pending_retry = pr;
  }

  r.attempts.clear();
  if (j.contains("attempts") && j.at("attempts").is_array()) {
    for (const J& aj : j.at("attempts")) {
      AttemptRecord a;
      from_json(aj, a);
      r.attempts.push_back(std::move(a));
    }
  }

  r.summary.reset();
  if (j.contains("summary") && !j.at("summary").is_null()) {
    SessionSummary s;
    from_json(j.at("summary"), s);
    r.summary = std::move(s);
  }
  get_to(j, "summary_partial", r.summary_partial);
}

}  // namespace rtlangle
