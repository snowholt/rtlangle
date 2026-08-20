#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace rtlangle {

struct AngleOutcome {
  enum class Cmd { Proceed, Retry, Skip, Quit };
  Cmd                        cmd = Cmd::Proceed;
  double                     actual_deg = 0.0;   // operator estimate, or servo feedback
  std::optional<std::string> note;
};

// The forward-compatibility seam. A future SerialServoAngleProvider must be
// addable without modifying DSP, metrics, persistence, or experiment logic;
// this interface is the whole of that provision.
//
// Settling is deliberately NOT here: ExperimentController applies it between a
// Proceed outcome and the capture, so the timing is identical for every
// provider and a future servo provider cannot accidentally change the settling
// behaviour and thereby the measurement.
class IAngleProvider {
 public:
  virtual ~IAngleProvider() = default;

  virtual std::string_view name() const = 0;    // "manual" | a future "serial-servo"
  virtual bool is_automated() const = 0;        // recorded in session metadata

  // Ask for the antenna to be placed at `planned_deg` and report the outcome.
  // `attempt` is 1 for a first visit and increments on retry, so the provider
  // can word its prompt correctly.
  virtual AngleOutcome request(double planned_deg, int attempt) = 0;
};

}  // namespace rtlangle
