#include "app/cli_parser.h"

#include "core/version.h"
#include "persist/path_safety.h"

#include <algorithm>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <sstream>

namespace rtlangle::app {
namespace {

using J = nlohmann::json;

const FieldSpec* field_for_flag(std::string_view flag) {
  for (const FieldSpec& f : field_registry()) {
    if (f.flag == flag) return &f;
  }
  return nullptr;
}

bool takes_value(const FieldSpec& f) { return f.type != FieldType::Bool; }

// Converts one flag argument into the JSON shape the Config deserialiser
// already validates, so the parser has one type-checking path rather than a
// second one that could drift from it.
bool to_json_value(const FieldSpec& f, std::string_view text, J& out, std::string& error) {
  error.clear();
  const std::string value(text);

  auto parse_integer = [&](long long& dest) {
    char* end = nullptr;
    const long long v = std::strtoll(value.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || value.empty()) {
      error = "\"" + value + "\" is not an integer.";
      return false;
    }
    dest = v;
    return true;
  };

  switch (f.type) {
    case FieldType::Bool: {
      if (value.empty() || value == "true" || value == "1") {
        out = true;
      } else if (value == "false" || value == "0") {
        out = false;
      } else {
        error = "\"" + value + "\" is not a true or false value.";
        return false;
      }
      return true;
    }
    case FieldType::Int: {
      long long v = 0;
      if (!parse_integer(v)) return false;
      out = static_cast<int>(v);
      return true;
    }
    case FieldType::UInt32:
    case FieldType::OptionalUInt32: {
      // Frequency and rate literals: 118.1M, 121500k, or bare hertz.
      const auto hz = parse_frequency(value);
      if (!hz.has_value() || *hz < 0.0 || *hz > 4.294967295e9) {
        error = "\"" + value + "\" is not a frequency or a positive whole number.";
        return false;
      }
      out = static_cast<std::uint32_t>(*hz + 0.5);
      return true;
    }
    case FieldType::UInt64: {
      char* end = nullptr;
      const unsigned long long v = std::strtoull(value.c_str(), &end, 10);
      if (end == nullptr || *end != '\0' || value.empty()) {
        error = "\"" + value + "\" is not a whole number.";
        return false;
      }
      out = static_cast<std::uint64_t>(v);
      return true;
    }
    case FieldType::Int64: {
      const auto hz = parse_frequency(value);
      if (!hz.has_value()) {
        error = "\"" + value + "\" is not a frequency or a whole number.";
        return false;
      }
      out = static_cast<std::int64_t>(*hz < 0 ? *hz - 0.5 : *hz + 0.5);
      return true;
    }
    case FieldType::Double: {
      char* end = nullptr;
      const double v = std::strtod(value.c_str(), &end);
      if (end == nullptr || *end != '\0' || value.empty()) {
        error = "\"" + value + "\" is not a number.";
        return false;
      }
      out = v;
      return true;
    }
    case FieldType::String:
    case FieldType::Path:
    case FieldType::OrderEnum:
    case FieldType::MetricEnum: {
      if (value.size() > kMaxCliStringBytes) {
        error = "the value is " + std::to_string(value.size()) +
                " characters, above the " + std::to_string(kMaxCliStringBytes) +
                " character limit for a single argument.";
        return false;
      }
      out = value;
      return true;
    }
    case FieldType::GainOrMax: {
      if (value == "max") {
        out = "max";
        return true;
      }
      long long v = 0;
      if (!parse_integer(v)) {
        error = "\"" + value + "\" is neither a gain in tenths of a dB nor \"max\".";
        return false;
      }
      out = static_cast<int>(v);
      return true;
    }
    case FieldType::DoubleList: {
      J list = J::array();
      std::istringstream stream(value);
      std::string token;
      while (std::getline(stream, token, ',')) {
        if (token.empty()) continue;
        char* end = nullptr;
        const double v = std::strtod(token.c_str(), &end);
        if (end == nullptr || *end != '\0') {
          error = "\"" + token + "\" in the angle list is not a number.";
          return false;
        }
        list.push_back(v);
      }
      if (list.empty()) {
        error = "the angle list is empty.";
        return false;
      }
      out = std::move(list);
      return true;
    }
  }
  error = "unsupported field type.";
  return false;
}

}  // namespace

std::string usage_text() {
  std::ostringstream os;
  os << kToolVersion << "\n\n";
  os << "Usage:\n";
  os << "  rtlangle run [options]         run an angle experiment (interactive by default)\n";
  os << "  rtlangle scan [options]        find active airband channels\n";
  os << "  rtlangle devices               enumerate devices, gain table, tuner type\n";
  os << "  rtlangle report <session-dir>  re-render a report from a stored session\n";
  os << "  rtlangle --help | --version\n\n";
  os << "Options (a flag a command does not use is a usage error naming both):\n";
  for (const FieldSpec& f : field_registry()) {
    os << "  " << f.flag;
    if (takes_value(f)) os << " <value>";
    os << "\n";
  }
  os << "  --config <file.json>           read options from a file, below the flags\n";
  return os.str();
}

ParseResult parse_arguments(const std::vector<std::string>& arguments) {
  ParseResult result;
  J overlay = J::object();
  std::string config_path;

  std::size_t index = 0;
  if (!arguments.empty() && !arguments[0].empty() && arguments[0][0] != '-') {
    result.command = arguments[0];
    index = 1;
  }

  for (; index < arguments.size(); ++index) {
    const std::string& argument = arguments[index];

    if (argument == "--help" || argument == "-h") {
      result.help = true;
      continue;
    }
    if (argument == "--version") {
      result.version = true;
      continue;
    }
    if (argument == "--config") {
      if (index + 1 >= arguments.size()) {
        result.errors.push_back({"--config", "--config needs a file path."});
        return result;
      }
      config_path = arguments[++index];
      continue;
    }
    if (!argument.empty() && argument[0] != '-') {
      if (result.positional.empty()) {
        result.positional = argument;
      } else {
        result.errors.push_back(
            {"argument", "unexpected argument \"" + argument + "\"."});
      }
      continue;
    }

    // A flag and, where it takes one, its value. Both --flag value and
    // --flag=value are accepted.
    std::string name = argument;
    std::string inline_value;
    bool has_inline = false;
    if (const auto equals = argument.find('='); equals != std::string::npos) {
      name = argument.substr(0, equals);
      inline_value = argument.substr(equals + 1);
      has_inline = true;
    }

    const FieldSpec* f = field_for_flag(name);
    if (f == nullptr) {
      result.errors.push_back(
          {name, "unknown flag \"" + name + "\". Run rtlangle --help for the flags this "
                 "build accepts."});
      continue;
    }

    std::string value = inline_value;
    if (takes_value(*f) && !has_inline) {
      if (index + 1 >= arguments.size()) {
        result.errors.push_back({std::string(f->key),
                                 std::string(f->flag) + " needs a value."});
        continue;
      }
      value = arguments[++index];
    }

    J node;
    std::string error;
    if (!to_json_value(*f, value, node, error)) {
      result.errors.push_back({std::string(f->key), std::string(f->flag) + ": " + error});
      continue;
    }
    overlay[std::string(f->key)] = std::move(node);
    result.explicitly_set.insert(std::string(f->key));
  }

  // Precedence: defaults, then the configuration file, then the flags.
  J document;
  to_json(document, result.config);

  if (!config_path.empty()) {
    std::string read_error;
    const auto text = read_file_limited(config_path, kMaxConfigFileBytes, read_error);
    if (!text.has_value()) {
      result.errors.push_back({"--config", read_error});
      return result;
    }
    try {
      const J file = J::parse(*text);
      if (!file.is_object()) {
        result.errors.push_back({"--config", "the configuration file must be a JSON object."});
        return result;
      }
      for (auto it = file.begin(); it != file.end(); ++it) {
        if (find_field(it.key()) == nullptr) {
          result.errors.push_back(
              {it.key(), "unknown configuration key \"" + it.key() + "\" in " + config_path});
          continue;
        }
        document[it.key()] = it.value();
        result.explicitly_set.insert(it.key());
      }
    } catch (const J::exception& e) {
      result.errors.push_back({"--config", "cannot parse " + config_path + ": " + e.what()});
      return result;
    }
  }

  for (auto it = overlay.begin(); it != overlay.end(); ++it) {
    document[it.key()] = it.value();
  }

  try {
    from_json(document, result.config);
  } catch (const J::exception& e) {
    result.errors.push_back({"config", e.what()});
    return result;
  }

  apply_derived_defaults(result.config, result.explicitly_set);
  return result;
}

ParseResult parse_cli(int argc, char** argv) {
  std::vector<std::string> arguments;
  for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
  return parse_arguments(arguments);
}

}  // namespace rtlangle::app
