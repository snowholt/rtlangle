#pragma once

#include "ui/terminal_ui.h"

#include <string>

namespace rtlangle::ui {

// True when colour may be emitted: stdout is a TTY, NO_COLOR is unset, and
// --no-color was not given. Written as a predicate rather than a constant so a
// test can assert the RULE; CTest does not guarantee whether stdout is a pipe.
bool color_enabled(bool no_color_requested);

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
