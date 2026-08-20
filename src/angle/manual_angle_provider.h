#pragma once

#include "angle/angle_provider.h"
#include "core/config.h"
#include "ui/terminal_ui.h"

#include <utility>

namespace rtlangle {

// The operator positions the antenna by hand. This provider asks, validates the
// angle they report, and returns what they chose.
//
// Settling is not applied here: the controller applies it between a Proceed
// outcome and the capture, so every provider is settled identically.
class ManualAngleProvider final : public IAngleProvider {
 public:
  ManualAngleProvider(ui::ITerminalUi& terminal, Config cfg)
      : terminal_(terminal), cfg_(std::move(cfg)) {}

  std::string_view name() const override { return "manual"; }
  bool is_automated() const override { return false; }

  AngleOutcome request(double planned_deg, int attempt) override;

 private:
  // Prompts until the answer parses as a finite double within [0, 360], and
  // until a deviation beyond max_angle_deviation_deg has been confirmed. A
  // declined confirmation re-prompts rather than accepting, which is what
  // catches typing 9 for 90.
  std::optional<double> ask_actual_angle(double planned_deg);

  ui::ITerminalUi& terminal_;
  // Held by value, not by reference: the provider outlives the expression that
  // constructs it, and a dangling configuration would corrupt the angle bounds
  // and the reference text the operator is prompted with.
  Config cfg_;
};

}  // namespace rtlangle
