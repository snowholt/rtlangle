#include "core/config.h"

#include "core/statistics.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace rtlangle {
namespace {

using J = nlohmann::json;

std::string num(double v) {
  std::ostringstream os;
  os.precision(10);
  os << v;
  return os.str();
}

std::string num(std::uint64_t v) { return std::to_string(v); }
std::string num(std::int64_t v) { return std::to_string(v); }
std::string num(int v) { return std::to_string(v); }
std::string num(std::uint32_t v) { return std::to_string(v); }

void add(std::vector<ValidationError>& out, std::string_view field, std::string message) {
  out.push_back({std::string(field), std::move(message)});
}

// The registry. Declaration order, one entry per serialised key.
constexpr std::array<FieldSpec, 59> kFields{{
    // 7.1 Receiver
    {"device_index",             "--device",                   FieldType::Int,            FieldClass::ExperimentDefining},
    {"center_hz",                "--freq",                     FieldType::OptionalUInt32, FieldClass::ExperimentDefining},
    {"sample_rate_hz",           "--sample-rate",              FieldType::UInt32,         FieldClass::ExperimentDefining},
    {"gain_tenth_db",            "--gain",                     FieldType::GainOrMax,      FieldClass::ExperimentDefining},
    {"ppm",                      "--ppm",                      FieldType::Int,            FieldClass::ExperimentDefining},
    {"offset_tune_hz",           "--offset-tune-hz",           FieldType::Int64,          FieldClass::ExperimentDefining},
    {"channel_bw_hz",            "--channel-bw",               FieldType::UInt32,         FieldClass::ExperimentDefining},
    {"channel_rate_hz",          "--channel-rate",             FieldType::UInt32,         FieldClass::ExperimentDefining},
    {"bias_tee",                 "--bias-tee",                 FieldType::Bool,           FieldClass::ExperimentDefining},
    // 7.2 Experiment
    {"duration_s",               "--duration",                 FieldType::Double,         FieldClass::ExperimentDefining},
    {"settle_s",                 "--settle",                   FieldType::Double,         FieldClass::ReportOrOperational},
    {"start_deg",                "--start-deg",                FieldType::Double,         FieldClass::ExperimentDefining},
    {"end_deg",                  "--end-deg",                  FieldType::Double,         FieldClass::ExperimentDefining},
    {"step_deg",                 "--step-deg",                 FieldType::Double,         FieldClass::ExperimentDefining},
    {"angles_deg",               "--angles",                   FieldType::DoubleList,     FieldClass::ExperimentDefining},
    {"rounds",                   "--rounds",                   FieldType::Int,            FieldClass::ExperimentDefining},
    {"order",                    "--order",                    FieldType::OrderEnum,      FieldClass::ExperimentDefining},
    {"seed",                     "--seed",                     FieldType::UInt64,         FieldClass::ExperimentDefining},
    {"angle_reference",          "--angle-reference",          FieldType::String,         FieldClass::ExperimentDefining},
    {"setup_note",               "--setup-note",               FieldType::String,         FieldClass::ReportOrOperational},
    {"max_angle_deviation_deg",  "--max-angle-deviation-deg",  FieldType::Double,         FieldClass::ExperimentDefining},
    {"angle_deviation_warn_deg", "--angle-deviation-warn-deg", FieldType::Double,         FieldClass::ExperimentDefining},
    // 7.3 Measurement
    {"noise_percentile",         "--noise-percentile",         FieldType::Double,         FieldClass::ExperimentDefining},
    {"probe_percentile",         "--probe-percentile",         FieldType::Double,         FieldClass::ExperimentDefining},
    {"open_db",                  "--open-db",                  FieldType::Double,         FieldClass::ExperimentDefining},
    {"close_db",                 "--close-db",                 FieldType::Double,         FieldClass::ExperimentDefining},
    {"min_event_ms",             "--min-event-ms",             FieldType::Double,         FieldClass::ExperimentDefining},
    {"merge_gap_ms",             "--merge-gap-ms",             FieldType::Double,         FieldClass::ExperimentDefining},
    {"min_valid_events",         "--min-valid-events",         FieldType::Int,            FieldClass::ExperimentDefining},
    {"max_active_fraction",      "--max-active-fraction",      FieldType::Double,         FieldClass::ExperimentDefining},
    {"carrier_prominence_db",    "--carrier-prominence-db",    FieldType::Double,         FieldClass::ExperimentDefining},
    {"carrier_persistence",      "--carrier-persistence",      FieldType::Double,         FieldClass::ExperimentDefining},
    {"audio_guard_ms",           "--audio-guard-ms",           FieldType::Double,         FieldClass::ExperimentDefining},
    {"min_audio_window_ms",      "--min-audio-window-ms",      FieldType::Double,         FieldClass::ExperimentDefining},
    {"max_clipped_fraction",     "--max-clipped-fraction",     FieldType::Double,         FieldClass::ExperimentDefining},
    {"max_retained_events",      "--max-retained-events",      FieldType::Int,            FieldClass::ReportOrOperational},
    {"read_timeout_factor",      "--read-timeout-factor",      FieldType::Double,         FieldClass::ReportOrOperational},
    {"read_timeout_slack_s",     "--read-timeout-slack",       FieldType::Double,         FieldClass::ReportOrOperational},
    // 7.4 Reporting thresholds
    {"report_metric",            "--report-metric",            FieldType::MetricEnum,     FieldClass::ReportOrOperational},
    {"min_captures_advisory",    "--min-captures",             FieldType::Int,            FieldClass::ReportOrOperational},
    {"min_effect_db",            "--min-effect-db",            FieldType::Double,         FieldClass::ReportOrOperational},
    {"yield_concordance_ratio",  "--yield-concordance-ratio",  FieldType::Double,         FieldClass::ReportOrOperational},
    {"noise_drift_warn_db",      "--noise-drift-warn-db",      FieldType::Double,         FieldClass::ReportOrOperational},
    // 7.5 Session, source, and output
    {"session_root",             "--session-dir",              FieldType::Path,           FieldClass::PathDetermined},
    {"label",                    "--label",                    FieldType::String,         FieldClass::PathDetermined},
    {"resume_dir",               "--resume",                   FieldType::Path,           FieldClass::PathDetermined},
    {"source_spec",              "--source",                   FieldType::String,         FieldClass::ExperimentDefining},
    {"synthetic_snr_db",         "--synthetic-snr-db",         FieldType::Double,         FieldClass::ExperimentDefining},
    {"synthetic_duty",           "--synthetic-duty",           FieldType::Double,         FieldClass::ExperimentDefining},
    {"no_color",                 "--no-color",                 FieldType::Bool,           FieldClass::ReportOrOperational},
    {"non_interactive",          "--non-interactive",          FieldType::Bool,           FieldClass::ReportOrOperational},
    // 7.6 Scan
    {"scan_start_hz",            "--scan-start",               FieldType::UInt32,         FieldClass::ReportOrOperational},
    {"scan_end_hz",              "--scan-end",                 FieldType::UInt32,         FieldClass::ReportOrOperational},
    {"scan_channel_hz",          "--scan-channel",             FieldType::UInt32,         FieldClass::ReportOrOperational},
    {"scan_dwell_ms",            "--scan-dwell",               FieldType::Double,         FieldClass::ReportOrOperational},
    {"scan_passes",              "--scan-passes",              FieldType::Int,            FieldClass::ReportOrOperational},
    {"scan_dc_exclusion_hz",     "--scan-dc-exclusion",        FieldType::UInt32,         FieldClass::ReportOrOperational},
    {"scan_usable_fraction",     "--scan-usable-fraction",     FieldType::Double,         FieldClass::ReportOrOperational},
    {"scan_top_n",               "--scan-top",                 FieldType::Int,            FieldClass::ReportOrOperational},
}};

bool is_scan_field(std::string_view key) { return key.rfind("scan_", 0) == 0; }

// Spec section 7.9, expressed as the accepted set per command. Everything a
// command does not accept is Refused, which is exactly the spec's rule that a
// flag a command does not use is a usage error.
const std::unordered_set<std::string_view>& scan_used() {
  static const std::unordered_set<std::string_view> s = {
      "device_index", "sample_rate_hz", "gain_tenth_db", "ppm", "offset_tune_hz",
      "channel_bw_hz", "channel_rate_hz", "bias_tee",
      "open_db",
      "source_spec", "synthetic_snr_db", "synthetic_duty", "no_color", "non_interactive"};
  return s;
}

const std::unordered_set<std::string_view>& devices_used() {
  static const std::unordered_set<std::string_view> s = {"device_index", "no_color"};
  return s;
}

const std::unordered_set<std::string_view>& report_used() {
  static const std::unordered_set<std::string_view> s = {
      "report_metric", "min_captures_advisory", "min_effect_db",
      "yield_concordance_ratio", "noise_drift_warn_db", "no_color", "non_interactive"};
  return s;
}

// The decimation cascade builds stages from factors in [2,16]. A ratio with a
// prime factor above 16 has no such factorisation.
bool factorises_into_small_stages(std::uint32_t ratio) {
  std::uint32_t r = ratio;
  for (std::uint32_t p = 2; p <= 16; ++p) {
    while (r % p == 0) r /= p;
  }
  return r == 1;
}

std::uint32_t nearest_compatible_sample_rate(std::uint32_t channel_rate_hz,
                                             std::uint32_t sample_rate_hz) {
  std::uint32_t best = 0;
  std::uint64_t best_delta = 0;
  for (std::uint32_t ratio = 2; ratio <= 256; ++ratio) {
    if (!factorises_into_small_stages(ratio)) continue;
    const std::uint64_t candidate = static_cast<std::uint64_t>(channel_rate_hz) * ratio;
    if (candidate > 3200000ULL) break;
    const bool legal = (candidate >= 225001 && candidate <= 300000) ||
                       (candidate >= 900001 && candidate <= 3200000);
    if (!legal) continue;
    const std::uint64_t delta = candidate > sample_rate_hz
                                    ? candidate - sample_rate_hz
                                    : static_cast<std::uint64_t>(sample_rate_hz) - candidate;
    if (best == 0 || delta < best_delta) {
      best = static_cast<std::uint32_t>(candidate);
      best_delta = delta;
    }
  }
  return best;
}

}  // namespace

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------

