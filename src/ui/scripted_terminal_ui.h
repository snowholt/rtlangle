#pragma once

#include "ui/terminal_ui.h"

#include <deque>
#include <string>
#include <vector>

namespace rtlangle::ui {

// The test double for ITerminalUi. It lives in src/ rather than in the
// test-support directory for the same reason SyntheticSource does: the
// `--non-interactive` path needs a UI that never touches a terminal, and a
// production path may not depend on test code.
//
// Scripted answers are consumed in order. Running out of answers is reported
// rather than guessed at: menu() returns -1 (cancel), prompt_line() returns
// nullopt (input ended), and confirm() returns its default.
class ScriptedTerminalUi final : public ITerminalUi {
 public:
  explicit ScriptedTerminalUi(bool interactive = true) : interactive_(interactive) {}

  void push_line(std::string line) { lines_.push_back(std::move(line)); }
  void push_menu_choice(int index) { menu_choices_.push_back(index); }
  void push_cancel() { menu_choices_.push_back(-1); }
  void push_confirm(bool yes) { confirms_.push_back(yes); }

  // Every line the UI rendered, in order, already neutralised.
  const std::vector<std::string>& emitted() const { return emitted_; }
  const std::vector<Table>&       tables() const { return tables_; }
  bool                            exhausted() const { return lines_.empty() && menu_choices_.empty(); }

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
  bool                     interactive_ = true;
  std::deque<std::string>  lines_;
  std::deque<int>          menu_choices_;
  std::deque<bool>         confirms_;
  std::vector<std::string> emitted_;
  std::vector<Table>       tables_;
};

}  // namespace rtlangle::ui
