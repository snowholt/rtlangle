#pragma once

#include "core/config.h"
#include "dsp/spectrum.h"

#include <complex>
#include <span>

namespace rtlangle::metrics {

enum class FloorStatus {
  Reliable,        // the capture has a usable quiet reference
  Unreliable,      // R1: too much of the capture is above the squelch to trust a percentile
  Unidentifiable,  // R2: occupied by something that never keys down
};
std::string_view to_string(FloorStatus);

struct NoiseFloor {
  FloorStatus status = FloorStatus::Reliable;
  double power = 0.0;                    // linear, at the configured percentile
  double probe_power = 0.0;              // linear, at the probe percentile
  double active_probe_fraction = 0.0;
  double dynamic_range_db = 0.0;
  double carrier_persistence = 0.0;      // fraction of analysis windows with a carrier
  std::string detail;                    // why, when the status is not Reliable
};

// The two-stage probe of spec section 9.1.
//
// The superseded rule was circular: it used the 20th-percentile floor itself to
// decide whether enough quiet frames existed to make that percentile
// meaningful. When 90 percent of a capture is occupied, the 20th percentile
// lands INSIDE a transmission, the floor looks normal, and the computed active
// fraction comes out near zero - so a capture that was almost entirely signal
// would be accepted as quiet noise and would produce confident-looking SNR.
//
// The replacement estimates the floor twice, at two different percentiles, and
// uses the lower one only to judge occupancy. The reported floor is never used
// to judge its own validity.
//
// `stream` is the time-domain signal the frame powers were computed from; it
// feeds the persistent-carrier test. Passing an empty span skips that test, in
// which case rule R2 cannot fire.
NoiseFloor estimate_noise_floor(std::span<const double> frame_powers,
                                std::span<const std::complex<float>> stream,
                                const Config& cfg, dsp::Spectrum& spectrum);

// The same estimator over a real-valued stream, used for the audio floor. Spec
// section 9.4.2 requires the audio floor to obey the same two-stage check.
NoiseFloor estimate_noise_floor(std::span<const double> frame_powers,
                                std::span<const float> stream, const Config& cfg,
                                dsp::Spectrum& spectrum);

}  // namespace rtlangle::metrics
