#pragma once

#include "core/config.h"
#include "ui/terminal_ui.h"

namespace rtlangle::app {

// The main menu of spec section 12.2, shown when rtlangle is invoked with no
// arguments on a terminal. Returns the process exit code.
int main_menu(const Config&, ui::ITerminalUi&);

}  // namespace rtlangle::app
