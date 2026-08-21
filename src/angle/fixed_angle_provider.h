#pragma once

#include "angle/angle_provider.h"

namespace rtlangle {

// Returns Proceed with actual_deg == planned_deg and never prompts. It serves
// --non-interactive replay, and it is the second implementation that proves
// IAngleProvider is not coupled to a terminal.
class FixedAngleProvider final : public IAngleProvider {
 public:
  std::string_view name() const override { return "fixed"; }
  bool is_automated() const override { return true; }

  AngleOutcome request(double planned_deg, int) override {
    AngleOutcome out;
    out.cmd = AngleOutcome::Cmd::Proceed;
    out.actual_deg = planned_deg;
    return out;
  }
};

}  // namespace rtlangle
