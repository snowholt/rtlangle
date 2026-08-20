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
#include <sys/ioctl.h>
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

constexpr const char* kHideCursor = "\033[?25l";
constexpr const char* kShowCursorSeq = "\033[?25h";
constexpr const char* kClearBelow = "\033[J";

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
    if (::tcsetattr(STDIN_FILENO, TCSANOW, &attributes(false)) != 0) return;
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

  // A lone Escape must not hang the menu. With VMIN 1 the read of the byte
  // after an Escape blocks until the operator presses something else, which
  // from the keyboard is indistinguishable from a hung program. For the
  // continuation of an escape sequence only, VMIN 0 with VTIME 1 turns that
  // block into a 100 ms wait after which read returns 0 and the bare Escape
  // cancels. It is restored immediately afterwards so ordinary keys still
  // block rather than spinning.
  void sequence_timeout(bool on) {
    if (!entered_) return;
    ::tcsetattr(STDIN_FILENO, TCSANOW, &attributes(on));
  }

 private:
  static const termios& attributes(bool timeout) {
    static thread_local termios t{};
    t = g_saved.state;
    t.c_lflag = static_cast<tcflag_t>(t.c_lflag & ~static_cast<tcflag_t>(ICANON | ECHO));
    t.c_cc[VMIN] = timeout ? 0 : 1;
    t.c_cc[VTIME] = timeout ? 1 : 0;
    return t;
  }

  bool entered_ = false;
};

std::string pad(std::string_view s, std::size_t width) {
  std::string out(s);
  while (out.size() < width) out.push_back(' ');
  return out;
}

// Copies whole code points until `columns` of them have been taken. Stopping on
// a lead byte is what keeps the result well-formed UTF-8; cutting mid-sequence
// would emit a byte no terminal can attribute a width to, which is the same
// failure the truncation exists to prevent.
std::string truncate_to(std::string_view s, std::size_t columns) {
  if (display_width(s) <= columns) return std::string(s);
  std::string out;
  std::size_t taken = 0;
  for (const char byte : s) {
    if ((static_cast<unsigned char>(byte) & 0xC0) != 0x80) {
      if (taken == columns) break;
      ++taken;
    }
    out.push_back(byte);
  }
  return out;
}

// The block a menu draws, painted in place. Each paint moves the cursor back
// over the previous block and clears from there, so navigating replaces the
// menu rather than appending another copy of it.
class MenuPainter {
 public:
  explicit MenuPainter(bool color) : color_(color) {
    std::cout << kHideCursor;
    std::cout.flush();
  }
  ~MenuPainter() {
    std::cout << kShowCursorSeq;
    std::cout.flush();
  }
  MenuPainter(const MenuPainter&) = delete;
  MenuPainter& operator=(const MenuPainter&) = delete;

  void paint(const std::vector<MenuLine>& lines) {
    std::ostringstream os;
    if (painted_ > 0) os << "\033[" << painted_ << 'A' << kClearBelow;
    for (const MenuLine& line : lines) {
      const char* color = nullptr;
      if (color_) {
        if (line.style == MenuLine::Style::Title) color = kBold;
        if (line.style == MenuLine::Style::Hint) color = kDim;
      }
      if (color != nullptr) os << color;
      os << line.text;
      if (color != nullptr) os << kReset;
      os << '\n';
    }
    std::cout << os.str();
    std::cout.flush();
    painted_ = lines.size();
  }

 private:
  bool        color_ = false;
  std::size_t painted_ = 0;
};

}  // namespace

bool color_enabled(bool no_color_requested) {
  if (no_color_requested) return false;
  if (const char* env = std::getenv("NO_COLOR"); env != nullptr && env[0] != '\0') return false;
  return ::isatty(STDOUT_FILENO) != 0;
}

bool unicode_enabled() {
  for (const char* name : {"LC_ALL", "LC_CTYPE", "LANG"}) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') continue;
    const std::string setting(value);
    return setting.find("UTF-8") != std::string::npos ||
           setting.find("utf8") != std::string::npos ||
           setting.find("UTF8") != std::string::npos ||
           setting.find("utf-8") != std::string::npos;
  }
  return false;
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

std::size_t display_width(std::string_view s) {
  std::size_t columns = 0;
  for (const char byte : s) {
    // Continuation bytes carry no column of their own.
    if ((static_cast<unsigned char>(byte) & 0xC0) != 0x80) ++columns;
  }
  return columns;
}

std::size_t terminal_width() {
  winsize window{};
  if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) == 0 && window.ws_col > 0) {
    return static_cast<std::size_t>(window.ws_col);
  }
  if (const char* env = std::getenv("COLUMNS"); env != nullptr) {
    const int columns = std::atoi(env);
    if (columns > 0) return static_cast<std::size_t>(columns);
  }
  return 80;
}

