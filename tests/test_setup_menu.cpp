// The interactive "Set up experiment" screen. Every edit is applied through the
// same flag parser the command line uses, so what the screen accepts and what
// the command line accepts cannot drift apart. These tests hold that property
// and the rendering of the rows.

#include <doctest/doctest.h>

#include "app/cli_parser.h"
#include "app/setup_menu.h"
#include "ui/scripted_terminal_ui.h"

#include <set>
#include <string>
#include <vector>

using namespace rtlangle;
using namespace rtlangle::app;
using rtlangle::ui::ScriptedTerminalUi;

namespace {

int row(SetupRow r) { return static_cast<int>(r); }

std::string detail_of(const std::vector<ui::MenuItem>& rows, SetupRow r) {
  return rows.at(static_cast<std::size_t>(row(r))).detail;
}

Config with_frequency() {
  Config cfg;
  cfg.center_hz = 118350000;
  return cfg;
}

}  // namespace

TEST_SUITE("setup_menu") {

// ---------------------------------------------------------------------------
// apply_option — one option, applied exactly as the command line would
// ---------------------------------------------------------------------------

TEST_CASE("apply_option applies a value and records the key as explicitly set") {
  Config                cfg;
  std::set<std::string> keys;
  const auto errors = apply_option(cfg, keys, "--rounds", "6");
  CHECK(errors.empty());
  CHECK(cfg.rounds == 6);
  CHECK(keys.count("rounds") == 1);
  // Nothing else moved.
  CHECK(cfg.duration_s == doctest::Approx(60.0));
}

TEST_CASE("apply_option accepts the same literals the command line accepts") {
  Config                cfg;
  std::set<std::string> keys;
  CHECK(apply_option(cfg, keys, "--freq", "118.35M").empty());
  CHECK(cfg.center_hz.has_value());
  CHECK(*cfg.center_hz == 118350000u);

  CHECK(apply_option(cfg, keys, "--gain", "max").empty());
  CHECK_FALSE(cfg.gain_tenth_db.has_value());
  CHECK(apply_option(cfg, keys, "--gain", "404").empty());
  CHECK(cfg.gain_tenth_db.has_value());

  CHECK(apply_option(cfg, keys, "--angles", "0,45,90").empty());
  CHECK(cfg.angles_deg.size() == 3);

  CHECK(apply_option(cfg, keys, "--order", "forward").empty());
  CHECK(cfg.order == VisitOrder::Forward);

  CHECK(apply_option(cfg, keys, "--bias-tee", "true").empty());
  CHECK(cfg.bias_tee);
}

TEST_CASE("apply_option leaves the configuration untouched when it refuses") {
  Config                cfg;
  std::set<std::string> keys;
  const Config before = cfg;

  SUBCASE("an unknown flag names itself") {
    const auto errors = apply_option(cfg, keys, "--not-a-flag", "1");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].message.find("--not-a-flag") != std::string::npos);
  }
  SUBCASE("a value of the wrong shape names the flag") {
    const auto errors = apply_option(cfg, keys, "--rounds", "many");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].message.find("--rounds") != std::string::npos);
  }
  CHECK(cfg.rounds == before.rounds);
  CHECK_FALSE(cfg.center_hz.has_value());
  // A refused option is not an option the operator set.
  CHECK(keys.empty());
}

TEST_CASE("apply_option carries a derived default with the field it derives from") {
  Config                cfg;
  std::set<std::string> keys;
  CHECK(apply_option(cfg, keys, "--noise-percentile", "3").empty());
  // Spec section 7.3: the probe follows the floor down rather than sitting above it.
  CHECK(cfg.probe_percentile == doctest::Approx(3.0));

  CHECK(apply_option(cfg, keys, "--probe-percentile", "2").empty());
  CHECK(apply_option(cfg, keys, "--noise-percentile", "9").empty());
  // Once set explicitly it stops following.
  CHECK(cfg.probe_percentile == doctest::Approx(2.0));
}

// ---------------------------------------------------------------------------
// The rows
// ---------------------------------------------------------------------------

TEST_CASE("the rows show the value each option currently holds") {
  Config cfg = with_frequency();
  cfg.rounds = 3;
  cfg.duration_s = 20.0;
  const auto rows = setup_rows(cfg, {});
  CHECK(rows.size() == kSetupRowCount);

  CHECK(detail_of(rows, SetupRow::Frequency).find("118.350") != std::string::npos);
  CHECK(detail_of(rows, SetupRow::Rounds) == "3");
  CHECK(detail_of(rows, SetupRow::Duration).find("20") != std::string::npos);
  CHECK(detail_of(rows, SetupRow::Order).find("alternating") != std::string::npos);
  CHECK(detail_of(rows, SetupRow::Gain) == "max");
  // 0 to 90 step 15 is seven angles.
  CHECK(detail_of(rows, SetupRow::Angles).find("7 angles") != std::string::npos);
  // An unset optional says so rather than showing an empty column.
  CHECK(detail_of(rows, SetupRow::Label) == "(none)");
}

