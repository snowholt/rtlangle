#include "app/setup_menu.h"

#include "app/cli_parser.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace rtlangle::app {
namespace {

constexpr std::array<VisitOrder, 4> kOrders = {VisitOrder::Forward, VisitOrder::Reverse,
                                               VisitOrder::Alternating, VisitOrder::Random};

// Plain formatting: 60 prints as 60, 12.5 as 12.5. The report has its own
// formatter for the numbers it publishes; this one exists only so a menu row
// does not read 12.500000.
std::string num(double v) {
  std::ostringstream os;
  os << v;
  return os.str();
}

std::string megahertz(std::uint32_t hz) {
  std::ostringstream os;
  os << std::fixed << std::setprecision(3) << static_cast<double>(hz) / 1e6 << " MHz";
  return os.str();
}

std::string or_none(const std::string& s) { return s.empty() ? "(none)" : s; }

std::string describe_angles(const Config& cfg) {
  const std::size_t count = resolved_angles(cfg).size();
  std::ostringstream os;
  if (!cfg.angles_deg.empty()) {
    // A list, shown as a list. Saying "step" here would describe a range the
    // list is overriding.
    constexpr std::size_t kShown = 6;
    for (std::size_t i = 0; i < cfg.angles_deg.size() && i < kShown; ++i) {
      if (i > 0) os << ", ";
      os << num(cfg.angles_deg[i]);
    }
    if (cfg.angles_deg.size() > kShown) os << ", ...";
  } else {
    os << num(cfg.start_deg) << " to " << num(cfg.end_deg) << " step " << num(cfg.step_deg);
  }
  os << "  (" << count << (count == 1 ? " angle)" : " angles)");
  return os.str();
}

std::string describe_gain(const Config& cfg) {
  if (!cfg.gain_tenth_db.has_value()) return "max";
  std::ostringstream os;
  os << std::fixed << std::setprecision(1) << static_cast<double>(*cfg.gain_tenth_db) / 10.0
     << " dB";
  return os.str();
}

// The flag an operator would use for a field, for a message that has to be
// short enough to sit in a menu row.
std::string flag_for(const std::string& field) {
  const FieldSpec* f = find_field(field);
  return f != nullptr ? std::string(f->flag) : field;
}

std::vector<ValidationError> blocking_errors(const Config& cfg,
                                             const std::set<std::string>& explicitly_set) {
  std::vector<ValidationError> errors = validate_for_command(cfg, explicitly_set, Command::Run);
  const auto bounds = validate(cfg);
  errors.insert(errors.end(), bounds.begin(), bounds.end());
  return errors;
}

std::string describe_start(const Config& cfg, const std::set<std::string>& explicitly_set) {
  const auto errors = blocking_errors(cfg, explicitly_set);
  if (errors.empty()) return "ready";
  std::ostringstream os;
  os << "blocked by " << flag_for(errors.front().field);
  if (errors.size() > 1) os << " and " << (errors.size() - 1) << " more";
  return os.str();
}

// Splits `--flag value` or `--flag=value` the way the parser splits an argument
// pair, so the free-form row accepts exactly what the command line accepts.
void split_option(const std::string& text, std::string& flag, std::string& value) {
  std::string trimmed = text;
  const auto first = trimmed.find_first_not_of(" \t");
  const auto last = trimmed.find_last_not_of(" \t");
  trimmed = first == std::string::npos ? "" : trimmed.substr(first, last - first + 1);

  const auto equals = trimmed.find('=');
  const auto space = trimmed.find_first_of(" \t");
  const auto at = std::min(equals, space);
  if (at == std::string::npos) {
    flag = trimmed;
    value = "";
    return;
  }
  flag = trimmed.substr(0, at);
  value = trimmed.substr(at + 1);
  const auto begin = value.find_first_not_of(" \t");
  value = begin == std::string::npos ? "" : value.substr(begin);
}

void report(ui::ITerminalUi& terminal, const std::vector<ValidationError>& errors) {
  for (const ValidationError& e : errors) terminal.error(e.message);
}

// A range typed over an explicit list has to clear the list: `angles_deg`
// overrides the range wherever it is non-empty (spec section 4.3), so leaving
// it in place would make the operator's edit silently do nothing.
void clear_angle_list(Config& cfg, std::set<std::string>& explicitly_set) {
  cfg.angles_deg.clear();
  explicitly_set.erase("angles_deg");
}

// `start:step:end`, the form spec section 4.3 uses, or a comma-separated list.
bool edit_angles(Config& cfg, std::set<std::string>& explicitly_set, const std::string& answer,
                 ui::ITerminalUi& terminal) {
  if (answer.find(',') != std::string::npos) {
    const auto errors = apply_option(cfg, explicitly_set, "--angles", answer);
    report(terminal, errors);
    return errors.empty();
  }

  std::vector<std::string> parts;
  std::istringstream stream(answer);
  std::string token;
  while (std::getline(stream, token, ':')) parts.push_back(token);
  if (parts.size() != 3) {
    terminal.error("Angles are either start:step:end, for example 0:15:90, or a "
                   "comma-separated list, for example 0,45,90.");
    return false;
  }

  // Applied to a copy so a bad third value cannot leave the first two behind.
  Config                candidate = cfg;
  std::set<std::string> keys = explicitly_set;
  clear_angle_list(candidate, keys);
  const std::array<std::string_view, 3> flags = {"--start-deg", "--step-deg", "--end-deg"};
  for (std::size_t i = 0; i < flags.size(); ++i) {
    const auto errors = apply_option(candidate, keys, flags[i], parts[i]);
    if (!errors.empty()) {
      report(terminal, errors);
      return false;
    }
  }
  cfg = std::move(candidate);
  explicitly_set = std::move(keys);
  return true;
}

// The current value, shown in the prompt rather than offered as its default:
// the default is what a blank line returns, and a blank line here means "leave
// it alone", not "set it again". Offering it would record an untouched field as
// one the operator chose, which the resume classification would then treat as
// an override.
std::string now(const std::vector<ui::MenuItem>& rows, int index) {
  const std::string& detail = rows.at(static_cast<std::size_t>(index)).detail;
  // A detail that is already parenthesised says "(none)" or "(not set)"; the
  // word "now" in front of it would read as "(now (not set))".
  if (!detail.empty() && detail.front() == '(') return "  " + detail;
  return "  (now " + detail + ")";
}

// One named row: the flag it edits and the prompt that asks for its value.
struct TextRow {
  std::string_view flag;
  std::string_view prompt;
};

std::optional<TextRow> text_row(SetupRow row) {
  switch (row) {
    case SetupRow::Frequency:
      return TextRow{"--freq", "Centre frequency, for example 118.35M"};
    case SetupRow::Rounds:
      return TextRow{"--rounds", "Rounds, one visit to every angle each"};
    case SetupRow::Duration:
      return TextRow{"--duration", "Capture duration in seconds"};
    case SetupRow::Settle:
      return TextRow{"--settle", "Settling delay after positioning, in seconds"};
    case SetupRow::Gain:
      return TextRow{"--gain", "Gain in tenths of a dB, or max"};
    case SetupRow::Device:
      return TextRow{"--device", "Device index"};
    case SetupRow::Source:
      return TextRow{"--source", "Source: rtlsdr, synthetic, or file:<path.cu8>"};
    case SetupRow::Label:
      return TextRow{"--label", "Session label"};
    case SetupRow::AngleReference:
      return TextRow{"--angle-reference", "What 0 degrees points at"};
    default:
      return std::nullopt;
  }
}

}  // namespace

