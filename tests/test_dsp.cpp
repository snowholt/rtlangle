// WP3 — the signal path: explicit filter edges, exact rational latency, chunk
// invariance, the integer-accumulator mixer, framing, and the spectrum. Spec
// section 8.

#include <doctest/doctest.h>

#include "core/config.h"
#include "dsp/am_demodulator.h"
#include "dsp/chain.h"
#include "dsp/fir_decimator.h"
#include "dsp/fir_design.h"
#include "dsp/framer.h"
#include "dsp/offset_mixer.h"
#include "dsp/spectrum.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <random>
#include <vector>

using namespace rtlangle;
using namespace rtlangle::dsp;

namespace {

constexpr double kPi = std::numbers::pi_v<double>;

// The response of a real FIR at one frequency, by direct evaluation of the DTFT
// rather than through an FFT, so the test shares no code with the design.
double response_db(const std::vector<float>& taps, double f_hz, double fs_hz) {
  std::complex<double> h(0.0, 0.0);
  for (std::size_t n = 0; n < taps.size(); ++n) {
    const double w = -2.0 * kPi * f_hz * static_cast<double>(n) / fs_hz;
    h += static_cast<double>(taps[n]) * std::complex<double>(std::cos(w), std::sin(w));
  }
  const double mag = std::abs(h);
  return 20.0 * std::log10(std::max(mag, 1e-300));
}

Config chain_config() {
  Config c;
  c.center_hz = 118350000;
  return c;
}

std::vector<std::complex<float>> noise_block(std::size_t n, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::normal_distribution<float> g(0.0F, 0.2F);
  std::vector<std::complex<float>> v(n);
  for (auto& x : v) x = {g(rng), g(rng)};
  return v;
}

}  // namespace

