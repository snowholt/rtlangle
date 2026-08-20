#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rtlangle::ui {

struct MenuItem {
  std::string label;
  std::string detail;
};

struct Table {
  std::string                           title;
  std::vector<std::string>              headers;
  std::vector<std::vector<std::string>> rows;
  // Printed under the table. Used for the "with n = 2 the median equals the
  // mean" note and for the spread column's explanation, so a reader cannot take
  // a spread for an interval estimate.
  std::vector<std::string> notes;
};

class ITerminalUi {
 public:
  virtual ~ITerminalUi() = default;

  // False for pipes and for --non-interactive. Nothing may call tcsetattr when
  // this is false.
  virtual bool interactive() const = 0;

  // Returns the chosen index, or -1 for cancel.
  virtual int menu(std::string_view title, std::span<const MenuItem> items,
                   int initial_index) = 0;

  // Returns the typed line, the default on blank input, or nullopt when the
  // input ended.
  virtual std::optional<std::string> prompt_line(std::string_view label,
                                                 std::string_view default_value) = 0;

  virtual bool confirm(std::string_view question, bool default_yes) = 0;

  virtual void heading(std::string_view) = 0;
  virtual void info(std::string_view) = 0;
  virtual void warn(std::string_view) = 0;
  virtual void error(std::string_view) = 0;
  virtual void progress(std::string_view label, double fraction) = 0;
  virtual void table(const Table&) = 0;
};

}  // namespace rtlangle::ui
