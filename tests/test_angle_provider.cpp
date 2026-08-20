// WP8 — the two angle providers. Spec sections 4.7 and 6.4.

#include <doctest/doctest.h>

#include "angle/fixed_angle_provider.h"
#include "angle/manual_angle_provider.h"
#include "ui/scripted_terminal_ui.h"

#include <string>

using namespace rtlangle;
using rtlangle::ui::ScriptedTerminalUi;

namespace {

Config provider_config() {
  Config c;
  c.center_hz = 118350000;
  c.angle_reference = "marked arm along the balcony rail, pointing at the street";
  return c;
}

bool emitted_contains(const ScriptedTerminalUi& ui, std::string_view needle) {
  for (const std::string& line : ui.emitted()) {
    if (line.find(needle) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

TEST_SUITE("angle_provider") {

TEST_CASE("blank input accepts the planned angle") {
  ScriptedTerminalUi ui;
  ui.push_menu_choice(0);   // Continue
  ui.push_line("");         // blank: accept the planned angle
  ManualAngleProvider provider(ui, provider_config());

  const AngleOutcome out = provider.request(45.0, 1);
  CHECK(out.cmd == AngleOutcome::Cmd::Proceed);
  CHECK(out.actual_deg == doctest::Approx(45.0));
  CHECK(emitted_contains(ui, "marked arm along the balcony rail"));
}

TEST_CASE("a typed angle is accepted and a non-numeric answer re-prompts") {
  ScriptedTerminalUi ui;
  ui.push_menu_choice(0);
  ui.push_line("north-ish");   // not a number
  ui.push_line("44.5");
  ManualAngleProvider provider(ui, provider_config());

  const AngleOutcome out = provider.request(45.0, 1);
  CHECK(out.cmd == AngleOutcome::Cmd::Proceed);
  CHECK(out.actual_deg == doctest::Approx(44.5));
  CHECK(emitted_contains(ui, "not a number"));
}

TEST_CASE("an out-of-range or non-finite angle re-prompts") {
  ScriptedTerminalUi ui;
  ui.push_menu_choice(0);
  ui.push_line("400");
  ui.push_line("-5");
  ui.push_line("nan");
  ui.push_line("60");
  ManualAngleProvider provider(ui, provider_config());

  const AngleOutcome out = provider.request(60.0, 1);
  CHECK(out.cmd == AngleOutcome::Cmd::Proceed);
  CHECK(out.actual_deg == doctest::Approx(60.0));
  CHECK(emitted_contains(ui, "0 to 360 degrees"));
}

TEST_CASE("a large deviation requires confirmation and a declined one re-prompts") {
  // The case this exists to catch: typing 9 for 90.
  ScriptedTerminalUi ui;
  ui.push_menu_choice(0);
  ui.push_line("9");        // 81 degrees away from the planned 90
  ui.push_confirm(false);   // declined
  ui.push_line("90");
  ManualAngleProvider provider(ui, provider_config());

  const AngleOutcome out = provider.request(90.0, 1);
  CHECK(out.cmd == AngleOutcome::Cmd::Proceed);
  CHECK(out.actual_deg == doctest::Approx(90.0));
  CHECK(emitted_contains(ui, "from the planned"));
}

TEST_CASE("a confirmed large deviation is accepted as entered") {
  ScriptedTerminalUi ui;
  ui.push_menu_choice(0);
  ui.push_line("9");
  ui.push_confirm(true);
  ManualAngleProvider provider(ui, provider_config());

  const AngleOutcome out = provider.request(90.0, 1);
  CHECK(out.cmd == AngleOutcome::Cmd::Proceed);
  CHECK(out.actual_deg == doctest::Approx(9.0));
}

TEST_CASE("a small deviation warns without demanding confirmation") {
  ScriptedTerminalUi ui;
  ui.push_menu_choice(0);
  ui.push_line("52");   // 7 degrees from 45: past the warn threshold, inside the max
  ManualAngleProvider provider(ui, provider_config());

  const AngleOutcome out = provider.request(45.0, 1);
  CHECK(out.cmd == AngleOutcome::Cmd::Proceed);
  CHECK(out.actual_deg == doctest::Approx(52.0));
  CHECK(emitted_contains(ui, "will flag this capture"));
}

TEST_CASE("retry, skip, and quit each produce their outcome") {
  {
    ScriptedTerminalUi ui;
    ui.push_menu_choice(1);
    ManualAngleProvider provider(ui, provider_config());
    CHECK(provider.request(45.0, 1).cmd == AngleOutcome::Cmd::Retry);
  }
  {
    ScriptedTerminalUi ui;
    ui.push_menu_choice(2);
    ManualAngleProvider provider(ui, provider_config());
    CHECK(provider.request(45.0, 1).cmd == AngleOutcome::Cmd::Skip);
  }
  {
    ScriptedTerminalUi ui;
    ui.push_menu_choice(4);
    ManualAngleProvider provider(ui, provider_config());
    CHECK(provider.request(45.0, 1).cmd == AngleOutcome::Cmd::Quit);
  }
  {
    // A cancelled menu is the same decision as Quit.
    ScriptedTerminalUi ui;
    ui.push_cancel();
    ManualAngleProvider provider(ui, provider_config());
    CHECK(provider.request(45.0, 1).cmd == AngleOutcome::Cmd::Quit);
  }
}

TEST_CASE("a note is attached without being an outcome of its own") {
  ScriptedTerminalUi ui;
  ui.push_menu_choice(3);   // Add a note
  ui.push_line("cable routed along the balcony rail");
  ui.push_menu_choice(0);   // then Continue
  ui.push_line("");
  ManualAngleProvider provider(ui, provider_config());

  const AngleOutcome out = provider.request(45.0, 1);
  CHECK(out.cmd == AngleOutcome::Cmd::Proceed);
  REQUIRE(out.note.has_value());
  CHECK(*out.note == "cable routed along the balcony rail");
}

TEST_CASE("the retry wording says THIS angle and never previous") {
  ScriptedTerminalUi ui;
  ui.push_menu_choice(0);
  ui.push_line("");
  ManualAngleProvider provider(ui, provider_config());
  (void)provider.request(45.0, 2);

  CHECK(emitted_contains(ui, "Retry this angle"));
  CHECK(emitted_contains(ui, "attempt 2"));
  // "Retry previous" would imply a backtracking capability that does not exist.
  for (const std::string& line : ui.emitted()) {
    CHECK(line.find("previous") == std::string::npos);
  }
}

TEST_CASE("the provider reports its identity for the session record") {
  ScriptedTerminalUi ui;
  ManualAngleProvider manual(ui, provider_config());
  CHECK(manual.name() == "manual");
  CHECK_FALSE(manual.is_automated());

  FixedAngleProvider fixed;
  CHECK(fixed.name() == "fixed");
  CHECK(fixed.is_automated());
}

TEST_CASE("the fixed provider proceeds at the planned angle and never touches a terminal") {
  ScriptedTerminalUi ui;
  FixedAngleProvider provider;
  const AngleOutcome out = provider.request(75.0, 3);
  CHECK(out.cmd == AngleOutcome::Cmd::Proceed);
  CHECK(out.actual_deg == doctest::Approx(75.0));
  CHECK_FALSE(out.note.has_value());
  // The second implementation of the interface, and it proves the interface is
  // not coupled to a terminal.
  CHECK(ui.emitted().empty());
}

TEST_CASE("input ending during the angle prompt is a quit, not an accepted angle") {
  ScriptedTerminalUi ui;
  ui.push_menu_choice(0);
  // No line pushed: the input ended.
  ManualAngleProvider provider(ui, provider_config());
  CHECK(provider.request(45.0, 1).cmd == AngleOutcome::Cmd::Quit);
}

}  // TEST_SUITE