TEST_SUITE("dsp") {

TEST_CASE("the channel filter meets its stated passband and stopband edges") {
  const auto taps = design_lowpass(32000.0, 4000.0, 5000.0, 60.0);
  REQUIRE(!taps.empty());
  CHECK(taps.size() % 2 == 1);   // odd, for an integer group delay

  for (int i = 0; i <= 200; ++i) {
    const double f = 4000.0 * static_cast<double>(i) / 200.0;
    const double db = response_db(taps, f, 32000.0);
    CHECK_MESSAGE(std::fabs(db) < 0.5, "passband ripple at " << f << " Hz is " << db << " dB");
  }
  for (int i = 0; i <= 200; ++i) {
    const double f = 5000.0 + (16000.0 - 5000.0) * static_cast<double>(i) / 200.0;
    const double db = response_db(taps, f, 32000.0);
    CHECK_MESSAGE(db < -60.0, "stopband response at " << f << " Hz is " << db << " dB");
  }
}

TEST_CASE("every designed filter in the chain has an odd tap count") {
  const Chain chain = build_chain(chain_config());
  REQUIRE(chain.stages.size() == 2);
  for (const auto& s : chain.stages) {
    CHECK(s.taps() % 2 == 1);
  }
  CHECK(chain.channel.taps() % 2 == 1);
  // The audio low-pass too: AmDemodulator::latency_samples() assumes it.
  const auto audio_taps = design_lowpass(32000.0, 3400.0, 4000.0, 60.0);
  CHECK(audio_taps.size() % 2 == 1);
}

TEST_CASE("the audio low-pass meets its edges") {
  const auto taps = design_lowpass(32000.0, 3400.0, 4000.0, 60.0);
  REQUIRE(!taps.empty());
  for (int i = 0; i <= 100; ++i) {
    const double f = 3400.0 * static_cast<double>(i) / 100.0;
    CHECK(std::fabs(response_db(taps, f, 32000.0)) < 0.5);
  }
  for (int i = 0; i <= 100; ++i) {
    const double f = 4000.0 + 12000.0 * static_cast<double>(i) / 100.0;
    CHECK(response_db(taps, f, 32000.0) < -60.0);
  }
}

TEST_CASE("filter design is total over inputs validation refuses") {
  CHECK(design_lowpass(32000.0, 5000.0, 4000.0, 60.0).empty());   // edges inverted
  CHECK(design_lowpass(32000.0, 4000.0, 20000.0, 60.0).empty());  // stopband past Nyquist
  CHECK(design_lowpass(0.0, 4000.0, 5000.0, 60.0).empty());
  CHECK(design_lowpass(32000.0, 4000.0, 5000.0, 0.0).empty());
  CHECK(design_lowpass(std::nan(""), 4000.0, 5000.0, 60.0).empty());
}

TEST_CASE("decimation factors are deterministic and refuse an impossible ratio") {
  const std::vector<int> expect_32 = {8, 4};
  const std::vector<int> expect_16 = {8, 2};
  CHECK(decimation_factors(1024000, 32000) == expect_32);
  CHECK(decimation_factors(1024000, 64000) == expect_16);
  CHECK(decimation_factors(1024000, 32000) == decimation_factors(1024000, 32000));
  CHECK(decimation_factors(1020000, 60000).empty());   // ratio 17
  CHECK(decimation_factors(32000, 32000).empty());     // ratio 1
  CHECK(decimation_factors(1024000, 0).empty());
}

TEST_CASE("each block's group delay at its own input rate is (T-1)/2") {
  const auto taps = design_lowpass(32000.0, 4000.0, 5000.0, 60.0);
  ChannelFilter cf(taps);
  const std::size_t expected = (taps.size() - 1) / 2;
  CHECK(cf.group_delay_input_samples() == expected);

  std::vector<std::complex<float>> impulse(taps.size() * 2, {0.0F, 0.0F});
  impulse[0] = {1.0F, 0.0F};
  std::vector<std::complex<float>> out;
  cf.process(impulse, out);
  REQUIRE(out.size() == impulse.size());
  std::size_t peak = 0;
  for (std::size_t k = 0; k < out.size(); ++k) {
    if (std::abs(out[k]) > std::abs(out[peak])) peak = k;
  }
  CHECK(peak == expected);
}

TEST_CASE("a decimator's impulse peak lands within one output sample of its delay") {
  const auto taps = design_lowpass(256000.0, 12800.0, 16000.0, 60.0);
  FirDecimator d(taps, 8);
  const std::size_t delay_in = (taps.size() - 1) / 2;
  CHECK(d.group_delay_input_samples() == delay_in);

  std::vector<std::complex<float>> impulse(taps.size() * 3, {0.0F, 0.0F});
  impulse[0] = {1.0F, 0.0F};
  std::vector<std::complex<float>> out;
  d.process(impulse, out);
  REQUIRE(!out.empty());
  std::size_t peak = 0;
  for (std::size_t k = 0; k < out.size(); ++k) {
    if (std::abs(out[k]) > std::abs(out[peak])) peak = k;
  }
  // Output index k corresponds to input index k*factor + (factor-1). The delay
  // need not land on an output sample, so the assertion is a range of one
  // output sample rather than a single value it cannot always hit.
  const double peak_input = static_cast<double>(peak) * 8.0 + 7.0;
  CHECK(std::fabs(peak_input - static_cast<double>(delay_in)) <= 8.0);
}

TEST_CASE("chunk invariance: each stateful block, then the whole chain") {
  const std::size_t n = 200000;
  const auto input = noise_block(n, 4242);

  auto chunked = [&](auto&& run_whole, auto&& run_chunks) {
    auto whole = run_whole();
    auto pieces = run_chunks();
    REQUIRE(whole.size() == pieces.size());
    for (std::size_t i = 0; i < whole.size(); ++i) {
      REQUIRE_MESSAGE(whole[i] == pieces[i], "divergence at index " << i);
    }
  };

  std::mt19937 rng(7);
  std::uniform_int_distribution<std::size_t> chunk(1, 8192);

  SUBCASE("decimator") {
    const auto taps = design_lowpass(1024000.0, 51200.0, 64000.0, 60.0);
    chunked(
        [&] {
          FirDecimator d(taps, 8);
          std::vector<std::complex<float>> out;
          d.process(input, out);
          return out;
        },
        [&] {
          FirDecimator d(taps, 8);
          std::vector<std::complex<float>> out;
          std::size_t i = 0;
          while (i < input.size()) {
            const std::size_t len = std::min(chunk(rng), input.size() - i);
            d.process(std::span<const std::complex<float>>(input).subspan(i, len), out);
            i += len;
          }
          return out;
        });
  }

  SUBCASE("mixer") {
    chunked(
        [&] {
          OffsetMixer m(1024000, 250000);
          std::vector<std::complex<float>> v = input;
          m.process(v);
          return v;
        },
        [&] {
          OffsetMixer m(1024000, 250000);
          std::vector<std::complex<float>> v = input;
          std::size_t i = 0;
          while (i < v.size()) {
            const std::size_t len = std::min(chunk(rng), v.size() - i);
            m.process(std::span<std::complex<float>>(v).subspan(i, len));
            i += len;
          }
          return v;
        });
  }

  SUBCASE("am demodulator") {
    chunked(
        [&] {
          AmDemodulator a(32000);
          std::vector<float> out;
          a.process(std::span<const std::complex<float>>(input).subspan(0, 50000), out);
          return out;
        },
        [&] {
          AmDemodulator a(32000);
          std::vector<float> out;
          std::size_t i = 0;
          while (i < 50000) {
            const std::size_t len = std::min(chunk(rng), std::size_t{50000} - i);
            a.process(std::span<const std::complex<float>>(input).subspan(i, len), out);
            i += len;
          }
          return out;
        });
  }

  SUBCASE("whole chain") {
    const Config cfg = chain_config();
    chunked(
        [&] {
          Chain c = build_chain(cfg);
          std::vector<std::complex<float>> ch;
          std::vector<float> au;
          c.process(input, ch, au);
          return ch;
        },
        [&] {
          Chain c = build_chain(cfg);
          std::vector<std::complex<float>> ch;
          std::vector<float> au;
          std::size_t i = 0;
          while (i < input.size()) {
            const std::size_t len = std::min(chunk(rng), input.size() - i);
            c.process(std::span<const std::complex<float>>(input).subspan(i, len), ch, au);
            i += len;
          }
          return ch;
        });
  }
}

TEST_CASE("reset restores a block to its constructed state") {
  const auto taps = design_lowpass(32000.0, 4000.0, 5000.0, 60.0);
  const auto input = noise_block(4096, 11);

  ChannelFilter a(taps);
  std::vector<std::complex<float>> first;
  a.process(input, first);
  a.reset();
  std::vector<std::complex<float>> second;
  a.process(input, second);
  REQUIRE(first.size() == second.size());
  for (std::size_t i = 0; i < first.size(); ++i) CHECK(first[i] == second[i]);
}

TEST_CASE("chain latency is an exact rational whose ceiling and residual agree") {
  const Chain chain = build_chain(chain_config());
  const Latency l = chain.latency();
  CHECK(l.den > 0);
  CHECK(l.num > 0);

  const double exact = static_cast<double>(l.num) / static_cast<double>(l.den);
  CHECK(static_cast<double>(l.ceil_samples) >= exact);
  CHECK(l.residual == doctest::Approx(static_cast<double>(l.ceil_samples) - exact));
  CHECK(l.residual >= 0.0);
  CHECK(l.residual < 1.0);
  CHECK(chain.latency_samples() == l.ceil_samples);

  // The measured impulse peak equals floor or ceil of the rational latency. The
  // assertion is a two-value set on purpose: a fractional latency cannot land
  // on one integer, and demanding that it does is what makes a correct
  // implementation look broken and invites a widened tolerance.
  Chain c = build_chain(chain_config());
  std::vector<std::complex<float>> impulse(400000, {0.0F, 0.0F});
  impulse[0] = {1.0F, 0.0F};
  std::vector<std::complex<float>> ch;
  std::vector<float> au;
  c.process(impulse, ch, au);
  REQUIRE(!ch.empty());
  std::size_t peak = 0;
  for (std::size_t k = 0; k < ch.size(); ++k) {
    if (std::abs(ch[k]) > std::abs(ch[peak])) peak = k;
  }
  const std::size_t floor_v = static_cast<std::size_t>(l.num / l.den);
  CHECK_MESSAGE((peak == floor_v || peak == l.ceil_samples),
                "impulse peak at " << peak << ", expected " << floor_v << " or "
                                   << l.ceil_samples);
}

TEST_CASE("a deliberately fractional latency exercises the rational path") {
  // 131 taps at factor 8 feeding 65 taps at factor 4, plus a channel filter:
  //   num/den = 65/32 + 32/4 + d_channel  ->  denominator 32, numerator odd.
  Chain c;
  c.channel_rate_hz = 32000;
  c.stages.emplace_back(std::vector<float>(131, 0.0F), 8);
  c.stages.emplace_back(std::vector<float>(65, 0.0F), 4);
  c.channel = ChannelFilter(std::vector<float>(101, 0.0F));

  const Latency l = c.latency();
  CHECK(l.den == 32);
  CHECK(l.residual > 0.0);
  CHECK(l.residual < 1.0);
  const double exact = static_cast<double>(l.num) / static_cast<double>(l.den);
  //   65/32 + 8 + 50 = 60.03125
  CHECK(exact == doctest::Approx(60.03125));
  CHECK(l.ceil_samples == 61);
}

TEST_CASE("the mixer accumulator is exact and drift-free over sixty seconds") {
  const std::uint32_t fs = 1024000;
  const std::int64_t offset = 250000;
  OffsetMixer m(fs, offset);
  CHECK(m.applied_offset_hz() == offset);

  // Every integer offset is exactly representable in an accumulator modulo the
  // sample rate, so the applied offset equals the request and the accumulator
  // matches exact integer arithmetic sample for sample.
  const std::size_t block = 1 << 16;
  std::vector<std::complex<float>> buf(block, {1.0F, 0.0F});
  std::int64_t n = 0;
  const std::int64_t total = static_cast<std::int64_t>(fs) * 60;
  while (n < total) {
    const std::size_t len = static_cast<std::size_t>(
        std::min<std::int64_t>(static_cast<std::int64_t>(block), total - n));
    m.process(std::span<std::complex<float>>(buf).subspan(0, len));
    n += static_cast<std::int64_t>(len);
  }
  const std::int64_t exact = (n % static_cast<std::int64_t>(fs)) * offset %
                             static_cast<std::int64_t>(fs);
  const std::int64_t expected = ((n * offset) % static_cast<std::int64_t>(fs));
  CHECK(expected == exact);
  const double phase_error =
      std::fabs(static_cast<double>(m.phase_accumulator() - expected)) / static_cast<double>(fs);
  CHECK(phase_error < 1e-6);
}

TEST_CASE("a tone at minus the offset lands at DC") {
  const std::uint32_t fs = 1024000;
  const std::int64_t offset = 250000;
  const std::size_t n = 8192;

  std::vector<std::complex<float>> x(n);
  for (std::size_t k = 0; k < n; ++k) {
    const double ph = -2.0 * kPi * static_cast<double>(offset) * static_cast<double>(k) /
                      static_cast<double>(fs);
    x[k] = {static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph))};
  }
  OffsetMixer m(fs, offset);
  m.process(x);

  Spectrum spec(2048, 4);
  const auto psd = spec.welch_psd(x);
  REQUIRE(!psd.empty());
  const double bin_hz = static_cast<double>(fs) / 2048.0;
  const double off = spec.carrier_offset_hz(psd, bin_hz);
  CHECK(std::fabs(off) < bin_hz);
}

