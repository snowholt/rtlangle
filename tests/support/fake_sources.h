#pragma once

// Test doubles for the source contract. These live under tests/support/ and are
// never reachable from src/: the dependency rule is one-directional, and the
// production synthetic generator lives in src/source/ precisely so that no
// production type has to reach here.

#include "source/sample_ring.h"
#include "source/sample_source.h"
#include "source/synthetic_source.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

namespace rtlangle::test {

// Drives the production SampleRing from a worker thread, so the deadline,
// cancellation, and worker-failure paths that RtlSdrSource will depend on are
// exercised with no hardware.
class RingBackedSource final : public ISampleSource {
 public:
  enum class Behaviour {
    Stall,          // the worker produces nothing at all
    FailAfterDelay, // the worker reports an error while the reader is blocked
    Feed,           // the worker feeds a constant stream
  };

  RingBackedSource(Behaviour behaviour, std::chrono::milliseconds delay,
                   std::size_t capacity = 4096)
      : ring_(capacity), behaviour_(behaviour), delay_(delay) {
    worker_ = std::thread([this] { run(); });
  }

  ~RingBackedSource() override {
    stop_ = true;
    ring_.cancel();
    if (worker_.joinable()) worker_.join();
  }

  SourceInfo info() const override {
    SourceInfo i;
    i.driver = "test-ring";
    return i;
  }

  ReadResult read(std::span<std::complex<float>> out,
                  std::chrono::steady_clock::time_point deadline) override {
    return ring_.pop(out, deadline);
  }

  void   flush() override { ring_.flush(); }
  void   cancel() override { ring_.cancel(); }
  double clipped_fraction() const override { return 0.0; }

 private:
  void run() {
    if (behaviour_ == Behaviour::Stall) {
      while (!stop_) std::this_thread::sleep_for(std::chrono::milliseconds(5));
      return;
    }
    if (behaviour_ == Behaviour::FailAfterDelay) {
      std::this_thread::sleep_for(delay_);
      if (!stop_) {
        // The error is set and the reader notified BEFORE the worker exits,
        // which is what wakes a blocked reader with the real reason instead of
        // leaving it to time out.
        ring_.set_error("the device worker thread failed: simulated USB fault");
      }
      return;
    }
    std::vector<std::complex<float>> block(512, std::complex<float>(0.25F, -0.25F));
    while (!stop_) {
      ring_.push(block);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  SampleRing        ring_;
  Behaviour         behaviour_;
  std::chrono::milliseconds delay_;
  std::atomic<bool> stop_{false};
  std::thread       worker_;
};

// Reports a scripted sequence of cumulative host-drop counts, one per read, so
// the capture runner's per-capture delta can be checked directly.
class ScriptedDropSource final : public ISampleSource {
 public:
  explicit ScriptedDropSource(std::vector<std::uint64_t> cumulative)
      : cumulative_(std::move(cumulative)) {}

  SourceInfo info() const override {
    SourceInfo i;
    i.driver = "test-drops";
    return i;
  }

  ReadResult read(std::span<std::complex<float>> out,
                  std::chrono::steady_clock::time_point) override {
    ReadResult r;
    for (auto& s : out) s = {0.01F, 0.01F};
    r.samples = out.size();
    r.host_dropped_samples = cumulative_[std::min(index_, cumulative_.size() - 1)];
    if (index_ + 1 < cumulative_.size()) ++index_;
    return r;
  }

  // Advances to the next scripted value, standing in for the next capture.
  void next_capture() { ++index_; }

  void   flush() override {}
  void   cancel() override {}
  double clipped_fraction() const override { return 0.0; }

 private:
  std::vector<std::uint64_t> cumulative_;
  std::size_t                index_ = 0;
};

// Wraps a production SyntheticSource and adds behaviour the capture runner needs
// to be driven through: a configurable clipped fraction, a settable info(), and
// a first-read delay.
class InstrumentedSyntheticSource final : public ISampleSource {
 public:
  explicit InstrumentedSyntheticSource(SyntheticParams p) : inner_(p) { info_ = inner_.info(); }

  void set_clipped_fraction(double f) { clipped_ = f; }
  void set_first_read_delay(std::chrono::milliseconds d) { first_delay_ = d; }
  void set_info(SourceInfo i) { info_ = std::move(i); }
  int  flush_count() const { return flush_count_; }

  SourceInfo info() const override { return info_; }

  ReadResult read(std::span<std::complex<float>> out,
                  std::chrono::steady_clock::time_point deadline) override {
    if (!first_read_done_) {
      first_read_done_ = true;
      if (first_delay_.count() > 0) std::this_thread::sleep_for(first_delay_);
    }
    return inner_.read(out, deadline);
  }

  void   flush() override { ++flush_count_; inner_.flush(); }
  void   cancel() override { inner_.cancel(); }
  double clipped_fraction() const override { return clipped_; }

 private:
  SyntheticSource inner_;
  SourceInfo      info_;
  double          clipped_ = 0.0;
  std::chrono::milliseconds first_delay_{0};
  bool            first_read_done_ = false;
  int             flush_count_ = 0;
};

// Produces a fixed number of samples and then reports end of stream, so the
// short-capture path is reachable without a file.
class ShortSource final : public ISampleSource {
 public:
  explicit ShortSource(std::size_t total) : remaining_(total) {}

  SourceInfo info() const override {
    SourceInfo i;
    i.driver = "test-short";
    return i;
  }

  ReadResult read(std::span<std::complex<float>> out,
                  std::chrono::steady_clock::time_point) override {
    ReadResult r;
    const std::size_t take = std::min(remaining_, out.size());
    for (std::size_t i = 0; i < take; ++i) out[i] = {0.01F, 0.0F};
    remaining_ -= take;
    r.samples = take;
    if (take < out.size()) r.status = ReadStatus::EndOfStream;
    return r;
  }

  void   flush() override {}
  void   cancel() override {}
  double clipped_fraction() const override { return 0.0; }

 private:
  std::size_t remaining_ = 0;
};

}  // namespace rtlangle::test
