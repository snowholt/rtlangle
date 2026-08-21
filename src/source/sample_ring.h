#pragma once

#include "source/sample_source.h"

#include <complex>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace rtlangle {

// The bounded hand-off between a device worker thread and the reader.
//
// One mutex and one condition variable. The worker sets an error or
// end-of-stream flag and notifies BEFORE exiting, which is what wakes a blocked
// reader with the real reason instead of leaving it to time out silently.
//
// When the buffer is full the worker discards the samples it cannot store and
// counts them. That count is host-side by construction and is named so
// throughout.
class SampleRing {
 public:
  explicit SampleRing(std::size_t capacity_samples);

  // Worker side. Returns the number of samples discarded because the buffer was
  // full; those samples are also added to the cumulative host-drop count.
  std::size_t push(std::span<const std::complex<float>> in);

  // Worker side. Both wake a blocked reader.
  void set_error(std::string message);
  void set_end_of_stream();

  // Reader side. Blocks until `out` is full, the deadline passes, or the worker
  // reports a terminal condition. Partial data is returned with the status that
  // ended the wait.
  ReadResult pop(std::span<std::complex<float>> out,
                 std::chrono::steady_clock::time_point deadline);

  // Wake any blocked reader; every later pop returns Cancelled.
  void cancel();

  // Discard buffered samples. The host-drop count is cumulative across the
  // session and is deliberately NOT reset here: the capture runner takes a
  // per-capture delta, and resetting would make the delta meaningless.
  void flush();

  std::uint64_t host_dropped_samples() const;
  std::size_t   size() const;
  bool          cancelled() const;

 private:
  std::size_t available_locked() const;

  mutable std::mutex      mutex_;
  std::condition_variable data_ready_;
  std::vector<std::complex<float>> buffer_;
  std::size_t   head_ = 0;   // next read position
  std::size_t   count_ = 0;
  std::uint64_t dropped_ = 0;
  bool          end_of_stream_ = false;
  bool          cancelled_ = false;
  std::string   error_;
};

}  // namespace rtlangle
