#include "dsp/framer.h"

namespace rtlangle::dsp {
namespace {

template <typename T>
std::vector<double> frame_powers_impl(std::span<const T> x, std::size_t len, std::size_t hop) {
  std::vector<double> out;
  if (len == 0 || hop == 0 || x.size() < len) return out;
  const std::size_t count = 1 + (x.size() - len) / hop;
  out.reserve(count);
  for (std::size_t k = 0; k < count; ++k) {
    const std::size_t base = k * hop;
    double sum = 0.0;
    for (std::size_t n = 0; n < len; ++n) {
      const T& v = x[base + n];
      if constexpr (std::is_same_v<T, std::complex<float>>) {
        const double re = v.real();
        const double im = v.imag();
        sum += re * re + im * im;
      } else {
        const double s = static_cast<double>(v);
        sum += s * s;
      }
    }
    out.push_back(sum / static_cast<double>(len));
  }
  return out;
}

}  // namespace

std::vector<double> frame_powers(std::span<const std::complex<float>> x, std::size_t len,
                                 std::size_t hop) {
  return frame_powers_impl<std::complex<float>>(x, len, hop);
}

std::vector<double> frame_powers(std::span<const float> x, std::size_t len, std::size_t hop) {
  return frame_powers_impl<float>(x, len, hop);
}

}  // namespace rtlangle::dsp
