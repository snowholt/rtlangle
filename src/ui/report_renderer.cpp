#include "ui/report_renderer.h"

#include "core/db.h"
#include "ui/safe_text.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace rtlangle::ui {
namespace {

std::string num(double v, int precision = 2) {
  if (!std::isfinite(v)) return "-";
  std::ostringstream os;
  os.precision(precision);
  os << std::fixed << v;
  return os.str();
}

// An absent measurement renders as a dash, never as a zero: an angle with no
// audio-eligible capture is absent from the audio ranking, and "0" would read
// as a measurement of zero decibels.
std::string opt(const std::optional<double>& v, int precision = 2) {
  return v.has_value() ? num(*v, precision) : "-";
}

std::string spread_cell(const DescriptiveSpread& s) {
  if (!s.valid) return "-";
  return num(s.low, 1) + " to " + num(s.high, 1) + (s.is_iqr ? " (iqr)" : " (min-max)");
}

std::string pad(const std::string& s, std::size_t width) {
  std::string out = s;
  while (out.size() < width) out.push_back(' ');
  return out;
}

void render_table(std::ostringstream& os, const std::string& title,
                  const std::vector<std::string>& headers,
                  const std::vector<std::vector<std::string>>& rows,
                  const std::vector<std::string>& notes) {
  os << '\n' << title << '\n';
  std::vector<std::size_t> widths(headers.size(), 0);
  for (std::size_t i = 0; i < headers.size(); ++i) widths[i] = headers[i].size();
  for (const auto& row : rows) {
    for (std::size_t i = 0; i < row.size() && i < widths.size(); ++i) {
      widths[i] = std::max(widths[i], row[i].size());
    }
  }
  for (std::size_t i = 0; i < headers.size(); ++i) {
    if (i > 0) os << "  ";
    os << pad(headers[i], widths[i]);
  }
  os << '\n';
  for (std::size_t i = 0; i < headers.size(); ++i) {
    if (i > 0) os << "  ";
    os << std::string(widths[i], '-');
  }
  os << '\n';
  if (rows.empty()) os << "(no angle produced a score for this metric)\n";
  for (const auto& row : rows) {
    for (std::size_t i = 0; i < row.size() && i < widths.size(); ++i) {
      if (i > 0) os << "  ";
      os << pad(row[i], widths[i]);
    }
    os << '\n';
  }
  for (const std::string& note : notes) os << "  " << note << '\n';
}

const AngleSummary* find(const SessionSummary& summary, double planned_deg) {
  for (const AngleSummary& a : summary.angles) {
    if (std::fabs(a.planned_deg - planned_deg) < 1e-9) return &a;
  }
  return nullptr;
}

std::string join(const std::vector<std::string>& parts, const char* separator) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) out += separator;
    out += parts[i];
  }
  return out.empty() ? "-" : out;
}

// One ranking table. Every row that shows a score also shows its yield: spec
// section 10.0 makes a score without a yield uninterpretable, so the two are
// never separated.
void render_ranking(std::ostringstream& os, const SessionSummary& summary, Metric metric) {
  const bool channel = metric == Metric::Channel;
  const std::vector<double>& ranking =
      channel ? summary.ranking_channel : summary.ranking_audio;

  std::vector<std::vector<std::string>> rows;
  bool any_pair_median = false;
  for (double angle : ranking) {
    const AngleSummary* a = find(summary, angle);
    if (a == nullptr) continue;
    const auto& score = channel ? a->score_channel_db : a->score_audio_db;
    const auto& spread = channel ? a->spread_channel : a->spread_audio;
    const int n = channel ? a->n_captures : a->n_captures_audio;
    if (n == 2) any_pair_median = true;
    rows.push_back({num(a->planned_deg, 1), num(a->actual_mean_deg, 1),
                    std::isnan(a->actual_spread_deg) ? "-" : num(a->actual_spread_deg, 1),
                    opt(score), spread_cell(spread), opt(a->yield_events_per_min),
                    opt(a->detected_fraction, 3), std::to_string(n),
                    std::to_string(a->n_excluded), std::to_string(a->n_valid_events),
                    opt(a->noise_floor_dbfs, 1), num(a->noise_floor_spread_db, 1),
                    join(a->warning_ids, " ")});
  }

  // Angles with no eligible capture appear with their status and are excluded
  // from the ordering.
  std::vector<std::string> unranked;
  for (const AngleSummary& a : summary.angles) {
    const auto& score = channel ? a.score_channel_db : a.score_audio_db;
    if (score.has_value()) continue;
    unranked.push_back(num(a.planned_deg, 1) + " deg (" + a.status + ")");
  }

  std::vector<std::string> notes;
  notes.push_back(
      "The spread column is the range the contributing captures covered: min-max below "
      "four captures, interquartile at four or more. It describes how much they varied. "
      "It is not an interval estimate and carries no claim about repeatability.");
  notes.push_back(
      "The yield column is how many transmissions per minute the angle heard. A score "
      "without it cannot be read: a higher score can mean the angle received better, or "
      "that it heard only the loud ones.");
  if (any_pair_median) {
    notes.push_back("Where n is 2, the median shown is the mean of the two capture scores.");
  }
  if (!unranked.empty()) {
    notes.push_back("Not ordered here, for want of an eligible capture: " +
                    join(unranked, ", ") + ".");
  }

  render_table(os,
               channel ? "Channel ranking (carrier-plus-sideband to noise, in the channel "
                         "bandwidth)"
                       : "Audio ranking (after demodulation, 300-3400 Hz)",
               {"planned", "actual", "spread", channel ? "chan dB" : "audio dB", "capture spread",
                "events/min", "detected", "n", "excl", "events", "floor dBFS", "floor span",
                "warnings"},
               rows, notes);
}

}  // namespace

