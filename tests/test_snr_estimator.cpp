// WP5 Tests A, B, C, D, F, G — the measurement itself. Spec sections 9.3, 9.4,
// 9.4.1, 9.4.2, and 9.5.
//
// Read this before changing a low-SNR case. Both Test B and Test D exercise a
// true SNR of 0 dB. At 0 dB the TOTAL in-channel power is 10*log10(2) = 3.01 dB
// above the noise floor, which is below the production default open_db of 6.0,
// so the squelch never opens, no event is detected, and there is no median to
// assert on. Every low-SNR case therefore uses the test-only squelch of
// with_test_squelch(), and each is PAIRED with a production-default case
// asserting the documented behaviour at open_db = 6.0. Do not raise open_db
// back to the default in a low-SNR case to make it pass, and do not lower the
// production default to make the test simpler: the two settings answer two
// different questions.

#include <doctest/doctest.h>

#include "core/config.h"
#include "core/db.h"
#include "dsp/chain.h"
#include "dsp/framer.h"
#include "dsp/spectrum.h"
#include "experiment/capture_runner.h"
#include "metrics/event_detector.h"
#include "metrics/noise_floor.h"
#include "metrics/snr_estimator.h"
#include "tests/support/signal_fixtures.h"

#include <cmath>
#include <vector>

using namespace rtlangle;
using namespace rtlangle::metrics;
using namespace rtlangle::test;

namespace {

Config base_config() {
  Config c;
  c.center_hz = 118350000;
  c.duration_s = 4.0;
  return c;
}

CaptureOutcome make_capture(std::vector<std::complex<float>> channel,
                            std::vector<float> audio, std::size_t audio_latency = 0) {
  CaptureOutcome out;
  out.status = CaptureOutcome::Status::Ok;
  out.started_utc = std::chrono::system_clock::now();
  out.channel = std::move(channel);
  out.audio = std::move(audio);
  out.audio_latency_samples = audio_latency;
  return out;
}

// Runs the whole measurement path over an already-channel-rate stream.
AttemptRecord measure(const Config& cfg, const std::vector<std::complex<float>>& channel,
                      const std::vector<float>& audio, std::size_t audio_latency = 0) {
  dsp::Spectrum spectrum(1024, 16);
  const auto channel_powers = dsp::frame_powers(channel, dsp::kFrameLength, dsp::kFrameHop);
  const auto audio_powers = dsp::frame_powers(audio, dsp::kFrameLength, dsp::kFrameHop);
  const NoiseFloor cf = estimate_noise_floor(channel_powers, channel, cfg, spectrum);
  const NoiseFloor af = audio.empty()
                            ? NoiseFloor{FloorStatus::Unreliable, 0, 0, 0, 0, 0, "no audio"}
                            : estimate_noise_floor(audio_powers, audio, cfg, spectrum);
  const auto events = detect_events(channel_powers, cf, cfg);
  AttemptRecord rec;
  const CaptureOutcome capture = make_capture(channel, audio, audio_latency);
  estimate_snr(capture, cf, af, events, cfg, rec, &spectrum);
  return rec;
}

// A constant-envelope channel-rate stream: quiet at magnitude sqrt(noise), and
// `burst_power` inside each burst. Frame powers are then exactly the two
// levels, so the percentile floor and the event median are exact and Test D can
// assert an exact arithmetic identity rather than a statistical one.
std::vector<std::complex<float>> two_level_stream(std::size_t n, double quiet_power,
                                                  double burst_power,
                                                  std::span<const Burst> bursts) {
  std::vector<std::complex<float>> v(n);
  const float quiet_mag = static_cast<float>(std::sqrt(quiet_power));
  for (std::size_t k = 0; k < n; ++k) {
    // A sign flip every sample keeps the mean at zero, so the stream is not a
    // DC carrier the persistent-carrier test would see.
    const float s = (k % 2 == 0) ? 1.0F : -1.0F;
    v[k] = {quiet_mag * s, 0.0F};
  }
  const float burst_mag = static_cast<float>(std::sqrt(burst_power));
  for (const Burst& b : bursts) {
    for (std::size_t k = b.first; k < std::min(b.last, n); ++k) {
      const float s = (k % 2 == 0) ? 1.0F : -1.0F;
      v[k] = {burst_mag * s, 0.0F};
    }
  }
  return v;
}

// Bursts separated by `gap` samples. The gap must exceed merge_gap_ms plus one
// frame length, or the detector merges the bursts into a single event - which
// is correct behaviour, and would make a test that expected several events fail
// for a reason that has nothing to do with the estimator.
std::vector<Burst> spaced_bursts(std::size_t n, int count, std::size_t length,
                                 std::size_t gap, std::size_t lead_in) {
  std::vector<Burst> bursts;
  std::size_t cursor = lead_in;
  for (int i = 0; i < count; ++i) {
    if (cursor + length >= n) break;
    bursts.push_back({cursor, cursor + length});
    cursor += length + gap;
  }
  return bursts;
}

}  // namespace

