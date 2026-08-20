// WP8 — control-character neutralisation, menu navigation, prompt handling, and
// the non-TTY safety rule. Spec sections 6.5 and 12.3.

#include <doctest/doctest.h>

#include "ui/ansi_terminal_ui.h"
#include "ui/safe_text.h"
#include "ui/scripted_terminal_ui.h"

#include <array>
#include <cstdlib>
#include <string>
#include <unistd.h>

using namespace rtlangle::ui;

TEST_SUITE("ui") {

TEST_CASE("safe_text neutralises escape sequences without swallowing text") {
  const std::string out = safe_text("\x1b[31mred");
  // The escape is visible rather than live: a crafted device string cannot
  // reposition the cursor or inject colour.
  CHECK(out.find("\\x1B") != std::string::npos);
  CHECK(out.find('\x1b') == std::string::npos);
  CHECK(out.find("[31mred") != std::string::npos);
}

TEST_CASE("safe_text neutralises C0, DEL, and the C1 controls") {
  CHECK(safe_text(std::string("a\x01""b")) == "a\\x01b");
  CHECK(safe_text("tab\there") == "tab\\x09here");
  CHECK(safe_text("line\nbreak") == "line\\x0Abreak");
  CHECK(safe_text(std::string("del\x7f")) == "del\\x7F");
  // The C1 controls are 0xC2 0x80..0x9F in UTF-8.
  CHECK(safe_text("\xc2\x85") == "\\xC2\\x85");
  CHECK(safe_text("\xc2\x9f") == "\\xC2\\x9F");
}

TEST_CASE("UTF-8 above U+007F passes through unchanged") {
  const std::string accented = "Straße";           // U+00DF
  const std::string arrows = "→←";       // U+2192, U+2190
  const std::string emoji = "\U0001F4E1";          // U+1F4E1
  const std::string nbsp = "\xc2\xa0";             // U+00A0, above the C1 range
  CHECK(safe_text(accented) == accented);
  CHECK(safe_text(arrows) == arrows);
  CHECK(safe_text(emoji) == emoji);
  CHECK(safe_text(nbsp) == nbsp);
  CHECK(safe_text("") == "");
  CHECK(safe_text("plain ascii 123") == "plain ascii 123");
}

TEST_CASE("menu navigation, cancel, and clamping through the scripted terminal") {
  const std::array<MenuItem, 3> items = {MenuItem{"First", ""}, MenuItem{"Second", ""},
                                         MenuItem{"Third", ""}};
  ScriptedTerminalUi ui;
  ui.push_menu_choice(2);
  CHECK(ui.menu("Choose", items, 0) == 2);

  ui.push_cancel();
  CHECK(ui.menu("Choose", items, 0) == -1);

  // An out-of-range choice clamps rather than indexing past the end.
  ui.push_menu_choice(99);
  CHECK(ui.menu("Choose", items, 0) == 2);
  ui.push_menu_choice(0);
  CHECK(ui.menu("Choose", items, 7) == 0);

  // Running out of scripted answers is a cancel, not a guess.
  CHECK(ui.menu("Choose", items, 0) == -1);
}

TEST_CASE("prompt_line returns the default on blank input and the typed value otherwise") {
  ScriptedTerminalUi ui;
  ui.push_line("");
  CHECK(ui.prompt_line("Angle", "45").value() == "45");
  ui.push_line("47.5");
  CHECK(ui.prompt_line("Angle", "45").value() == "47.5");
  // Exhausted input is an ended stream, not an empty answer.
  CHECK_FALSE(ui.prompt_line("Angle", "45").has_value());
}

TEST_CASE("confirm falls back to its default when nothing is scripted") {
  ScriptedTerminalUi ui;
  ui.push_confirm(true);
  CHECK(ui.confirm("Proceed?", false));
  ui.push_confirm(false);
  CHECK_FALSE(ui.confirm("Proceed?", true));
  CHECK(ui.confirm("Proceed?", true));
  CHECK_FALSE(ui.confirm("Proceed?", false));
}

TEST_CASE("everything rendered passes through neutralisation") {
  ScriptedTerminalUi ui;
  ui.heading("\x1b[2Jheading");
  ui.info("\x1b[31minfo");
  ui.warn("\x1b[31mwarn");
  ui.error("\x1b[31merror");
  ui.progress("\x1b[31mcapturing", 0.5);

  Table t;
  t.title = "\x1b[31mRanking";
  t.headers = {"angle", "score"};
  t.rows = {{"\x1b[31m0", "18.2"}};
  t.notes = {"\x1b[31mwith n = 2 the median equals the mean"};
  ui.table(t);

  for (const std::string& line : ui.emitted()) {
    CHECK_MESSAGE(line.find('\x1b') == std::string::npos, line);
  }
  CHECK(ui.tables().size() == 1);
}

TEST_CASE("a non-interactive terminal never enters raw mode") {
  // The rule, asserted rather than a fixed expectation: CTest does not
  // guarantee whether stdin is a pipe or a terminal.
  ScriptedTerminalUi scripted(false);
  CHECK_FALSE(scripted.interactive());

  AnsiTerminalUi ansi;
  const bool both_ttys = ::isatty(STDIN_FILENO) != 0 && ::isatty(STDOUT_FILENO) != 0;
  CHECK(ansi.interactive() == both_ttys);
  // Nothing has entered raw mode merely by constructing the UI.
  CHECK_FALSE(raw_mode_active());

  // --non-interactive wins over whatever isatty reports. Without this the flag
  // would be defeated from a real terminal: the run would still stop at the
  // first prompt, which is the one thing it exists to prevent.
  AnsiTerminalUi forced(false, true);
  CHECK_FALSE(forced.interactive());
}

TEST_CASE("the colour predicate is a rule, not a constant") {
  const char* previous = std::getenv("NO_COLOR");
  ::unsetenv("NO_COLOR");
  CHECK(color_enabled(false) == (::isatty(STDOUT_FILENO) != 0));
  // --no-color always wins.
  CHECK_FALSE(color_enabled(true));

  ::setenv("NO_COLOR", "1", 1);
  CHECK_FALSE(color_enabled(false));

  if (previous != nullptr) {
    ::setenv("NO_COLOR", previous, 1);
  } else {
    ::unsetenv("NO_COLOR");
  }
}

}  // TEST_SUITE
