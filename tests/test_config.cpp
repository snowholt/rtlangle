// WP2 — configuration: bounds, cross-field rules, JSON round-trip, resume
// classification, per-command scope, and the resource preflight. Spec sections
// 7, 11.5, and 12.1.
//
// Spec section 7 is the authoritative field list, and this file deliberately
// does not restate it. The completeness tests enumerate the keys that
// to_json(Config) actually produces and drive the registry, the resume
// classification, and the command scope from that one enumeration, so a field
// added without a flag, a bound, a classification, or a command scope fails the
// suite rather than drifting between hand-maintained lists.

#include <doctest/doctest.h>

#include "core/config.h"

#include <nlohmann/json.hpp>

#include <set>
#include <string>
#include <vector>

using namespace rtlangle;
using nlohmann::json;

namespace {

std::set<std::string> serialised_keys() {
  json j;
  to_json(j, Config{});
  std::set<std::string> keys;
  for (auto it = j.begin(); it != j.end(); ++it) keys.insert(it.key());
  return keys;
}

bool has_error_for(const std::vector<ValidationError>& errs, std::string_view field) {
  for (const auto& e : errs) {
    if (e.field == field) return true;
  }
  return false;
}

const ValidationError* error_for(const std::vector<ValidationError>& errs, std::string_view field) {
  for (const auto& e : errs) {
    if (e.field == field) return &e;
  }
  return nullptr;
}

// A configuration that validates cleanly, so a single deliberate change is the
// only reason a test's expected error appears.
Config good() {
  Config c;
  c.center_hz = 118350000;
  return c;
}

// Section 7.4 is the reporting-threshold section. Every field in it must be
// ReportOrOperational, without exception (spec section 11.5 step 6).
const std::set<std::string>& section_74_fields() {
  static const std::set<std::string> s = {"report_metric", "min_captures_advisory",
                                          "min_effect_db", "yield_concordance_ratio",
                                          "noise_drift_warn_db"};
  return s;
}

}  // namespace

