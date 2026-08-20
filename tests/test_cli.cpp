// WP11 — the command line. Spec sections 7.9 and 12.1.
//
// Spec section 7 is the authoritative field list and this file does not restate
// it: the completeness tests enumerate the keys to_json(Config) produces and
// require each to have a flag, a bound, a resume classification, and a command
// scope for all four commands. A field added to Config without one of them
// fails here rather than silently becoming unreachable.

#include <doctest/doctest.h>

#include "app/cli_parser.h"
#include "persist/path_safety.h"
#include "tests/support/temp_dir.h"

#include <nlohmann/json.hpp>

#include <set>
#include <string>
#include <vector>

using namespace rtlangle;
using namespace rtlangle::app;
using rtlangle::test::TempDir;
using rtlangle::test::write_whole;

namespace {

ParseResult parse(std::vector<std::string> arguments) { return parse_arguments(arguments); }

bool has_error_for(const ParseResult& r, std::string_view field) {
  for (const ValidationError& e : r.errors) {
    if (e.field == field) return true;
  }
  return false;
}

std::set<std::string> serialised_keys() {
  nlohmann::json j;
  to_json(j, Config{});
  std::set<std::string> keys;
  for (auto it = j.begin(); it != j.end(); ++it) keys.insert(it.key());
  return keys;
}

// A legal value for each field type, so every flag can be exercised.
std::string sample_value(const FieldSpec& f) {
  if (f.key == "order") return "forward";
  if (f.key == "report_metric") return "audio";
  if (f.key == "gain_tenth_db") return "max";
  if (f.key == "source_spec") return "synthetic";
  if (f.key == "angles_deg") return "0,45,90";
  if (f.key == "center_hz") return "118.35M";
  if (f.key == "sample_rate_hz") return "1024000";
  if (f.key == "channel_rate_hz") return "32000";
  if (f.key == "channel_bw_hz") return "8000";
  switch (f.type) {
    case FieldType::Bool: return "";
    case FieldType::Int: return "3";
    case FieldType::UInt32:
    case FieldType::OptionalUInt32: return "120000000";
    case FieldType::UInt64: return "1234567";
    case FieldType::Int64: return "250000";
    case FieldType::Double: return "1.5";
    case FieldType::String:
    case FieldType::Path: return "value";
    case FieldType::GainOrMax: return "300";
    case FieldType::DoubleList: return "0,45";
    case FieldType::OrderEnum: return "forward";
    case FieldType::MetricEnum: return "channel";
  }
  return "1";
}

}  // namespace