TEST_CASE("byte conversion at the rails and the midpoint") {
  // The normalisation of spec section 8: (x - 127.4f) / 127.5f.
  auto convert = [](int byte) {
    return (static_cast<float>(byte) - 127.4F) / 127.5F;
  };
  CHECK(convert(0) == doctest::Approx((0.0 - 127.4) / 127.5).epsilon(1e-6));
  CHECK(convert(127) == doctest::Approx((127.0 - 127.4) / 127.5).epsilon(1e-6));
  CHECK(convert(128) == doctest::Approx((128.0 - 127.4) / 127.5).epsilon(1e-6));
  CHECK(convert(255) == doctest::Approx((255.0 - 127.4) / 127.5).epsilon(1e-6));
  CHECK(convert(0) > -1.001F);
  CHECK(convert(255) < 1.001F);
}

TEST_CASE("frame powers") {
  std::vector<std::complex<float>> unit(4096, {1.0F, 0.0F});
  const auto p = frame_powers(unit, kFrameLength, kFrameHop);
  REQUIRE(p.size() == 1 + (4096 - kFrameLength) / kFrameHop);
  for (double v : p) CHECK(v == doctest::Approx(1.0));

  // Shorter than one frame yields nothing, not a partial frame.
  std::vector<std::complex<float>> tiny(100, {1.0F, 0.0F});
  CHECK(frame_powers(tiny, kFrameLength, kFrameHop).empty());

  std::vector<float> real(2048, 0.5F);
  const auto pr = frame_powers(real, kFrameLength, kFrameHop);
  REQUIRE(pr.size() == 3);
  for (double v : pr) CHECK(v == doctest::Approx(0.25));
}