TEST_CASE("an explicit angle list is shown as a list, not as a range") {
  Config cfg = with_frequency();
  cfg.angles_deg = {0.0, 45.0, 90.0};
  const auto rows = setup_rows(cfg, {});
  const std::string angles = detail_of(rows, SetupRow::Angles);
  CHECK(angles.find("3 angles") != std::string::npos);
  CHECK(angles.find("step") == std::string::npos);
}

TEST_CASE("the Start row says what is blocking, and says ready when nothing is") {
  SUBCASE("a missing frequency is named") {
    const auto rows = setup_rows(Config{}, {});
    const std::string status = detail_of(rows, SetupRow::Start);
    CHECK(status != "ready");
    CHECK(status.find("--freq") != std::string::npos);
  }
  SUBCASE("a flag the run command refuses is named") {
    Config cfg = with_frequency();
    cfg.scan_top_n = 5;
    const auto rows = setup_rows(cfg, {"scan_top_n"});
    CHECK(detail_of(rows, SetupRow::Start).find("--scan-top") != std::string::npos);
  }
  SUBCASE("ready when nothing is blocking") {
    CHECK(detail_of(setup_rows(with_frequency(), {}), SetupRow::Start) == "ready");
  }
}

// ---------------------------------------------------------------------------
// The screen
// ---------------------------------------------------------------------------

TEST_CASE("editing a row changes the configuration the screen returns") {
  ScriptedTerminalUi terminal;
  terminal.push_menu_choice(row(SetupRow::Rounds));
  terminal.push_line("5");
  terminal.push_menu_choice(row(SetupRow::Duration));
  terminal.push_line("12.5");
  terminal.push_menu_choice(row(SetupRow::Start));

  std::set<std::string> keys;
  const auto result = setup_experiment(with_frequency(), keys, terminal);
  REQUIRE(result.has_value());
  CHECK(result->rounds == 5);
  CHECK(result->duration_s == doctest::Approx(12.5));
  // The resume classification of spec section 11.5 needs to know these were
  // typed rather than defaulted, so the screen records them.
  CHECK(keys.count("rounds") == 1);
  CHECK(keys.count("duration_s") == 1);
}

TEST_CASE("Back returns nothing and the caller runs nothing") {
  ScriptedTerminalUi terminal;
  terminal.push_menu_choice(row(SetupRow::Back));
  std::set<std::string> keys;
  CHECK_FALSE(setup_experiment(with_frequency(), keys, terminal).has_value());
}

TEST_CASE("cancelling the menu is the same as Back") {
  ScriptedTerminalUi terminal;
  terminal.push_cancel();
  std::set<std::string> keys;
  CHECK_FALSE(setup_experiment(with_frequency(), keys, terminal).has_value());
}

TEST_CASE("a refused value is reported and nothing changes") {
  ScriptedTerminalUi terminal;
  terminal.push_menu_choice(row(SetupRow::Rounds));
  terminal.push_line("many");
  terminal.push_menu_choice(row(SetupRow::Start));

  std::set<std::string> keys;
  const auto result = setup_experiment(with_frequency(), keys, terminal);
  REQUIRE(result.has_value());
  CHECK(result->rounds == 2);
  CHECK(keys.count("rounds") == 0);

  bool reported = false;
  for (const std::string& line : terminal.emitted()) {
    if (line.rfind("error: ", 0) == 0 && line.find("--rounds") != std::string::npos) {
      reported = true;
    }
  }
  CHECK(reported);
}

TEST_CASE("Start refuses to leave while the configuration is not runnable") {
  ScriptedTerminalUi terminal;
  terminal.push_menu_choice(row(SetupRow::Start));   // no frequency yet
  terminal.push_menu_choice(row(SetupRow::Frequency));
  terminal.push_line("121.5M");
  terminal.push_menu_choice(row(SetupRow::Start));

  std::set<std::string> keys;
  const auto result = setup_experiment(Config{}, keys, terminal);
  REQUIRE(result.has_value());
  REQUIRE(result->center_hz.has_value());
  CHECK(*result->center_hz == 121500000u);
}

TEST_CASE("the free-form row reaches an option the screen does not name") {
  ScriptedTerminalUi terminal;
  terminal.push_menu_choice(row(SetupRow::OtherOption));
  terminal.push_line("--noise-percentile 25");
  terminal.push_menu_choice(row(SetupRow::OtherOption));
  terminal.push_line("--min-event-ms=450");
  terminal.push_menu_choice(row(SetupRow::Start));

  std::set<std::string> keys;
  const auto result = setup_experiment(with_frequency(), keys, terminal);
  REQUIRE(result.has_value());
  CHECK(result->noise_percentile == doctest::Approx(25.0));
  CHECK(result->min_event_ms == doctest::Approx(450.0));
  CHECK(keys.count("noise_percentile") == 1);
  CHECK(keys.count("min_event_ms") == 1);
}

