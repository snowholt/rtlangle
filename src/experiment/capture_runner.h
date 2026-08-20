#pragma once

#include "core/config.h"
#include "core/records.h"
#include "dsp/chain.h"
#include "source/sample_source.h"

#include <chrono>
#include <complex>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rtlangle {

struct CaptureOutcome {
  enum class Status { Ok, Timeout, SourceError, Clipped, InsufficientSamples, Cancelled };

  Status      status = Status::Ok;
  std::string detail;

  // Sampled BEFORE the first read, never after the capture loop: a timestamp
  // taken afterwards would name the moment the capture ended.
  std::chrono::system_clock::time_point started_utc{};
  double wall_duration_s = 0.0;

  // This capture only, never cumulative. A cumulative counter would make every
  // later capture look worse than the first.
  std::uint64_t host_dropped_samples = 0;
  double        clipped_fraction = 0.0;

  // A snapshot of source.info() taken at capture start. It is what the attempt
  // persists and what warning W7 compares against the receiver-segment
  // baseline; without it W7 would have nothing to examine.
  SourceInfo applied;

  std::vector<std::complex<float>> channel;   // channel-rate, transient-trimmed
  std::vector<float>               audio;     // channel-rate audio band
  std::size_t audio_latency_samples = 0;      // spec section 8.4
};

std::string_view to_string(CaptureOutcome::Status);

class CaptureRunner {
 public:
  CaptureRunner(ISampleSource& source, dsp::Chain& chain, const Config& cfg);

  // `on_progress` receives a fraction in [0,1]; it may be empty.
  CaptureOutcome run(const std::function<void(double)>& on_progress);

 private:
  ISampleSource& source_;
  dsp::Chain&    chain_;
  const Config&  cfg_;
};

}  // namespace rtlangle
