#pragma once

#include <complex>
#include <cstdint>
#include <span>
#include <vector>

namespace rtlangle::dsp {

// The number of entries in the sine and cosine lookup table (spec section 8.1).
inline constexpr std::size_t kNcoTableSize = 65536;

// Digital down-conversion by exp(+j 2 pi f_off t).
//
// The sign is POSITIVE because the hardware is tuned above the wanted signal at
// (center_hz + offset), so the signal appears at baseband frequency -offset and
// must be shifted up to DC.
//
// Phase is held in an exact integer accumulator modulo the sample rate, feeding
// a 65536-entry table. An integer accumulator cannot drift over a 60-second
// capture, whereas a floating-point recurrence can. Every integer offset is
// therefore represented exactly, and applied_offset_hz() equals the request;
// the field exists so the record shows what was actually used rather than what
// was asked for.
class OffsetMixer {
 public:
  OffsetMixer(std::uint32_t sample_rate_hz, std::int64_t offset_hz);

  std::int64_t applied_offset_hz() const { return applied_offset_hz_; }

  // The exact accumulator state, in units of 1/sample_rate cycles. Exposed so a
  // test can compare it against exact integer arithmetic rather than against
  // the mixer's own output.
  std::int64_t phase_accumulator() const { return acc_; }

  void process(std::span<std::complex<float>> inout);
  void reset();

 private:
  std::uint32_t sample_rate_hz_ = 1;
  std::int64_t  applied_offset_hz_ = 0;
  std::int64_t  step_ = 0;   // offset reduced into [0, sample_rate)
  std::int64_t  acc_ = 0;
};

}  // namespace rtlangle::dsp