TEST_CASE("decimation rejects an out-of-band tone by at least 55 dB") {
  const std::uint32_t fs = 1024000;
  const std::size_t n = 262144;
  const Config cfg = chain_config();

  auto run = [&](double tone_hz) {
    std::vector<std::complex<float>> x(n);
    for (std::size_t k = 0; k < n; ++k) {
      const double ph = 2.0 * kPi * tone_hz * static_cast<double>(k) / static_cast<double>(fs);
      x[k] = {static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph))};
    }
    Chain c = build_chain(cfg);
    std::vector<std::complex<float>> ch;
    std::vector<float> au;
    c.process(x, ch, au);
    // Skip the transient before measuring.
    const std::size_t skip = std::min(ch.size() / 2, std::size_t{2000});
    double sum = 0.0;
    for (std::size_t k = skip; k < ch.size(); ++k) sum += std::norm(ch[k]);
    return sum / static_cast<double>(ch.size() - skip);
  };

  // In band, at the tuning offset so the mixer brings it to DC.
  const double in_band = run(-250000.0 + 1000.0);
  // In the first stage's stopband, referred to the same mixer offset.
  const double out_band = run(-250000.0 + 200000.0);
  const double rejection_db = 10.0 * std::log10(in_band / std::max(out_band, 1e-300));
  CHECK_MESSAGE(rejection_db > 55.0, "rejection was " << rejection_db << " dB");
}

