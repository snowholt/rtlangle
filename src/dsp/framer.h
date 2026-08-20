#pragma once

#include <complex>
#include <cstddef>
#include <span>
#include <vector>

namespace rtlangle::dsp {

// Frame length and hop at the channel rate (spec section 8.3): 1024 samples and
// 512, so a frame is 32 ms and the hop 16 ms at 32 kHz. The 50 percent overlap
// means adjacent frames are correlated, which is why frames are never treated
// as independent samples.
inline constexpr std::size_t kFrameLength = 1024;
inline constexpr std::size_t kFrameHop = 512;

// P[k] = (1/M) * sum over the frame of |x[n]|^2, in normalised full-scale units
// squared. Measured over the channel only, never over the full input band, so
// the number reflects the antenna rather than the receiver's filter width.
//
// An input shorter than one frame yields an empty vector rather than a partial
// frame: a partial frame would have a different noise bandwidth from every
// other frame and would bias the percentile floor.
std::vector<double> frame_powers(std::span<const std::complex<float>> x, std::size_t len,
                                 std::size_t hop);
std::vector<double> frame_powers(std::span<const float> x, std::size_t len, std::size_t hop);

}  // namespace rtlangle::dsp
