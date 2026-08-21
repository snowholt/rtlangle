#pragma once

#include <complex>
#include <cstddef>
#include <span>
#include <vector>

// fftwf_plan is an opaque pointer type in <fftw3.h>. Declaring it here rather
// than including the header keeps the FFTW dependency out of every translation
// unit that merely mentions a Spectrum.
struct fftwf_plan_s;

namespace rtlangle::dsp {

// Welch-averaged periodogram, single precision throughout (spec section 3: the
// two FFTW precisions are never mixed).
//
// Averaging is required rather than optional: the peak-to-median ratio of a
// single unaveraged periodogram over 1024 bins is already about 10 dB for pure
// noise, which is the same order as the carrier-prominence threshold that has
// to distinguish a carrier from noise (spec section 9.1).
class Spectrum {
 public:
  Spectrum(std::size_t fft_size, std::size_t segments);
  ~Spectrum();
  Spectrum(const Spectrum&) = delete;
  Spectrum& operator=(const Spectrum&) = delete;
  Spectrum(Spectrum&&) = delete;
  Spectrum& operator=(Spectrum&&) = delete;

  std::size_t fft_size() const { return fft_size_; }
  std::size_t segments() const { return segments_; }

  // Returns fft_size power bins, frequency-shifted so that bin 0 is -fs/2 and
  // bin fft_size/2 is DC. Shifting here rather than at every call site is what
  // makes a plus-or-minus search around DC a contiguous range.
  //
  // Returns an empty vector when the input is shorter than one segment.
  std::vector<double> welch_psd(std::span<const std::complex<float>> x);

  // The ratio of the strongest bin within plus or minus `half_band_bins` of DC
  // to the median bin power of the whole window, in dB. This is the quantity
  // spec section 9.1 compares against carrier_prominence_db.
  double peak_prominence_db(std::span<const double> psd, double half_band_bins) const;

  // The parabolically interpolated peak position, in Hz relative to DC. The
  // search is restricted to plus or minus `search_half_bw_hz` of DC; passing a
  // non-positive value searches the whole span.
  //
  // A comparability diagnostic only: it does NOT identify a transmitter or an
  // aircraft (spec section 9.2).
  double carrier_offset_hz(std::span<const double> psd, double bin_hz,
                           double search_half_bw_hz = 0.0) const;

 private:
  std::size_t                      fft_size_ = 0;
  std::size_t                      segments_ = 0;
  fftwf_plan_s*                    plan_ = nullptr;
  std::vector<std::complex<float>> in_;
  std::vector<std::complex<float>> out_;
  std::vector<float>               window_;
  double                           window_power_ = 1.0;
};

}  // namespace rtlangle::dsp
