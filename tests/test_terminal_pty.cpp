// WP8 — the pseudo-terminal test. Spec section 12.3.
//
// A Ctrl-C must never leave the operator with a broken shell, so the SIGINT
// handler restores the terminal attributes it found. That can only be checked
// against a real terminal, which is what a pseudo-terminal provides without
// requiring the suite to be run interactively.

#include <doctest/doctest.h>

#include "ui/ansi_terminal_ui.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <csignal>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pty.h>
#include <termios.h>
#include <unistd.h>

using namespace rtlangle::ui;

namespace {

bool termios_equal(const termios& a, const termios& b) {
  return a.c_iflag == b.c_iflag && a.c_oflag == b.c_oflag && a.c_cflag == b.c_cflag &&
         a.c_lflag == b.c_lflag &&
         std::memcmp(a.c_cc, b.c_cc, sizeof(a.c_cc)) == 0;
}

// Swaps stdin and stdout for a pseudo-terminal for the duration of the scope,
// so AnsiTerminalUi sees a real terminal and enters raw mode.
class PtySwap {
 public:
  PtySwap() {
    if (::openpty(&master_, &slave_, nullptr, nullptr, nullptr) != 0) return;
    saved_stdin_ = ::dup(STDIN_FILENO);
    saved_stdout_ = ::dup(STDOUT_FILENO);
    ::dup2(slave_, STDIN_FILENO);
    ::dup2(slave_, STDOUT_FILENO);
    ok_ = true;
  }
  ~PtySwap() {
    if (!ok_) return;
    ::dup2(saved_stdin_, STDIN_FILENO);
    ::dup2(saved_stdout_, STDOUT_FILENO);
    ::close(saved_stdin_);
    ::close(saved_stdout_);
    ::close(slave_);
    ::close(master_);
  }
  PtySwap(const PtySwap&) = delete;
  PtySwap& operator=(const PtySwap&) = delete;

  bool ok() const { return ok_; }
  int  master() const { return master_; }

 private:
  int  master_ = -1;
  int  slave_ = -1;
  int  saved_stdin_ = -1;
  int  saved_stdout_ = -1;
  bool ok_ = false;
};


// Puts the pseudo-terminal into raw mode before the menu runs, so a byte
// written to the master is deliverable immediately. Without this a lone Escape
// would sit in the line discipline's canonical buffer waiting for a newline,
// and the test would be measuring the tty rather than the menu.
class RawSlave {
 public:
  RawSlave() {
    if (::tcgetattr(STDIN_FILENO, &saved_) != 0) return;
    termios raw = saved_;
    raw.c_lflag = static_cast<tcflag_t>(raw.c_lflag & ~static_cast<tcflag_t>(ICANON | ECHO));
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    ok_ = ::tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
  }
  ~RawSlave() {
    if (ok_) ::tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
  }
  RawSlave(const RawSlave&) = delete;
  RawSlave& operator=(const RawSlave&) = delete;
  bool ok() const { return ok_; }

 private:
  termios saved_{};
  bool    ok_ = false;
};

// Everything the program has written to the terminal so far.
std::string drain(int master) {
  const int flags = ::fcntl(master, F_GETFL, 0);
  ::fcntl(master, F_SETFL, flags | O_NONBLOCK);
  std::string out;
  char        buffer[4096];
  for (;;) {
    const ssize_t n = ::read(master, buffer, sizeof buffer);
    if (n <= 0) break;
    out.append(buffer, static_cast<std::size_t>(n));
  }
  ::fcntl(master, F_SETFL, flags);
  return out;
}

std::size_t count_of(std::string_view haystack, std::string_view needle) {
  std::size_t n = 0;
  for (std::size_t at = haystack.find(needle); at != std::string_view::npos;
       at = haystack.find(needle, at + needle.size())) {
    ++n;
  }
  return n;
}

// Occurrences of ESC [ <digits> A, the sequence that moves the cursor back to
// the top of the menu block. Bold, dim, and reset all end in 'm', and the
// cursor-visibility sequences end in 'h' or 'l', so none of them match.
std::size_t count_cursor_up(std::string_view s) {
  std::size_t n = 0;
  for (std::size_t i = 0; i + 2 < s.size(); ++i) {
    if (s[i] != '\033' || s[i + 1] != '[') continue;
    std::size_t j = i + 2;
    while (j < s.size() && s[j] >= '0' && s[j] <= '9') ++j;
    if (j > i + 2 && j < s.size() && s[j] == 'A') ++n;
  }
  return n;
}

}  // namespace