std::vector<ui::MenuItem> setup_rows(const Config& cfg,
                                     const std::set<std::string>& explicitly_set) {
  std::vector<ui::MenuItem> rows;
  rows.reserve(kSetupRowCount);
  rows.push_back({"Centre frequency",
                  cfg.center_hz.has_value() ? megahertz(*cfg.center_hz) : "(not set)"});
  rows.push_back({"Angles", describe_angles(cfg)});
  rows.push_back({"Rounds", std::to_string(cfg.rounds)});
  rows.push_back({"Visit order", std::string(to_string(cfg.order))});
  rows.push_back({"Capture duration", num(cfg.duration_s) + " s"});
  rows.push_back({"Settle delay", num(cfg.settle_s) + " s"});
  rows.push_back({"Gain", describe_gain(cfg)});
  rows.push_back({"Device index", std::to_string(cfg.device_index)});
  rows.push_back({"Source", or_none(cfg.source_spec)});
  rows.push_back({"Session label", or_none(cfg.label)});
  rows.push_back({"Angle reference", or_none(cfg.angle_reference)});
  rows.push_back({"Other option...", "type any flag"});
  rows.push_back({"Start", describe_start(cfg, explicitly_set)});
  rows.push_back({"Back", "return without starting"});
  return rows;
}

