#pragma once

#include "core/config.h"
#include "source/sample_source.h"
#include "ui/terminal_ui.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rtlangle::app {

struct ScanChannel {
  std::uint32_t center_hz = 0;
  int           exposures = 0;
  int           hits = 0;
  // hits / exposures. Absent when the channel was never observed: a channel
  // with no exposures is reported as "-" and never as 0 percent, because
  // dividing by a denominator that does not exist is not a measurement.
  std::optional<double> activity;
};

// The dwell placement of spec section 12.5.1.
//
//   U = scan_usable_fraction * sample_rate_hz / 2      the usable half-span
//   D = scan_dc_exclusion_hz                           the DC exclusion
//   S = U - D                                          the dwell step
//
// A dwell centred at c covers [c-U, c-D] and [c+D, c+U]. It covers NOTHING in
// (c-D, c+D), where the receiver's DC spur sits. Stepping by the whole usable
// span, S = 2U, makes consecutive dwells abut at their outer edges and leaves
// the 2D-wide hole around every dwell centre covered by no dwell at all - at
// the defaults a 60 kHz hole against a 25 kHz channel spacing, so two channels
// per dwell would be permanently invisible.
//
// The hole around c must instead be covered by the neighbours at c +/- S:
//
//   c - S + U >= c + D   gives   S <= U - D            reach the far edge
//   c - S + D <= c - D   gives   S >= 2D               reach the near edge
//
// Such an S exists exactly when U >= 3D, which is the section 7.6 cross-field
// rule. The chosen step is the largest legal one, S = U - D, because it
// minimises the dwell count while satisfying both.
struct DwellPlan {
  std::vector<std::uint32_t> centers;
  double usable_half_hz = 0.0;
  double dc_exclusion_hz = 0.0;
  double step_hz = 0.0;
  bool   feasible = false;   // false when U < 3D, which validate() already refuses
};

// `step_override`, when positive, replaces the computed step. It exists so the
// regression fixture can drive the placement with the old S = 2U and show the
// hole it leaves; production never passes it.
DwellPlan plan_dwells(const Config&, double step_override = 0.0);

// The channel grid the scan reports on.
std::vector<std::uint32_t> scan_channels(const Config&);

// Runs the sweep. Retune failure aborts with the failing frequency named, and
// `error` is then non-empty.
std::vector<ScanChannel> run_scan(ITunableSampleSource&, const Config&, ui::ITerminalUi&,
                                  std::string& error);

// The command wiring: builds a source, runs the sweep, prints the busiest
// channels and a copyable run line for the top result. Returns the exit code.
int scan_command(const Config&, ui::ITerminalUi&);

}  // namespace rtlangle::app