std::string_view menu_key_hint(bool unicode) {
  if (unicode) return "  ↑/↓ or j/k move · Enter select · q quit";
  return "  Up/Down or j/k move | Enter select | q quit";
}

std::vector<MenuLine> menu_lines(std::string_view title, std::span<const MenuItem> items,
                                 int cursor, std::size_t width, bool include_hint) {
  // One column is left free: a glyph in the last column defers the wrap on some
  // terminals, which would move the cursor a row without emitting a newline and
  // put the count out by one at exactly the moment it matters.
  const std::size_t columns = width > 1 ? width - 1 : 1;

  std::vector<std::string> labels;
  std::vector<std::string> details;
  labels.reserve(items.size());
  details.reserve(items.size());
  std::size_t label_width = 0;
  std::size_t detail_width = 0;
  for (const MenuItem& item : items) {
    labels.push_back(safe_text(item.label));
    details.push_back(safe_text(item.detail));
    label_width = std::max(label_width, display_width(labels.back()));
    detail_width = std::max(detail_width, display_width(details.back()));
  }

  // Details read better in a column, but padding to it can push a line past the
  // terminal width and cost the end of a detail to truncation. Align only when
  // the widest aligned line still fits; a ragged column beats a cut sentence.
  constexpr std::size_t kMarker = 2;
  constexpr std::size_t kGap = 3;
  const bool aligned = kMarker + label_width + kGap + detail_width <= columns;

  std::vector<MenuLine> lines;
  lines.reserve(items.size() + 4);
  lines.push_back({"", MenuLine::Style::Plain});
  lines.push_back({truncate_to(safe_text(title), columns), MenuLine::Style::Title});

  for (std::size_t i = 0; i < items.size(); ++i) {
    std::string text = static_cast<int>(i) == cursor ? "> " : "  ";
    text += labels[i];
    if (!details[i].empty()) {
      if (aligned) text.append(label_width - display_width(labels[i]), ' ');
      text.append(kGap, ' ');
      text += details[i];
    }
    lines.push_back({truncate_to(text, columns), MenuLine::Style::Plain});
  }

  if (include_hint) {
    lines.push_back({"", MenuLine::Style::Plain});
    lines.push_back(
        {truncate_to(menu_key_hint(unicode_enabled()), columns), MenuLine::Style::Hint});
  }
  return lines;
}

AnsiTerminalUi::AnsiTerminalUi(bool no_color, bool non_interactive)
    : interactive_(!non_interactive && ::isatty(STDIN_FILENO) != 0 &&
                   ::isatty(STDOUT_FILENO) != 0),
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
  // The fallback path for a pipe and for --non-interactive. It emits no cursor
  // control of any kind: nothing may write an escape sequence to something that
  // is not a terminal.
  heading(title);
  for (std::size_t i = 0; i < items.size(); ++i) {
    std::ostringstream os;
    os << "  " << (i + 1) << ") " << safe_text(items[i].label);
    if (!items[i].detail.empty()) os << "   " << safe_text(items[i].detail);
    info(os.str());
  }
  std::ostringstream label;
  label << "Choose 1-" << items.size() << ", or q to cancel";
  const auto answer = prompt_line(label.str(), "");
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

  const int last = static_cast<int>(items.size()) - 1;
  int       cursor = std::clamp(initial_index, 0, last);

  MenuPainter painter(color_);
  // Leaves the block on screen without its key hint, so what remains in the
  // scrollback is the menu and the choice rather than an instruction for a
  // menu that is no longer accepting keys.
  const auto settle = [&](int result) {
    painter.paint(menu_lines(title, items, cursor, terminal_width(), false));
    return result;
  };

  for (;;) {
    painter.paint(menu_lines(title, items, cursor, terminal_width(), true));

    char          c = 0;
    const ssize_t n = ::read(STDIN_FILENO, &c, 1);
    if (n <= 0) return settle(-1);

    if (c == '\n' || c == '\r') return settle(cursor);
    if (c == 'q' || c == 'Q') return settle(-1);
    if (c == 'j') cursor = std::min(cursor + 1, last);
    if (c == 'k') cursor = std::max(cursor - 1, 0);
    if (c == 27) {   // an escape, which begins an arrow key or is a bare Escape
      char seq[2] = {0, 0};
      raw.sequence_timeout(true);
      const ssize_t introducer = ::read(STDIN_FILENO, &seq[0], 1);
      const ssize_t final_byte =
          introducer > 0 && seq[0] == '[' ? ::read(STDIN_FILENO, &seq[1], 1) : 0;
      raw.sequence_timeout(false);

      // Nothing followed within the timeout, or what followed was not an arrow:
      // a bare Escape cancels, and it does so without waiting for another key.
      if (introducer <= 0 || seq[0] != '[' || final_byte <= 0) return settle(-1);
      if (seq[1] == 'B') cursor = std::min(cursor + 1, last);
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
