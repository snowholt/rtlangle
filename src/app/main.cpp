#include "app/cli_parser.h"
#include "app/menus.h"
#include "app/run_command.h"
#include "app/scan_command.h"
#include "core/version.h"
#include "ui/ansi_terminal_ui.h"

#include <cstdio>
#include <filesystem>
#include <iostream>

// A thin dispatcher: parse, validate for the command, print the errors, run,
// return the code.
int main(int argc, char** argv) {
  using namespace rtlangle;

  const app::ParseResult parsed = app::parse_cli(argc, argv);

  if (parsed.version) {
    std::printf("%s\n", kToolVersion);
    return 0;
  }
  if (parsed.help) {
    std::printf("%s", app::usage_text().c_str());
    return 0;
  }

  ui::AnsiTerminalUi terminal(parsed.config.no_color, parsed.config.non_interactive);
  ui::install_signal_handlers();

  if (!parsed.errors.empty()) {
    for (const ValidationError& e : parsed.errors) terminal.error(e.message);
    std::fprintf(stderr, "\n%s", app::usage_text().c_str());
    return 2;   // usage or configuration error
  }

  if (parsed.command.empty()) {
    if (terminal.interactive()) return app::main_menu(parsed.config, terminal);
    std::fprintf(stderr, "%s", app::usage_text().c_str());
    return 2;
  }

  const auto command = parse_command(parsed.command);
  if (!command.has_value()) {
    terminal.error("unknown command \"" + parsed.command +
                   "\": expected run, scan, devices, or report.");
    std::fprintf(stderr, "\n%s", app::usage_text().c_str());
    return 2;
  }

  // A flag a command does not use is a usage error naming both. Silently
  // ignoring it would let an operator believe a setting took effect when it
  // could not have.
  auto scope_errors =
      validate_for_command(parsed.config, parsed.explicitly_set, *command);
  if (*command == Command::Report && parsed.positional.empty()) {
    scope_errors.push_back(
        {"session_dir", "the report command needs a session directory as its argument."});
  }
  if (!scope_errors.empty()) {
    for (const ValidationError& e : scope_errors) terminal.error(e.message);
    return 2;
  }

  const auto config_errors = validate(parsed.config);
  if (!config_errors.empty()) {
    for (const ValidationError& e : config_errors) terminal.error(e.message);
    return 2;
  }

  switch (*command) {
    case Command::Run:
      return app::run_command(parsed.config, terminal, {}, parsed.explicitly_set);
    case Command::Scan:
      return app::scan_command(parsed.config, terminal);
    case Command::Devices:
      return app::device_check_command(parsed.config, terminal);
    case Command::Report:
      return app::report_command(std::filesystem::path(parsed.positional), parsed.config,
                                 terminal);
  }
  return 2;
}
