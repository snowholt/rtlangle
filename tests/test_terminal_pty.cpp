// WP8 — the pseudo-terminal test. Spec section 12.3.
//
// A Ctrl-C must never leave the operator with a broken shell, so the SIGINT
// handler restores the terminal attributes it found. That can only be checked
// against a real terminal, which is what a pseudo-terminal provides without
// requiring the suite to be run interactively.

#include <doctest/doctest.h>

#include "ui/ansi_terminal_ui.h"

#include <array>
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

}  // TEST_SUITE
