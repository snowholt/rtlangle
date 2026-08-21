#pragma once

#include "core/config.h"
#include "source/sample_source.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rtlangle {

// The optional sidecar of spec section 7.5. A raw .cu8 file carries no header,
// so nothing in the bytes says what produced them; a sidecar that disagrees
// with the resolved Config is a configuration error naming both values, never a
// silent override and never a silent mismatch.
struct IqFileMetadata {
  std::optional<std::uint32_t> sample_rate_hz;
  std::optional<std::uint32_t> center_hz;
};

// Replay of a raw interleaved unsigned-8-bit IQ file.
//
// The stream is consumed SEQUENTIALLY ACROSS THE WHOLE SESSION: visit 2
// continues where visit 1 stopped. A file is a recording of one span of time,
// and rewinding it for every visit would present the same seconds as if they
// were different captures at different angles. End of file is not looped
// either, because looping would fabricate replicates that do not exist.
class IqFileSource final : public ISampleSource {
 public:
  // Opens the file and reads any sidecar. On failure the object is left in a
  // state where read() returns Error, and `error` describes why; callers should
  // use make_source(), which reports the failure instead of constructing one.
  IqFileSource(std::string path, const Config& cfg, std::string& error);
  ~IqFileSource() override;
  IqFileSource(const IqFileSource&) = delete;
  IqFileSource& operator=(const IqFileSource&) = delete;

  SourceInfo info() const override;
  ReadResult read(std::span<std::complex<float>> out,
                  std::chrono::steady_clock::time_point deadline) override;
  void   flush() override;
  void   cancel() override;
  double clipped_fraction() const override;

  bool ok() const { return fd_ >= 0; }

  // Messages emitted once at open time, such as an odd trailing byte having
  // been dropped. Reporting at open is what makes "exactly once" structural
  // rather than a flag the read path has to remember.
  const std::vector<std::string>& notices() const { return notices_; }

  // Whole IQ pairs in the file, for the preflight length check.
  std::uint64_t total_samples() const { return total_samples_; }

 private:
  std::string   path_;
  SourceInfo    info_;
  int           fd_ = -1;
  std::uint64_t total_samples_ = 0;
  std::uint64_t consumed_samples_ = 0;
  std::uint64_t rail_bytes_ = 0;
  std::uint64_t total_bytes_seen_ = 0;
  bool          cancelled_ = false;
  bool          end_of_stream_ = false;
  std::vector<std::string> notices_;
  std::vector<std::uint8_t> scratch_;
};

}  // namespace rtlangle