TEST_CASE("the spectrum recovers a tone position to a fraction of a bin") {
  const std::size_t n = 8192;
  const std::uint32_t fs = 32000;
  Spectrum spec(1024, 8);
  const double bin_hz = static_cast<double>(fs) / 1024.0;

  for (double tone : {0.0, 100.0, -250.0, 731.0}) {
    std::vector<std::complex<float>> x(n);
    for (std::size_t k = 0; k < n; ++k) {
      const double ph = 2.0 * kPi * tone * static_cast<double>(k) / static_cast<double>(fs);
      x[k] = {static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph))};
    }
    const auto psd = spec.welch_psd(x);
    REQUIRE(psd.size() == 1024);
    const double got = spec.carrier_offset_hz(psd, bin_hz, 2000.0);
    CHECK_MESSAGE(std::fabs(got - tone) < 0.1 * bin_hz,
                  "tone " << tone << " Hz recovered as " << got << " Hz");
  }
}

TEST_CASE("Welch averaging keeps the noise peak-to-median ratio well below a carrier's") {
  // This is the measurement WP5's carrier-prominence calibration rests on: an
  // unaveraged periodogram over 1024 bins already reaches about 10 dB on pure
  // noise, which is why averaging is required rather than optional.
  Spectrum spec(1024, 16);
  const auto noise = noise_block(1024 * 16, 99);
  const auto psd_noise = spec.welch_psd(noise);
  REQUIRE(!psd_noise.empty());
  const double noise_prominence = spec.peak_prominence_db(psd_noise, 128.0);
  CHECK_MESSAGE(noise_prominence < 8.0,
                "noise peak-to-median was " << noise_prominence << " dB");

  auto carrier = noise;
  for (std::size_t k = 0; k < carrier.size(); ++k) {
    carrier[k] += std::complex<float>(0.4F, 0.0F);
  }
  const auto psd_carrier = spec.welch_psd(carrier);
  const double carrier_prominence = spec.peak_prominence_db(psd_carrier, 128.0);
  CHECK(carrier_prominence > noise_prominence + 10.0);
}

TEST_CASE("the audio path's own delay is an exact integer and its IIR residual is small") {
  AmDemodulator a(32000);
  const std::size_t delay = a.latency_samples();
  CHECK(delay > 0);

  // A carrier burst: the envelope steps up, and the shaped audio output's
  // response to that step must arrive within the guard interval of the
  // uncompensated single-pole sections.
  const std::size_t n = 32000;
  std::vector<std::complex<float>> x(n, {0.0F, 0.0F});
  for (std::size_t k = n / 4; k < n / 2; ++k) {
    const double ph = 2.0 * kPi * 1000.0 * static_cast<double>(k) / 32000.0;
    x[k] = {static_cast<float>(0.5 * (1.0 + 0.5 * std::sin(ph))), 0.0F};
  }
  std::vector<float> audio;
  a.process(x, audio);
  REQUIRE(audio.size() == n);

  std::size_t onset = 0;
  for (std::size_t k = n / 4; k < n; ++k) {
    if (std::fabs(audio[k]) > 0.01F) {
      onset = k;
      break;
    }
  }
  REQUIRE(onset > 0);
  const double lag_samples = static_cast<double>(onset) - static_cast<double>(n / 4);
  const double lag_ms = lag_samples / 32000.0 * 1000.0;
  // The guard interval is 50 ms by default; the residual must be well inside it.
  CHECK_MESSAGE(lag_ms < 50.0, "audio onset lagged by " << lag_ms << " ms");
}

}  // TEST_SUITE
