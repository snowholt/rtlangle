// WP2 — angle generation, circular geometry, and visit order. Spec section 4.

#include <doctest/doctest.h>

#include "core/angle_math.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <vector>

using namespace rtlangle;

TEST_SUITE("angle_math") {

TEST_CASE("the default sequence is 0,15,30,45,60,75,90") {
  const std::vector<double> expected = {0, 15, 30, 45, 60, 75, 90};
  const auto got = generate_angles(0.0, 90.0, 15.0);
  REQUIRE(got.size() == expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    CHECK(got[i] == doctest::Approx(expected[i]));
  }
}

TEST_CASE("a non-grid endpoint is not appended") {
  // The single rule is start + i*step <= end + 1e-9. A test asserting that 90
  // is appended here would be a bug in the test, not in the generator.
  const std::vector<double> expected = {0, 20, 40, 60, 80};
  const auto got = generate_angles(0.0, 90.0, 20.0);
  REQUIRE(got.size() == expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    CHECK(got[i] == doctest::Approx(expected[i]));
  }

  const auto notice = endpoint_notice(0.0, 90.0, 20.0, got);
  REQUIRE(notice.has_value());
  CHECK(notice->find("80") != std::string::npos);
  CHECK(notice->find("90") != std::string::npos);

  // On the grid, there is nothing to say.
  const auto exact = generate_angles(0.0, 90.0, 15.0);
  CHECK_FALSE(endpoint_notice(0.0, 90.0, 15.0, exact).has_value());
}

TEST_CASE("generation is total over inputs validation will reject") {
  CHECK(generate_angles(0.0, 90.0, 0.0).empty());
  CHECK(generate_angles(0.0, 90.0, -5.0).empty());
  CHECK(generate_angles(90.0, 0.0, 5.0).empty());
  CHECK(generate_angles(std::nan(""), 90.0, 5.0).empty());
  CHECK(generate_angles(0.0, std::numeric_limits<double>::infinity(), 5.0).empty());
  // A pathologically small step is bounded rather than allowed to run away.
  CHECK(generate_angles(0.0, 360.0, 1e-12).size() <= 1000001);
}

TEST_CASE("explicit angle lists are de-duplicated within 1e-6 and sorted") {
  const std::vector<double> in = {90.0, 0.0, 30.0, 30.0000001, 60.0};
  const auto got = dedupe_and_sort_angles(in);
  REQUIRE(got.size() == 4);
  CHECK(got[0] == doctest::Approx(0.0));
  CHECK(got[1] == doctest::Approx(30.0));
  CHECK(got[2] == doctest::Approx(60.0));
  CHECK(got[3] == doctest::Approx(90.0));

  // Two angles that differ by more than the tolerance stay distinct, which is
  // what the index-based visit identifier depends on.
  const auto near = dedupe_and_sort_angles(std::vector<double>{45.001, 45.002});
  REQUIRE(near.size() == 2);
  CHECK(near[1] - near[0] == doctest::Approx(0.001));
}

TEST_CASE("circular distance wraps") {
  CHECK(circular_distance_deg(350.0, 10.0) == doctest::Approx(20.0));
  CHECK(circular_distance_deg(10.0, 350.0) == doctest::Approx(20.0));
  CHECK(circular_distance_deg(0.0, 180.0) == doctest::Approx(180.0));
  CHECK(circular_distance_deg(0.0, 181.0) == doctest::Approx(179.0));
  CHECK(circular_distance_deg(45.0, 45.0) == doctest::Approx(0.0));
  CHECK(std::isnan(circular_distance_deg(std::nan(""), 10.0)));
}

TEST_CASE("circular mean and spread") {
  const std::vector<double> wrap = {350.0, 10.0};
  CHECK(circular_mean_deg(wrap) == doctest::Approx(0.0).epsilon(1e-9));
  CHECK(circular_spread_deg(wrap) > 0.0);

  const std::vector<double> same = {45.0, 45.0, 45.0};
  CHECK(circular_mean_deg(same) == doctest::Approx(45.0));
  CHECK(circular_spread_deg(same) == doctest::Approx(0.0).epsilon(1e-6));

  // A zero resultant leaves the mean direction and the spread undefined rather
  // than infinite, and rounding noise must not turn either into a number.
  const std::vector<double> cross = {0.0, 90.0, 180.0, 270.0};
  CHECK(std::isnan(circular_spread_deg(cross)));
  CHECK(std::isnan(circular_mean_deg(cross)));

  const std::vector<double> empty;
  CHECK(std::isnan(circular_mean_deg(empty)));
  CHECK(std::isnan(circular_spread_deg(empty)));
}

TEST_CASE("angles are never folded modulo 180") {
  // theta and theta+180 are two distinct orientations (spec section 4.2). The
  // generator keeps both, and the informational message says what a difference
  // between them would mean.
  const auto got = generate_angles(0.0, 180.0, 180.0);
  REQUIRE(got.size() == 2);
  CHECK(got[0] == doctest::Approx(0.0));
  CHECK(got[1] == doctest::Approx(180.0));

  const auto notices = opposed_pair_notices(got);
  REQUIRE(notices.size() == 1);
  CHECK(notices[0].find("environmental") != std::string::npos);
  CHECK(notices[0].find("180") != std::string::npos);

  // No pair, no message.
  CHECK(opposed_pair_notices(std::vector<double>{0.0, 15.0, 30.0}).empty());
}

TEST_CASE("visit order: forward, reverse, and alternating") {
  const std::vector<std::size_t> asc = {0, 1, 2, 3};
  const std::vector<std::size_t> desc = {3, 2, 1, 0};

  CHECK(order_for_round(4, VisitOrder::Forward, 1, 7) == asc);
  CHECK(order_for_round(4, VisitOrder::Forward, 2, 7) == asc);
  CHECK(order_for_round(4, VisitOrder::Reverse, 1, 7) == desc);
  CHECK(order_for_round(4, VisitOrder::Reverse, 2, 7) == desc);

  CHECK(order_for_round(4, VisitOrder::Alternating, 1, 7) == asc);
  CHECK(order_for_round(4, VisitOrder::Alternating, 2, 7) == desc);
  CHECK(order_for_round(4, VisitOrder::Alternating, 3, 7) == asc);
  CHECK(order_for_round(4, VisitOrder::Alternating, 4, 7) == desc);
}

TEST_CASE("random visit order is a reproducible permutation") {
  const auto a = order_for_round(7, VisitOrder::Random, 1, 1234567);
  const auto b = order_for_round(7, VisitOrder::Random, 1, 1234567);
  CHECK(a == b);

  const std::set<std::size_t> as_set(a.begin(), a.end());
  CHECK(as_set.size() == 7);
  CHECK(*as_set.begin() == 0);
  CHECK(*as_set.rbegin() == 6);

  // Independently shuffled per round, and dependent on the seed.
  CHECK(order_for_round(7, VisitOrder::Random, 2, 1234567) != a);
  CHECK(order_for_round(7, VisitOrder::Random, 1, 7654321) != a);
}

TEST_CASE("visit order parses and round-trips") {
  for (auto o : {VisitOrder::Forward, VisitOrder::Reverse, VisitOrder::Alternating,
                 VisitOrder::Random}) {
    const auto parsed = parse_visit_order(to_string(o));
    REQUIRE(parsed.has_value());
    CHECK(*parsed == o);
  }
  CHECK_FALSE(parse_visit_order("sideways").has_value());
  CHECK_FALSE(parse_visit_order("").has_value());
}

}  // TEST_SUITE
