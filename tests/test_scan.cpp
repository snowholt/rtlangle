// WP11 — dwell placement and exposure accounting. Spec sections 12.5 and
// 12.5.1, whose proof these tests assert rather than restate.

#include <doctest/doctest.h>

#include "app/scan_command.h"
#include "source/synthetic_source.h"
#include "ui/scripted_terminal_ui.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>

using namespace rtlangle;
using namespace rtlangle::app;
using rtlangle::ui::ScriptedTerminalUi;

namespace {

Config scan_config() {
  Config c;
  c.center_hz = 118350000;
  return c;
}

// Whether a dwell centred at `center` can see a channel at `channel`.
bool covers(double center, double channel, double usable_half, double dc_exclusion) {
  const double offset = std::fabs(channel - center);
  return offset <= usable_half && offset >= dc_exclusion;
}

}  // namespace

TEST_SUITE("scan") {

TEST_CASE("the default dwell placement is the one the spec works through") {
  const Config cfg = scan_config();
  const DwellPlan plan = plan_dwells(cfg);

  //   U = 0.80 * 1024000 / 2 = 409600
  //   D = 30000
  //   S = U - D              = 379600
  CHECK(plan.usable_half_hz == doctest::Approx(409600.0));
  CHECK(plan.dc_exclusion_hz == doctest::Approx(30000.0));
  CHECK(plan.step_hz == doctest::Approx(379600.0));
  CHECK(plan.feasible);

  // Both conditions of the proof hold: 2D <= S <= U - D.
  CHECK(plan.step_hz >= 2.0 * plan.dc_exclusion_hz);
  CHECK(plan.step_hz <= plan.usable_half_hz - plan.dc_exclusion_hz);

  // Naming the number is deliberate: an assertion with no expected value is
  // what let the old 2U step survive a test meant to check coverage.
  CHECK(plan.centers.size() == 50);
  CHECK(plan.centers.front() == 118409600U);
  CHECK(plan.centers.back() == 137010000U);

  // Coverage of the last channel: c_49 - U <= scan_end <= c_49 - D.
  const double last = static_cast<double>(plan.centers.back());
  CHECK(last - plan.usable_half_hz <= static_cast<double>(cfg.scan_end_hz));
  CHECK(static_cast<double>(cfg.scan_end_hz) <= last - plan.dc_exclusion_hz);
}

TEST_CASE("every channel in the band ends with at least one exposure") {
  const Config cfg = scan_config();
  const DwellPlan plan = plan_dwells(cfg);
  const auto channels = scan_channels(cfg);
  REQUIRE(!channels.empty());
  CHECK(channels.front() == cfg.scan_start_hz);
  CHECK(channels.back() <= cfg.scan_end_hz);

  int uncovered = 0;
  for (std::uint32_t channel : channels) {
    int exposures = 0;
    for (std::uint32_t center : plan.centers) {
      if (covers(static_cast<double>(center), static_cast<double>(channel),
                 plan.usable_half_hz, plan.dc_exclusion_hz)) {
        ++exposures;
      }
    }
    if (exposures == 0) {
      ++uncovered;
      MESSAGE("no dwell covers " << channel << " Hz");
    }
  }
  CHECK(uncovered == 0);

  // Explicitly the first and the last, which are the two the proof singles out.
  auto exposures_for = [&](std::uint32_t channel) {
    int n = 0;
    for (std::uint32_t center : plan.centers) {
      if (covers(static_cast<double>(center), static_cast<double>(channel), plan.usable_half_hz,
                 plan.dc_exclusion_hz)) {
        ++n;
      }
    }
    return n;
  };
  CHECK(exposures_for(channels.front()) >= 1);
  CHECK(exposures_for(channels.back()) >= 1);
}

TEST_CASE("the regression fixture: stepping by the whole usable span leaves holes") {
  // Without this fixture the coverage test above could pass for the wrong
  // reason. Consecutive dwells stepping by 2U abut at their outer edges, and
  // the 2D-wide hole around every centre is covered by no dwell at all - 60 kHz
  // against a 25 kHz channel spacing at the defaults, so two channels per dwell
  // would be permanently invisible.
  const Config cfg = scan_config();
  const DwellPlan good = plan_dwells(cfg);
  const DwellPlan bad = plan_dwells(cfg, 2.0 * good.usable_half_hz);
  const auto channels = scan_channels(cfg);

  auto uncovered_with = [&](const DwellPlan& plan) {
    int n = 0;
    for (std::uint32_t channel : channels) {
      bool seen = false;
      for (std::uint32_t center : plan.centers) {
        if (covers(static_cast<double>(center), static_cast<double>(channel),
                   plan.usable_half_hz, plan.dc_exclusion_hz)) {
          seen = true;
          break;
        }
      }
      if (!seen) ++n;
    }
    return n;
  };

  CHECK(uncovered_with(bad) > 0);
  CHECK(uncovered_with(good) == 0);
  // And the cost of the correct rule: roughly twice as many dwells.
  CHECK(good.centers.size() > bad.centers.size());
}

TEST_CASE("an infeasible configuration is refused before any dwell is generated") {
  Config cfg = scan_config();
  cfg.scan_dc_exclusion_hz = 256000;   // U = 409600, 3D = 768000
  CHECK_FALSE(plan_dwells(cfg).feasible);

  bool refused = false;
  for (const ValidationError& e : validate(cfg)) {
    if (e.field == "scan_usable_fraction") refused = true;
  }
  CHECK(refused);
}

TEST_CASE("at least two dwells are always generated") {
  Config cfg = scan_config();
  cfg.scan_start_hz = 118000000;
  cfg.scan_end_hz = 118010000;   // a band narrower than one dwell
  const DwellPlan plan = plan_dwells(cfg);
  CHECK(plan.centers.size() >= 2);
}

TEST_CASE("a channel inside a dwell's DC exclusion gains no exposure from that dwell") {
  const Config cfg = scan_config();
  const DwellPlan plan = plan_dwells(cfg);
  const double center = static_cast<double>(plan.centers.front());
  // A channel right at the dwell centre, where the receiver's DC spur sits.
  CHECK_FALSE(covers(center, center, plan.usable_half_hz, plan.dc_exclusion_hz));
  CHECK_FALSE(covers(center, center + plan.dc_exclusion_hz / 2.0, plan.usable_half_hz,
                     plan.dc_exclusion_hz));
  // And just outside it, where it does.
  CHECK(covers(center, center + plan.dc_exclusion_hz + 1000.0, plan.usable_half_hz,
               plan.dc_exclusion_hz));
}

TEST_CASE("activity is hits over exposures, and an unobserved channel reports nothing") {
  Config cfg = scan_config();
  // A narrow band and one pass, so the sweep runs quickly.
  cfg.scan_start_hz = 118000000;
  cfg.scan_end_hz = 118200000;
  cfg.scan_passes = 1;
  cfg.scan_dwell_ms = 20.0;

  SyntheticParams p;
  p.sample_rate_hz = cfg.sample_rate_hz;
  p.center_hz = cfg.scan_start_hz;
  p.offset_hz = 0;
  p.snr_db = 12.0;
  p.talk_s = 0.09;
  p.duty = 0.9;
  p.band_transmitters = {{118050000, 0.95}, {118150000, 0.95}};
  SyntheticSource source(p);

  ScriptedTerminalUi terminal(false);
  std::string error;
  const auto channels = run_scan(source, cfg, terminal, error);
  CHECK(error.empty());
  REQUIRE(!channels.empty());

  for (const ScanChannel& c : channels) {
    if (c.exposures == 0) {
      // Never 0 percent: dividing by a denominator that does not exist is not a
      // measurement.
      CHECK_FALSE(c.activity.has_value());
    } else {
      REQUIRE(c.activity.has_value());
      CHECK(*c.activity ==
            doctest::Approx(static_cast<double>(c.hits) / static_cast<double>(c.exposures)));
      CHECK(*c.activity >= 0.0);
      CHECK(*c.activity <= 1.0);
      CHECK(c.hits <= c.exposures);
    }
  }
}

TEST_CASE("a synthetic band recovers its transmitters in the right rank order") {
  Config cfg = scan_config();
  cfg.scan_start_hz = 118000000;
  cfg.scan_end_hz = 120000000;
  cfg.scan_passes = 4;
  cfg.scan_dwell_ms = 25.0;

  SyntheticParams p;
  p.sample_rate_hz = cfg.sample_rate_hz;
  p.center_hz = cfg.scan_start_hz;
  p.offset_hz = 0;
  // A dwell averages over its whole window, so a duty cycle only shows as a
  // lower activity when the averaged power falls below the hit threshold. At
  // 12 dB the almost-always-keyed transmitter clears open_db comfortably and
  // the rarely-keyed one does not, even in the dwells that catch part of it.
  p.snr_db = 12.0;
  p.talk_s = 0.09;
  p.duty = 0.9;
  // One transmitter keyed almost always, one keyed rarely.
  p.band_transmitters = {{118050000, 0.95}, {119050000, 0.02}};
  SyntheticSource source(p);

  ScriptedTerminalUi terminal(false);
  std::string error;
  auto channels = run_scan(source, cfg, terminal, error);
  REQUIRE(error.empty());

  auto activity_at = [&](std::uint32_t hz) {
    for (const ScanChannel& c : channels) {
      if (c.center_hz == hz) return c.activity.value_or(-1.0);
    }
    return -1.0;
  };
  CHECK(activity_at(118050000) > activity_at(119050000));
  CHECK(activity_at(118050000) > 0.5);
}

TEST_CASE("a retune failure aborts with the failing frequency named") {
  class FailingSource final : public ITunableSampleSource {
   public:
    SourceInfo info() const override { return SourceInfo{}; }
    ReadResult read(std::span<std::complex<float>> out,
                    std::chrono::steady_clock::time_point) override {
      ReadResult r;
      for (auto& s : out) s = {0.01F, 0.0F};
      r.samples = out.size();
      return r;
    }
    void   flush() override {}
    void   cancel() override {}
    double clipped_fraction() const override { return 0.0; }
    bool retune(std::uint32_t center_hz, std::string& error) override {
      error = "the tuner refused " + std::to_string(center_hz) + " Hz";
      return false;
    }
  };

  Config cfg = scan_config();
  cfg.scan_end_hz = 118500000;
  FailingSource source;
  ScriptedTerminalUi terminal(false);
  std::string error;
  (void)run_scan(source, cfg, terminal, error);

  CHECK_FALSE(error.empty());
  CHECK(error.find("retune") != std::string::npos);
  CHECK(error.find("118409600") != std::string::npos);
}

TEST_CASE("scan writes nothing under the session root") {
  // The command path is exercised in the orchestration tests; here the
  // guarantee is structural: run_scan takes no store and no session directory,
  // so there is nothing it could write.
  Config cfg = scan_config();
  cfg.scan_start_hz = 118000000;
  cfg.scan_end_hz = 118100000;
  cfg.scan_passes = 1;
  cfg.scan_dwell_ms = 20.0;

  SyntheticParams p;
  p.sample_rate_hz = cfg.sample_rate_hz;
  p.center_hz = cfg.scan_start_hz;
  p.offset_hz = 0;
  SyntheticSource source(p);
  ScriptedTerminalUi terminal(false);
  std::string error;
  (void)run_scan(source, cfg, terminal, error);
  CHECK(error.empty());
  CHECK_FALSE(std::filesystem::exists(cfg.session_root));
}

}  // TEST_SUITE