std::string render_report(const SessionRecord& record, const SessionSummary& summary,
                          bool color) {
  // Colour is deliberately not applied to the report text: report.txt is a file
  // and a terminal rendering must be byte-identical to it, so the choice cannot
  // change what the operator reads.
  (void)color;

  std::ostringstream os;

  // The first line marks a report built from a subset.
  if (record.state == SessionState::Aborted) {
    os << "ABORTED - PARTIAL\n";
  } else if (summary.partial || record.state == SessionState::Paused) {
    os << "PARTIAL\n";
  }

  os << "rtlangle session " << safe_text(record.session_id) << '\n';
  os << "started " << safe_text(record.started_utc);
  if (record.finished_utc.has_value()) os << ", finished " << safe_text(*record.finished_utc);
  os << ", state " << to_string(record.state) << '\n';
  if (record.abort_reason.has_value()) {
    os << "aborted because: " << safe_text(*record.abort_reason) << '\n';
  }
  os << "angle reference: "
     << (record.config.angle_reference.empty() ? "(not recorded)"
                                               : safe_text(record.config.angle_reference))
     << '\n';
  if (!record.config.setup_note.empty()) {
    os << "setup note: " << safe_text(record.config.setup_note) << '\n';
  }
  os << record.plan.angles_deg.size() << " angles over " << record.plan.rounds
     << " round(s), " << num(record.config.duration_s, 0) << " s per capture, "
     << to_string(record.plan.order) << " order, seed " << record.plan.seed << '\n';
  os << "weakest SNR this squelch can report: "
     << num(minimum_detectable_snr_db(record.config.open_db)) << " dB at open_db "
     << num(record.config.open_db, 1) << '\n';

  // Both rankings, always, in the order the display choice selects.
  if (summary.report_metric == Metric::Channel) {
    render_ranking(os, summary, Metric::Channel);
    render_ranking(os, summary, Metric::Audio);
  } else {
    render_ranking(os, summary, Metric::Audio);
    render_ranking(os, summary, Metric::Channel);
  }

  os << "\nData-quality warnings\n";
  if (summary.warnings.empty()) {
    os << "None fired. That is not a clean bill of health: it means none of the eight "
          "named checks had anything to say about this session.\n";
  } else {
    for (const Warning& w : summary.warnings) os << safe_text(w.message) << '\n';
  }

  if (!summary.notes.empty()) {
    os << "\nComparability notes\n";
    for (const std::string& note : summary.notes) os << "- " << safe_text(note) << '\n';
  }

  os << '\n' << kResultHeadline << '\n';
  return os.str();
}

void choose_report_metric(SessionSummary& summary, ITerminalUi& terminal) {
  if (!terminal.interactive()) return;

  const std::array<MenuItem, 2> items = {
      MenuItem{"Channel ranking first", "carrier-plus-sideband to noise"},
      MenuItem{"Audio ranking first", "after demodulation"}};
  const int initial = summary.report_metric == Metric::Channel ? 0 : 1;
  const int choice = terminal.menu("Which ranking should be listed first?", items, initial);
  if (choice < 0) return;

  // Only these two fields change. Both rankings are already computed and both
  // are printed in full, so nothing downstream can consume the choice.
  summary.report_metric = choice == 0 ? Metric::Channel : Metric::Audio;
  summary.report_metric_source = "interactive";
}

}  // namespace rtlangle::ui