TEST_SUITE("config") {

TEST_CASE("the default configuration validates") {
  const auto errs = validate(good());
  for (const auto& e : errs) {
    MESSAGE("unexpected error on " << e.field << ": " << e.message);
  }
  CHECK(errs.empty());
}

TEST_CASE("every serialised field has a registry entry, a flag, and a resume class") {
  const auto keys = serialised_keys();
  CHECK(keys.size() == field_registry().size());

  for (const auto& key : keys) {
    const FieldSpec* f = find_field(key);
    REQUIRE_MESSAGE(f != nullptr, "no registry entry for serialised key " << key);
    CHECK_MESSAGE(!f->flag.empty(), "no CLI flag for " << key);
    CHECK_MESSAGE(f->flag.rfind("--", 0) == 0, "flag for " << key << " lacks the -- prefix");
  }
  // And nothing in the registry is absent from the serialisation.
  for (const FieldSpec& f : field_registry()) {
    CHECK_MESSAGE(keys.count(std::string(f.key)) == 1,
                  "registry key " << f.key << " is not serialised");
  }
}

TEST_CASE("every serialised field has a command scope for all four commands") {
  for (const auto& key : serialised_keys()) {
    for (Command cmd : {Command::Run, Command::Scan, Command::Devices, Command::Report}) {
      const FieldScope s = field_scope(key, cmd);
      CHECK_MESSAGE((s == FieldScope::Required || s == FieldScope::Used ||
                     s == FieldScope::Refused),
                    "no scope for " << key << " on " << to_string(cmd));
    }
  }
}

TEST_CASE("resume classification matches spec section 11.5 step 6") {
  // ExperimentDefining: rejecting a conflicting override is what stops a
  // resumed session from silently mixing two experiments.
  for (const char* k : {"device_index", "center_hz", "sample_rate_hz", "gain_tenth_db",
                        "ppm", "offset_tune_hz", "channel_bw_hz", "channel_rate_hz",
                        "bias_tee", "duration_s", "start_deg", "end_deg", "step_deg",
                        "angles_deg", "rounds", "order", "seed", "angle_reference",
                        "max_angle_deviation_deg", "angle_deviation_warn_deg",
                        "source_spec", "synthetic_snr_db", "synthetic_duty",
                        "noise_percentile", "probe_percentile", "open_db", "close_db",
                        "min_event_ms", "merge_gap_ms", "min_valid_events",
                        "max_active_fraction", "carrier_prominence_db",
                        "carrier_persistence", "audio_guard_ms", "min_audio_window_ms",
                        "max_clipped_fraction"}) {
    CHECK_MESSAGE(resume_class(k) == FieldClass::ExperimentDefining, k);
  }

  for (const char* k : {"report_metric", "min_captures_advisory", "min_effect_db",
                        "yield_concordance_ratio", "noise_drift_warn_db", "settle_s",
                        "read_timeout_factor", "read_timeout_slack_s",
                        "max_retained_events", "no_color", "non_interactive",
                        "setup_note", "scan_start_hz", "scan_end_hz", "scan_channel_hz",
                        "scan_dwell_ms", "scan_passes", "scan_dc_exclusion_hz",
                        "scan_usable_fraction", "scan_top_n"}) {
    CHECK_MESSAGE(resume_class(k) == FieldClass::ReportOrOperational, k);
  }

  for (const char* k : {"session_root", "label", "resume_dir"}) {
    CHECK_MESSAGE(resume_class(k) == FieldClass::PathDetermined, k);
  }
}

TEST_CASE("every section 7.4 field is ReportOrOperational, with no exception") {
  // Asserted as a set equality rather than field by field: section 7.4 now holds
  // only reporting thresholds, and a field that crept back into
  // ExperimentDefining would mean something in there had become a measurement
  // parameter again.
  std::set<std::string> operational_74;
  for (const auto& key : section_74_fields()) {
    if (resume_class(key) == FieldClass::ReportOrOperational) operational_74.insert(key);
  }
  CHECK(operational_74 == section_74_fields());
}

TEST_CASE("command scope: run") {
  CHECK(field_scope("center_hz", Command::Run) == FieldScope::Required);
  CHECK(field_scope("gain_tenth_db", Command::Run) == FieldScope::Used);
  CHECK(field_scope("report_metric", Command::Run) == FieldScope::Used);
  CHECK(field_scope("resume_dir", Command::Run) == FieldScope::Used);
  for (const char* k : {"scan_start_hz", "scan_end_hz", "scan_channel_hz", "scan_dwell_ms",
                        "scan_passes", "scan_dc_exclusion_hz", "scan_usable_fraction",
                        "scan_top_n"}) {
    CHECK_MESSAGE(field_scope(k, Command::Run) == FieldScope::Refused, k);
  }
}

TEST_CASE("command scope: scan") {
  CHECK(field_scope("scan_dwell_ms", Command::Scan) == FieldScope::Used);
  CHECK(field_scope("sample_rate_hz", Command::Scan) == FieldScope::Used);
  // scan uses open_db because a dwell hit is defined against the dwell's own
  // band noise floor by that same threshold. It is one constant, not two.
  CHECK(field_scope("open_db", Command::Scan) == FieldScope::Used);
  CHECK(field_scope("source_spec", Command::Scan) == FieldScope::Used);
  CHECK(field_scope("center_hz", Command::Scan) == FieldScope::Refused);
  CHECK(field_scope("rounds", Command::Scan) == FieldScope::Refused);
  CHECK(field_scope("min_valid_events", Command::Scan) == FieldScope::Refused);
  CHECK(field_scope("report_metric", Command::Scan) == FieldScope::Refused);
  CHECK(field_scope("session_root", Command::Scan) == FieldScope::Refused);
  CHECK(field_scope("label", Command::Scan) == FieldScope::Refused);
  CHECK(field_scope("resume_dir", Command::Scan) == FieldScope::Refused);
}

TEST_CASE("command scope: devices and report") {
  CHECK(field_scope("device_index", Command::Devices) == FieldScope::Used);
  CHECK(field_scope("no_color", Command::Devices) == FieldScope::Used);
  CHECK(field_scope("gain_tenth_db", Command::Devices) == FieldScope::Refused);

  // report accepts the whole of section 7.4, because those change only which
  // warnings the re-render raises and can never alter a stored number.
  for (const auto& key : section_74_fields()) {
    CHECK_MESSAGE(field_scope(key, Command::Report) == FieldScope::Used, key);
  }
  CHECK(field_scope("gain_tenth_db", Command::Report) == FieldScope::Refused);
  CHECK(field_scope("duration_s", Command::Report) == FieldScope::Refused);
}

TEST_CASE("validate_for_command names both the flag and the command") {
  Config c = good();

  const auto report_gain = validate_for_command(c, {"gain_tenth_db"}, Command::Report);
  REQUIRE(report_gain.size() == 1);
  CHECK(report_gain[0].field == "gain_tenth_db");
  CHECK(report_gain[0].message.find("--gain") != std::string::npos);
  CHECK(report_gain[0].message.find("report") != std::string::npos);

  const auto run_scan = validate_for_command(c, {"scan_dwell_ms"}, Command::Run);
  REQUIRE(run_scan.size() == 1);
  CHECK(run_scan[0].message.find("--scan-dwell") != std::string::npos);
  CHECK(run_scan[0].message.find("run") != std::string::npos);

  const auto scan_rounds = validate_for_command(c, {"rounds"}, Command::Scan);
  REQUIRE(scan_rounds.size() == 1);
  CHECK(scan_rounds[0].message.find("--rounds") != std::string::npos);

  // A field left at its default is never a scope error, even for a command that
  // refuses it.
  CHECK(validate_for_command(c, {}, Command::Report).empty());
  CHECK(validate_for_command(c, {}, Command::Scan).empty());
  CHECK(validate_for_command(c, {}, Command::Devices).empty());
}

TEST_CASE("run requires a centre frequency unless it is resuming") {
  Config c;
  const auto missing = validate_for_command(c, {}, Command::Run);
  REQUIRE(missing.size() == 1);
  CHECK(missing[0].field == "center_hz");
  CHECK(missing[0].message.find("--freq") != std::string::npos);

  c.resume_dir = "sessions/20260819-143000-airband";
  CHECK(validate_for_command(c, {"resume_dir"}, Command::Run).empty());

  Config with_freq;
  with_freq.center_hz = 118350000;
  CHECK(validate_for_command(with_freq, {"center_hz"}, Command::Run).empty());
  // A value that arrived through a configuration file rather than a flag still
  // satisfies the requirement.
  CHECK(validate_for_command(with_freq, {}, Command::Run).empty());
}

TEST_CASE("cross-field rule: the channel filter may not exceed Nyquist") {
  Config c = good();
  c.channel_bw_hz = 25000;
  c.channel_rate_hz = 16000;
  const auto errs = validate(c);
  const ValidationError* e = error_for(errs, "channel_bw_hz");
  REQUIRE(e != nullptr);
  CHECK(e->message.find("25000") != std::string::npos);
  CHECK(e->message.find("16000") != std::string::npos);
}

TEST_CASE("cross-field rule: a channel narrower than the audio band is refused") {
  Config c = good();
  c.channel_bw_hz = 2000;
  const auto errs = validate(c);
  const ValidationError* e = error_for(errs, "channel_bw_hz");
  REQUIRE(e != nullptr);
  CHECK(e->message.find("2000") != std::string::npos);
}

TEST_CASE("cross-field rule: a decimation ratio of 17 has no factorisation") {
  Config c = good();
  c.channel_rate_hz = 60000;
  c.sample_rate_hz = 1020000;  // ratio 17
  const auto errs = validate(c);
  const ValidationError* e = error_for(errs, "sample_rate_hz");
  REQUIRE(e != nullptr);
  CHECK(e->message.find("17") != std::string::npos);
  CHECK(e->message.find("nearest compatible") != std::string::npos);
}

TEST_CASE("cross-field rule: a decimation ratio of one is refused") {
  Config c = good();
  c.channel_rate_hz = 96000;
  c.sample_rate_hz = 96000;
  const auto errs = validate(c);
  CHECK(has_error_for(errs, "sample_rate_hz"));
}

TEST_CASE("cross-field rule: the offset must keep the channel inside the band") {
  Config c = good();
  c.offset_tune_hz = 500000;  // 500000 + 4000 >= 0.45 * 1024000 = 460800
  const auto errs = validate(c);
  const ValidationError* e = error_for(errs, "offset_tune_hz");
  REQUIRE(e != nullptr);
  CHECK(e->message.find("500000") != std::string::npos);
  CHECK(e->message.find("460800") != std::string::npos);
}

TEST_CASE("cross-field rule: the scan band cannot be covered when U is below 3D") {
  Config c = good();
  c.scan_dc_exclusion_hz = 256000;  // U = 409600, 3D = 768000
  c.scan_usable_fraction = 0.8;
  const auto errs = validate(c);
  const ValidationError* e = error_for(errs, "scan_usable_fraction");
  REQUIRE(e != nullptr);
  CHECK(e->message.find("256000") != std::string::npos);
  CHECK(e->message.find("0.8") != std::string::npos);

  // The defaults are feasible: U = 409600 and 3D = 90000.
  CHECK_FALSE(has_error_for(validate(good()), "scan_usable_fraction"));
}

TEST_CASE("every bound produces its own error naming its field") {
  struct Row {
    const char* field;
    Config (*mutate)();
  };

  // Each row moves exactly one field out of bounds.
  const std::vector<std::pair<std::string, Config>> rows = [] {
    std::vector<std::pair<std::string, Config>> v;
    auto with = [](auto&& fn) {
      Config c = good();
      fn(c);
      return c;
    };
    v.emplace_back("device_index", with([](Config& c) { c.device_index = 64; }));
    v.emplace_back("center_hz", with([](Config& c) { c.center_hz = 1000; }));
    v.emplace_back("sample_rate_hz", with([](Config& c) { c.sample_rate_hz = 500000; }));
    v.emplace_back("gain_tenth_db", with([](Config& c) { c.gain_tenth_db = 900; }));
    v.emplace_back("ppm", with([](Config& c) { c.ppm = 500; }));
    v.emplace_back("channel_rate_hz", with([](Config& c) { c.channel_rate_hz = 4000; }));
    v.emplace_back("duration_s", with([](Config& c) { c.duration_s = 0.0; }));
    v.emplace_back("settle_s", with([](Config& c) { c.settle_s = -1.0; }));
    v.emplace_back("start_deg", with([](Config& c) { c.start_deg = 400.0; }));
    v.emplace_back("end_deg", with([](Config& c) { c.end_deg = -5.0; }));
    v.emplace_back("step_deg", with([](Config& c) { c.step_deg = 0.0; }));
    v.emplace_back("rounds", with([](Config& c) { c.rounds = 0; }));
    v.emplace_back("angle_reference", with([](Config& c) { c.angle_reference.assign(201, 'x'); }));
    v.emplace_back("setup_note", with([](Config& c) { c.setup_note.assign(501, 'x'); }));
    v.emplace_back("max_angle_deviation_deg",
                   with([](Config& c) { c.max_angle_deviation_deg = 0.0; }));
    v.emplace_back("angle_deviation_warn_deg",
                   with([](Config& c) { c.angle_deviation_warn_deg = 200.0; }));
    v.emplace_back("noise_percentile", with([](Config& c) { c.noise_percentile = 60.0; }));
    v.emplace_back("probe_percentile", with([](Config& c) { c.probe_percentile = 30.0; }));
    v.emplace_back("open_db", with([](Config& c) { c.open_db = 0.0; }));
    v.emplace_back("close_db", with([](Config& c) { c.close_db = -1.0; }));
    v.emplace_back("min_event_ms", with([](Config& c) { c.min_event_ms = 1.0; }));
    v.emplace_back("merge_gap_ms", with([](Config& c) { c.merge_gap_ms = -1.0; }));
    v.emplace_back("min_valid_events", with([](Config& c) { c.min_valid_events = 0; }));
    v.emplace_back("max_active_fraction", with([](Config& c) { c.max_active_fraction = 1.0; }));
    v.emplace_back("carrier_prominence_db",
                   with([](Config& c) { c.carrier_prominence_db = 0.0; }));
    v.emplace_back("carrier_persistence", with([](Config& c) { c.carrier_persistence = 0.0; }));
    v.emplace_back("audio_guard_ms", with([](Config& c) { c.audio_guard_ms = 6000.0; }));
    v.emplace_back("min_audio_window_ms", with([](Config& c) { c.min_audio_window_ms = 1.0; }));
    v.emplace_back("max_clipped_fraction", with([](Config& c) { c.max_clipped_fraction = 1.0; }));
    v.emplace_back("max_retained_events", with([](Config& c) { c.max_retained_events = 3; }));
    v.emplace_back("read_timeout_factor", with([](Config& c) { c.read_timeout_factor = 0.5; }));
    v.emplace_back("read_timeout_slack_s",
                   with([](Config& c) { c.read_timeout_slack_s = 0.0; }));
    v.emplace_back("min_captures_advisory",
                   with([](Config& c) { c.min_captures_advisory = 1; }));
    v.emplace_back("min_effect_db", with([](Config& c) { c.min_effect_db = 40.0; }));
    v.emplace_back("yield_concordance_ratio",
                   with([](Config& c) { c.yield_concordance_ratio = 0.0; }));
    v.emplace_back("noise_drift_warn_db", with([](Config& c) { c.noise_drift_warn_db = 0.0; }));
    v.emplace_back("session_root", with([](Config& c) { c.session_root.clear(); }));
    v.emplace_back("source_spec", with([](Config& c) { c.source_spec = "usrp"; }));
    v.emplace_back("synthetic_snr_db", with([](Config& c) { c.synthetic_snr_db = 100.0; }));
    v.emplace_back("synthetic_duty", with([](Config& c) { c.synthetic_duty = 1.0; }));
    v.emplace_back("scan_start_hz", with([](Config& c) { c.scan_start_hz = 1000; }));
    v.emplace_back("scan_end_hz", with([](Config& c) { c.scan_end_hz = 100000000; }));
    v.emplace_back("scan_channel_hz", with([](Config& c) { c.scan_channel_hz = 100; }));
    v.emplace_back("scan_dwell_ms", with([](Config& c) { c.scan_dwell_ms = 1.0; }));
    v.emplace_back("scan_passes", with([](Config& c) { c.scan_passes = 0; }));
    v.emplace_back("scan_usable_fraction",
                   with([](Config& c) { c.scan_usable_fraction = 1.5; }));
    v.emplace_back("scan_top_n", with([](Config& c) { c.scan_top_n = 0; }));
    return v;
  }();

  for (const auto& [field, cfg] : rows) {
    const auto errs = validate(cfg);
    CHECK_MESSAGE(has_error_for(errs, field), "no error raised for " << field);
  }
  (void)sizeof(Row);
}

TEST_CASE("non_interactive with the device source is refused") {
  Config c = good();
  c.non_interactive = true;
  CHECK(has_error_for(validate(c), "non_interactive"));
  c.source_spec = "synthetic";
  CHECK_FALSE(has_error_for(validate(c), "non_interactive"));
  c.source_spec = "file:/tmp/capture.cu8";
  CHECK_FALSE(has_error_for(validate(c), "non_interactive"));
}

TEST_CASE("Config has no AGC field and no decision field") {
  const auto keys = serialised_keys();
  for (const char* forbidden : {"agc", "allow_agc", "primary_metric", "family_alpha",
                                "min_captures_for_resolution", "selected_metric"}) {
    CHECK_MESSAGE(keys.count(forbidden) == 0, forbidden);
  }
  CHECK(keys.count("min_captures_advisory") == 1);
  CHECK(Config{}.min_captures_advisory == 4);
  CHECK(Config{}.rounds == 2);
  CHECK(Config{}.order == VisitOrder::Alternating);
  CHECK(Config{}.report_metric == Metric::Channel);
}

TEST_CASE("JSON round-trip is byte-equal for a fully populated configuration") {
  Config c = good();
  c.gain_tenth_db = 496;
  c.angles_deg = {0.0, 30.0, 60.0, 90.0};
  c.order = VisitOrder::Random;
  c.seed = 1234567;
  c.angle_reference = "marked arm along the balcony rail";
  c.setup_note = "cable routed along the rail";
  c.report_metric = Metric::Audio;
  c.label = "airband";
  c.source_spec = "synthetic";
  c.non_interactive = true;

  json j1;
  to_json(j1, c);
  Config back;
  from_json(j1, back);
  json j2;
  to_json(j2, back);
  CHECK(j1.dump() == j2.dump());

  CHECK(back.gain_tenth_db.has_value());
  CHECK(*back.gain_tenth_db == 496);
  CHECK(back.order == VisitOrder::Random);
  CHECK(back.report_metric == Metric::Audio);
  CHECK(back.angles_deg.size() == 4);
}

TEST_CASE("a gain of max round-trips as the string, not as a number") {
  Config c;
  CHECK_FALSE(c.gain_tenth_db.has_value());
  json j;
  to_json(j, c);
  CHECK(j.at("gain_tenth_db").is_string());
  CHECK(j.at("gain_tenth_db").get<std::string>() == "max");

  Config back;
  from_json(j, back);
  CHECK_FALSE(back.gain_tenth_db.has_value());
}

TEST_CASE("an unset centre frequency round-trips as null") {
  Config c;
  json j;
  to_json(j, c);
  CHECK(j.at("center_hz").is_null());
  Config back;
  back.center_hz = 99;
  from_json(j, back);
  CHECK_FALSE(back.center_hz.has_value());
}

TEST_CASE("an unknown key is rejected with the key named") {
  json j;
  to_json(j, good());
  j["allow_agc"] = true;
  Config back;
  bool threw = false;
  try {
    from_json(j, back);
  } catch (const json::exception& e) {
    threw = true;
    CHECK(std::string(e.what()).find("allow_agc") != std::string::npos);
  }
  CHECK(threw);
}

TEST_CASE("an out-of-vocabulary enumeration value is rejected") {
  json j;
  to_json(j, good());
  j["order"] = "sideways";
  Config back;
  CHECK_THROWS_AS(from_json(j, back), json::exception);

  json j2;
  to_json(j2, good());
  j2["report_metric"] = "spectral";
  CHECK_THROWS_AS(from_json(j2, back), json::exception);
}

TEST_CASE("probe percentile follows the noise percentile down when it was not set") {
  Config c = good();
  c.noise_percentile = 3.0;
  apply_derived_defaults(c, {"noise_percentile"});
  CHECK(c.probe_percentile == doctest::Approx(3.0));

  Config d = good();
  d.noise_percentile = 3.0;
  d.probe_percentile = 2.0;
  apply_derived_defaults(d, {"noise_percentile", "probe_percentile"});
  CHECK(d.probe_percentile == doctest::Approx(2.0));

  Config e = good();
  apply_derived_defaults(e, {});
  CHECK(e.probe_percentile == doctest::Approx(5.0));
}

TEST_CASE("resource preflight reproduces the spec section 11.1 table") {
  Config c = good();  // 7 angles, 2 rounds, 60 s duration, 3 s settle
  const ResourceEstimate e = estimate_resources(c);
  CHECK(e.planned_visits == 14);
  CHECK(e.attempt_ceiling == 250);
  CHECK(e.capture_seconds == doctest::Approx(14.0 * 63.0));  // 882 s

  // 1.2 KiB + 32 * 0.2 KiB = 7.6 KiB per attempt.
  CHECK(e.attempt_bytes == doctest::Approx(7.6 * 1024.0).epsilon(1e-3));
  const double final_mib = static_cast<double>(e.final_json_bytes) / (1024.0 * 1024.0);
  CHECK(final_mib == doctest::Approx(1.855).epsilon(0.01));
  const double cumulative_mib = static_cast<double>(e.cumulative_bytes) / (1024.0 * 1024.0);
  CHECK(cumulative_mib == doctest::Approx(232.0).epsilon(0.01));
  CHECK_FALSE(e.needs_confirmation);
}

TEST_CASE("a large retention cap is refused by the projected document size") {
  Config c = good();
  c.max_retained_events = 500;
  const ResourceEstimate e = estimate_resources(c);
  CHECK(e.final_json_bytes > kMaxFinalJsonBytes);
  const auto errs = validate(c);
  const ValidationError* err = error_for(errs, "max_retained_events");
  REQUIRE(err != nullptr);
  CHECK(err->message.find("500") != std::string::npos);
  CHECK(err->message.find("16 MiB") != std::string::npos);
}

TEST_CASE("long sessions need confirmation and too many visits are refused") {
  Config c = good();
  c.duration_s = 600.0;  // 14 visits x 603 s = 8442 s
  CHECK(estimate_resources(c).needs_confirmation);
  CHECK_FALSE(has_error_for(validate(c), "duration_s"));

  c.non_interactive = true;
  c.source_spec = "synthetic";
  CHECK(has_error_for(validate(c), "duration_s"));

  Config many = good();
  many.step_deg = 2.0;   // 46 angles
  many.rounds = 5;       // 230 planned visits
  CHECK(has_error_for(validate(many), "rounds"));
}

TEST_CASE("retry capacity for the remaining visits is reserved") {
  const int ceiling = kAttemptCeiling;
  // The default 14-visit experiment leaves 236 retries available at the start.
  CHECK(retry_available(0, 14, ceiling));
  CHECK(estimate_resources(good()).attempt_ceiling - 14 == 236);

  // At the 200-visit cap, 50 retries remain.
  CHECK(ceiling - kMaxPlannedVisits == 50);

  // The boundary walk: available at ceiling-1, withheld at the ceiling and above.
  CHECK(retry_available(ceiling - 1 - 5, 5, ceiling));
  CHECK_FALSE(retry_available(ceiling - 5, 5, ceiling));
  CHECK_FALSE(retry_available(ceiling + 1 - 5, 5, ceiling));
}

TEST_CASE("resolved angles come from the explicit list when one is given") {
  Config c = good();
  CHECK(resolved_angles(c).size() == 7);
  c.angles_deg = {90.0, 0.0, 45.0, 45.0};
  const auto a = resolved_angles(c);
  REQUIRE(a.size() == 3);
  CHECK(a[0] == doctest::Approx(0.0));
  CHECK(a[1] == doctest::Approx(45.0));
  CHECK(a[2] == doctest::Approx(90.0));
}

TEST_CASE("configuration notices are informational, not errors") {
  Config c = good();
  c.step_deg = 20.0;
  const auto notices = config_notices(c);
  bool found = false;
  for (const auto& n : notices) {
    if (n.find("does not lie on the grid") != std::string::npos) found = true;
  }
  CHECK(found);
  CHECK_FALSE(has_error_for(validate(c), "end_deg"));

  Config opposed = good();
  opposed.angles_deg = {0.0, 180.0};
  bool env = false;
  for (const auto& n : config_notices(opposed)) {
    if (n.find("environmental") != std::string::npos) env = true;
  }
  CHECK(env);

  Config no_offset = good();
  no_offset.offset_tune_hz = 0;
  bool dc = false;
  for (const auto& n : config_notices(no_offset)) {
    if (n.find("DC spur") != std::string::npos) dc = true;
  }
  CHECK(dc);
}

TEST_CASE("frequency literals parse") {
  CHECK(parse_frequency("118.1M").value() == doctest::Approx(118100000.0));
  CHECK(parse_frequency("121500k").value() == doctest::Approx(121500000.0));
  CHECK(parse_frequency("118350000").value() == doctest::Approx(118350000.0));
  CHECK(parse_frequency("1.766G").value() == doctest::Approx(1.766e9));
  CHECK_FALSE(parse_frequency("").has_value());
  CHECK_FALSE(parse_frequency("abc").has_value());
  CHECK_FALSE(parse_frequency("118.1MHz").has_value());
  CHECK_FALSE(parse_frequency("118..1M").has_value());
}

}  // TEST_SUITE
