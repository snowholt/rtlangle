#pragma once

// A real IAngleProvider whose request() is already told the planned angle, so
// it is the natural place to tell the scripted source which visit is starting.
// That one line is the whole coupling, and it is entirely test-side.

#include "angle/angle_provider.h"
#include "tests/support/scripted_capture_source.h"

#include <deque>
#include <utility>

namespace rtlangle::test {

class ScriptedAngleProvider final : public IAngleProvider {
 public:
  ScriptedAngleProvider(ScriptedCaptureSource& source, std::deque<AngleOutcome> script)
      : source_(source), script_(std::move(script)) {}

  std::string_view name() const override { return "scripted"; }
  bool is_automated() const override { return true; }

  AngleOutcome request(double planned_deg, int) override {
    source_.advance_to(planned_deg);   // <- the whole coupling, test-side only
    AngleOutcome out;
    out.actual_deg = planned_deg;
    if (!script_.empty()) {
      out = script_.front();
      script_.pop_front();
      if (out.actual_deg == 0.0) out.actual_deg = planned_deg;
    }
    return out;
  }

 private:
  ScriptedCaptureSource&   source_;
  std::deque<AngleOutcome> script_;
};

}  // namespace rtlangle::test
