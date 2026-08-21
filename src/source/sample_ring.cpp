#include "source/sample_ring.h"

#include <algorithm>

namespace rtlangle {

std::string_view to_string(ReadStatus s) {
  switch (s) {
    case ReadStatus::Ok: return "ok";
    case ReadStatus::Timeout: return "timeout";
    case ReadStatus::EndOfStream: return "end_of_stream";
    case ReadStatus::Error: return "error";
    case ReadStatus::Cancelled: return "cancelled";
  }
  return "ok";
}

SampleRing::SampleRing(std::size_t capacity_samples)
    : buffer_(capacity_samples == 0 ? 1 : capacity_samples) {}

std::size_t SampleRing::available_locked() const { return buffer_.size() - count_; }

std::size_t SampleRing::push(std::span<const std::complex<float>> in) {
  std::size_t dropped = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t space = available_locked();
    const std::size_t take = std::min(space, in.size());
    dropped = in.size() - take;
    for (std::size_t i = 0; i < take; ++i) {
      buffer_[(head_ + count_ + i) % buffer_.size()] = in[i];
    }
    count_ += take;
    dropped_ += dropped;
  }
  data_ready_.notify_all();
  return dropped;
}

void SampleRing::set_error(std::string message) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (error_.empty()) error_ = std::move(message);
  }
  data_ready_.notify_all();
}

void SampleRing::set_end_of_stream() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    end_of_stream_ = true;
  }
  data_ready_.notify_all();
}

void SampleRing::cancel() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_ = true;
  }
  data_ready_.notify_all();
}

void SampleRing::flush() {
  std::lock_guard<std::mutex> lock(mutex_);
  head_ = 0;
  count_ = 0;
}

std::uint64_t SampleRing::host_dropped_samples() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return dropped_;
}

std::size_t SampleRing::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return count_;
}

bool SampleRing::cancelled() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return cancelled_;
}

ReadResult SampleRing::pop(std::span<std::complex<float>> out,
                           std::chrono::steady_clock::time_point deadline) {
  ReadResult result;
  std::unique_lock<std::mutex> lock(mutex_);

  while (result.samples < out.size()) {
    if (cancelled_) {
      result.status = ReadStatus::Cancelled;
      break;
    }
    if (count_ > 0) {
      const std::size_t take = std::min(count_, out.size() - result.samples);
      for (std::size_t i = 0; i < take; ++i) {
        out[result.samples + i] = buffer_[(head_ + i) % buffer_.size()];
      }
      head_ = (head_ + take) % buffer_.size();
      count_ -= take;
      result.samples += take;
      continue;
    }
    // Terminal conditions are checked only once the buffer is drained, so no
    // sample already handed over is lost to an error that arrived after it.
    if (!error_.empty()) {
      result.status = ReadStatus::Error;
      result.error = error_;
      break;
    }
    if (end_of_stream_) {
      result.status = ReadStatus::EndOfStream;
      break;
    }
    if (data_ready_.wait_until(lock, deadline) == std::cv_status::timeout &&
        count_ == 0 && error_.empty() && !end_of_stream_ && !cancelled_) {
      result.status = ReadStatus::Timeout;
      break;
    }
  }

  result.host_dropped_samples = dropped_;
  return result;
}

}  // namespace rtlangle
