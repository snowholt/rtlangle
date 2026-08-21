#include "dsp/offset_mixer.h"

#include <array>
#include <cmath>
#include <numbers>

namespace rtlangle::dsp {
namespace {

// One shared table, built once. Plan creation is the only FFTW-style hazard in
// this file and there is none here: the table is immutable after construction.
const std::array<float, kNcoTableSize * 2>& nco_table() {
  static const std::array<float, kNcoTableSize * 2> table = [] {
    std::array<float, kNcoTableSize * 2> t{};
    for (std::size_t i = 0; i < kNcoTableSize; ++i) {
      const double phase = 2.0 * std::numbers::pi_v<double> * static_cast<double>(i) /
                           static_cast<double>(kNcoTableSize);
      t[2 * i] = static_cast<float>(std::cos(phase));
      t[2 * i + 1] = static_cast<float>(std::sin(phase));
    }
    return t;
  }();
  return table;
}

}  // namespace

OffsetMixer::OffsetMixer(std::uint32_t sample_rate_hz, std::int64_t offset_hz)
    : sample_rate_hz_(sample_rate_hz == 0 ? 1 : sample_rate_hz),
      applied_offset_hz_(offset_hz) {
  const std::int64_t sr = static_cast<std::int64_t>(sample_rate_hz_);
  step_ = ((offset_hz % sr) + sr) % sr;
}

void OffsetMixer::reset() { acc_ = 0; }

void OffsetMixer::process(std::span<std::complex<float>> inout) {
  if (step_ == 0) return;   // no offset tuning: the signal is already at DC
  const auto& table = nco_table();
  const std::int64_t sr = static_cast<std::int64_t>(sample_rate_hz_);
  const std::int64_t table_size = static_cast<std::int64_t>(kNcoTableSize);

  for (std::complex<float>& x : inout) {
    // Round rather than truncate, so the table index error is at most half a
    // table step in either direction instead of always lagging.
    const std::int64_t idx =
        ((acc_ * table_size + sr / 2) / sr) % table_size;
    const float c = table[static_cast<std::size_t>(2 * idx)];
    const float s = table[static_cast<std::size_t>(2 * idx + 1)];
    const std::complex<float> osc(c, s);   // exp(+j 2 pi f_off t)
    x *= osc;
    acc_ += step_;
    if (acc_ >= sr) acc_ -= sr;
  }
}

}  // namespace rtlangle::dsp
