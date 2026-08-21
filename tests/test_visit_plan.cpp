// WP9 — the visit plan and its identifiers. Spec sections 4.6 and 11.2.

#include <doctest/doctest.h>

#include "experiment/visit_plan.h"

#include <set>
#include <string>

using namespace rtlangle;

namespace {

Config plan_config() {
  Config c;
  c.center_hz = 118350000;
  return c;
}

}  // namespace

TEST_SUITE("visit_plan") {

TEST_CASE("seven angles over two alternating rounds give fourteen unique visits") {
  const auto visits = build_visit_plan(plan_config());
  REQUIRE(visits.size() == 14);

  std::set<std::string> ids;
  for (const Visit& v : visits) ids.insert(v.visit_id);
  CHECK(ids.size() == 14);

  // Round 1 ascends, round 2 descends.
  for (std::size_t i = 0; i < 7; ++i) {
    CHECK(visits[i].round == 1);
    CHECK(visits[i].angle_index == i);
    CHECK(visits[i].planned_deg == doctest::Approx(static_cast<double>(i) * 15.0));
  }
  for (std::size_t i = 0; i < 7; ++i) {
    CHECK(visits[7 + i].round == 2);
    CHECK(visits[7 + i].angle_index == 6 - i);
  }
}

TEST_CASE("identifiers use the r<round>-i<three-digit index> form") {
  const auto visits = build_visit_plan(plan_config());
  CHECK(visits[0].visit_id == "r1-i000");
  CHECK(visits[3].visit_id == "r1-i003");
  CHECK(visits[7].visit_id == "r2-i006");
  CHECK(visits[13].visit_id == "r2-i000");
}

TEST_CASE("two angles a thousandth of a degree apart get distinct identifiers") {
  // The regression this identifier form exists for: an angle-formatted id
  // rounds to two decimals while the angle list de-duplicates only within 1e-6,
  // so these two angles would have shared one id and one of them would have
  // been silently lost on resume.
  Config c = plan_config();
  c.angles_deg = {45.001, 45.002};
  c.rounds = 1;

  const auto visits = build_visit_plan(c);
  REQUIRE(visits.size() == 2);
  CHECK(visits[0].visit_id != visits[1].visit_id);
  CHECK(visits[0].planned_deg == doctest::Approx(45.001));
  CHECK(visits[1].planned_deg == doctest::Approx(45.002));
  CHECK(visits[1].planned_deg - visits[0].planned_deg == doctest::Approx(0.001));
}

TEST_CASE("forward, reverse, and random orders are honoured") {
  Config forward = plan_config();
  forward.order = VisitOrder::Forward;
  forward.rounds = 2;
  const auto f = build_visit_plan(forward);
  CHECK(f[0].angle_index == 0);
  CHECK(f[7].angle_index == 0);

  Config reverse = plan_config();
  reverse.order = VisitOrder::Reverse;
  reverse.rounds = 2;
  const auto r = build_visit_plan(reverse);
  CHECK(r[0].angle_index == 6);
  CHECK(r[7].angle_index == 6);

  Config random = plan_config();
  random.order = VisitOrder::Random;
  random.seed = 1234567;
  const auto a = build_visit_plan(random);
  const auto b = build_visit_plan(random);
  REQUIRE(a.size() == b.size());
  for (std::size_t i = 0; i < a.size(); ++i) CHECK(a[i].visit_id == b[i].visit_id);

  std::set<std::size_t> first_round;
  for (std::size_t i = 0; i < 7; ++i) first_round.insert(a[i].angle_index);
  CHECK(first_round.size() == 7);
}

TEST_CASE("the planned angle is carried at full precision, never re-derived from the id") {
  Config c = plan_config();
  c.angles_deg = {0.125, 33.3333333};
  c.rounds = 1;
  const auto visits = build_visit_plan(c);
  REQUIRE(visits.size() == 2);
  CHECK(visits[0].planned_deg == doctest::Approx(0.125).epsilon(1e-12));
  CHECK(visits[1].planned_deg == doctest::Approx(33.3333333).epsilon(1e-12));
}

}  // TEST_SUITE
