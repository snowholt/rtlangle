// WP4 — the capture runner's timing, health, and trimming rules. Spec sections
// 6.3 and 8.5.

#include <doctest/doctest.h>

#include "core/config.h"
#include "dsp/chain.h"
#include "experiment/capture_runner.h"
#include "source/synthetic_source.h"
#include "tests/support/fake_sources.h"

#include <chrono>
#include <cmath>
#include <thread>

using namespace rtlangle;
using namespace std::chrono_literals;

namespace {

Config short_capture_config() {
  Config c;
  c.center_hz = 118350000;
  c.duration_s = 0.25;
  c.settle_s = 0.0;
  return c;
}

SyntheticParams params_for(const Config& c) {
  SyntheticParams p;
  p.sample_rate_hz = c.sample_rate_hz;
  p.center_hz = c.center_hz.value_or(118350000);
  p.offset_hz = c.offset_tune_hz;
  p.snr_db = 20.0;
  return p;
}

}  // namespace

TEST_SUITE("capture_runner") {

TEST_CASE("a clean capture collects exactly the requested channel samples") {
  const Config cfg = short_capture_config();
  SyntheticSource src(params_for(cfg));
  dsp::Chain chain = dsp::build_chain(cfg);
  CaptureRunner runner(src, chain, cfg);

  double last_fraction = -1.0;
  const CaptureOutcome out = runner.run([&](double f) { last_fraction = f; });

  CHECK(out.status == CaptureOutcome::Status::Ok);
  const std::size_t wanted =
      static_cast<std::size_t>(std::llround(cfg.duration_s * cfg.channel_rate_hz));
  CHECK(out.channel.size() == wanted);
  CHECK(out.audio.size() == wanted);
  CHECK(out.audio_latency_samples == chain.audio.latency_samples());
  CHECK(last_fraction > 0.0);
  CHECK(out.wall_duration_s >= 0.0);
}

TEST_CASE("started_utc is sampled before the first read") {
  Config cfg = short_capture_config();
  test::InstrumentedSyntheticSource src(params_for(cfg));
  src.set_first_read_delay(300ms);
  dsp::Chain chain = dsp::build_chain(cfg);
  CaptureRunner runner(src, chain, cfg);

  const auto before = std::chrono::system_clock::now();
  const CaptureOutcome out = runner.run({});
  const auto after = std::chrono::system_clock::now();

  CHECK(out.status == CaptureOutcome::Status::Ok);
  CHECK(out.started_utc >= before);
  // The timestamp precedes the delayed first read's completion by at least the
  // delay, which a timestamp taken after the capture loop could not do.
  CHECK(out.started_utc < after - 250ms);
}

TEST_CASE("host drops are a per-capture delta, never cumulative") {
  Config cfg = short_capture_config();
  cfg.duration_s = 0.05;
  dsp::Chain chain = dsp::build_chain(cfg);

  // A source whose cumulative count reads 5 during the first capture and 9
  // during the second must yield 5 and then 4, not 5 and then 9.
  test::ScriptedDropSource src({5, 5, 5, 5, 5, 5, 5, 5});
  CaptureRunner runner(src, chain, cfg);
  const CaptureOutcome first = runner.run({});
  CHECK(first.status == CaptureOutcome::Status::Ok);
  CHECK(first.host_dropped_samples == 0);

  test::ScriptedDropSource src2({5, 9, 9, 9, 9, 9, 9, 9});
  CaptureRunner runner2(src2, chain, cfg);
  const CaptureOutcome second = runner2.run({});
  CHECK(second.status == CaptureOutcome::Status::Ok);
  CHECK(second.host_dropped_samples == 4);
}

TEST_CASE("the applied settings are a snapshot taken at capture start") {
  Config cfg = short_capture_config();
  test::InstrumentedSyntheticSource src(params_for(cfg));
  SourceInfo at_start = src.info();
  at_start.applied_gain_tenth_db = 496;
  src.set_info(at_start);

  dsp::Chain chain = dsp::build_chain(cfg);
  CaptureRunner runner(src, chain, cfg);

  // Move info() after construction but before run(): the snapshot must be the
  // value in force when the capture began.
  SourceInfo changed = at_start;
  changed.applied_gain_tenth_db = 200;
  const CaptureOutcome out = runner.run([&](double) {
    SourceInfo mid = changed;
    src.set_info(mid);
  });

  CHECK(out.status == CaptureOutcome::Status::Ok);
  CHECK(out.applied.applied_gain_tenth_db == 496);
  CHECK(src.info().applied_gain_tenth_db == 200);
}

TEST_CASE("the transient trim removes the chain latency ceiling plus the guard") {
  Config cfg = short_capture_config();
  dsp::Chain chain = dsp::build_chain(cfg);
  const std::size_t trim = chain.latency_samples() + 2 * chain.total_taps_at_channel_rate();
  CHECK(trim > chain.latency_samples());

  // A step function: without the trim the output would begin inside the
  // filters' rise; with it the output begins in steady state.
  const std::size_t wanted =
      static_cast<std::size_t>(std::llround(cfg.duration_s * cfg.channel_rate_hz));
  test::ShortSource src(cfg.sample_rate_hz * 2);
  CaptureRunner runner(src, chain, cfg);
  const CaptureOutcome out = runner.run({});
  REQUIRE(out.status == CaptureOutcome::Status::Ok);
  REQUIRE(out.channel.size() == wanted);

  // The first retained sample already carries the steady-state level of the
  // constant input, rather than a fraction of it.
  const double first = std::abs(out.channel.front());
  const double middle = std::abs(out.channel[out.channel.size() / 2]);
  CHECK(first == doctest::Approx(middle).epsilon(0.05));
}

TEST_CASE("a stream that ends early yields InsufficientSamples") {
  Config cfg = short_capture_config();
  dsp::Chain chain = dsp::build_chain(cfg);
  test::ShortSource src(cfg.sample_rate_hz / 10);   // far short of 0.25 s
  CaptureRunner runner(src, chain, cfg);
  const CaptureOutcome out = runner.run({});
  CHECK(out.status == CaptureOutcome::Status::InsufficientSamples);
  CHECK(out.channel.empty());
  CHECK_FALSE(out.detail.empty());
}

TEST_CASE("clipping above the limit fails the capture and produces no samples") {
  Config cfg = short_capture_config();
  test::InstrumentedSyntheticSource src(params_for(cfg));
  src.set_clipped_fraction(cfg.max_clipped_fraction * 10.0);
  dsp::Chain chain = dsp::build_chain(cfg);
  CaptureRunner runner(src, chain, cfg);

  const CaptureOutcome out = runner.run({});
  CHECK(out.status == CaptureOutcome::Status::Clipped);
  CHECK(out.channel.empty());
  // The instruction is to restart at a lower fixed gain: the gain is never
  // reduced part-way through a session, because captures before and after would
  // not be comparable.
  CHECK(out.detail.find("lower fixed gain") != std::string::npos);
  CHECK(out.clipped_fraction == doctest::Approx(cfg.max_clipped_fraction * 10.0));
}

TEST_CASE("a stalled source yields Timeout at the configured budget, not later") {
  Config cfg = short_capture_config();
  cfg.duration_s = 0.2;
  cfg.read_timeout_factor = 1.5;
  cfg.read_timeout_slack_s = 0.2;   // budget 0.5 s
  dsp::Chain chain = dsp::build_chain(cfg);
  test::RingBackedSource src(test::RingBackedSource::Behaviour::Stall, 0ms);
  CaptureRunner runner(src, chain, cfg);

  const auto start = std::chrono::steady_clock::now();
  const CaptureOutcome out = runner.run({});
  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  CHECK(out.status == CaptureOutcome::Status::Timeout);
  CHECK(elapsed >= 0.4);
  CHECK(elapsed < 3.0);
  CHECK(out.channel.empty());
}

TEST_CASE("a worker failure becomes SourceError carrying the worker's text") {
  Config cfg = short_capture_config();
  dsp::Chain chain = dsp::build_chain(cfg);
  test::RingBackedSource src(test::RingBackedSource::Behaviour::FailAfterDelay, 30ms);
  CaptureRunner runner(src, chain, cfg);

  const CaptureOutcome out = runner.run({});
  CHECK(out.status == CaptureOutcome::Status::SourceError);
  CHECK(out.detail.find("simulated USB fault") != std::string::npos);
  CHECK(out.channel.empty());
}

TEST_CASE("a cancelled capture reports Cancelled") {
  Config cfg = short_capture_config();
  cfg.duration_s = 5.0;
  dsp::Chain chain = dsp::build_chain(cfg);
  test::RingBackedSource src(test::RingBackedSource::Behaviour::Stall, 0ms);
  CaptureRunner runner(src, chain, cfg);

  std::thread canceller([&] {
    std::this_thread::sleep_for(50ms);
    src.cancel();
  });
  const CaptureOutcome out = runner.run({});
  canceller.join();

  CHECK(out.status == CaptureOutcome::Status::Cancelled);
  CHECK(out.channel.empty());
}

TEST_CASE("the capture status names are the honest ones") {
  CHECK(to_string(CaptureOutcome::Status::Ok) == "ok");
  CHECK(to_string(CaptureOutcome::Status::Timeout) == "timeout");
  CHECK(to_string(CaptureOutcome::Status::SourceError) == "source_error");
  CHECK(to_string(CaptureOutcome::Status::Clipped) == "clipped");
  CHECK(to_string(CaptureOutcome::Status::InsufficientSamples) == "insufficient_samples");
  CHECK(to_string(CaptureOutcome::Status::Cancelled) == "cancelled");
}

}  // TEST_SUITE