TEST_CASE("a range typed over a list replaces the list rather than being ignored by it") {
  Config cfg = with_frequency();
  cfg.angles_deg = {0.0, 45.0, 90.0};

  ScriptedTerminalUi terminal;
  terminal.push_menu_choice(row(SetupRow::Angles));
  terminal.push_line("0:30:90");
  terminal.push_menu_choice(row(SetupRow::Start));

  std::set<std::string> keys = {"angles_deg"};
  const auto result = setup_experiment(cfg, keys, terminal);
  REQUIRE(result.has_value());
  // angles_deg overrides the range wherever it is non-empty, so a range typed
  // here has to clear it or the operator's edit would silently do nothing.
  CHECK(result->angles_deg.empty());
  CHECK(keys.count("angles_deg") == 0);
  CHECK(result->start_deg == doctest::Approx(0.0));
  CHECK(result->step_deg == doctest::Approx(30.0));
  CHECK(result->end_deg == doctest::Approx(90.0));
  CHECK(resolved_angles(*result).size() == 4);
}

TEST_CASE("a list typed over a range replaces the range") {
  ScriptedTerminalUi terminal;
  terminal.push_menu_choice(row(SetupRow::Angles));
  terminal.push_line("0, 45, 90");
  terminal.push_menu_choice(row(SetupRow::Start));

  std::set<std::string> keys;
  const auto result = setup_experiment(with_frequency(), keys, terminal);
  REQUIRE(result.has_value());
  CHECK(result->angles_deg.size() == 3);
  CHECK(keys.count("angles_deg") == 1);
}

TEST_CASE("the visit order is chosen from a list rather than typed") {
  ScriptedTerminalUi terminal;
  terminal.push_menu_choice(row(SetupRow::Order));
  terminal.push_menu_choice(0);   // the first order in the sub-menu
  terminal.push_menu_choice(row(SetupRow::Start));

  std::set<std::string> keys;
  const auto result = setup_experiment(with_frequency(), keys, terminal);
  REQUIRE(result.has_value());
  CHECK(result->order == VisitOrder::Forward);
  CHECK(keys.count("order") == 1);
}

TEST_CASE("input ending during an edit keeps the previous value") {
  ScriptedTerminalUi terminal;
  terminal.push_menu_choice(row(SetupRow::Rounds));
  // No line pushed: prompt_line reports that the input ended.
  terminal.push_menu_choice(row(SetupRow::Start));

  std::set<std::string> keys;
  const auto result = setup_experiment(with_frequency(), keys, terminal);
  REQUIRE(result.has_value());
  CHECK(result->rounds == 2);
}

TEST_CASE("the cursor stays where the operator left it between edits") {
  ScriptedTerminalUi terminal;
  terminal.push_menu_choice(row(SetupRow::Gain));
  terminal.push_line("404");
  terminal.push_menu_choice(row(SetupRow::Start));

  std::set<std::string> keys;
  const auto result = setup_experiment(with_frequency(), keys, terminal);
  REQUIRE(result.has_value());

  // First paint starts at the top; the second reopens on the row just edited
  // rather than sending the operator back down the list.
  const auto& initial = terminal.menu_initial_indices();
  REQUIRE(initial.size() >= 2);
  CHECK(initial[0] == 0);
  CHECK(initial[1] == row(SetupRow::Gain));
}

TEST_CASE("a flag the run command refuses is reported once and then dropped") {
  // Reached by `rtlangle --scan-dwell 500` with no command: the main menu
  // hands the screen a key that Start would otherwise block on forever, and no
  // row can clear it.
  Config cfg = with_frequency();
  cfg.scan_dwell_ms = 500.0;

  ScriptedTerminalUi terminal;
  terminal.push_menu_choice(row(SetupRow::Start));

  std::set<std::string> keys = {"scan_dwell_ms"};
  const auto result = setup_experiment(cfg, keys, terminal);
  REQUIRE(result.has_value());
  CHECK(keys.count("scan_dwell_ms") == 0);

  bool warned = false;
  for (const std::string& line : terminal.emitted()) {
    if (line.rfind("warn: ", 0) == 0 && line.find("--scan-dwell") != std::string::npos) {
      warned = true;
    }
  }
  CHECK(warned);
}

TEST_CASE("a degenerate angle step renders and blocks rather than running away") {
  // The screen renders from a configuration that has passed type conversion but
  // not validation, so the angle generator's own guard is what stands between a
  // typo and a runaway loop. It returns nothing for a step of zero or less.
  Config cfg = with_frequency();
  cfg.step_deg = 0.0;
  const auto rows = setup_rows(cfg, {});
  CHECK(rows.at(static_cast<std::size_t>(row(SetupRow::Angles))).detail.find("0 angles") !=
        std::string::npos);
  CHECK(detail_of(rows, SetupRow::Start).find("--step-deg") != std::string::npos);

  cfg.step_deg = -5.0;
  CHECK(setup_rows(cfg, {}).at(static_cast<std::size_t>(row(SetupRow::Angles)))
            .detail.find("0 angles") != std::string::npos);
}

}  // TEST_SUITE
