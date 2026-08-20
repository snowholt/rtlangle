#pragma once

#include "core/config.h"

#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace rtlangle::app {

struct ParseResult {
  Config      config;
  std::string command;      // run | scan | devices | report
  std::string positional;   // the session directory, for `report`
  // Which keys the operator actually set, from a configuration file or a flag.
  // A field left at its default is never a command-scope error, which is why
  // the set is carried rather than inferred by comparing against the defaults.
  std::set<std::string>        explicitly_set;
  std::vector<ValidationError> errors;
  bool help = false;
  bool version = false;
};

// Precedence, lowest to highest: built-in defaults, --config <file.json>, then
// command-line flags. The fully resolved Config is what a session records, so
// the session is reproducible from its own record.
//
// Every flag comes from the field registry, so a Config field without a flag is
// unreachable and a flag without a field cannot exist. An unrecognised flag is
// a usage error naming it, never a silent no-op.
ParseResult parse_cli(int argc, char** argv);

// The same parser over an already-split argument list, for tests and for the
// interactive menu.
ParseResult parse_arguments(const std::vector<std::string>& arguments);

// Applies one option to an existing configuration, exactly as the command line
// would apply it: the same flag lookup, the same conversion, the same derived
// defaults. `value` is ignored for a boolean flag beyond the true/false forms
// the command line accepts.
//
// On any error the configuration and the key set are left untouched, so a
// refused edit cannot half-apply. This is what lets the interactive screen
// share one type-checking path with the parser rather than growing a second.
std::vector<ValidationError> apply_option(Config&, std::set<std::string>& explicitly_set,
                                          std::string_view flag, std::string_view value);

// The usage text, listing every command and every flag the registry defines.
std::string usage_text();

}  // namespace rtlangle::app
