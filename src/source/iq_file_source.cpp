#include "source/iq_file_source.h"

#include "persist/path_safety.h"

#include <cerrno>
#include <cstring>
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <unistd.h>

namespace rtlangle {
namespace {

using J = nlohmann::json;

ssize_t retry_read(int fd, void* buf, std::size_t n) {
  for (;;) {
    const ssize_t r = ::read(fd, buf, n);
    if (r >= 0 || errno != EINTR) return r;
  }
}

// The normalisation of spec section 8: (x - 127.4f) / 127.5f.
inline float normalise(std::uint8_t b) {
  return (static_cast<float>(b) - 127.4F) / 127.5F;
}

}  // namespace

IqFileSource::IqFileSource(std::string path, const Config& cfg, std::string& error)
    : path_(std::move(path)) {
  error.clear();

  fd_ = open_read_no_symlink(path_, error);
  if (fd_ < 0) return;

  struct stat st {};
  if (::fstat(fd_, &st) != 0) {
    error = "cannot stat \"" + path_ + "\": " + std::strerror(errno);
    ::close(fd_);
    fd_ = -1;
    return;
  }
  const std::uint64_t bytes = static_cast<std::uint64_t>(st.st_size);
  total_samples_ = bytes / 2;
  if (bytes % 2 != 0) {
    notices_.push_back("The replay file \"" + path_ +
                       "\" does not end on a whole I/Q pair; the trailing byte was dropped.");
  }

  // The optional sidecar. Its absence is not an error, and Phase 1 never writes
  // one (spec decision Q2).
  const std::string sidecar = path_ + ".json";
  struct stat sst {};
  if (::stat(sidecar.c_str(), &sst) == 0) {
    std::string read_error;
    const auto text = read_file_limited(sidecar, kMaxSidecarBytes, read_error);
    if (!text.has_value()) {
      error = read_error;
      ::close(fd_);
      fd_ = -1;
      return;
    }
    IqFileMetadata meta;
    try {
      const J j = J::parse(*text);
      if (j.contains("sample_rate_hz")) meta.sample_rate_hz = j.at("sample_rate_hz").get<std::uint32_t>();
      if (j.contains("center_hz")) meta.center_hz = j.at("center_hz").get<std::uint32_t>();
    } catch (const J::exception& e) {
      error = "cannot parse the sidecar \"" + sidecar + "\": " + e.what();
      ::close(fd_);
      fd_ = -1;
      return;
    }
    if (!meta.sample_rate_hz.has_value() || !meta.center_hz.has_value()) {
      error = "the sidecar \"" + sidecar +
              "\" must contain both sample_rate_hz and center_hz.";
      ::close(fd_);
      fd_ = -1;
      return;
    }
    if (*meta.sample_rate_hz != cfg.sample_rate_hz) {
      error = "the sidecar \"" + sidecar + "\" records sample_rate_hz " +
              std::to_string(*meta.sample_rate_hz) + " Hz, but the configuration is " +
              std::to_string(cfg.sample_rate_hz) +
              " Hz. Reconcile them rather than replaying at the wrong rate.";
      ::close(fd_);
      fd_ = -1;
      return;
    }
    const std::uint32_t cfg_center = cfg.center_hz.value_or(0);
    if (*meta.center_hz != cfg_center) {
      error = "the sidecar \"" + sidecar + "\" records center_hz " +
              std::to_string(*meta.center_hz) + " Hz, but the configuration is " +
              std::to_string(cfg_center) + " Hz.";
      ::close(fd_);
      fd_ = -1;
      return;
    }
  }

  // A file has no hardware to snap anything, so requested and applied are the
  // same values by construction.
  info_.driver = "iq-file";
  info_.device_name = basename_of(path_);
  info_.serial.clear();
  info_.requested_sample_rate_hz = cfg.sample_rate_hz;
  info_.applied_sample_rate_hz = cfg.sample_rate_hz;
  const std::uint32_t tuned = static_cast<std::uint32_t>(
      static_cast<std::int64_t>(cfg.center_hz.value_or(0)) + cfg.offset_tune_hz);
  info_.requested_center_hz = tuned;
  info_.applied_center_hz = tuned;
  info_.requested_gain_tenth_db = cfg.gain_tenth_db.value_or(0);
  info_.applied_gain_tenth_db = cfg.gain_tenth_db.value_or(0);
  info_.agc_enabled = false;
  info_.ppm = cfg.ppm;
  info_.applied_offset_hz = cfg.offset_tune_hz;
}

IqFileSource::~IqFileSource() {
  if (fd_ >= 0) ::close(fd_);
}

SourceInfo IqFileSource::info() const { return info_; }

ReadResult IqFileSource::read(std::span<std::complex<float>> out,
                              std::chrono::steady_clock::time_point deadline) {
  ReadResult r;
  if (cancelled_) {
    r.status = ReadStatus::Cancelled;
    return r;
  }
  if (fd_ < 0) {
    r.status = ReadStatus::Error;
    r.error = "the replay file \"" + path_ + "\" is not open.";
    return r;
  }
  if (end_of_stream_) {
    r.status = ReadStatus::EndOfStream;
    return r;
  }
  if (std::chrono::steady_clock::now() >= deadline) {
    r.status = ReadStatus::Timeout;
    return r;
  }

  scratch_.resize(out.size() * 2);
  std::size_t got_bytes = 0;
  while (got_bytes < scratch_.size()) {
    const ssize_t n = retry_read(fd_, scratch_.data() + got_bytes, scratch_.size() - got_bytes);
    if (n < 0) {
      r.status = ReadStatus::Error;
      r.error = "cannot read \"" + path_ + "\": " + std::strerror(errno);
      return r;
    }
    if (n == 0) {
      end_of_stream_ = true;
      break;
    }
    got_bytes += static_cast<std::size_t>(n);
  }

  const std::size_t pairs = got_bytes / 2;
  for (std::size_t i = 0; i < pairs; ++i) {
    const std::uint8_t bi = scratch_[2 * i];
    const std::uint8_t bq = scratch_[2 * i + 1];
    if (bi == 0 || bi == 255) ++rail_bytes_;
    if (bq == 0 || bq == 255) ++rail_bytes_;
    out[i] = {normalise(bi), normalise(bq)};
  }
  total_bytes_seen_ += pairs * 2;
  consumed_samples_ += pairs;
  r.samples = pairs;
  if (pairs < out.size()) {
    end_of_stream_ = true;
    r.status = ReadStatus::EndOfStream;
  }
  return r;
}

void IqFileSource::flush() {
  // Nothing is buffered; the file position is the whole state, and it must NOT
  // be rewound - the stream is consumed sequentially across the session.
  rail_bytes_ = 0;
  total_bytes_seen_ = 0;
}

void IqFileSource::cancel() { cancelled_ = true; }

double IqFileSource::clipped_fraction() const {
  if (total_bytes_seen_ == 0) return 0.0;
  return static_cast<double>(rail_bytes_) / static_cast<double>(total_bytes_seen_);
}

}  // namespace rtlangle
