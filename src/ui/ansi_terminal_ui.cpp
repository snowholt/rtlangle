#include "ui/ansi_terminal_ui.h"

#include "ui/safe_text.h"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <termios.h>
#include <unistd.h>

namespace rtlangle::ui {
namespace {

// Namespace-scope, never on the stack: a signal handler that referenced a
// destroyed stack object would be undefined behaviour precisely at the moment
// the operator most needs the terminal restored.
struct SavedTerminal {
  std::atomic<bool> valid{false};
  termios           state{};
};
SavedTerminal g_saved;

constexpr const char* kReset = "\033[0m";
constexpr const char* kBold = "\033[1m";
constexpr const char* kYellow = "\033[33m";
constexpr const char* kRed = "\033[31m";
constexpr const char* kDim = "\033[2m";

// Async-signal-safe: tcsetattr and write are, and nothing else is called.
void restore_and_reraise(int signal_number) {
  if (g_saved.valid.load()) {
    ::tcsetattr(STDIN_FILENO, TCSANOW, &g_saved.state);
    g_saved.valid.store(false);
  }
  static const char kShowCursor[] = "\033[?25h";
  ssize_t ignored = ::write(STDOUT_FILENO, kShowCursor, sizeof(kShowCursor) - 1);
  (void)ignored;

  struct sigaction default_action {};
  default_action.sa_handler = SIG_DFL;
  ::sigemptyset(&default_action.sa_mask);
  ::sigaction(signal_number, &default_action, nullptr);
  ::raise(signal_number);
}

// RAII raw mode. It is entered only when both stdin and stdout are terminals,
// so no termios call is ever made against a pipe.
class RawMode {
 public:
  RawMode() {
    if (::tcgetattr(STDIN_FILENO, &g_saved.state) != 0) return;
    termios raw = g_saved.state;
    raw.c_lflag = static_cast<tcflag_t>(raw.c_lflag & ~static_cast<tcflag_t>(ICANON | ECHO));
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (::tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return;
    g_saved.valid.store(true);
    entered_ = true;
  }
  ~RawMode() {
    if (!entered_) return;
    ::tcsetattr(STDIN_FILENO, TCSANOW, &g_saved.state);
    g_saved.valid.store(false);
  }
  RawMode(const RawMode&) = delete;
  RawMode& operator=(const RawMode&) = delete;
  bool entered() const { return entered_; }

 private:
  bool entered_ = false;
};

std::string pad(std::string_view s, std::size_t width) {
  std::string out(s);
  while (out.size() < width) out.push_back(' ');
  return out;
}

}  // namespace

bool color_enabled(bool no_color_requested) {
  if (no_color_requested) return false;
  if (const char* env = std::getenv("NO_COLOR"); env != nullptr && env[0] != '\0') return false;
  return ::isatty(STDOUT_FILENO) != 0;
}

bool raw_mode_active() { return g_saved.valid.load(); }

void install_signal_handlers() {
  struct sigaction action {};
  action.sa_handler = restore_and_reraise;
  ::sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  ::sigaction(SIGINT, &action, nullptr);
  ::sigaction(SIGTERM, &action, nullptr);
}

AnsiTerminalUi::AnsiTerminalUi(bool no_color)
    : interactive_(::isatty(STDIN_FILENO) != 0 && ::isatty(STDOUT_FILENO) != 0),
      color_(color_enabled(no_color)) {}

void AnsiTerminalUi::write_line(const char* color, std::string_view prefix,
                                std::string_view text) {
  std::ostringstream os;
  if (color_ && color != nullptr) os << color;
  if (!prefix.empty()) os << prefix;
  os << safe_text(text);
  if (color_ && color != nullptr) os << kReset;
  std::cout << os.str() << '\n';
  std::cout.flush();
}

void AnsiTerminalUi::heading(std::string_view text) {
  std::cout << '\n';
  write_line(kBold, "", text);
}
void AnsiTerminalUi::info(std::string_view text) { write_line(nullptr, "", text); }
void AnsiTerminalUi::warn(std::string_view text) { write_line(kYellow, "warning: ", text); }
void AnsiTerminalUi::error(std::string_view text) { write_line(kRed, "error: ", text); }

void AnsiTerminalUi::progress(std::string_view label, double fraction) {
  const double f = std::clamp(fraction, 0.0, 1.0);
  constexpr int kWidth = 30;
  const int filled = static_cast<int>(f * kWidth);
  std::ostringstream os;
  os << '\r' << safe_text(label) << "  [";
  for (int i = 0; i < kWidth; ++i) os << (i < filled ? '#' : '.');
  os << "] " << static_cast<int>(f * 100.0) << "%   ";
  std::cout << os.str();
  std::cout.flush();
  if (f >= 1.0) std::cout << '\n';
}

void AnsiTerminalUi::table(const Table& t) {
  if (!t.title.empty()) heading(t.title);

  std::vector<std::size_t> widths(t.headers.size(), 0);
  for (std::size_t i = 0; i < t.headers.size(); ++i) {
    widths[i] = safe_text(t.headers[i]).size();
  }
  for (const auto& row : t.rows) {
    for (std::size_t i = 0; i < row.size() && i < widths.size(); ++i) {
      widths[i] = std::max(widths[i], safe_text(row[i]).size());
    }
  }

  std::ostringstream header;
  for (std::size_t i = 0; i < t.headers.size(); ++i) {
    if (i > 0) header << "  ";
    header << pad(safe_text(t.headers[i]), widths[i]);
  }
  write_line(kBold, "", header.str());

  for (const auto& row : t.rows) {
    std::ostringstream line;
    for (std::size_t i = 0; i < row.size() && i < widths.size(); ++i) {
      if (i > 0) line << "  ";
      line << pad(safe_text(row[i]), widths[i]);
    }
    write_line(nullptr, "", line.str());
  }
  for (const auto& note : t.notes) write_line(kDim, "", note);
}

std::optional<std::string> AnsiTerminalUi::prompt_line(std::string_view label,
                                                       std::string_view default_value) {
  std::cout << safe_text(label);
  if (!default_value.empty()) std::cout << " [" << safe_text(default_value) << "]";
  std::cout << ": ";
  std::cout.flush();

  std::string line;
  if (!std::getline(std::cin, line)) return std::nullopt;
  if (line.empty()) return std::string(default_value);
  return line;
}

bool AnsiTerminalUi::confirm(std::string_view question, bool default_yes) {
  const auto answer = prompt_line(
      std::string(safe_text(question)) + (default_yes ? " (Y/n)" : " (y/N)"), "");
  if (!answer.has_value()) return default_yes;
  if (answer->empty()) return default_yes;
  const char c = static_cast<char>(std::tolower(static_cast<unsigned char>((*answer)[0])));
  if (c == 'y') return true;
  if (c == 'n') return false;
  return default_yes;
}

int AnsiTerminalUi::menu_numbered(std::string_view title, std::span<const MenuItem> items) {
  heading(title);
  for (std::size_t i = 0; i < items.size(); ++i) {
    std::ostringstream os;
    os << "  " << (i + 1) << ") " << safe_text(items[i].label);
    if (!items[i].detail.empty()) os << "   " << safe_text(items[i].detail);
    info(os.str());
  }
  const auto answer = prompt_line("Choose a number, or q to cancel", "");
  if (!answer.has_value() || answer->empty()) return -1;
  if ((*answer)[0] == 'q' || (*answer)[0] == 'Q') return -1;
  const int chosen = std::atoi(answer->c_str());
  if (chosen < 1 || chosen > static_cast<int>(items.size())) return -1;
  return chosen - 1;
}

int AnsiTerminalUi::menu_raw(std::string_view title, std::span<const MenuItem> items,
                             int initial_index) {
  RawMode raw;
  if (!raw.entered()) return menu_numbered(title, items);

  int cursor = std::clamp(initial_index, 0, static_cast<int>(items.size()) - 1);
  for (;;) {
    heading(title);
    for (std::size_t i = 0; i < items.size(); ++i) {
      std::ostringstream os;
      os << (static_cast<int>(i) == cursor ? "> " : "  ") << safe_text(items[i].label);
      if (!items[i].detail.empty()) os << "   " << safe_text(items[i].detail);
      info(os.str());
    }

    char c = 0;
    const ssize_t n = ::read(STDIN_FILENO, &c, 1);
    if (n <= 0) return -1;

    if (c == '\n' || c == '\r') return cursor;
    if (c == 'q' || c == 'Q') return -1;
    if (c == 'j') cursor = std::min(cursor + 1, static_cast<int>(items.size()) - 1);
    if (c == 'k') cursor = std::max(cursor - 1, 0);
    if (c == 27) {   // an escape, which begins an arrow key or is a bare Escape
      char seq[2] = {0, 0};
      if (::read(STDIN_FILENO, &seq[0], 1) <= 0) return -1;
      if (seq[0] != '[') return -1;   // a bare Escape cancels
      if (::read(STDIN_FILENO, &seq[1], 1) <= 0) return -1;
      if (seq[1] == 'B') cursor = std::min(cursor + 1, static_cast<int>(items.size()) - 1);
      if (seq[1] == 'A') cursor = std::max(cursor - 1, 0);
    }
  }
}

int AnsiTerminalUi::menu(std::string_view title, std::span<const MenuItem> items,
                         int initial_index) {
  if (items.empty()) return -1;
  // Raw mode only when it is safe. With stdin a pipe, the menu falls back to
  // reading a numbered choice as a line and no termios call is made.
  if (!interactive_) return menu_numbered(title, items);
  return menu_raw(title, items, initial_index);
}

}  // namespace rtlangle::ui
