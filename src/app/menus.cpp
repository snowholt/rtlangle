#include "app/menus.h"

#include "app/run_command.h"
#include "app/scan_command.h"
#include "core/version.h"

#include <array>
#include <filesystem>

namespace rtlangle::app {

int main_menu(const Config& cfg, ui::ITerminalUi& terminal) {
  const std::array<ui::MenuItem, 6> items = {
      ui::MenuItem{"Start new experiment", "needs a frequency"},
      ui::MenuItem{"Resume session", "continue a paused or interrupted session"},
      ui::MenuItem{"Scan airband (find active channels)", ""},
      ui::MenuItem{"Device check / gain table", ""},
      ui::MenuItem{"Replay synthetic session", "no hardware needed"},
      ui::MenuItem{"Quit", ""},
  };

  for (;;) {
    const int choice = terminal.menu(std::string(kToolVersion), items, 0);
    switch (choice) {
      case 0: {
        Config run_config = cfg;
        if (!run_config.center_hz.has_value()) {
          const auto answer = terminal.prompt_line("Centre frequency, for example 118.35M", "");
          if (!answer.has_value() || answer->empty()) {
            terminal.warn("A frequency is required to start an experiment.");
            continue;
          }
          const auto hz = parse_frequency(*answer);
          if (!hz.has_value()) {
            terminal.error("\"" + *answer + "\" is not a frequency.");
            continue;
          }
          run_config.center_hz = static_cast<std::uint32_t>(*hz + 0.5);
        }
        const auto errors = validate(run_config);
        if (!errors.empty()) {
          for (const ValidationError& e : errors) terminal.error(e.message);
          continue;
        }
        return run_command(run_config, terminal);
      }
      case 1: {
        const auto answer = terminal.prompt_line("Session directory to resume", "");
        if (!answer.has_value() || answer->empty()) continue;
        Config resume_config = cfg;
        resume_config.resume_dir = *answer;
        return run_command(resume_config, terminal);
      }
      case 2:
        return scan_command(cfg, terminal);
      case 3:
        return device_check_command(cfg, terminal);
      case 4: {
        Config synthetic = cfg;
        synthetic.source_spec = "synthetic";
        synthetic.non_interactive = true;
        if (!synthetic.center_hz.has_value()) synthetic.center_hz = 118350000;
        const auto errors = validate(synthetic);
        if (!errors.empty()) {
          for (const ValidationError& e : errors) terminal.error(e.message);
          continue;
        }
        return run_command(synthetic, terminal);
      }
      case 5:
      default:
        return 0;
    }
  }
}

}  // namespace rtlangle::app
