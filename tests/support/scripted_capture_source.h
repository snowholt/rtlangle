#pragma once

// How the synthetic end-to-end session varies its SNR by angle.
//
// SyntheticSource has ONE configured SNR, and nothing in the production wiring
// tells it which angle is being visited. A source that knew would be a
// production type coupled to the controller, which the layering forbids. The
// mechanism is therefore two test doubles, both here, neither of them reachable
// from src/: this one owns a production SyntheticSource and forwards every
// ISampleSource call to it, and ScriptedAngleProvider tells it which visit is
// starting.
//
// The SNR change takes effect at the next flush(), which the controller calls
// exactly once per capture after settling, so a capture is never generated at
// two SNRs.

#include "source/sample_source.h"
#include "source/synthetic_source.h"

#include <cmath>
#include <map>
#include <utility>
#include <vector>

namespace rtlangle::test {

class ScriptedCaptureSource final : public ISampleSource {
 public:
  ScriptedCaptureSource(SyntheticParams base, std::map<double, double> snr_by_angle)
      : inner_(base), snr_by_angle_(std::move(snr_by_angle)), base_snr_(base.snr_db) {}

  // Called by ScriptedAngleProvider when a visit begins.
  void advance_to(double planned_deg) {
    double snr = base_snr_;
    for (const auto& [angle, value] : snr_by_angle_) {
      if (std::fabs(angle - planned_deg) < 1e-6) snr = value;
    }
    inner_.set_snr_db(snr);
    visited.push_back(planned_deg);
  }

  std::vector<double> visited;

  SourceInfo info() const override { return inner_.info(); }
  ReadResult read(std::span<std::complex<float>> out,
                  std::chrono::steady_clock::time_point deadline) override {
    return inner_.read(out, deadline);
  }
  void   flush() override { inner_.flush(); }
  void   cancel() override { inner_.cancel(); }
  double clipped_fraction() const override { return inner_.clipped_fraction(); }

 private:
  SyntheticSource            inner_;
  std::map<double, double>   snr_by_angle_;
  double                     base_snr_ = 12.0;
};

}  // namespace rtlangle::test