std::optional<Config> setup_experiment(Config working, std::set<std::string>& explicitly_set,
                                       ui::ITerminalUi& terminal) {
  std::set<std::string> keys = explicitly_set;

  // A flag the run command refuses cannot be unset from inside this screen —
  // every row sets a value and none clears one — so inheriting one from the
  // command line would block Start with no way out but killing the process. It
  // is reported once and then dropped. Saying so and continuing is the middle
  // ground between the old menu, which ignored it silently, and a dead end.
  std::vector<std::string> refused;
  for (const std::string& key : keys) {
    if (field_scope(key, Command::Run) == FieldScope::Refused) refused.push_back(key);
  }
  for (const std::string& key : refused) {
    terminal.warn("The flag " + flag_for(key) +
                  " is not accepted by the run command, so it is left out of this "
                  "experiment.");
    keys.erase(key);
  }

  // The cursor stays where the operator left it. Returning to the first row
  // after every edit would make setting several options in a row a climb back
  // down the list each time.
  int cursor = 0;

  for (;;) {
    const auto rows = setup_rows(working, keys);
    const int choice = terminal.menu("Set up experiment", rows, cursor);
    if (choice < 0) return std::nullopt;
    cursor = choice;
    const auto row = static_cast<SetupRow>(choice);

    if (row == SetupRow::Back) return std::nullopt;

    if (row == SetupRow::Start) {
      const auto errors = blocking_errors(working, keys);
      if (!errors.empty()) {
        report(terminal, errors);
        continue;
      }
      for (const std::string& notice : config_notices(working)) terminal.info(notice);
      explicitly_set = std::move(keys);
      return working;
    }

    if (row == SetupRow::Order) {
      std::vector<ui::MenuItem> items;
      items.reserve(kOrders.size());
      for (const VisitOrder order : kOrders) items.push_back({std::string(to_string(order)), ""});
      const int chosen = terminal.menu("Visit order", items, 0);
      if (chosen < 0) continue;
      report(terminal, apply_option(working, keys, "--order",
                                    to_string(kOrders[static_cast<std::size_t>(chosen)])));
      continue;
    }

    if (row == SetupRow::Angles) {
      const auto answer = terminal.prompt_line(
          "Angles as start:step:end, or a comma-separated list" + now(rows, choice), "");
      if (!answer.has_value() || answer->empty()) continue;
      (void)edit_angles(working, keys, *answer, terminal);
      continue;
    }

    if (row == SetupRow::OtherOption) {
      const auto answer =
          terminal.prompt_line("Option, for example --noise-percentile 25", "");
      if (!answer.has_value() || answer->empty()) continue;
      std::string flag;
      std::string value;
      split_option(*answer, flag, value);
      report(terminal, apply_option(working, keys, flag, value));
      continue;
    }

    const auto text = text_row(row);
    if (!text.has_value()) continue;
    const auto answer = terminal.prompt_line(std::string(text->prompt) + now(rows, choice), "");
    if (!answer.has_value() || answer->empty()) continue;
    report(terminal, apply_option(working, keys, text->flag, *answer));
  }
}

}  // namespace rtlangle::app
