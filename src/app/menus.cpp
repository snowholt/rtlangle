#include "app/menus.h"

#include "app/run_command.h"
#include "app/scan_command.h"
#include "app/setup_menu.h"
#include "core/version.h"

#include <array>

namespace rtlangle::app {

int main_menu(const Config& cfg, const std::set<std::string>& explicitly_set,
              ui::ITerminalUi& terminal) {
  const std::array<ui::MenuItem, 6> items = {
      ui::MenuItem{"Start new experiment", "set frequency, angles, and timing"},
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
        // Whatever the operator passed on the command line is where the screen
        // starts, so the two ways of setting an option compose rather than
        // compete.
        std::set<std::string> keys = explicitly_set;
        const auto chosen = setup_experiment(cfg, keys, terminal);
        if (!chosen.has_value()) continue;
        return run_command(*chosen, terminal, {}, keys);
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
