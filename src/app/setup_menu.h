#pragma once

#include "core/config.h"
#include "ui/terminal_ui.h"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace rtlangle::app {

// ---------------------------------------------------------------------------
// The "Set up experiment" screen
//
// Every edit is applied through `apply_option`, which is the same conversion
// and validation path the command line uses. A value this screen accepts is
// therefore exactly a value `rtlangle run` would have accepted, and there is no
// second table of types or bounds that could drift from the first.
//
// The screen names the options an operator sets often. Everything else in the
// registry is reachable through the free-form row, which takes a flag verbatim,
// so naming a subset here costs no coverage.
// ---------------------------------------------------------------------------

enum class SetupRow {
  Frequency,
  Angles,
  Rounds,
  Order,
  Duration,
  Settle,
  Gain,
  Device,
  Source,
  Label,
  AngleReference,
  OtherOption,
  Start,
  Back,
};

inline constexpr std::size_t kSetupRowCount = 14;

// The rows as the menu shows them: the option's name, and the value it holds
// now. The Start row's detail is `ready`, or names the flag that is blocking.
std::vector<ui::MenuItem> setup_rows(const Config&, const std::set<std::string>& explicitly_set);

// Runs the screen. Returns the configuration to run, or nullopt when the
// operator goes back without starting.
//
// `explicitly_set` is updated with every key the operator edits here. The
// resume classification of spec section 11.5 needs that: a stored value
// differing from a DEFAULT is not an override, while the same value differing
// from something the operator chose is exactly the conflict that would
// otherwise mix two experiments into one record.
std::optional<Config> setup_experiment(Config working, std::set<std::string>& explicitly_set,
                                       ui::ITerminalUi&);

}  // namespace rtlangle::app
