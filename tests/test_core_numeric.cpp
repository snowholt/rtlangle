// WP1 — decibel helpers and the descriptive statistics surface.
//
// Spec section 9.3 and section 10.2; plan WP1. Phase 1 runs no statistical test
// (spec decision Q1), so this is the whole numeric surface: there is no
// t-quantile, no paired confidence bound, and no permutation test to exercise.
// Totality is therefore checkable by hand, and the table below checks it.

#include <doctest/doctest.h>

#include "core/db.h"
#include "core/statistics.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <span>
#include <vector>

using rtlangle::descriptive_spread;
using rtlangle::from_db;
using rtlangle::median;
using rtlangle::minimum_detectable_snr_db;
using rtlangle::percentile;
using rtlangle::to_db;

namespace {

std::vector<double> nine() { return {5.0, 9.0, 1.0, 7.0, 3.0, 8.0, 2.0, 6.0, 4.0}; }

}  // namespace

TEST_SUITE("core_numeric") {

TEST_CASE("to_db and from_db are inverse over a decade sweep") {
  CHECK(to_db(1.0) == doctest::Approx(0.0));
  CHECK(to_db(2.0) == doctest::Approx(3.0103).epsilon(1e-4));
  CHECK(to_db(10.0) == doctest::Approx(10.0));

  for (int e = -6; e <= 6; ++e) {
    for (double m : {1.0, 2.5, 7.3}) {
      const double x = m * std::pow(10.0, e);
      CHECK(from_db(to_db(x)) == doctest::Approx(x).epsilon(1e-9));
    }
  }
  CHECK(from_db(0.0) == doctest::Approx(1.0));
  CHECK(from_db(3.0103) == doctest::Approx(2.0).epsilon(1e-4));
}

TEST_CASE("to_db of a non-positive argument is the documented sentinel, not NaN") {
  CHECK(to_db(0.0) == rtlangle::kDecibelFloor);
  CHECK(to_db(-1.0) == rtlangle::kDecibelFloor);
  CHECK(to_db(std::numeric_limits<double>::quiet_NaN()) == rtlangle::kDecibelFloor);
  CHECK_FALSE(std::isnan(to_db(0.0)));
  CHECK_FALSE(std::isnan(to_db(-1.0)));
}

TEST_CASE("minimum detectable SNR at the default squelch is 4.7437 dB") {
  // This number is printed in the session header and stated in the README.
  //
  //   10*log10(10^0.6 - 1) = 10*log10(2.981071706...) = 4.7437242250818485
  //
  // Spec section 9.3 states it as "about 4.74 dB", which this matches. The
  // plan's WP1 test text gives 4.7407, which is the same expression evaluated
  // wrongly in the fourth significant figure; the arithmetic is pinned here
  // rather than the typo.
  CHECK(minimum_detectable_snr_db(6.0) == doctest::Approx(4.7437242250818485).epsilon(1e-9));
  CHECK(std::round(minimum_detectable_snr_db(6.0) * 100.0) / 100.0 == doctest::Approx(4.74));
  // An open threshold at or below 0 dB leaves nothing detectable above the
  // floor, which is the sentinel rather than a NaN.
  CHECK(minimum_detectable_snr_db(0.0) == rtlangle::kDecibelFloor);
}

TEST_CASE("percentile matches hand-computed values on a nine-element vector") {
  const std::vector<double> xs = nine();
  // n = 9, so the interpolation rank is pct/100 * 8.
  CHECK(percentile(xs, 0.0).value == doctest::Approx(1.0));
  CHECK(percentile(xs, 20.0).value == doctest::Approx(2.6));
  CHECK(percentile(xs, 50.0).value == doctest::Approx(5.0));
  CHECK(percentile(xs, 95.0).value == doctest::Approx(8.6));
  CHECK(percentile(xs, 100.0).value == doctest::Approx(9.0));
  for (double p : {0.0, 20.0, 50.0, 95.0, 100.0}) {
    CHECK(percentile(xs, p).valid);
    CHECK_FALSE(percentile(xs, p).clamped);
  }
}

TEST_CASE("percentile does not depend on input order") {
  std::vector<double> unsorted = nine();
  std::vector<double> sorted = nine();
  std::sort(sorted.begin(), sorted.end());
  for (double p : {0.0, 12.5, 37.0, 50.0, 91.0, 100.0}) {
    CHECK(percentile(unsorted, p).value == doctest::Approx(percentile(sorted, p).value));
  }
  // The input is not mutated: percentile takes a view and sorts a copy.
  CHECK(unsorted == nine());
}

TEST_CASE("median equals the fiftieth percentile") {
  const std::vector<double> xs = nine();
  CHECK(median(xs).value == doctest::Approx(percentile(xs, 50.0).value));
  const std::vector<double> even = {1.0, 2.0, 3.0, 4.0};
  CHECK(median(even).value == doctest::Approx(2.5));
}

TEST_CASE("empty input is an error, not a silent zero") {
  const std::vector<double> empty;
  CHECK_FALSE(percentile(empty, 50.0).valid);
  CHECK_FALSE(median(empty).valid);
  CHECK_FALSE(descriptive_spread(empty).valid);
}

TEST_CASE("an out-of-range percentile is clamped and the clamp is visible") {
  const std::vector<double> xs = nine();
  const auto low = percentile(xs, -1.0);
  CHECK(low.valid);
  CHECK(low.clamped);
  CHECK(low.value == doctest::Approx(percentile(xs, 0.0).value));

  const auto high = percentile(xs, 101.0);
  CHECK(high.valid);
  CHECK(high.clamped);
  CHECK(high.value == doctest::Approx(percentile(xs, 100.0).value));
}

TEST_CASE("descriptive spread is min-max below four points and interquartile at four") {
  const std::vector<double> three = {2.0, 8.0, 5.0};
  const auto s3 = descriptive_spread(three);
  CHECK(s3.valid);
  CHECK_FALSE(s3.is_iqr);
  CHECK(s3.n == 3);
  CHECK(s3.low == doctest::Approx(2.0));
  CHECK(s3.high == doctest::Approx(8.0));

  const std::vector<double> four = {1.0, 2.0, 3.0, 4.0};
  const auto s4 = descriptive_spread(four);
  CHECK(s4.valid);
  CHECK(s4.is_iqr);
  CHECK(s4.n == 4);
  CHECK(s4.low == doctest::Approx(percentile(four, 25.0).value));
  CHECK(s4.high == doctest::Approx(percentile(four, 75.0).value));

  const std::vector<double> one = {42.0};
  const auto s1 = descriptive_spread(one);
  CHECK(s1.valid);
  CHECK_FALSE(s1.is_iqr);
  CHECK(s1.low == doctest::Approx(42.0));
  CHECK(s1.high == doctest::Approx(42.0));
}

TEST_CASE("every helper is total: no undefined behaviour and no NaN escapes") {
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();

  std::vector<std::vector<double>> corpus = {
      {},
      {1.0},
      {3.0, 3.0},
      {inf},
      {nan},
      {-inf},
      {inf, -inf, nan},
      {1.0, nan, 2.0, inf, 3.0, -inf},
      {0.0, 0.0, 0.0, 0.0},
      {-5.0, -4.0, -3.0, -2.0, -1.0},
  };

  std::mt19937_64 rng(20260819U);
  std::uniform_real_distribution<double> value(-1e6, 1e6);
  std::uniform_int_distribution<int> length(0, 40);
  for (int i = 0; i < 10000; ++i) {
    std::vector<double> v(static_cast<std::size_t>(length(rng)));
    for (double& x : v) x = value(rng);
    corpus.push_back(std::move(v));
  }

  const double pcts[] = {-1.0, 0.0, 12.5, 50.0, 100.0, 101.0};
  for (const auto& xs : corpus) {
    for (double p : pcts) {
      const auto s = percentile(xs, p);
      if (s.valid) {
        CHECK(std::isfinite(s.value));
      }
      CHECK(s.clamped == (p < 0.0 || p > 100.0));
    }
    const auto m = median(xs);
    if (m.valid) CHECK(std::isfinite(m.value));

    const auto d = descriptive_spread(xs);
    if (d.valid) {
      CHECK(std::isfinite(d.low));
      CHECK(std::isfinite(d.high));
      CHECK(d.low <= d.high);
      CHECK(d.n >= 1);
      CHECK(d.is_iqr == (d.n >= 4));
    }
  }
}

TEST_CASE("non-finite input never produces a valid non-finite answer") {
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const std::vector<double> mixed = {1.0, nan, inf, 2.0, -inf, 3.0};

  // The three finite values are 1, 2, 3; the rest carry no information a
  // percentile could use, so they are excluded rather than propagated.
  const auto m = median(mixed);
  REQUIRE(m.valid);
  CHECK(m.value == doctest::Approx(2.0));

  const std::vector<double> all_bad = {nan, inf, -inf};
  CHECK_FALSE(median(all_bad).valid);
  CHECK_FALSE(descriptive_spread(all_bad).valid);
}

}  // TEST_SUITE
