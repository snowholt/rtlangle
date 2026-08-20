#include "ui/scripted_terminal_ui.h"

#include "ui/safe_text.h"

#include <algorithm>
#include <sstream>

namespace rtlangle::ui {

int ScriptedTerminalUi::menu(std::string_view title, std::span<const MenuItem> items,
                             int initial_index) {
  emitted_.push_back("menu: " + safe_text(title));
  for (const MenuItem& item : items) emitted_.push_back("  - " + safe_text(item.label));

  if (menu_choices_.empty()) return -1;
  const int chosen = menu_choices_.front();
  menu_choices_.pop_front();
  if (chosen < 0) return -1;
  if (items.empty()) return -1;
  // An out-of-range choice clamps, as an arrow-key menu's cursor does, rather
  // than indexing past the end.
  (void)initial_index;
  return std::clamp(chosen, 0, static_cast<int>(items.size()) - 1);
}

std::optional<std::string> ScriptedTerminalUi::prompt_line(std::string_view label,
                                                           std::string_view default_value) {
  emitted_.push_back("prompt: " + safe_text(label));
  if (lines_.empty()) return std::nullopt;
  const std::string line = lines_.front();
  lines_.pop_front();
  if (line.empty()) return std::string(default_value);
  return line;
}

bool ScriptedTerminalUi::confirm(std::string_view question, bool default_yes) {
  emitted_.push_back("confirm: " + safe_text(question));
  if (confirms_.empty()) return default_yes;
  const bool answer = confirms_.front();
  confirms_.pop_front();
  return answer;
}

void ScriptedTerminalUi::heading(std::string_view text) {
  emitted_.push_back("heading: " + safe_text(text));
}
void ScriptedTerminalUi::info(std::string_view text) {
  emitted_.push_back("info: " + safe_text(text));
}
void ScriptedTerminalUi::warn(std::string_view text) {
  emitted_.push_back("warn: " + safe_text(text));
}
void ScriptedTerminalUi::error(std::string_view text) {
  emitted_.push_back("error: " + safe_text(text));
}

void ScriptedTerminalUi::progress(std::string_view label, double fraction) {
  std::ostringstream os;
  os.precision(2);
  os << "progress: " << safe_text(label) << " " << std::fixed << fraction;
  emitted_.push_back(os.str());
}

void ScriptedTerminalUi::table(const Table& t) {
  tables_.push_back(t);
  emitted_.push_back("table: " + safe_text(t.title));
  for (const auto& row : t.rows) {
    std::string line = "  row:";
    for (const auto& cell : row) line += " " + safe_text(cell);
    emitted_.push_back(line);
  }
  for (const auto& note : t.notes) emitted_.push_back("  note: " + safe_text(note));
}

}  // namespace rtlangle::ui
