#pragma once

#include "core/records.h"

#include <chrono>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace rtlangle {

enum class ReadStatus { Ok, Timeout, EndOfStream, Error, Cancelled };
std::string_view to_string(ReadStatus);

struct ReadResult {
  std::size_t samples = 0;   // samples actually written into `out`
  ReadStatus  status = ReadStatus::Ok;
  std::string error;         // non-empty only when status == Error

  // Cumulative count of samples the HOST dropped because the ring buffer
  // between the device worker and the reader was full.
  //
  // This is NOT a device-level or USB-level overrun count, and no message,
  // column, or document may call it one. librtlsdr exposes no such counter; a
  // true device overflow reaches this program only as an unobservable
  // discontinuity in the sample stream. What can honestly be measured is how
  // many samples this program discarded because its own bounded buffer was full
  // while the reader was busy in the DSP chain, and that is what this counts.
  std::uint64_t host_dropped_samples = 0;
};

// A blocking, cancellable, timeout-bounded producer of normalised complex
// samples at a fixed rate.
//
// Every implementation must honour the deadline, must return Error with a
// populated `error` string when its worker thread fails, and must make a worker
// failure wake a blocked reader rather than let it time out silently.
class ISampleSource {
 public:
  virtual ~ISampleSource() = default;

  virtual SourceInfo info() const = 0;

  // Fills at most out.size() samples. Returns early on deadline, cancel, end of
  // stream, or error. Never blocks past `deadline`.
  virtual ReadResult read(std::span<std::complex<float>> out,
                          std::chrono::steady_clock::time_point deadline) = 0;

  virtual void flush() = 0;    // discard buffered samples; used after settling
  virtual void cancel() = 0;   // wake any blocked read; later reads return Cancelled

  // Fraction of raw input samples at the ADC rails since the last flush().
  virtual double clipped_fraction() const = 0;
};

// Scan needs to retune; the experiment must not. Retuning is a separate
// interface rather than a down-cast of an experiment source, so nothing on the
// experiment path can reach it.
class ITunableSampleSource : public ISampleSource {
 public:
  // Retune and discard all buffered samples captured at the previous frequency.
  // Returns false and sets `error` on failure.
  virtual bool retune(std::uint32_t center_hz, std::string& error) = 0;
};

}  // namespace rtlangle