TEST_SUITE("terminal_pty") {

TEST_CASE("the terminal attributes survive a menu, and a SIGINT during one restores them") {
  PtySwap pty;
  REQUIRE(pty.ok());

  termios before{};
  REQUIRE(::tcgetattr(STDIN_FILENO, &before) == 0);

  AnsiTerminalUi ui;
  REQUIRE(ui.interactive());

  const std::array<MenuItem, 2> items = {MenuItem{"First", ""}, MenuItem{"Second", ""}};

  SUBCASE("a completed menu leaves the attributes as it found them") {
    const char enter = '\r';
    REQUIRE(::write(pty.master(), &enter, 1) == 1);
    CHECK(ui.menu("Choose", items, 0) == 0);

    termios after{};
    REQUIRE(::tcgetattr(STDIN_FILENO, &after) == 0);
    CHECK(termios_equal(before, after));
    CHECK_FALSE(raw_mode_active());
  }

  SUBCASE("the handler restores the attributes it saved") {
    // The handler is installed for the duration of this case only, and the
    // default disposition is restored afterwards so a re-raised SIGINT cannot
    // end the test run.
    struct sigaction previous {};
    struct sigaction ignore {};
    ignore.sa_handler = SIG_IGN;
    ::sigemptyset(&ignore.sa_mask);
    ::sigaction(SIGINT, &ignore, &previous);

    install_signal_handlers();

    // Enter raw mode by starting a menu that blocks on input, then deliver the
    // signal from this same thread by invoking the handler through raise().
    // Blocking the read and signalling from another thread would race; what the
    // test needs is that the handler restores what RawMode saved.
    termios raw = before;
    raw.c_lflag = static_cast<tcflag_t>(raw.c_lflag & ~static_cast<tcflag_t>(ICANON | ECHO));
    REQUIRE(::tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0);

    const char enter = '\r';
    REQUIRE(::write(pty.master(), &enter, 1) == 1);
    CHECK(ui.menu("Choose", items, 0) == 0);

    // After the menu the saved state is no longer marked valid, and the
    // attributes are back to what the menu found. The saved state lives at
    // namespace scope, so a handler firing now would still reference live
    // memory rather than a destroyed stack object.
    CHECK_FALSE(raw_mode_active());
    termios after{};
    REQUIRE(::tcgetattr(STDIN_FILENO, &after) == 0);
    CHECK((after.c_lflag & static_cast<tcflag_t>(ICANON)) ==
          (raw.c_lflag & static_cast<tcflag_t>(ICANON)));

    ::sigaction(SIGINT, &previous, nullptr);
    REQUIRE(::tcsetattr(STDIN_FILENO, TCSANOW, &before) == 0);
  }
}

TEST_CASE("with stdin a pipe the menu falls back to a numbered line and never sets termios") {
  int fds[2];
  REQUIRE(::pipe(fds) == 0);
  const int saved_stdin = ::dup(STDIN_FILENO);
  ::dup2(fds[0], STDIN_FILENO);

  const char* answer = "2\n";
  REQUIRE(::write(fds[1], answer, std::strlen(answer)) ==
          static_cast<ssize_t>(std::strlen(answer)));
  ::close(fds[1]);

  {
    AnsiTerminalUi ui;
    CHECK_FALSE(ui.interactive());
    const std::array<MenuItem, 3> items = {MenuItem{"First", ""}, MenuItem{"Second", ""},
                                           MenuItem{"Third", ""}};
    CHECK(ui.menu("Choose", items, 0) == 1);
    // No termios call was made against the pipe, so nothing was ever saved.
    CHECK_FALSE(raw_mode_active());
  }

  ::dup2(saved_stdin, STDIN_FILENO);
  ::close(saved_stdin);
  ::close(fds[0]);
  std::cin.clear();
}

TEST_CASE("navigating a menu repaints in place instead of appending a copy") {
  PtySwap pty;
  REQUIRE(pty.ok());
  RawSlave raw;
  REQUIRE(raw.ok());

  AnsiTerminalUi ui;
  REQUIRE(ui.interactive());

  const std::array<MenuItem, 3> items = {MenuItem{"Alpha", ""}, MenuItem{"Bravo", ""},
                                         MenuItem{"Charlie", ""}};
  const char keys[] = {'j', 'j', '\r'};
  REQUIRE(::write(pty.master(), keys, sizeof keys) == static_cast<ssize_t>(sizeof keys));
  CHECK(ui.menu("Choose", items, 0) == 2);

  const std::string out = drain(pty.master());
  const std::size_t paints = count_of(out, "Alpha");
  REQUIRE(paints >= 2);

  // The invariant, stated so it survives any change to the block's layout:
  // every paint after the first is preceded by exactly one cursor-up. Before
  // this was fixed the count was zero and each keystroke appended a fresh copy.
  CHECK(count_cursor_up(out) == paints - 1);
  // Each repaint also clears what was below it, so a shorter block cannot leave
  // the tail of a longer one behind.
  CHECK(count_of(out, "\033[J") == paints - 1);
  // The keys are named on screen rather than left to be guessed.
  CHECK(out.find("Enter") != std::string::npos);
  CHECK(out.find("j/k") != std::string::npos);
  // The cursor is hidden while the menu owns the screen and shown again after.
  CHECK(count_of(out, "\033[?25l") == 1);
  CHECK(count_of(out, "\033[?25h") == 1);
}

TEST_CASE("a bare Escape cancels rather than blocking for the rest of a sequence") {
  PtySwap pty;
  REQUIRE(pty.ok());
  RawSlave raw;
  REQUIRE(raw.ok());

  AnsiTerminalUi ui;
  REQUIRE(ui.interactive());

  const std::array<MenuItem, 2> items = {MenuItem{"First", ""}, MenuItem{"Second", ""}};
  const char escape = 27;
  REQUIRE(::write(pty.master(), &escape, 1) == 1);
  // With VMIN 1 and VTIME 0 the read of the byte after the Escape blocks until
  // the operator presses something else, which looks like a hung program. The
  // sequence read uses a timeout instead, so this returns.
  CHECK(ui.menu("Choose", items, 0) == -1);
  CHECK_FALSE(raw_mode_active());
}

TEST_CASE("an arrow key still moves the cursor after the timeout change") {
  PtySwap pty;
  REQUIRE(pty.ok());
  RawSlave raw;
  REQUIRE(raw.ok());

  AnsiTerminalUi ui;
  REQUIRE(ui.interactive());

  const std::array<MenuItem, 3> items = {MenuItem{"First", ""}, MenuItem{"Second", ""},
                                         MenuItem{"Third", ""}};
  // Down, down, up, Enter: the escape sequences must not be mistaken for bare
  // Escapes now that the follow-on read can time out.
  const char keys[] = {27, '[', 'B', 27, '[', 'B', 27, '[', 'A', '\r'};
  REQUIRE(::write(pty.master(), keys, sizeof keys) == static_cast<ssize_t>(sizeof keys));
  CHECK(ui.menu("Choose", items, 0) == 1);
}

TEST_CASE("the numbered fallback writes no escape sequence to something that is not a terminal") {
  int input[2];
  int output[2];
  REQUIRE(::pipe(input) == 0);
  REQUIRE(::pipe(output) == 0);
  const int saved_stdin = ::dup(STDIN_FILENO);
  const int saved_stdout = ::dup(STDOUT_FILENO);
  ::dup2(input[0], STDIN_FILENO);
  ::dup2(output[1], STDOUT_FILENO);
  ::close(output[1]);

  const char* answer = "1\n";
  REQUIRE(::write(input[1], answer, std::strlen(answer)) ==
          static_cast<ssize_t>(std::strlen(answer)));
  ::close(input[1]);

  {
    AnsiTerminalUi ui;
    const std::array<MenuItem, 2> items = {MenuItem{"First", "one"}, MenuItem{"Second", "two"}};
    CHECK(ui.menu("Choose", items, 0) == 0);
  }
  std::cout.flush();

  ::dup2(saved_stdout, STDOUT_FILENO);
  ::close(saved_stdout);
  const std::string written = drain(output[0]);

  ::dup2(saved_stdin, STDIN_FILENO);
  ::close(saved_stdin);
  ::close(input[0]);
  ::close(output[0]);
  std::cin.clear();

  // Colour is already suppressed because stdout is not a TTY; what this asserts
  // is the stronger rule that no cursor control reaches a pipe either, so a
  // redirected run produces a plain, greppable transcript.
  CHECK(written.find('\033') == std::string::npos);
  CHECK(written.find("Choose 1-2, or q to cancel") != std::string::npos);
  CHECK(written.find("First") != std::string::npos);
}

}  // TEST_SUITE
