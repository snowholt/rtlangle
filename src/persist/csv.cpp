#include "persist/csv.h"

#include <cmath>
#include <sstream>

namespace rtlangle {
namespace {

std::string number(double v) {
  if (!std::isfinite(v)) return {};
  std::ostringstream os;
  os.precision(6);
  os << std::fixed << v;
  return os.str();
}

// An absent measurement is an EMPTY field, never a zero: a zero here would be
// read as a measurement of zero decibels.
std::string opt_number(const std::optional<double>& v) {
  if (!v.has_value()) return {};
  return number(*v);
}

std::string percentile_field(const std::optional<Percentiles>& p, double Percentiles::*member) {
  if (!p.has_value()) return {};
  return number(p.value().*member);
}

}  // namespace

std::string csv_escape(std::string_view field) {
  const bool needs_quotes = field.find_first_of(",\"\r\n") != std::string_view::npos;
  if (!needs_quotes) return std::string(field);
  std::string out;
  out.reserve(field.size() + 2);
  out.push_back('"');
  for (char c : field) {
    if (c == '"') out.push_back('"');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

std::string csv_header() {
  return "visit_id,round,angle_index,planned_deg,actual_deg,attempt,segment_id,disposition,"
         "status,started_utc,duration_s,valid_events,valid_audio_events,audio_insufficient,"
         "events_total,events_per_minute,"
         "detected_fraction,capture_score_channel_db,capture_score_audio_db,"
         "channel_snr_p25_db,channel_snr_median_db,channel_snr_p75_db,"
         "audio_snr_median_db,noise_floor_dbfs,active_probe_fraction,dynamic_range_db,"
         "applied_gain_tenth_db,applied_sample_rate_hz,applied_center_hz,"
         "host_dropped_samples,clipped_fraction,note\n";
}

std::string csv_row(const AttemptRecord& a) {
  std::ostringstream os;
  os << csv_escape(a.visit_id) << ',' << a.round << ',' << a.angle_index << ','
     << number(a.planned_deg) << ',' << number(a.actual_deg) << ',' << a.attempt << ','
     << csv_escape(a.segment_id) << ',' << to_string(a.disposition) << ','
     << to_string(a.status) << ',' << csv_escape(a.started_utc) << ','
     << number(a.duration_s) << ',' << a.valid_event_count << ',' << a.valid_audio_event_count
     << ',' << (a.audio_insufficient ? "true" : "false") << ',' << a.events_total << ','
     << number(a.events_per_minute) << ',' << number(a.detected_fraction) << ','
     << opt_number(a.capture_score_channel_db) << ','
     << opt_number(a.capture_score_audio_db) << ','
     << percentile_field(a.channel_snr, &Percentiles::p25) << ','
     << percentile_field(a.channel_snr, &Percentiles::median) << ','
     << percentile_field(a.channel_snr, &Percentiles::p75) << ','
     << percentile_field(a.audio_snr, &Percentiles::median) << ','
     << opt_number(a.noise_floor_dbfs) << ',' << number(a.active_probe_fraction) << ','
     << number(a.dynamic_range_db) << ',' << a.applied_gain_tenth_db << ','
     << a.applied_sample_rate_hz << ',' << a.applied_center_hz << ','
     << a.host_dropped_samples << ',' << number(a.clipped_fraction) << ','
     << csv_escape(a.note.value_or(std::string{})) << '\n';
  return os.str();
}

std::string render_csv(const SessionRecord& rec) {
  std::string out = csv_header();
  for (const AttemptRecord& a : rec.attempts) out += csv_row(a);
  return out;
}

int csv_row_count(std::string_view text) {
  if (text.empty()) return -1;
  const std::string header = csv_header();
  if (text.size() < header.size() || text.substr(0, header.size()) != header) return -1;

  // Rows may contain quoted newlines, so counting line feeds is not enough:
  // the parser tracks whether it is inside a quoted field.
  int rows = 0;
  bool in_quotes = false;
  bool row_started = false;
  for (std::size_t i = header.size(); i < text.size(); ++i) {
    const char c = text[i];
    if (in_quotes) {
      if (c == '"') {
        if (i + 1 < text.size() && text[i + 1] == '"') {
          ++i;
        } else {
          in_quotes = false;
        }
      }
      continue;
    }
    if (c == '"') {
      in_quotes = true;
      row_started = true;
      continue;
    }
    if (c == '\n') {
      ++rows;
      row_started = false;
      continue;
    }
    row_started = true;
  }
  if (in_quotes) return -1;      // truncated inside a quoted field
  if (row_started) return -1;    // a final row with no terminator: truncated
  return rows;
}

}  // namespace rtlangle
