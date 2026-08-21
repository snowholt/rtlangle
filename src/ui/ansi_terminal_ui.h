#pragma once

#include "ui/terminal_ui.h"

#include <cstddef>
#include <string>

namespace rtlangle::ui {

// True when colour may be emitted: stdout is a TTY, NO_COLOR is unset, and
// --no-color was not given. Written as a predicate rather than a constant so a
// test can assert the RULE; CTest does not guarantee whether stdout is a pipe.
bool color_enabled(bool no_color_requested);

// True when the locale environment names UTF-8. A terminal that cannot render
// an arrow would also count its three bytes as three columns, which would wrap
// the hint line and put the repaint's line count out by one, so the key hint
// has an ASCII form for that case. A predicate, for the same reason as above.
bool unicode_enabled();

// Installs the SIGINT and SIGTERM handlers that restore the terminal.
//
// The saved termios state lives at namespace scope, never on the stack, so a
// handler can never reference destroyed state. The handlers use only
// async-signal-safe calls: tcsetattr to restore, write to re-show the cursor,
// then re-raise with the default disposition. A Ctrl-C must never leave the
// operator with a broken shell.
void install_signal_handlers();

// True while raw mode is active, for the pty test to observe.
bool raw_mode_active();

// ---------------------------------------------------------------------------
// Menu painting
//
// A menu repaints by moving the cursor up over the block it last drew and
// clearing from there. That is only correct while the number of lines drawn
// equals the number of physical rows they occupy, so every line is truncated
// to the terminal width before it is written. Without the truncation a single
// over-wide line wraps, the count is short, and each keystroke smears a
// partial copy of the menu down the screen instead of replacing it.
//
// Width is counted in UTF-8 code points. A double-width glyph therefore counts
// as one column; no menu built by this program contains one, and stating the
// limit is preferable to a table that would pretend to more than it covers.
// ---------------------------------------------------------------------------

// Physical columns available: TIOCGWINSZ, then COLUMNS, then 80. Never zero.
std::size_t terminal_width();

// The line that names the keys a menu accepts, in a UTF-8 and an ASCII form.
std::string_view menu_key_hint(bool unicode);

struct MenuLine {
  enum class Style { Plain, Title, Hint };
  std::string text;
  Style       style = Style::Plain;
};

// The exact physical lines a menu occupies: a blank, the title, one per item,
// then a blank and the key hint when `include_hint`. Text is neutralised and
// then truncated, in that order, so truncation can never cut a neutralised
// sequence back into a live one.
std::vector<MenuLine> menu_lines(std::string_view title, std::span<const MenuItem> items,
                                 int cursor, std::size_t width, bool include_hint);

// Code points, not bytes. See the note on width above.
std::size_t display_width(std::string_view);

class AnsiTerminalUi final : public ITerminalUi {
 public:
  // `non_interactive` is the operator saying there is nobody at the keyboard,
  // and it wins over what isatty reports. Without it a run started from a real
  // terminal with --non-interactive would still stop at the first prompt, which
  // is the one thing the flag exists to prevent.
  explicit AnsiTerminalUi(bool no_color = false, bool non_interactive = false);

  bool interactive() const override { return interactive_; }

  int menu(std::string_view title, std::span<const MenuItem> items, int initial_index) override;
  std::optional<std::string> prompt_line(std::string_view label,
                                         std::string_view default_value) override;
  bool confirm(std::string_view question, bool default_yes) override;
  void heading(std::string_view) override;
  void info(std::string_view) override;
  void warn(std::string_view) override;
  void error(std::string_view) override;
  void progress(std::string_view label, double fraction) override;
  void table(const Table&) override;

 private:
  void write_line(const char* color, std::string_view prefix, std::string_view text);
  int  menu_raw(std::string_view title, std::span<const MenuItem> items, int initial_index);
  int  menu_numbered(std::string_view title, std::span<const MenuItem> items);

  bool interactive_ = false;
  bool color_ = false;
};

}  // namespace rtlangle::ui
