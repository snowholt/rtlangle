// WP8 — control-character neutralisation, menu navigation, prompt handling, and
// the non-TTY safety rule. Spec sections 6.5 and 12.3.

#include <doctest/doctest.h>

#include "ui/ansi_terminal_ui.h"
#include "ui/safe_text.h"
#include "ui/scripted_terminal_ui.h"

#include <array>
#include <cstdlib>
#include <cstddef>
#include <string>
#include <string_view>
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

// ---------------------------------------------------------------------------
// Menu painting. The repaint moves the cursor up by the number of lines the
// menu occupies, so a line wider than the terminal would wrap onto a second
// physical row, the count would be short by one, and the redraw would smear
// instead of replacing. Truncation here is what keeps the count exact.
// ---------------------------------------------------------------------------

namespace {

// True when every byte sequence is a well-formed UTF-8 code point. Truncating
// mid-sequence would emit an invalid byte and put the column count in doubt.
bool valid_utf8(std::string_view s) {
  std::size_t i = 0;
  while (i < s.size()) {
    const auto c = static_cast<unsigned char>(s[i]);
    std::size_t extra = 0;
    if (c < 0x80) extra = 0;
    else if ((c & 0xE0) == 0xC0) extra = 1;
    else if ((c & 0xF0) == 0xE0) extra = 2;
    else if ((c & 0xF8) == 0xF0) extra = 3;
    else return false;
    if (extra > 0 && i + extra >= s.size()) return false;
    for (std::size_t k = 1; k <= extra; ++k) {
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
    }
    i += extra + 1;
  }
  return true;
}

}  // namespace

TEST_CASE("display_width counts code points, not bytes") {
  CHECK(display_width("") == 0);
  CHECK(display_width("plain") == 5);
  CHECK(display_width("Straße") == 6);            // U+00DF is two bytes
  CHECK(display_width("↑↓") == 2);  // two arrows, six bytes
}

TEST_CASE("menu lines are truncated so the repaint line count is exact") {
  const std::array<MenuItem, 2> items = {
      MenuItem{"A label long enough to overrun a narrow terminal on its own",
               "and a detail that makes it longer still"},
      MenuItem{"Short", ""}};
  const std::size_t width = 40;
  const auto lines = menu_lines("A title also far too wide for forty columns", items, 0, width, true);

  // blank, title, two items, blank, hint.
  CHECK(lines.size() == 6);
  for (const MenuLine& line : lines) {
    CHECK(display_width(line.text) <= width - 1);
    CHECK(valid_utf8(line.text));
  }
  CHECK(lines[1].style == MenuLine::Style::Title);
  CHECK(lines.back().style == MenuLine::Style::Hint);
  // The marker sits on the initial index and nowhere else.
  CHECK(lines[2].text.rfind("> ", 0) == 0);
  CHECK(lines[3].text.rfind("  ", 0) == 0);
}

TEST_CASE("truncation lands on a code point boundary") {
  const std::array<MenuItem, 1> items = {MenuItem{"Straße Straße Straße", ""}};
  for (std::size_t width = 2; width <= 24; ++width) {
    const auto lines = menu_lines("Straße", items, 0, width, false);
    for (const MenuLine& line : lines) {
      CHECK(display_width(line.text) <= width - 1);
      CHECK(valid_utf8(line.text));
    }
  }
}

TEST_CASE("the cursor marker follows the index and the hint is optional") {
  const std::array<MenuItem, 3> items = {MenuItem{"First", ""}, MenuItem{"Second", ""},
                                         MenuItem{"Third", ""}};
  const auto lines = menu_lines("Choose", items, 2, 80, false);
  // blank, title, three items — and no hint.
  CHECK(lines.size() == 5);
  CHECK(lines[2].text.rfind("  First", 0) == 0);
  CHECK(lines[4].text.rfind("> Third", 0) == 0);
  for (const MenuLine& line : lines) CHECK(line.style != MenuLine::Style::Hint);
}

TEST_CASE("the key hint names every key the menu accepts, in both forms") {
  for (const bool unicode : {false, true}) {
    const std::string hint(menu_key_hint(unicode));
    CHECK(hint.find("j/k") != std::string::npos);
    CHECK(hint.find("Enter") != std::string::npos);
    CHECK(hint.find("q") != std::string::npos);
    CHECK(valid_utf8(hint));
  }
  // The ASCII form is reachable, because a terminal that cannot render an arrow
  // would also count its bytes as columns and wrap the line the repaint counted.
  const std::string ascii(menu_key_hint(false));
  CHECK(display_width(ascii) == ascii.size());
  CHECK(std::string(menu_key_hint(true)).find("↑") != std::string::npos);
}

TEST_CASE("the unicode predicate is a rule, not a constant") {
  const char* previous = std::getenv("LC_ALL");
  ::setenv("LC_ALL", "C.UTF-8", 1);
  CHECK(unicode_enabled());
  ::setenv("LC_ALL", "C", 1);
  CHECK_FALSE(unicode_enabled());
  if (previous != nullptr) {
    ::setenv("LC_ALL", previous, 1);
  } else {
    ::unsetenv("LC_ALL");
  }
}

TEST_CASE("terminal_width falls back rather than returning zero") {
  // In the suite stdout may be a pipe, where TIOCGWINSZ fails. A width of zero
  // would make every line truncate to nothing.
  CHECK(terminal_width() > 0);
}

TEST_CASE("the detail column is aligned only while alignment still fits") {
  // Marker 2 + widest label 12 + gap 3 + widest detail 8 is 25 columns, so the
  // aligned form fits in 26 and does not fit in 25.
  const std::array<MenuItem, 2> items = {MenuItem{"Short", "a detail"},
                                         MenuItem{"Longer label", "another"}};

  const auto wide = menu_lines("t", items, 0, 80, false);
  CHECK(wide[2].text.find("a detail") == wide[3].text.find("another"));

  // One column too narrow for it. Padding to the widest label would push the
  // longer line past the width and cost the end of a detail to truncation, so
  // the column goes ragged instead and both details survive intact.
  const auto narrow = menu_lines("t", items, 0, 25, false);
  CHECK(narrow[2].text.find("a detail") != narrow[3].text.find("another"));
  for (const MenuLine& line : narrow) CHECK(display_width(line.text) <= 24);
  CHECK(narrow[2].text.find("a detail") != std::string::npos);
  CHECK(narrow[3].text.find("another") != std::string::npos);
}

}  // TEST_SUITE
