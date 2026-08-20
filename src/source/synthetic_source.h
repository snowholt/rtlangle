#pragma once

#include "source/sample_source.h"

#include <cstdint>
#include <random>
#include <utility>
#include <vector>

namespace rtlangle {

// Production code, not a test fixture: SyntheticSource is a production source
// type selected by `--source synthetic`, so it lives under src/. Tests may call
// it; production never includes anything from tests/ (spec section 5).
struct SyntheticParams {
  // Receiver geometry, so the generated stream is what a device tuned to
  // center_hz + offset_hz would deliver: the wanted carrier sits at baseband
  // -offset_hz and the mixer brings it to DC.
  std::uint32_t sample_rate_hz = 1024000;
  std::uint32_t center_hz = 118350000;
  std::int64_t  offset_hz = 250000;
  int           gain_tenth_db = 496;
  int           ppm = 0;

  double        snr_db = 12.0;
  double        duty = 0.25;
  double        talk_s = 3.0;
  double        gap_s = 9.0;      // recomputed from talk_s and duty at construction
  std::uint64_t seed = 1;

  // For the scan tests: transmitters at known absolute frequencies with known
  // duty cycles. When this is non-empty the generator produces a band rather
  // than a single channel, and retune() moves the window over it.
  std::vector<std::pair<std::uint32_t, double>> band_transmitters;
};

class SyntheticSource final : public ITunableSampleSource {
 public:
  explicit SyntheticSource(SyntheticParams params);

  // Production API. The end-to-end test double uses it to vary the generated
  // SNR per visit without the controller or any production type knowing that
  // varying is possible. The change takes effect at the next flush() boundary,
  // which the controller calls exactly once per capture after settling, so a
  // capture is never generated at two different SNRs.
  void set_snr_db(double snr_db);

  SourceInfo info() const override;
  ReadResult read(std::span<std::complex<float>> out,
                  std::chrono::steady_clock::time_point deadline) override;
  void   flush() override;
  void   cancel() override;
  double clipped_fraction() const override;
  bool   retune(std::uint32_t center_hz, std::string& error) override;

 private:
  std::complex<float> generate_one();
  bool burst_active(double t) const;

  SyntheticParams  params_;
  double           snr_db_ = 12.0;
  double           pending_snr_db_ = 12.0;
  std::uint32_t    center_hz_ = 0;
  std::uint64_t    sample_index_ = 0;
  std::mt19937_64  rng_;
  std::normal_distribution<double> noise_{0.0, 1.0};
  bool             cancelled_ = false;
};

}  // namespace rtlangle