TEST_SUITE("cli") {

TEST_CASE("every Config field has a flag the parser accepts") {
  const auto keys = serialised_keys();
  for (const FieldSpec& f : field_registry()) {
    CHECK(keys.count(std::string(f.key)) == 1);

    std::vector<std::string> arguments = {"run", std::string(f.flag)};
    const std::string value = sample_value(f);
    if (!value.empty()) arguments.push_back(value);

    const ParseResult r = parse(arguments);
    for (const ValidationError& e : r.errors) {
      MESSAGE("unexpected error for " << f.flag << ": " << e.message);
    }
    CHECK_MESSAGE(r.errors.empty(), f.flag);
    CHECK_MESSAGE(r.explicitly_set.count(std::string(f.key)) == 1, f.flag);
  }
}

TEST_CASE("the flags the spec names by hand all parse") {
  for (const char* flag : {"--resume", "--report-metric", "--min-captures", "--min-effect-db",
                           "--yield-concordance-ratio", "--max-retained-events",
                           "--angle-reference", "--setup-note", "--probe-percentile",
                           "--read-timeout-factor", "--read-timeout-slack",
                           "--scan-usable-fraction", "--scan-start", "--scan-end",
                           "--scan-channel", "--scan-dwell", "--scan-passes",
                           "--scan-dc-exclusion", "--scan-top"}) {
    const FieldSpec* f = nullptr;
    for (const FieldSpec& candidate : field_registry()) {
      if (candidate.flag == flag) f = &candidate;
    }
    REQUIRE_MESSAGE(f != nullptr, flag);
    const ParseResult r = parse({"run", flag, sample_value(*f)});
    CHECK_MESSAGE(r.errors.empty(), flag);
  }
}

TEST_CASE("flags that no longer exist are unknown-flag errors") {
  // Reintroducing any of these requires deleting an assertion.
  for (const char* removed : {"--agc", "--allow-agc", "--primary-metric", "--family-alpha",
                              "--save-iq", "--selected-metric"}) {
    const ParseResult r = parse({"run", removed, "channel"});
    bool named = false;
    for (const ValidationError& e : r.errors) {
      if (e.message.find(removed) != std::string::npos) named = true;
    }
    CHECK_MESSAGE(named, removed);
  }
}

TEST_CASE("precedence runs defaults, then the configuration file, then the flags") {
  TempDir temp;
  const auto path = temp.child("rtlangle.json");
  write_whole(path, R"({"rounds": 7, "duration_s": 30.0, "label": "from-file"})");

  const ParseResult r =
      parse({"run", "--config", path.string(), "--rounds", "9", "--freq", "118.35M"});
  REQUIRE(r.errors.empty());
  CHECK(r.config.rounds == 9);           // set in both: the flag wins
  CHECK(r.config.duration_s == doctest::Approx(30.0));   // file only
  CHECK(r.config.label == "from-file");
  CHECK(r.config.settle_s == doctest::Approx(3.0));      // neither: the default

  // Every key either source set counts as explicitly set.
  CHECK(r.explicitly_set.count("rounds") == 1);
  CHECK(r.explicitly_set.count("duration_s") == 1);
  CHECK(r.explicitly_set.count("settle_s") == 0);
}

TEST_CASE("a configuration file that is missing, oversized, or malformed is named") {
  TempDir temp;
  const ParseResult missing = parse({"run", "--config", temp.child("absent.json").string()});
  CHECK(has_error_for(missing, "--config"));

  const auto big = temp.child("big.json");
  write_whole(big, std::string(kMaxConfigFileBytes + 16, 'x'));
  const ParseResult oversized = parse({"run", "--config", big.string()});
  CHECK(has_error_for(oversized, "--config"));

  const auto bad = temp.child("bad.json");
  write_whole(bad, "{not json");
  const ParseResult malformed = parse({"run", "--config", bad.string()});
  CHECK(has_error_for(malformed, "--config"));

  const auto unknown = temp.child("unknown.json");
  write_whole(unknown, R"({"allow_agc": true})");
  const ParseResult unknown_key = parse({"run", "--config", unknown.string()});
  CHECK(has_error_for(unknown_key, "allow_agc"));
}

TEST_CASE("command scope is enforced over the whole field list") {
  const auto keys = serialised_keys();
  for (const auto& key : keys) {
    const FieldSpec* f = find_field(key);
    REQUIRE(f != nullptr);
    for (Command command : {Command::Run, Command::Scan, Command::Devices, Command::Report}) {
      Config cfg;
      cfg.center_hz = 118350000;
      const auto errors = validate_for_command(cfg, {key}, command);
      const FieldScope scope = field_scope(key, command);
      if (scope == FieldScope::Refused) {
        REQUIRE_MESSAGE(!errors.empty(), key << " on " << to_string(command));
        bool named_both = false;
        for (const ValidationError& e : errors) {
          if (e.message.find(std::string(f->flag)) != std::string::npos &&
              e.message.find(std::string(to_string(command))) != std::string::npos) {
            named_both = true;
          }
        }
        CHECK_MESSAGE(named_both, key << " on " << to_string(command));
      } else {
        for (const ValidationError& e : errors) {
          CHECK_MESSAGE(e.field != key, key << " on " << to_string(command));
        }
      }
    }
  }
}

TEST_CASE("the command-scope spot checks the spec names") {
  Config cfg;
  cfg.center_hz = 118350000;

  CHECK_FALSE(validate_for_command(cfg, {"gain_tenth_db"}, Command::Report).empty());
  CHECK_FALSE(validate_for_command(cfg, {"scan_dwell_ms"}, Command::Run).empty());
  CHECK_FALSE(validate_for_command(cfg, {"rounds"}, Command::Scan).empty());

  Config no_freq;
  const auto missing = validate_for_command(no_freq, {}, Command::Run);
  REQUIRE(missing.size() == 1);
  CHECK(missing[0].message.find("--freq") != std::string::npos);

  Config resuming;
  resuming.resume_dir = "sessions/20260819-143000";
  CHECK(validate_for_command(resuming, {"resume_dir"}, Command::Run).empty());

  // A field left at its default is never a scope error, even for a command that
  // refuses it.
  CHECK(validate_for_command(cfg, {}, Command::Report).empty());
  CHECK(validate_for_command(cfg, {}, Command::Devices).empty());
}

TEST_CASE("report accepts the whole of the reporting-threshold section") {
  Config cfg;
  const std::set<std::string> section_74 = {"report_metric", "min_captures_advisory",
                                            "min_effect_db", "yield_concordance_ratio",
                                            "noise_drift_warn_db"};
  CHECK(validate_for_command(cfg, section_74, Command::Report).empty());
}

TEST_CASE("non-interactive with the device source is a configuration error") {
  const ParseResult device =
      parse({"run", "--freq", "118.35M", "--non-interactive"});
  REQUIRE(device.errors.empty());
  const auto errors = validate(device.config);
  bool named = false;
  for (const ValidationError& e : errors) {
    if (e.field == "non_interactive") named = true;
  }
  CHECK(named);

  const ParseResult synthetic =
      parse({"run", "--freq", "118.35M", "--non-interactive", "--source", "synthetic"});
  REQUIRE(synthetic.errors.empty());
  for (const ValidationError& e : validate(synthetic.config)) {
    CHECK(e.field != "non_interactive");
  }
}

TEST_CASE("frequency literals, angle lists, and gain values parse") {
  const ParseResult r = parse({"run", "--freq", "118.1M", "--scan-start", "121500k",
                               "--angles", "0,30,60,90", "--gain", "max"});
  REQUIRE(r.errors.empty());
  CHECK(r.config.center_hz.value() == 118100000);
  CHECK(r.config.scan_start_hz == 121500000);
  REQUIRE(r.config.angles_deg.size() == 4);
  CHECK(r.config.angles_deg[3] == doctest::Approx(90.0));
  CHECK_FALSE(r.config.gain_tenth_db.has_value());

  const ParseResult numeric = parse({"run", "--gain", "300"});
  REQUIRE(numeric.errors.empty());
  CHECK(numeric.config.gain_tenth_db.value() == 300);
}

TEST_CASE("a malformed value is an error naming the flag") {
  CHECK(has_error_for(parse({"run", "--rounds", "many"}), "rounds"));
  CHECK(has_error_for(parse({"run", "--duration", "soon"}), "duration_s"));
  CHECK(has_error_for(parse({"run", "--angles", "north"}), "angles_deg"));
  CHECK(has_error_for(parse({"run", "--order", "sideways"}), "config"));
  CHECK(has_error_for(parse({"run", "--rounds"}), "rounds"));   // a flag with no value
}

TEST_CASE("boolean flags need no value, and both spellings of a value work") {
  const ParseResult bare = parse({"run", "--no-color", "--bias-tee", "--freq", "118.35M"});
  REQUIRE(bare.errors.empty());
  CHECK(bare.config.no_color);
  CHECK(bare.config.bias_tee);

  const ParseResult inline_value = parse({"run", "--rounds=5", "--freq=118.35M"});
  REQUIRE(inline_value.errors.empty());
  CHECK(inline_value.config.rounds == 5);
  CHECK(inline_value.config.center_hz.value() == 118350000);
}

TEST_CASE("the command and a positional argument are recognised") {
  const ParseResult report = parse({"report", "sessions/20260819-143000-airband"});
  CHECK(report.command == "report");
  CHECK(report.positional == "sessions/20260819-143000-airband");
  CHECK(report.errors.empty());

  const ParseResult help = parse({"--help"});
  CHECK(help.help);
  const ParseResult version = parse({"--version"});
  CHECK(version.version);

  const ParseResult unknown = parse({"run", "--nonsense"});
  CHECK_FALSE(unknown.errors.empty());
}

TEST_CASE("the probe percentile follows the noise percentile unless it was set") {
  const ParseResult derived = parse({"run", "--noise-percentile", "3"});
  REQUIRE(derived.errors.empty());
  CHECK(derived.config.probe_percentile == doctest::Approx(3.0));

  const ParseResult explicit_probe =
      parse({"run", "--noise-percentile", "3", "--probe-percentile", "2"});
  REQUIRE(explicit_probe.errors.empty());
  CHECK(explicit_probe.config.probe_percentile == doctest::Approx(2.0));
}

TEST_CASE("the usage text lists every command and every flag") {
  const std::string usage = usage_text();
  for (const char* command : {"run", "scan", "devices", "report"}) {
    CHECK(usage.find(command) != std::string::npos);
  }
  for (const FieldSpec& f : field_registry()) {
    CHECK_MESSAGE(usage.find(std::string(f.flag)) != std::string::npos, f.flag);
  }
  CHECK(usage.find("--config") != std::string::npos);
}

}  // TEST_SUITE
