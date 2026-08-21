// WP10 Test K — no decision exists. Structural and greppable rather than
// argued. Spec sections 10.2, 10.3, 10.6, and 15.2.

#include <doctest/doctest.h>

#include "core/utc.h"
#include "experiment/aggregator.h"
#include "experiment/quality_checks.h"
#include "experiment/visit_plan.h"
#include "ui/report_renderer.h"
#include "ui/scripted_terminal_ui.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <string>

using namespace rtlangle;
using rtlangle::ui::kResultHeadline;
using rtlangle::ui::render_report;
using rtlangle::ui::ScriptedTerminalUi;

namespace {

Config report_config() {
  Config c;
  c.center_hz = 118350000;
  c.angles_deg = {0.0, 45.0, 90.0};
  c.rounds = 4;
  c.angle_reference = "marked arm along the balcony rail";
  return c;
}

SessionRecord record_with(const Config& cfg, int rounds_of_data) {
  SessionRecord r;
  r.session_id = "20260819-143000-airband";
  r.config = cfg;
  r.started_utc = "2026-08-19T14:30:00Z";
  r.plan.angles_deg = resolved_angles(cfg);
  r.plan.rounds = cfg.rounds;
  r.plan.visits = build_visit_plan(cfg);
  ReceiverSegment seg;
  seg.segment_id = "seg1";
  seg.baseline.applied_gain_tenth_db = 496;
  seg.baseline.applied_sample_rate_hz = 1024000;
  seg.baseline.applied_center_hz = 118600000;
  r.receiver_segments.push_back(seg);

  const double scores[] = {22.0, 16.0, 12.0};
  const double audio[] = {18.0, 12.0, 9.0};
  for (int round = 1; round <= rounds_of_data; ++round) {
    for (std::size_t index = 0; index < 3; ++index) {
      AttemptRecord a;
      a.visit_id = make_visit_id(round, index);
      a.round = round;
      a.angle_index = index;
      a.planned_deg = r.plan.angles_deg[index];
      a.actual_deg = a.planned_deg;
      a.segment_id = "seg1";
      a.attempt = 1;
      a.disposition = Disposition::Accepted;
      a.status = AttemptStatus::Ok;
      a.started_utc = "2026-08-19T14:36:07Z";
      a.applied_gain_tenth_db = 496;
      a.applied_sample_rate_hz = 1024000;
      a.applied_center_hz = 118600000;
      a.valid_event_count = 6;
      a.valid_audio_event_count = 6;
      a.events_total = 6;
      a.events_retained = 6;
      a.events_per_minute = 6.0;
      a.detected_fraction = 0.1;
      a.capture_score_channel_db = scores[index];
      a.capture_score_audio_db = audio[index];
      a.noise_floor_dbfs = -62.0;
      r.attempts.push_back(std::move(a));
    }
  }
  return r;
}

SessionSummary summarise(const SessionRecord& rec) {
  SessionSummary s = aggregate(rec);
  evaluate_warnings(s, rec, rec.config);
  return s;
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

}  // namespace

TEST_SUITE("report") {

TEST_CASE("Test K: the summary serialisation carries no decision field") {
  const Config cfg = report_config();
  const SessionRecord rec = record_with(cfg, 4);
  const SessionSummary s = summarise(rec);

  nlohmann::json j;
  to_json(j, s);
  const std::string dumped = j.dump();
  for (const char* forbidden : {"resolved", "resolution", "winner"}) {
    CHECK_MESSAGE(dumped.find(forbidden) == std::string::npos, forbidden);
  }
}

TEST_CASE("Test K: no forbidden claim is reachable from render_report") {
  const Config cfg = report_config();

  // Rich, sparse, partial, and aborted alike.
  std::vector<SessionRecord> records;
  records.push_back(record_with(cfg, 4));
  records.push_back(record_with(cfg, 1));
  {
    SessionRecord paused = record_with(cfg, 2);
    paused.state = SessionState::Paused;
    records.push_back(paused);
  }
  {
    SessionRecord aborted = record_with(cfg, 2);
    aborted.state = SessionState::Aborted;
    aborted.abort_reason = "the record could not be trusted to continue";
    aborted.finished_utc = "2026-08-19T15:10:00Z";
    records.push_back(aborted);
  }
  {
    SessionRecord empty = record_with(cfg, 0);
    records.push_back(empty);
  }

  for (const SessionRecord& rec : records) {
    SessionSummary s = summarise(rec);
    s.partial = rec.state != SessionState::Completed;
    for (bool color : {false, true}) {
      const std::string text = lower(render_report(rec, s, color));
      for (const char* forbidden :
           {"resolved", "statistically", "significant", "highest measured", "best angle",
            "optimal", "confidence interval", "p-value", "winner"}) {
        CHECK_MESSAGE(text.find(forbidden) == std::string::npos, forbidden);
      }
    }
  }
}

TEST_CASE("Test K: the one headline appears in every rendered report") {
  const Config cfg = report_config();
  for (int rounds : {0, 1, 2, 4}) {
    const SessionRecord rec = record_with(cfg, rounds);
    const SessionSummary s = summarise(rec);
    const std::string text = render_report(rec, s, false);
    const std::size_t first = text.find(kResultHeadline);
    CHECK_MESSAGE(first != std::string::npos, "no headline at " << rounds << " rounds");
    // Exactly once: there is no second headline and no condition under which a
    // different one is printed.
    CHECK(text.find(kResultHeadline, first + 1) == std::string::npos);
  }
}

TEST_CASE("Test K: the headline is byte-identical at two rounds and at four") {
  const Config cfg = report_config();
  const SessionRecord thin = record_with(cfg, 2);
  const SessionRecord full = record_with(cfg, 4);
  const std::string a = render_report(thin, summarise(thin), false);
  const std::string b = render_report(full, summarise(full), false);

  CHECK(a.find(kResultHeadline) != std::string::npos);
  CHECK(b.find(kResultHeadline) != std::string::npos);
  CHECK(a.substr(a.find(kResultHeadline)) == b.substr(b.find(kResultHeadline)));

  // W1 fires at two rounds and not at four, and the headline is the same
  // either way.
  const SessionSummary thin_summary = summarise(thin);
  const SessionSummary full_summary = summarise(full);
  bool thin_w1 = false;
  bool full_w1 = false;
  for (const Warning& w : thin_summary.warnings) {
    if (w.id == "W1") thin_w1 = true;
  }
  for (const Warning& w : full_summary.warnings) {
    if (w.id == "W1") full_w1 = true;
  }
  CHECK(thin_w1);
  CHECK_FALSE(full_w1);
}

TEST_CASE("the report leads with the rankings and the warnings, headline last") {
  const Config cfg = report_config();
  const SessionRecord rec = record_with(cfg, 4);
  const std::string text = render_report(rec, summarise(rec), false);

  const std::size_t channel_table = text.find("Channel ranking");
  const std::size_t audio_table = text.find("Audio ranking");
  const std::size_t warnings = text.find("Data-quality warnings");
  const std::size_t headline = text.find(kResultHeadline);

  REQUIRE(channel_table != std::string::npos);
  REQUIRE(audio_table != std::string::npos);
  REQUIRE(warnings != std::string::npos);
  REQUIRE(headline != std::string::npos);
  CHECK(channel_table < warnings);
  CHECK(audio_table < warnings);
  CHECK(warnings < headline);
}

TEST_CASE("every ranking row that shows a score also shows a yield") {
  const Config cfg = report_config();
  const SessionRecord rec = record_with(cfg, 4);
  const std::string text = render_report(rec, summarise(rec), false);

  // The header carries both columns, and the explanation of why they are never
  // separated.
  CHECK(text.find("events/min") != std::string::npos);
  CHECK(text.find("heard only the loud ones") != std::string::npos);

  // Every data row of the channel table has a numeric score and a numeric
  // yield.
  std::istringstream lines(text);
  std::string line;
  int checked = 0;
  bool in_table = false;
  while (std::getline(lines, line)) {
    if (line.rfind("Channel ranking", 0) == 0) {
      in_table = true;
      continue;
    }
    if (in_table && line.empty()) break;
    if (!in_table) continue;
    if (line.rfind("planned", 0) == 0 || line.rfind("-------", 0) == 0) continue;
    if (line.rfind("  ", 0) == 0) continue;   // a note
    std::istringstream cells(line);
    std::vector<std::string> row;
    std::string cell;
    while (cells >> cell) row.push_back(cell);
    if (row.size() < 7) continue;
    CHECK(row[3] != "-");   // the score
    CHECK(row[5] != "-");   // the yield beside it
    ++checked;
  }
  CHECK(checked == 3);
}

TEST_CASE("the spread column cannot be read as an interval estimate") {
  const Config cfg = report_config();
  const SessionRecord rec = record_with(cfg, 4);
  const std::string text = render_report(rec, summarise(rec), false);

  CHECK(text.find("capture spread") != std::string::npos);
  CHECK(text.find("It is not an interval estimate") != std::string::npos);
  // Which of the two forms was used is stated.
  CHECK(text.find("(iqr)") != std::string::npos);

  const SessionRecord thin = record_with(cfg, 3);
  const std::string thin_text = render_report(thin, summarise(thin), false);
  CHECK(thin_text.find("(min-max)") != std::string::npos);
}

TEST_CASE("changing the display metric moves the tables and nothing else") {
  const Config cfg = report_config();
  const SessionRecord rec = record_with(cfg, 4);

  SessionSummary channel_first = summarise(rec);
  SessionSummary audio_first = channel_first;
  audio_first.report_metric = Metric::Audio;

  const std::string a = render_report(rec, channel_first, false);
  const std::string b = render_report(rec, audio_first, false);
  CHECK(a != b);   // the order differs

  // Every number in both tables is byte-identical: the two renderings contain
  // the same table blocks, in the opposite order.
  auto block = [](const std::string& text, const std::string& title) {
    const std::size_t start = text.find(title);
    REQUIRE(start != std::string::npos);
    const std::size_t end = text.find("\n\n", start);
    return text.substr(start, end - start);
  };
  CHECK(block(a, "Channel ranking") == block(b, "Channel ranking"));
  CHECK(block(a, "Audio ranking") == block(b, "Audio ranking"));

  // And nothing in the summary itself changed but the two display fields.
  nlohmann::json ja;
  nlohmann::json jb;
  to_json(ja, channel_first);
  to_json(jb, audio_first);
  ja.erase("report_metric");
  jb.erase("report_metric");
  ja.erase("report_metric_source");
  jb.erase("report_metric_source");
  CHECK(ja.dump() == jb.dump());
}

TEST_CASE("choose_report_metric mutates only the two display fields") {
  const Config cfg = report_config();
  const SessionRecord rec = record_with(cfg, 4);
  SessionSummary summary = summarise(rec);

  nlohmann::json before;
  to_json(before, summary);

  ScriptedTerminalUi ui(true);
  ui.push_menu_choice(1);   // audio first
  ui::choose_report_metric(summary, ui);

  CHECK(summary.report_metric == Metric::Audio);
  CHECK(summary.report_metric_source == "interactive");

  nlohmann::json after;
  to_json(after, summary);
  before.erase("report_metric");
  after.erase("report_metric");
  before.erase("report_metric_source");
  after.erase("report_metric_source");
  CHECK(before.dump() == after.dump());
}

TEST_CASE("a non-interactive terminal leaves the configured metric alone") {
  const Config cfg = report_config();
  const SessionRecord rec = record_with(cfg, 4);
  SessionSummary summary = summarise(rec);

  ScriptedTerminalUi ui(false);
  ui.push_menu_choice(1);
  ui::choose_report_metric(summary, ui);

  CHECK(summary.report_metric == Metric::Channel);
  CHECK(summary.report_metric_source == "default");
  CHECK(ui.emitted().empty());
}

TEST_CASE("a partial and an aborted report are marked on their first line") {
  const Config cfg = report_config();

  SessionRecord paused = record_with(cfg, 2);
  paused.state = SessionState::Paused;
  SessionSummary partial = summarise(paused);
  partial.partial = true;
  const std::string paused_text = render_report(paused, partial, false);
  CHECK(paused_text.rfind("PARTIAL", 0) == 0);

  SessionRecord aborted = record_with(cfg, 2);
  aborted.state = SessionState::Aborted;
  aborted.abort_reason = "the record could not be trusted to continue";
  aborted.finished_utc = "2026-08-19T15:10:00Z";
  SessionSummary aborted_summary = summarise(aborted);
  aborted_summary.partial = true;
  const std::string aborted_text = render_report(aborted, aborted_summary, false);
  CHECK(aborted_text.rfind("ABORTED - PARTIAL", 0) == 0);
  CHECK(aborted_text.find("could not be trusted") != std::string::npos);

  // A completed session is marked neither way.
  SessionRecord done = record_with(cfg, 4);
  done.state = SessionState::Completed;
  const std::string done_text = render_report(done, summarise(done), false);
  CHECK(done_text.rfind("rtlangle session", 0) == 0);
}

TEST_CASE("an angle absent from a ranking renders as a dash, never as a zero") {
  const Config cfg = report_config();
  SessionRecord rec = record_with(cfg, 4);
  // Strip the audio score from every capture at 90 degrees.
  for (AttemptRecord& a : rec.attempts) {
    if (a.angle_index == 2) {
      a.capture_score_audio_db.reset();
      a.valid_audio_event_count = 0;
      a.audio_insufficient = true;
    }
  }
  const SessionSummary s = summarise(rec);
  const std::string text = render_report(rec, s, false);

  CHECK(s.ranking_audio.size() == 2);
  CHECK(text.find("90.0 deg (ranked)") != std::string::npos);
  CHECK(text.find("audio-eligible captures against") != std::string::npos);
}

TEST_CASE("the report states the weakest SNR the squelch can report") {
  const Config cfg = report_config();
  const SessionRecord rec = record_with(cfg, 4);
  const std::string text = render_report(rec, summarise(rec), false);
  CHECK(text.find("weakest SNR this squelch can report") != std::string::npos);
  CHECK(text.find("4.74") != std::string::npos);
}

TEST_CASE("an empty warning list is not presented as a clean bill of health") {
  const Config cfg = report_config();
  const SessionRecord rec = record_with(cfg, 4);
  const SessionSummary s = summarise(rec);
  REQUIRE(s.warnings.empty());
  const std::string text = render_report(rec, s, false);
  CHECK(text.find("not a clean bill of health") != std::string::npos);
}

}  // TEST_SUITE
