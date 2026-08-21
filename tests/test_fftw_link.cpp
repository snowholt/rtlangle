// Link smoke test for the FFTW precision.
//
// The implementation uses the single-precision API throughout. fftwf_plan_dft_1d
// is defined in libfftw3f and absent from libfftw3, so a build that linked the
// double-precision library would fail here at link time rather than at runtime.
// Spec section 3 makes the precision a release gate; this file is what makes it
// a build-time one.

#include <doctest/doctest.h>

#include <fftw3.h>

#include <complex>
#include <vector>

TEST_SUITE("fftw_link") {

TEST_CASE("single-precision FFTW is linked and a plan round-trips a constant") {
  constexpr int kN = 16;
  std::vector<std::complex<float>> in(kN, std::complex<float>(1.0F, 0.0F));
  std::vector<std::complex<float>> out(kN, std::complex<float>(0.0F, 0.0F));

  fftwf_plan plan = fftwf_plan_dft_1d(
      kN, reinterpret_cast<fftwf_complex*>(in.data()),
      reinterpret_cast<fftwf_complex*>(out.data()), FFTW_FORWARD, FFTW_ESTIMATE);
  REQUIRE(plan != nullptr);
  fftwf_execute(plan);
  fftwf_destroy_plan(plan);

  // The DFT of a constant is a single non-zero bin at DC.
  CHECK(std::abs(out[0]) == doctest::Approx(static_cast<float>(kN)).epsilon(1e-4));
  for (int k = 1; k < kN; ++k) {
    CHECK(std::abs(out[static_cast<std::size_t>(k)]) < 1e-3F);
  }
}

}  // TEST_SUITE