TEST_SUITE("snr_estimator") {

// ---------------------------------------------------------------------------
// Test D — the forbidden metric. Written first, so the first implementation
// cannot accidentally ship P/N.
// ---------------------------------------------------------------------------
TEST_CASE("Test D: the reported SNR subtracts the floor before dividing") {
  const Config cfg = with_test_squelch(base_config());
  const std::size_t n = static_cast<std::size_t>(cfg.duration_s * cfg.channel_rate_hz);
  const double quiet_power = 1e-4;
  // P_event = 2N exactly, which is a true SNR of 0 dB.
  const auto bursts = spaced_bursts(n, 5, cfg.channel_rate_hz * 2 / 5,
                                    cfg.channel_rate_hz * 3 / 5, 4096);
  const auto channel = two_level_stream(n, quiet_power, 2.0 * quiet_power, bursts);

  const AttemptRecord rec = measure(cfg, channel, {});
  REQUIRE(rec.valid_event_count >= 3);
  REQUIRE(rec.capture_score_channel_db.has_value());
  REQUIRE(rec.channel_snr.has_value());

  CHECK(std::fabs(rec.channel_snr->median - 0.0) < 1.0);
  // 10*log10(P/N) with P = 2N reports 3.0103 dB. It must not.
  CHECK(std::fabs(rec.channel_snr->median - 3.0103) > 0.5);
}

TEST_CASE("Test D, paired production default: a 3 dB total lift detects nothing") {
  const Config cfg = base_config();   // open_db 6.0
  const std::size_t n = static_cast<std::size_t>(cfg.duration_s * cfg.channel_rate_hz);
  const double quiet_power = 1e-4;
  const auto bursts = spaced_bursts(n, 5, cfg.channel_rate_hz * 2 / 5,
                                    cfg.channel_rate_hz * 3 / 5, 4096);
  const auto channel = two_level_stream(n, quiet_power, 2.0 * quiet_power, bursts);

  const AttemptRecord rec = measure(cfg, channel, {});
  CHECK(rec.valid_event_count == 0);
  CHECK_FALSE(rec.capture_score_channel_db.has_value());
  // The weakest SNR this tool can report at the default squelch, which is why
  // nothing was detected. It is a property of the measurement, not a fault.
  CHECK(minimum_detectable_snr_db(cfg.open_db) == doctest::Approx(4.7437).epsilon(1e-3));
}

// ---------------------------------------------------------------------------
// Test A — the estimator in isolation, tolerance 0.5 dB.
// ---------------------------------------------------------------------------
TEST_CASE("Test A: the estimator recovers a measured reference within 0.5 dB") {
  const Config cfg = with_test_squelch(base_config());
  const std::size_t n = static_cast<std::size_t>(cfg.duration_s * cfg.channel_rate_hz);
  const double noise_power = 1e-4;

  for (double target_db : {3.0, 8.0, 15.0}) {
    const auto noise = awgn(n, noise_power, 4242);
    const double signal_power = noise_power * from_db(target_db);
    const auto bursts = spaced_bursts(n, 5, cfg.channel_rate_hz * 2 / 5,
                                      cfg.channel_rate_hz * 3 / 5, 4096);
    // A constant-envelope tone, so the frame median and the frame mean agree
    // and the reference is unambiguous.
    const auto signal = am_signal(n, cfg.channel_rate_hz, 900.0, 0.0, 0.0,
                                  std::sqrt(signal_power), bursts);
    const auto combined = add(noise, signal);

    // The reference is MEASURED from the separated streams, never taken from
    // the nominal input: the 20th-percentile floor reads about 0.12 dB low for
    // 1024-sample frames, so asserting against the nominal value would put the
    // test inside the estimator's own bias budget.
    std::vector<std::complex<float>> signal_only;
    for (const Burst& b : bursts) {
      for (std::size_t k = b.first; k < b.last; ++k) signal_only.push_back(signal[k]);
    }
    const double reference_db =
        to_db(mean_power(signal_only) / mean_power(const_cast<std::vector<std::complex<float>>&>(noise)));

    const AttemptRecord rec = measure(cfg, combined, {});
    REQUIRE_MESSAGE(rec.capture_score_channel_db.has_value(),
                    "no channel score at target " << target_db << " dB");
    CHECK_MESSAGE(std::fabs(*rec.capture_score_channel_db - reference_db) < 0.5,
                  "target " << target_db << " dB: measured "
                            << *rec.capture_score_channel_db << " dB against reference "
                            << reference_db << " dB");
  }
}

// ---------------------------------------------------------------------------
// Test C — the negative control.
// ---------------------------------------------------------------------------
TEST_CASE("Test C: pure noise yields no events and a perfectly good floor") {
  const Config cfg = base_config();
  const std::size_t n = static_cast<std::size_t>(cfg.duration_s * cfg.channel_rate_hz);
  const auto noise = awgn(n, 1e-4, 909);

  dsp::Spectrum spectrum(1024, 16);
  const auto powers = dsp::frame_powers(noise, dsp::kFrameLength, dsp::kFrameHop);
  const NoiseFloor nf = estimate_noise_floor(powers, noise, cfg, spectrum);
  // Pure noise is a perfectly good floor; it must not be flagged unreliable.
  CHECK(nf.status == FloorStatus::Reliable);

  const AttemptRecord rec = measure(cfg, noise, {});
  CHECK(rec.valid_event_count == 0);
  CHECK_FALSE(rec.capture_score_channel_db.has_value());
  CHECK_FALSE(rec.channel_snr.has_value());
  CHECK(rec.events_per_minute == doctest::Approx(0.0));
}

// ---------------------------------------------------------------------------
// Test B — the full chain, tolerance 1.5 dB.
// ---------------------------------------------------------------------------
TEST_CASE("Test B: the full chain recovers a measured reference within 1.5 dB") {
  Config cfg = with_test_squelch(base_config());
  cfg.duration_s = 4.0;
  const double fs = static_cast<double>(cfg.sample_rate_hz);
  const std::size_t n = static_cast<std::size_t>(cfg.duration_s * fs);
  const double noise_power = 1e-4;
  const double carrier_hz = -static_cast<double>(cfg.offset_tune_hz);

  // Bursts at the input rate, long enough to survive the channel-rate framing
  // and separated by more than merge_gap_ms plus a frame.
  const auto bursts = spaced_bursts(n, 4, static_cast<std::size_t>(fs * 0.35),
                                    static_cast<std::size_t>(fs * 0.45),
                                    static_cast<std::size_t>(fs * 0.15));

  dsp::Chain probe = dsp::build_chain(cfg);
  const std::size_t trim = probe.latency_samples() + 2 * probe.total_taps_at_channel_rate();
  const double decim = fs / static_cast<double>(cfg.channel_rate_hz);

  auto through_chain = [&](const std::vector<std::complex<float>>& x) {
    dsp::Chain c = dsp::build_chain(cfg);
    std::vector<std::complex<float>> ch;
    std::vector<float> au;
    c.process(x, ch, au);
    return ch;
  };
  // The channel-rate samples inside the bursts, with a margin at each edge so
  // the filters' rise and fall are excluded from the reference.
  auto burst_samples = [&](const std::vector<std::complex<float>>& ch) {
    std::vector<std::complex<float>> v;
    for (const Burst& b : bursts) {
      const std::size_t f = static_cast<std::size_t>(static_cast<double>(b.first) / decim) + 256;
      const std::size_t l = static_cast<std::size_t>(static_cast<double>(b.last) / decim) - 256;
      for (std::size_t k = f; k < std::min(l, ch.size()); ++k) v.push_back(ch[k]);
    }
    return v;
  };

  const auto noise = awgn(n, noise_power, 5150);
  const auto noise_out = through_chain(noise);
  std::vector<std::complex<float>> noise_settled(
      noise_out.begin() + static_cast<std::ptrdiff_t>(trim), noise_out.end());
  const double p_noise = mean_power(noise_settled);

  // The chain's noise-equivalent bandwidth is far narrower than the input band,
  // so an input-band amplitude ratio is not a channel SNR. The signal amplitude
  // is calibrated against the measured post-chain noise power, which is what
  // makes the sweep points genuinely 0, 5, 10, and 20 dB IN THE CHANNEL - the
  // quantity spec section 9.3 defines and the squelch acts on.
  const auto unit_signal = am_signal(n, fs, carrier_hz, 1000.0, 0.6, 1.0, bursts);
  const double p_signal_unit = mean_power(burst_samples(through_chain(unit_signal)));
  REQUIRE(p_signal_unit > 0.0);

  std::vector<double> measured;
  std::vector<double> reference;
  for (double target_db : {0.0, 5.0, 10.0, 20.0}) {
    const double amplitude = std::sqrt(from_db(target_db) * p_noise / p_signal_unit);
    const auto signal = am_signal(n, fs, carrier_hz, 1000.0, 0.6, amplitude, bursts);

    // The reference is the same separated streams through the IDENTICAL chain,
    // so the cascade's noise-equivalent bandwidth is accounted for without any
    // hand-derived constant.
    const double ref_db = to_db(mean_power(burst_samples(through_chain(signal))) / p_noise);

    const auto combined_out = through_chain(add(noise, signal));
    std::vector<std::complex<float>> trimmed(
        combined_out.begin() + static_cast<std::ptrdiff_t>(trim), combined_out.end());

    const AttemptRecord rec = measure(cfg, trimmed, {});
    REQUIRE_MESSAGE(rec.capture_score_channel_db.has_value(),
                    "no channel score at target " << target_db << " dB");
    CHECK_MESSAGE(std::fabs(*rec.capture_score_channel_db - ref_db) < 1.5,
                  "target " << target_db << " dB: measured "
                            << *rec.capture_score_channel_db << " dB against reference "
                            << ref_db << " dB");
    measured.push_back(*rec.capture_score_channel_db);
    reference.push_back(ref_db);
  }

  // Adjacent-step differences track the true differences, so a constant bias
  // cannot hide a scale error.
  for (std::size_t i = 1; i < measured.size(); ++i) {
    const double measured_step = measured[i] - measured[i - 1];
    const double true_step = reference[i] - reference[i - 1];
    CHECK_MESSAGE(std::fabs(measured_step - true_step) < 1.0,
                  "step " << i << ": measured " << measured_step << " dB against true "
                          << true_step << " dB");
  }
}

TEST_CASE("Test B, paired production default: 20 dB is detected and 0 dB is not") {
  Config cfg = base_config();   // open_db 6.0
  cfg.duration_s = 4.0;
  const double fs = static_cast<double>(cfg.sample_rate_hz);
  const std::size_t n = static_cast<std::size_t>(cfg.duration_s * fs);
  const double noise_power = 1e-4;
  const double carrier_hz = -static_cast<double>(cfg.offset_tune_hz);

  const auto bursts = spaced_bursts(n, 4, static_cast<std::size_t>(fs * 0.35),
                                    static_cast<std::size_t>(fs * 0.45),
                                    static_cast<std::size_t>(fs * 0.15));

  dsp::Chain probe = dsp::build_chain(cfg);
  const std::size_t trim = probe.latency_samples() + 2 * probe.total_taps_at_channel_rate();
  const double decim = fs / static_cast<double>(cfg.channel_rate_hz);

  auto through_chain = [&](const std::vector<std::complex<float>>& x) {
    dsp::Chain c = dsp::build_chain(cfg);
    std::vector<std::complex<float>> ch;
    std::vector<float> au;
    c.process(x, ch, au);
    return ch;
  };
  auto burst_samples = [&](const std::vector<std::complex<float>>& ch) {
    std::vector<std::complex<float>> v;
    for (const Burst& b : bursts) {
      const std::size_t f = static_cast<std::size_t>(static_cast<double>(b.first) / decim) + 256;
      const std::size_t l = static_cast<std::size_t>(static_cast<double>(b.last) / decim) - 256;
      for (std::size_t k = f; k < std::min(l, ch.size()); ++k) v.push_back(ch[k]);
    }
    return v;
  };

  const auto noise = awgn(n, noise_power, 5150);
  const auto noise_out = through_chain(noise);
  std::vector<std::complex<float>> noise_settled(
      noise_out.begin() + static_cast<std::ptrdiff_t>(trim), noise_out.end());
  const double p_noise = mean_power(noise_settled);
  const auto unit_signal = am_signal(n, fs, carrier_hz, 1000.0, 0.6, 1.0, bursts);
  const double p_signal_unit = mean_power(burst_samples(through_chain(unit_signal)));

  auto run_at = [&](double target_db) {
    const double amplitude = std::sqrt(from_db(target_db) * p_noise / p_signal_unit);
    const auto signal = am_signal(n, fs, carrier_hz, 1000.0, 0.6, amplitude, bursts);
    const auto combined = through_chain(add(noise, signal));
    std::vector<std::complex<float>> trimmed(
        combined.begin() + static_cast<std::ptrdiff_t>(trim), combined.end());
    return measure(cfg, trimmed, {});
  };

  CHECK(run_at(20.0).valid_event_count > 0);
  // At a true channel SNR of 0 dB the total in-channel power is only 3.01 dB
  // above the floor, which is below the production default open_db of 6.0.
  CHECK(run_at(0.0).valid_event_count == 0);
}

// ---------------------------------------------------------------------------
// Test F — audio SNR, absolute.
// ---------------------------------------------------------------------------
TEST_CASE("Test F: the audio SNR recovers a measured reference within 2.0 dB") {
  Config cfg = base_config();
  cfg.duration_s = 4.0;
  const double fs = static_cast<double>(cfg.sample_rate_hz);
  const std::size_t n = static_cast<std::size_t>(cfg.duration_s * fs);
  const double noise_power = 1e-4;
  const double carrier_hz = -static_cast<double>(cfg.offset_tune_hz);

  const auto bursts = spaced_bursts(n, 4, static_cast<std::size_t>(fs * 0.35),
                                    static_cast<std::size_t>(fs * 0.45),
                                    static_cast<std::size_t>(fs * 0.15));

  for (double target_db : {14.0, 18.0, 22.0}) {
    const auto noise = awgn(n, noise_power, 8080);
    const double amplitude = std::sqrt(noise_power * from_db(target_db));
    const auto signal = am_signal(n, fs, carrier_hz, 1000.0, 0.6, amplitude, bursts);

    auto audio_through_chain = [&](const std::vector<std::complex<float>>& x) {
      dsp::Chain c = dsp::build_chain(cfg);
      std::vector<std::complex<float>> ch;
      std::vector<float> au;
      c.process(x, ch, au);
      return au;
    };
    const auto noise_audio = audio_through_chain(noise);
    const auto signal_audio = audio_through_chain(signal);

    const double decim = fs / static_cast<double>(cfg.channel_rate_hz);
    std::vector<float> signal_active;
    for (const Burst& b : bursts) {
      const std::size_t f = static_cast<std::size_t>(static_cast<double>(b.first) / decim) + 256;
      const std::size_t l = static_cast<std::size_t>(static_cast<double>(b.last) / decim) - 256;
      for (std::size_t k = f; k < std::min(l, signal_audio.size()); ++k) {
        signal_active.push_back(signal_audio[k]);
      }
    }
    std::vector<float> noise_settled(noise_audio.begin() + 1024, noise_audio.end());
    const double ref_db = to_db(mean_power(signal_active) / mean_power(noise_settled));

    dsp::Chain c = dsp::build_chain(cfg);
    std::vector<std::complex<float>> ch;
    std::vector<float> au;
    c.process(add(noise, signal), ch, au);
    const std::size_t trim = c.latency_samples() + 2 * c.total_taps_at_channel_rate();
    std::vector<std::complex<float>> ch_trim(ch.begin() + static_cast<std::ptrdiff_t>(trim),
                                             ch.end());
    std::vector<float> au_trim(au.begin() + static_cast<std::ptrdiff_t>(trim), au.end());

    const AttemptRecord rec = measure(cfg, ch_trim, au_trim, c.audio.latency_samples());
    REQUIRE_MESSAGE(rec.capture_score_audio_db.has_value(),
                    "no audio score at target " << target_db << " dB");
    CHECK_MESSAGE(std::fabs(*rec.capture_score_audio_db - ref_db) < 2.0,
                  "target " << target_db << " dB: measured audio "
                            << *rec.capture_score_audio_db << " dB against reference "
                            << ref_db << " dB");
  }
}

// ---------------------------------------------------------------------------
// Test G — audio alignment.
// ---------------------------------------------------------------------------
TEST_CASE("Test G: the guarded audio window lies inside the transmission") {
  Config cfg = base_config();
  cfg.duration_s = 4.0;
  const std::size_t rate = cfg.channel_rate_hz;
  const std::size_t n = static_cast<std::size_t>(cfg.duration_s * static_cast<double>(rate));
  const double quiet = 1e-4;

  // A 400 ms burst, plus enough others to reach min_valid_events.
  const auto bursts = spaced_bursts(n, 4, rate * 400 / 1000, rate * 500 / 1000, rate / 10);
  const auto channel = two_level_stream(n, quiet, quiet * from_db(20.0), bursts);

  // The audio band mirrors the channel activity, delayed by the audio path.
  const std::size_t latency = 40;
  std::vector<float> audio(n, 0.0F);
  std::mt19937_64 rng(3);
  std::normal_distribution<float> g(0.0F, 0.01F);
  for (float& v : audio) v = g(rng);
  for (const Burst& b : bursts) {
    for (std::size_t k = b.first + latency; k < std::min(b.last + latency, n); ++k) {
      audio[k] += 0.3F * static_cast<float>(std::sin(2.0 * kPi * 1000.0 *
                                                     static_cast<double>(k) /
                                                     static_cast<double>(rate)));
    }
  }

  const AttemptRecord rec = measure(cfg, channel, audio, latency);
  REQUIRE(rec.valid_event_count >= 3);
  CHECK(rec.valid_audio_event_count >= 3);
  REQUIRE(rec.capture_score_audio_db.has_value());
  CHECK(*rec.capture_score_audio_db > 5.0);

  // The residual misalignment left by the uncompensated single-pole sections is
  // smaller than the guard interval, which is what the guard exists to absorb.
  const double guard_ms = cfg.audio_guard_ms;
  const double residual_ms = static_cast<double>(latency) / static_cast<double>(rate) * 1000.0;
  CHECK(residual_ms < guard_ms);
}

TEST_CASE("Test G: an event too short for the guarded window loses only its audio score") {
  Config cfg = base_config();
  cfg.duration_s = 4.0;
  cfg.min_event_ms = 100.0;
  cfg.min_audio_window_ms = 400.0;   // wider than any burst below can guard
  const std::size_t rate = cfg.channel_rate_hz;
  const std::size_t n = static_cast<std::size_t>(cfg.duration_s * static_cast<double>(rate));

  const auto bursts = spaced_bursts(n, 5, rate * 150 / 1000, rate * 500 / 1000, rate / 10);
  const auto channel = two_level_stream(n, 1e-4, 1e-4 * from_db(20.0), bursts);
  std::vector<float> audio(n, 0.0F);
  std::mt19937_64 rng(5);
  std::normal_distribution<float> g(0.0F, 0.01F);
  for (float& v : audio) v = g(rng);

  const AttemptRecord rec = measure(cfg, channel, audio, 40);
  CHECK(rec.valid_event_count >= 3);
  // Valid for the channel metric, ineligible for the audio one.
  CHECK(rec.valid_audio_event_count == 0);
  CHECK(rec.capture_score_channel_db.has_value());
  CHECK_FALSE(rec.capture_score_audio_db.has_value());
  CHECK(rec.audio_insufficient);
}

// ---------------------------------------------------------------------------
// Audio eligibility, one case per row of the spec section 9.4.2 table.
// ---------------------------------------------------------------------------
TEST_CASE("a reliable channel floor with an unreliable audio floor keeps the capture ok") {
  Config cfg = base_config();
  cfg.duration_s = 4.0;
  const std::size_t rate = cfg.channel_rate_hz;
  const std::size_t n = static_cast<std::size_t>(cfg.duration_s * static_cast<double>(rate));

  const auto bursts = spaced_bursts(n, 5, rate * 400 / 1000, rate * 600 / 1000, rate / 10);
  const auto channel = two_level_stream(n, 1e-4, 1e-4 * from_db(20.0), bursts);

  dsp::Spectrum spectrum(1024, 16);
  const auto channel_powers = dsp::frame_powers(channel, dsp::kFrameLength, dsp::kFrameHop);
  const NoiseFloor cf = estimate_noise_floor(channel_powers, channel, cfg, spectrum);
  REQUIRE(cf.status == FloorStatus::Reliable);

  NoiseFloor audio_unreliable;
  audio_unreliable.status = FloorStatus::Unreliable;
  audio_unreliable.detail = "the probe occupancy is above the limit";

  const auto events = detect_events(channel_powers, cf, cfg);
  AttemptRecord rec;
  const CaptureOutcome capture = make_capture(channel, std::vector<float>(n, 0.0F), 40);
  estimate_snr(capture, cf, audio_unreliable, events, cfg, rec, &spectrum);

  // The third row of the section 9.4.2 table: the capture is still ok, the
  // channel score is present, the audio score is absent, and status_detail
  // names the audio floor.
  CHECK(rec.capture_score_channel_db.has_value());
  CHECK_FALSE(rec.capture_score_audio_db.has_value());
  CHECK(rec.audio_insufficient);
  REQUIRE(rec.status_detail.has_value());
  CHECK(rec.status_detail->find("audio noise floor") != std::string::npos);
}

TEST_CASE("an unreliable channel floor leaves both scores absent and writes no metric") {
  Config cfg = base_config();
  const std::size_t n = static_cast<std::size_t>(cfg.duration_s * cfg.channel_rate_hz);
  const auto channel = two_level_stream(n, 1e-4, 1e-4, {});

  dsp::Spectrum spectrum(1024, 16);
  NoiseFloor unreliable;
  unreliable.status = FloorStatus::Unreliable;
  unreliable.detail = "the probe occupancy is above the limit";
  NoiseFloor audio_ok;
  audio_ok.status = FloorStatus::Reliable;
  audio_ok.power = 1e-6;

  AttemptRecord rec;
  const CaptureOutcome capture = make_capture(channel, std::vector<float>(n, 0.01F), 0);
  estimate_snr(capture, unreliable, audio_ok, {}, cfg, rec, &spectrum);

  CHECK_FALSE(rec.capture_score_channel_db.has_value());
  CHECK_FALSE(rec.capture_score_audio_db.has_value());
  CHECK_FALSE(rec.noise_floor_dbfs.has_value());
  CHECK(rec.valid_event_count == 0);
  // Nothing was invented: there is no partial result.
  CHECK(rec.events.empty());
  REQUIRE(rec.status_detail.has_value());
}

TEST_CASE("the audio-eligible count never exceeds the channel-eligible count") {
  Config cfg = base_config();
  cfg.duration_s = 3.0;
  const std::size_t rate = cfg.channel_rate_hz;
  const std::size_t n = static_cast<std::size_t>(cfg.duration_s * static_cast<double>(rate));

  for (std::uint64_t seed = 0; seed < 40; ++seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> burst_count(0, 8);
    std::uniform_int_distribution<std::size_t> length(rate / 20, rate / 2);
    std::vector<Burst> bursts;
    std::size_t cursor = rate / 10;
    const int count = burst_count(rng);
    for (int i = 0; i < count && cursor < n; ++i) {
      const std::size_t len = length(rng);
      if (cursor + len >= n) break;
      bursts.push_back({cursor, cursor + len});
      cursor += len + rate / 4;
    }
    const auto channel = two_level_stream(n, 1e-4, 1e-4 * from_db(18.0), bursts);
    std::vector<float> audio(n, 0.0F);
    std::normal_distribution<float> g(0.0F, 0.01F);
    for (float& v : audio) v = g(rng);
    for (const Burst& b : bursts) {
      for (std::size_t k = b.first + 40; k < std::min(b.last + 40, n); ++k) audio[k] += 0.2F;
    }

    const AttemptRecord rec = measure(cfg, channel, audio, 40);
    CHECK(rec.valid_audio_event_count <= rec.valid_event_count);
  }
}

// ---------------------------------------------------------------------------
// Yield, spec section 9.5.
// ---------------------------------------------------------------------------
TEST_CASE("the retention cap changes what is persisted and nothing that is measured") {
  Config cfg = base_config();
  cfg.duration_s = 8.0;
  const std::size_t rate = cfg.channel_rate_hz;
  const std::size_t n = static_cast<std::size_t>(cfg.duration_s * static_cast<double>(rate));

  const auto bursts = spaced_bursts(n, 7, rate * 350 / 1000, rate * 650 / 1000, rate / 20);
  const auto channel = two_level_stream(n, 1e-4, 1e-4 * from_db(15.0), bursts);

  const AttemptRecord full = measure(cfg, channel, {});
  REQUIRE(full.valid_event_count == 7);
  CHECK(full.events_total == 7);
  CHECK(full.events_retained == 7);
  CHECK(full.events.size() == 7);
  CHECK(full.events_per_minute == doctest::Approx(7.0 / (cfg.duration_s / 60.0)));

  Config capped = cfg;
  capped.max_retained_events = 4;
  const AttemptRecord small = measure(capped, channel, {});

  CHECK(small.events_retained == 4);
  CHECK(small.events.size() == 4);
  // Everything measured is unchanged: the cap touches only the persisted
  // detail, and events_total beside events_retained is what makes the
  // truncation visible instead of looking like a quiet capture.
  CHECK(small.events_total == full.events_total);
  CHECK(small.valid_event_count == full.valid_event_count);
  CHECK(small.events_per_minute == doctest::Approx(full.events_per_minute));
  CHECK(small.detected_fraction == doctest::Approx(full.detected_fraction));
  REQUIRE(small.capture_score_channel_db.has_value());
  CHECK(*small.capture_score_channel_db == doctest::Approx(*full.capture_score_channel_db));
  REQUIRE(small.channel_snr.has_value());
  CHECK(small.channel_snr->p25 == doctest::Approx(full.channel_snr->p25));
  CHECK(small.channel_snr->p75 == doctest::Approx(full.channel_snr->p75));
}

TEST_CASE("detected_fraction is the fraction of frames inside a valid event") {
  Config cfg = base_config();
  cfg.duration_s = 4.0;
  const std::size_t rate = cfg.channel_rate_hz;
  const std::size_t n = static_cast<std::size_t>(cfg.duration_s * static_cast<double>(rate));

  // Four 500 ms events in four seconds, separated by 450 ms.
  const auto bursts = spaced_bursts(n, 4, rate / 2, rate * 45 / 100, rate / 20);
  const auto channel = two_level_stream(n, 1e-4, 1e-4 * from_db(15.0), bursts);
  const AttemptRecord rec = measure(cfg, channel, {});

  REQUIRE(rec.valid_event_count == 4);
  // Four half-second events in four seconds is about half the frames.
  CHECK(rec.detected_fraction > 0.4);
  CHECK(rec.detected_fraction < 0.6);
  CHECK(rec.frames_active > 0);
  CHECK(rec.frames_total > rec.frames_active);
}

}  // TEST_SUITE