std::optional<Metric> parse_metric(std::string_view s) {
  if (s == "channel") return Metric::Channel;
  if (s == "audio") return Metric::Audio;
  return std::nullopt;
}

std::string_view to_string(Metric m) {
  return m == Metric::Channel ? "channel" : "audio";
}

std::optional<Command> parse_command(std::string_view s) {
  if (s == "run") return Command::Run;
  if (s == "scan") return Command::Scan;
  if (s == "devices") return Command::Devices;
  if (s == "report") return Command::Report;
  return std::nullopt;
}

std::string_view to_string(Command c) {
  switch (c) {
    case Command::Run: return "run";
    case Command::Scan: return "scan";
    case Command::Devices: return "devices";
    case Command::Report: return "report";
  }
  return "run";
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

std::span<const FieldSpec> field_registry() { return {kFields.data(), kFields.size()}; }

const FieldSpec* find_field(std::string_view key) {
  for (const FieldSpec& f : kFields) {
    if (f.key == key) return &f;
  }
  return nullptr;
}

FieldClass resume_class(std::string_view field) {
  const FieldSpec* f = find_field(field);
  // The registry is the single classification table. A key with no entry cannot
  // be classified, and the registry test fails rather than letting it default
  // to overridable.
  return f != nullptr ? f->resume : FieldClass::ExperimentDefining;
}

FieldScope field_scope(std::string_view field, Command command) {
  switch (command) {
    case Command::Run:
      if (field == "center_hz") return FieldScope::Required;
      if (is_scan_field(field)) return FieldScope::Refused;
      return FieldScope::Used;
    case Command::Scan:
      if (is_scan_field(field)) return FieldScope::Used;
      return scan_used().count(field) != 0 ? FieldScope::Used : FieldScope::Refused;
    case Command::Devices:
      return devices_used().count(field) != 0 ? FieldScope::Used : FieldScope::Refused;
    case Command::Report:
      return report_used().count(field) != 0 ? FieldScope::Used : FieldScope::Refused;
  }
  return FieldScope::Refused;
}

std::vector<ValidationError> validate_for_command(
    const Config& cfg, const std::set<std::string>& explicitly_set, Command command) {
  std::vector<ValidationError> out;
  for (const FieldSpec& f : kFields) {
    const FieldScope scope = field_scope(f.key, command);
    const bool was_set = explicitly_set.count(std::string(f.key)) != 0;

    if (scope == FieldScope::Refused && was_set) {
      add(out, f.key,
          std::string("The flag ") + std::string(f.flag) + " is not accepted by the " +
              std::string(to_string(command)) +
              " command. Remove it, or run a command that uses it.");
      continue;
    }
    if (scope == FieldScope::Required) {
      // A required field is satisfied by a value, wherever it came from - a
      // flag, a configuration file, or an interactive edit - not by having been
      // typed on the command line.
      if (f.key == "center_hz") {
        // `run --resume <dir>` does not require --freq: spec section 11.5
        // refuses any attempt to change it and the stored value is
        // authoritative.
        if (cfg.center_hz.has_value() || !cfg.resume_dir.empty()) continue;
      } else if (was_set) {
        continue;
      }
      add(out, f.key,
          std::string("The ") + std::string(to_string(command)) + " command requires " +
              std::string(f.flag) + ".");
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Derived defaults, validation, notices
// ---------------------------------------------------------------------------

void apply_derived_defaults(Config& cfg, const std::set<std::string>& explicitly_set) {
  // Spec section 7.3: probe_percentile defaults to min(5, noise_percentile), so
  // a lowered noise percentile carries the probe down with it rather than
  // leaving the probe above the floor it is meant to sit under.
  if (explicitly_set.count("probe_percentile") == 0) {
    cfg.probe_percentile = std::min(5.0, cfg.noise_percentile);
  }
}

std::vector<double> resolved_angles(const Config& cfg) {
  if (!cfg.angles_deg.empty()) return dedupe_and_sort_angles(cfg.angles_deg);
  return dedupe_and_sort_angles(generate_angles(cfg.start_deg, cfg.end_deg, cfg.step_deg));
}

namespace {

void check_range(std::vector<ValidationError>& out, std::string_view field, double value,
                 double lo, double hi, bool lo_open, bool hi_open) {
  const bool lo_ok = lo_open ? (value > lo) : (value >= lo);
  const bool hi_ok = hi_open ? (value < hi) : (value <= hi);
  if (std::isfinite(value) && lo_ok && hi_ok) return;
  std::ostringstream os;
  os << field << " is " << num(value) << ", which is outside " << (lo_open ? "(" : "[")
     << num(lo) << ", " << num(hi) << (hi_open ? ")" : "]") << ".";
  add(out, field, os.str());
}

}  // namespace

std::vector<ValidationError> validate(const Config& cfg) {
  std::vector<ValidationError> out;

  // ---- 7.1 Receiver -------------------------------------------------------
  if (cfg.device_index < 0 || cfg.device_index > 63) {
    add(out, "device_index",
        "device_index is " + num(cfg.device_index) + ", which is outside [0, 63].");
  }
  if (cfg.center_hz.has_value()) {
    const double f = static_cast<double>(*cfg.center_hz);
    if (f < 24e6 || f > 1.766e9) {
      add(out, "center_hz",
          "center_hz is " + num(*cfg.center_hz) +
              " Hz, which is outside the tuner range [24000000, 1766000000].");
    }
  }
  {
    const std::uint32_t r = cfg.sample_rate_hz;
    const bool ok = (r >= 225001 && r <= 300000) || (r >= 900001 && r <= 3200000);
    if (!ok) {
      add(out, "sample_rate_hz",
          "sample_rate_hz is " + num(r) +
              " Hz, which is outside the device's supported ranges "
              "[225001, 300000] and [900001, 3200000].");
    }
  }
  if (cfg.gain_tenth_db.has_value() &&
      (*cfg.gain_tenth_db < -10 || *cfg.gain_tenth_db > 500)) {
    add(out, "gain_tenth_db",
        "gain_tenth_db is " + num(*cfg.gain_tenth_db) +
            " tenths of a dB, which is outside [-10, 500]. Use \"max\" for the "
            "highest entry in the device's gain table.");
  }
  if (cfg.ppm < -200 || cfg.ppm > 200) {
    add(out, "ppm", "ppm is " + num(cfg.ppm) + ", which is outside [-200, 200].");
  }
  if (cfg.channel_bw_hz < 8000 || cfg.channel_bw_hz > 25000) {
    add(out, "channel_bw_hz",
        "channel_bw_hz is " + num(cfg.channel_bw_hz) +
            " Hz, which is outside [8000, 25000]. The audio low-pass stopband is "
            "4000 Hz, so a channel narrower than plus or minus 4 kHz would remove "
            "the audio band the demodulator recovers.");
  }
  if (cfg.channel_rate_hz < 16000 || cfg.channel_rate_hz > 96000) {
    add(out, "channel_rate_hz",
        "channel_rate_hz is " + num(cfg.channel_rate_hz) +
            " Hz, which is outside [16000, 96000].");
  }

  // ---- 7.1 cross-field rules ---------------------------------------------
  if (cfg.channel_rate_hz > 0 &&
      static_cast<std::uint64_t>(cfg.channel_bw_hz) + 2000 >= cfg.channel_rate_hz) {
    add(out, "channel_bw_hz",
        "channel_bw_hz " + num(cfg.channel_bw_hz) + " Hz and channel_rate_hz " +
            num(cfg.channel_rate_hz) +
            " Hz are incompatible: the channel filter's stopband edge is "
            "channel_bw_hz/2 + 1000 = " + num(cfg.channel_bw_hz / 2 + 1000) +
            " Hz and must stay below the channel-rate Nyquist frequency of " +
            num(cfg.channel_rate_hz / 2) + " Hz. Require channel_bw_hz + 2000 < channel_rate_hz.");
  }
  if (cfg.channel_rate_hz > 0) {
    if (cfg.sample_rate_hz % cfg.channel_rate_hz != 0) {
      add(out, "sample_rate_hz",
          "sample_rate_hz " + num(cfg.sample_rate_hz) + " Hz is not an integer multiple of "
          "channel_rate_hz " + num(cfg.channel_rate_hz) +
              " Hz; the decimation cascade is integer-factor only.");
    } else {
      const std::uint32_t ratio = cfg.sample_rate_hz / cfg.channel_rate_hz;
      if (ratio < 2 || ratio > 256) {
        add(out, "sample_rate_hz",
            "The decimation ratio sample_rate_hz / channel_rate_hz is " + num(ratio) +
                ", which is outside [2, 256]. sample_rate_hz is " + num(cfg.sample_rate_hz) +
                " Hz and channel_rate_hz is " + num(cfg.channel_rate_hz) + " Hz.");
      } else if (!factorises_into_small_stages(ratio)) {
        const std::uint32_t suggestion =
            nearest_compatible_sample_rate(cfg.channel_rate_hz, cfg.sample_rate_hz);
        std::string msg =
            "The decimation ratio sample_rate_hz / channel_rate_hz is " + num(ratio) +
            ", which has no factorisation into stages of 2 to 16. sample_rate_hz is " +
            num(cfg.sample_rate_hz) + " Hz and channel_rate_hz is " +
            num(cfg.channel_rate_hz) + " Hz.";
        if (suggestion != 0) {
          msg += " The nearest compatible sample_rate_hz is " + num(suggestion) + " Hz.";
        }
        add(out, "sample_rate_hz", std::move(msg));
      }
    }
  }
  {
    const double lhs = std::fabs(static_cast<double>(cfg.offset_tune_hz)) +
                       static_cast<double>(cfg.channel_bw_hz) / 2.0;
    const double rhs = 0.45 * static_cast<double>(cfg.sample_rate_hz);
    if (!(lhs < rhs)) {
      add(out, "offset_tune_hz",
          "offset_tune_hz " + num(cfg.offset_tune_hz) + " Hz with channel_bw_hz " +
              num(cfg.channel_bw_hz) + " Hz needs |offset| + channel_bw/2 = " + num(lhs) +
              " Hz to stay below 0.45 * sample_rate_hz = " + num(rhs) +
              " Hz, so the wanted channel remains inside the usable band.");
    }
  }

  // ---- 7.2 Experiment -----------------------------------------------------
  check_range(out, "duration_s", cfg.duration_s, 0.0, 3600.0, true, false);
  check_range(out, "settle_s", cfg.settle_s, 0.0, 600.0, false, false);
  check_range(out, "start_deg", cfg.start_deg, 0.0, 360.0, false, false);
  check_range(out, "end_deg", cfg.end_deg, 0.0, 360.0, false, false);
  if (!(std::isfinite(cfg.step_deg) && cfg.step_deg > 0.0)) {
    add(out, "step_deg", "step_deg is " + num(cfg.step_deg) + ", which must be finite and above 0.");
  }
  if (std::isfinite(cfg.end_deg) && std::isfinite(cfg.start_deg) && cfg.end_deg < cfg.start_deg) {
    add(out, "end_deg", "end_deg " + num(cfg.end_deg) + " is below start_deg " +
                            num(cfg.start_deg) + ".");
  }
  for (double a : cfg.angles_deg) {
    if (!std::isfinite(a) || a < 0.0 || a > 360.0) {
      add(out, "angles_deg",
          "An entry of angles_deg is " + num(a) + ", which must be finite and within [0, 360].");
      break;
    }
  }
  {
    const auto angles = resolved_angles(cfg);
    if (angles.size() < 2 || angles.size() > 180) {
      add(out, "angles_deg",
          "The resolved angle set has " + std::to_string(angles.size()) +
              " entries, which is outside [2, 180].");
    }
  }
  if (cfg.rounds < 1 || cfg.rounds > 100) {
    add(out, "rounds", "rounds is " + num(cfg.rounds) + ", which is outside [1, 100].");
  }
  if (cfg.angle_reference.size() > 200) {
    add(out, "angle_reference",
        "angle_reference is " + std::to_string(cfg.angle_reference.size()) +
            " characters, above the 200 character limit.");
  }
  if (cfg.setup_note.size() > 500) {
    add(out, "setup_note", "setup_note is " + std::to_string(cfg.setup_note.size()) +
                               " characters, above the 500 character limit.");
  }
  check_range(out, "max_angle_deviation_deg", cfg.max_angle_deviation_deg, 0.0, 180.0, true, false);
  check_range(out, "angle_deviation_warn_deg", cfg.angle_deviation_warn_deg, 0.0, 180.0, true, false);

  // ---- 7.3 Measurement ----------------------------------------------------
  check_range(out, "noise_percentile", cfg.noise_percentile, 0.0, 50.0, true, false);
  if (!(std::isfinite(cfg.probe_percentile) && cfg.probe_percentile > 0.0 &&
        cfg.probe_percentile <= cfg.noise_percentile)) {
    add(out, "probe_percentile",
        "probe_percentile is " + num(cfg.probe_percentile) +
            ", which must lie in (0, noise_percentile]; noise_percentile is " +
            num(cfg.noise_percentile) + ".");
  }
  check_range(out, "open_db", cfg.open_db, 0.0, 60.0, true, false);
  check_range(out, "close_db", cfg.close_db, 0.0, 60.0, false, false);
  if (std::isfinite(cfg.open_db) && std::isfinite(cfg.close_db) && !(cfg.open_db > cfg.close_db)) {
    add(out, "open_db", "open_db " + num(cfg.open_db) + " dB must be above close_db " +
                            num(cfg.close_db) + " dB for the squelch to have hysteresis.");
  }
  check_range(out, "min_event_ms", cfg.min_event_ms, 10.0, 60000.0, false, false);
  check_range(out, "merge_gap_ms", cfg.merge_gap_ms, 0.0, 60000.0, false, false);
  if (cfg.min_valid_events < 1 || cfg.min_valid_events > 1000) {
    add(out, "min_valid_events",
        "min_valid_events is " + num(cfg.min_valid_events) + ", which is outside [1, 1000].");
  }
  check_range(out, "max_active_fraction", cfg.max_active_fraction, 0.0, 1.0, true, true);
  check_range(out, "carrier_prominence_db", cfg.carrier_prominence_db, 0.0, 60.0, true, false);
  check_range(out, "carrier_persistence", cfg.carrier_persistence, 0.0, 1.0, true, false);
  check_range(out, "audio_guard_ms", cfg.audio_guard_ms, 0.0, 5000.0, false, false);
  check_range(out, "min_audio_window_ms", cfg.min_audio_window_ms, 10.0, 60000.0, false, false);
  check_range(out, "max_clipped_fraction", cfg.max_clipped_fraction, 0.0, 1.0, false, true);
  if (cfg.max_retained_events < 4 || cfg.max_retained_events > 500) {
    add(out, "max_retained_events",
        "max_retained_events is " + num(cfg.max_retained_events) + ", which is outside [4, 500].");
  }
  check_range(out, "read_timeout_factor", cfg.read_timeout_factor, 1.0, 10.0, false, false);
  check_range(out, "read_timeout_slack_s", cfg.read_timeout_slack_s, 0.1, 600.0, false, false);

  // ---- 7.4 Reporting thresholds ------------------------------------------
  if (cfg.min_captures_advisory < 2 || cfg.min_captures_advisory > 100) {
    add(out, "min_captures_advisory",
        "min_captures_advisory is " + num(cfg.min_captures_advisory) +
            ", which is outside [2, 100].");
  }
  check_range(out, "min_effect_db", cfg.min_effect_db, 0.0, 30.0, false, false);
  check_range(out, "yield_concordance_ratio", cfg.yield_concordance_ratio, 0.0, 1.0, true, false);
  check_range(out, "noise_drift_warn_db", cfg.noise_drift_warn_db, 0.1, 30.0, false, false);

  // ---- 7.5 Session, source, and output -----------------------------------
  if (cfg.session_root.empty()) {
    add(out, "session_root", "session_root is empty; it must name a directory.");
  }
  if (cfg.label.size() > 4096) {
    add(out, "label", "label is " + std::to_string(cfg.label.size()) +
                          " characters, above the 4096 character argument limit.");
  }
  {
    const bool is_file = cfg.source_spec.rfind("file:", 0) == 0;
    if (cfg.source_spec != "rtlsdr" && cfg.source_spec != "synthetic" && !is_file) {
      add(out, "source_spec",
          "source_spec is \"" + cfg.source_spec +
              "\"; it must be rtlsdr, synthetic, or file:<path.cu8>.");
    } else if (is_file && cfg.source_spec.size() <= 5) {
      add(out, "source_spec", "source_spec is \"" + cfg.source_spec + "\" with no path after file:.");
    }
    if (cfg.non_interactive && cfg.source_spec == "rtlsdr") {
      add(out, "non_interactive",
          "non_interactive requires --source synthetic or --source file:<path.cu8>, because "
          "the manual angle provider cannot position the antenna without prompts.");
    }
  }
  check_range(out, "synthetic_snr_db", cfg.synthetic_snr_db, -20.0, 60.0, false, false);
  check_range(out, "synthetic_duty", cfg.synthetic_duty, 0.0, 1.0, true, true);

  // ---- 7.6 Scan -----------------------------------------------------------
  {
    const double s = static_cast<double>(cfg.scan_start_hz);
    if (s < 24e6 || s > 1.766e9) {
      add(out, "scan_start_hz",
          "scan_start_hz is " + num(cfg.scan_start_hz) +
              " Hz, which is outside the tuner range [24000000, 1766000000].");
    }
  }
  if (cfg.scan_end_hz <= cfg.scan_start_hz) {
    add(out, "scan_end_hz", "scan_end_hz " + num(cfg.scan_end_hz) +
                                " Hz must be above scan_start_hz " + num(cfg.scan_start_hz) + " Hz.");
  }
  if (cfg.scan_channel_hz < 6250 || cfg.scan_channel_hz > 200000) {
    add(out, "scan_channel_hz",
        "scan_channel_hz is " + num(cfg.scan_channel_hz) + " Hz, which is outside [6250, 200000].");
  }
  check_range(out, "scan_dwell_ms", cfg.scan_dwell_ms, 20.0, 10000.0, false, false);
  if (cfg.scan_passes < 1 || cfg.scan_passes > 100) {
    add(out, "scan_passes", "scan_passes is " + num(cfg.scan_passes) + ", which is outside [1, 100].");
  }
  if (static_cast<std::uint64_t>(cfg.scan_dc_exclusion_hz) * 4 > cfg.sample_rate_hz) {
    add(out, "scan_dc_exclusion_hz",
        "scan_dc_exclusion_hz is " + num(cfg.scan_dc_exclusion_hz) +
            " Hz, above sample_rate_hz/4 = " + num(cfg.sample_rate_hz / 4) + " Hz.");
  }
  check_range(out, "scan_usable_fraction", cfg.scan_usable_fraction, 0.0, 1.0, true, false);
  if (cfg.scan_top_n < 1 || cfg.scan_top_n > 200) {
    add(out, "scan_top_n", "scan_top_n is " + num(cfg.scan_top_n) + ", which is outside [1, 200].");
  }
  {
    // Spec section 12.5.1: the band is covered only when a dwell step S exists
    // with 2D <= S <= U - D, and such an S exists exactly when U >= 3D.
    const double usable_half = cfg.scan_usable_fraction * static_cast<double>(cfg.sample_rate_hz) / 2.0;
    const double d = static_cast<double>(cfg.scan_dc_exclusion_hz);
    if (std::isfinite(usable_half) && usable_half < 3.0 * d) {
      const double needed = 6.0 * d / static_cast<double>(cfg.sample_rate_hz);
      add(out, "scan_usable_fraction",
          "scan_usable_fraction " + num(cfg.scan_usable_fraction) + " with sample_rate_hz " +
              num(cfg.sample_rate_hz) + " Hz gives a usable half-span of " + num(usable_half) +
              " Hz, which is below three times scan_dc_exclusion_hz " +
              num(cfg.scan_dc_exclusion_hz) +
              " Hz. No dwell step can then cover the band. Require "
              "scan_usable_fraction * sample_rate_hz >= 6 * scan_dc_exclusion_hz, which needs "
              "scan_usable_fraction of at least " + num(needed) + ".");
    }
  }

  // ---- 7.7 Resource limits ------------------------------------------------
  {
    const ResourceEstimate est = estimate_resources(cfg);
    if (est.planned_visits > kMaxPlannedVisits) {
      add(out, "rounds",
          "rounds " + num(cfg.rounds) + " over " +
              std::to_string(resolved_angles(cfg).size()) + " angles plans " +
              num(est.planned_visits) + " visits, above the limit of " +
              num(kMaxPlannedVisits) + ".");
    }
    if (est.final_json_bytes > kMaxFinalJsonBytes) {
      add(out, "max_retained_events",
          "max_retained_events " + num(cfg.max_retained_events) + " projects a final "
          "session.json of " + num(est.final_json_bytes) + " bytes at the attempt ceiling, "
          "above the 16 MiB limit.");
    }
    if (est.cumulative_bytes > kMaxCumulativeBytes) {
      add(out, "max_retained_events",
          "max_retained_events " + num(cfg.max_retained_events) + " projects a cumulative "
          "rewrite of " + num(est.cumulative_bytes) + " bytes, above the 2 GiB limit.");
    }
    if (est.needs_confirmation && cfg.non_interactive) {
      add(out, "duration_s",
          "The planned capture time is " + num(est.capture_seconds) +
              " s, above the " + num(kMaxUnconfirmedCaptureSeconds) +
              " s threshold that requires an explicit confirmation, and "
              "non_interactive leaves no way to confirm it.");
    }
  }

  return out;
}

std::vector<std::string> config_notices(const Config& cfg) {
  std::vector<std::string> out;
  const auto angles = resolved_angles(cfg);

  if (cfg.angles_deg.empty()) {
    const auto raw = generate_angles(cfg.start_deg, cfg.end_deg, cfg.step_deg);
    if (auto n = endpoint_notice(cfg.start_deg, cfg.end_deg, cfg.step_deg, raw)) {
      out.push_back(*n);
    }
  }
  for (auto& n : opposed_pair_notices(angles)) out.push_back(std::move(n));

  if (cfg.offset_tune_hz == 0) {
    out.push_back(
        "Offset tuning is disabled (offset_tune_hz 0). The receiver's DC spur sits exactly "
        "where the AM carrier lands, so both the signal measurement and the noise-floor "
        "estimate will be contaminated by it.");
  }
  if (cfg.rounds == 1) {
    out.push_back(
        "One round was requested, so each angle is measured once and angle is fully "
        "confounded with time. More rounds make the spread printed beside each score "
        "informative.");
  }
  return out;
}

// ---------------------------------------------------------------------------
// Resource preflight
// ---------------------------------------------------------------------------

ResourceEstimate estimate_resources(const Config& cfg) {
  ResourceEstimate e;
  const auto angles = resolved_angles(cfg);
  const long long visits =
      static_cast<long long>(angles.size()) * static_cast<long long>(std::max(cfg.rounds, 0));
  e.planned_visits = static_cast<int>(std::min<long long>(visits, 1000000));
  e.attempt_ceiling = kAttemptCeiling;
  e.capture_seconds = static_cast<double>(e.planned_visits) * (cfg.duration_s + cfg.settle_s);

  // Spec section 7.7: 1.2 KiB of fixed attempt fields plus 0.2 KiB per retained
  // event object.
  const double bytes = 1.2 * 1024.0 + static_cast<double>(std::max(cfg.max_retained_events, 0)) * 0.2 * 1024.0;
  e.attempt_bytes = static_cast<std::uint64_t>(bytes + 0.5);

  const std::uint64_t a = static_cast<std::uint64_t>(e.attempt_ceiling);
  e.final_json_bytes = e.attempt_bytes * a;
  // The whole canonical document is rewritten on every commit, so the cumulative
  // cost is quadratic in the attempt count: attempt_bytes * A(A+1)/2 (spec 11.1.4).
  e.cumulative_bytes = e.attempt_bytes * (a * (a + 1) / 2);

  e.needs_confirmation = e.capture_seconds > kMaxUnconfirmedCaptureSeconds;
  return e;
}

bool retry_available(int attempts_committed, int remaining_planned_visits, int attempt_ceiling) {
  // Capacity for one attempt per remaining visit is reserved and can never be
  // spent on a retry, so the session can always reach its last visit.
  return attempts_committed + remaining_planned_visits < attempt_ceiling;
}

// ---------------------------------------------------------------------------
// Serialisation
// ---------------------------------------------------------------------------

void to_json(J& j, const Config& c) {
  j = J::object();
  j["device_index"] = c.device_index;
  if (c.center_hz.has_value()) {
    j["center_hz"] = *c.center_hz;
  } else {
    j["center_hz"] = nullptr;
  }
  j["sample_rate_hz"] = c.sample_rate_hz;
  if (c.gain_tenth_db.has_value()) {
    j["gain_tenth_db"] = *c.gain_tenth_db;
  } else {
    j["gain_tenth_db"] = "max";
  }
  j["ppm"] = c.ppm;
  j["offset_tune_hz"] = c.offset_tune_hz;
  j["channel_bw_hz"] = c.channel_bw_hz;
  j["channel_rate_hz"] = c.channel_rate_hz;
  j["bias_tee"] = c.bias_tee;

  j["duration_s"] = c.duration_s;
  j["settle_s"] = c.settle_s;
  j["start_deg"] = c.start_deg;
  j["end_deg"] = c.end_deg;
  j["step_deg"] = c.step_deg;
  j["angles_deg"] = c.angles_deg;
  j["rounds"] = c.rounds;
  j["order"] = to_string(c.order);
  j["seed"] = c.seed;
  j["angle_reference"] = c.angle_reference;
  j["setup_note"] = c.setup_note;
  j["max_angle_deviation_deg"] = c.max_angle_deviation_deg;
  j["angle_deviation_warn_deg"] = c.angle_deviation_warn_deg;

  j["noise_percentile"] = c.noise_percentile;
  j["probe_percentile"] = c.probe_percentile;
  j["open_db"] = c.open_db;
  j["close_db"] = c.close_db;
  j["min_event_ms"] = c.min_event_ms;
  j["merge_gap_ms"] = c.merge_gap_ms;
  j["min_valid_events"] = c.min_valid_events;
  j["max_active_fraction"] = c.max_active_fraction;
  j["carrier_prominence_db"] = c.carrier_prominence_db;
  j["carrier_persistence"] = c.carrier_persistence;
  j["audio_guard_ms"] = c.audio_guard_ms;
  j["min_audio_window_ms"] = c.min_audio_window_ms;
  j["max_clipped_fraction"] = c.max_clipped_fraction;
  j["max_retained_events"] = c.max_retained_events;
  j["read_timeout_factor"] = c.read_timeout_factor;
  j["read_timeout_slack_s"] = c.read_timeout_slack_s;

  j["report_metric"] = to_string(c.report_metric);
  j["min_captures_advisory"] = c.min_captures_advisory;
  j["min_effect_db"] = c.min_effect_db;
  j["yield_concordance_ratio"] = c.yield_concordance_ratio;
  j["noise_drift_warn_db"] = c.noise_drift_warn_db;

  j["session_root"] = c.session_root;
  j["label"] = c.label;
  j["resume_dir"] = c.resume_dir;
  j["source_spec"] = c.source_spec;
  j["synthetic_snr_db"] = c.synthetic_snr_db;
  j["synthetic_duty"] = c.synthetic_duty;
  j["no_color"] = c.no_color;
  j["non_interactive"] = c.non_interactive;

  j["scan_start_hz"] = c.scan_start_hz;
  j["scan_end_hz"] = c.scan_end_hz;
  j["scan_channel_hz"] = c.scan_channel_hz;
  j["scan_dwell_ms"] = c.scan_dwell_ms;
  j["scan_passes"] = c.scan_passes;
  j["scan_dc_exclusion_hz"] = c.scan_dc_exclusion_hz;
  j["scan_usable_fraction"] = c.scan_usable_fraction;
  j["scan_top_n"] = c.scan_top_n;
}

void from_json(const J& j, Config& c) {
  if (!j.is_object()) {
    throw J::type_error::create(302, "configuration must be a JSON object", nullptr);
  }
  for (auto it = j.begin(); it != j.end(); ++it) {
    if (find_field(it.key()) == nullptr) {
      throw J::other_error::create(
          501, "unknown configuration key \"" + it.key() + "\"", nullptr);
    }
  }

  auto get = [&j](const char* key, auto& dest) {
    if (j.contains(key) && !j.at(key).is_null()) j.at(key).get_to(dest);
  };

  get("device_index", c.device_index);
  if (j.contains("center_hz")) {
    if (j.at("center_hz").is_null()) {
      c.center_hz.reset();
    } else {
      c.center_hz = j.at("center_hz").get<std::uint32_t>();
    }
  }
  get("sample_rate_hz", c.sample_rate_hz);
  if (j.contains("gain_tenth_db")) {
    const J& g = j.at("gain_tenth_db");
    if (g.is_string()) {
      if (g.get<std::string>() != "max") {
        throw J::other_error::create(
            501, "gain_tenth_db must be a number of tenths of a dB or the string \"max\"",
            nullptr);
      }
      c.gain_tenth_db.reset();
    } else if (g.is_null()) {
      c.gain_tenth_db.reset();
    } else {
      c.gain_tenth_db = g.get<int>();
    }
  }
  get("ppm", c.ppm);
  get("offset_tune_hz", c.offset_tune_hz);
  get("channel_bw_hz", c.channel_bw_hz);
  get("channel_rate_hz", c.channel_rate_hz);
  get("bias_tee", c.bias_tee);

  get("duration_s", c.duration_s);
  get("settle_s", c.settle_s);
  get("start_deg", c.start_deg);
  get("end_deg", c.end_deg);
  get("step_deg", c.step_deg);
  get("angles_deg", c.angles_deg);
  get("rounds", c.rounds);
  if (j.contains("order") && !j.at("order").is_null()) {
    const std::string s = j.at("order").get<std::string>();
    const auto parsed = parse_visit_order(s);
    if (!parsed.has_value()) {
      throw J::other_error::create(501, "order must be forward, reverse, alternating, or random; got \"" + s + "\"", nullptr);
    }
    c.order = *parsed;
  }
  get("seed", c.seed);
  get("angle_reference", c.angle_reference);
  get("setup_note", c.setup_note);
  get("max_angle_deviation_deg", c.max_angle_deviation_deg);
  get("angle_deviation_warn_deg", c.angle_deviation_warn_deg);

  get("noise_percentile", c.noise_percentile);
  get("probe_percentile", c.probe_percentile);
  get("open_db", c.open_db);
  get("close_db", c.close_db);
  get("min_event_ms", c.min_event_ms);
  get("merge_gap_ms", c.merge_gap_ms);
  get("min_valid_events", c.min_valid_events);
  get("max_active_fraction", c.max_active_fraction);
  get("carrier_prominence_db", c.carrier_prominence_db);
  get("carrier_persistence", c.carrier_persistence);
  get("audio_guard_ms", c.audio_guard_ms);
  get("min_audio_window_ms", c.min_audio_window_ms);
  get("max_clipped_fraction", c.max_clipped_fraction);
  get("max_retained_events", c.max_retained_events);
  get("read_timeout_factor", c.read_timeout_factor);
  get("read_timeout_slack_s", c.read_timeout_slack_s);

  if (j.contains("report_metric") && !j.at("report_metric").is_null()) {
    const std::string s = j.at("report_metric").get<std::string>();
    const auto parsed = parse_metric(s);
    if (!parsed.has_value()) {
      throw J::other_error::create(501, "report_metric must be channel or audio; got \"" + s + "\"", nullptr);
    }
    c.report_metric = *parsed;
  }
  get("min_captures_advisory", c.min_captures_advisory);
  get("min_effect_db", c.min_effect_db);
  get("yield_concordance_ratio", c.yield_concordance_ratio);
  get("noise_drift_warn_db", c.noise_drift_warn_db);

  get("session_root", c.session_root);
  get("label", c.label);
  get("resume_dir", c.resume_dir);
  get("source_spec", c.source_spec);
  get("synthetic_snr_db", c.synthetic_snr_db);
  get("synthetic_duty", c.synthetic_duty);
  get("no_color", c.no_color);
  get("non_interactive", c.non_interactive);

  get("scan_start_hz", c.scan_start_hz);
  get("scan_end_hz", c.scan_end_hz);
  get("scan_channel_hz", c.scan_channel_hz);
  get("scan_dwell_ms", c.scan_dwell_ms);
  get("scan_passes", c.scan_passes);
  get("scan_dc_exclusion_hz", c.scan_dc_exclusion_hz);
  get("scan_usable_fraction", c.scan_usable_fraction);
  get("scan_top_n", c.scan_top_n);
}

std::optional<double> parse_frequency(std::string_view text) {
  if (text.empty()) return std::nullopt;
  std::string s(text);
  double scale = 1.0;
  const char last = s.back();
  if (last == 'M' || last == 'm') {
    scale = 1e6;
    s.pop_back();
  } else if (last == 'k' || last == 'K') {
    scale = 1e3;
    s.pop_back();
  } else if (last == 'G' || last == 'g') {
    scale = 1e9;
    s.pop_back();
  } else if (last == 'H' || last == 'h' || last == 'z' || last == 'Z') {
    return std::nullopt;  // "118Hz" style suffixes are not accepted; the unit is implied
  }
  if (s.empty()) return std::nullopt;

  char* end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  if (end == nullptr || *end != '\0') return std::nullopt;
  if (!std::isfinite(v)) return std::nullopt;
  return v * scale;
}

}  // namespace rtlangle
