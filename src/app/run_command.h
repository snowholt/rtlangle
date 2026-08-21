#pragma once

#include "angle/angle_provider.h"
#include "core/config.h"
#include "persist/atomic_write.h"
#include "persist/session_store.h"
#include "source/sample_source.h"
#include "ui/terminal_ui.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>

namespace rtlangle::app {

// Seams for the orchestration tests. Production leaves them empty and the
// defaults are used; a test substitutes a recording or fault-injecting
// decorator without the command layer knowing.
struct RunHooks {
  std::function<std::unique_ptr<ISampleSource>(const Config&, std::string&)> make_source;
  std::function<std::unique_ptr<ISessionStore>(SessionDir, SessionRecord)>   make_store;
  std::function<std::unique_ptr<IAngleProvider>(const Config&, ui::ITerminalUi&)> make_provider;
  // Called with a short name at each step of the orchestration sequence, so the
  // order can be asserted rather than read.
  std::function<void(std::string_view)> observe;
};

// The orchestration of spec section 12.1, in this order:
//
//   open source -> create session | load_session(resume) -> begin_receiver_segment
//   -> build provider -> build controller -> controller.run()
//   -> aggregate -> evaluate_warnings -> choose_report_metric -> render
//   -> finalize | pause | abort -> exit code
//
// The order of the first steps is not cosmetic. begin_receiver_segment is a
// method ON THE STORE, so the store must exist before it can be called, and it
// needs source.info(), so the source must be open before that.
//
// THE APP LAYER, NOT THE CONTROLLER, WRITES EVERY TERMINAL OR PAUSED STATE:
//
//   Completed      -> finalize(summary)      -> completed  -> exit 0
//   QuitRequested  -> pause(partial)         -> paused     -> exit 0
//   Failed         -> abort(partial, reason) -> aborted    -> exit 1
//
// There is no decision step anywhere in the sequence.
// `explicitly_set` names the keys the operator actually set, from a flag or a
// configuration file. The resume classification of spec section 11.5 needs it:
// a stored value differing from a DEFAULT is not an override, while the same
// value differing from something the operator typed is exactly the conflict
// that would otherwise mix two experiments in one record.
int run_command(const Config&, ui::ITerminalUi&, const RunHooks& hooks = {},
                const std::set<std::string>& explicitly_set = {});

// Re-renders a stored session from session.json alone. It reaches the completed
// and aborted sessions that resumption refuses, and changes nothing they
// recorded.
int report_command(const std::filesystem::path& session_dir, const Config&,
                   ui::ITerminalUi&);

// Enumerates devices and their gain tables.
int device_check_command(const Config&, ui::ITerminalUi&);

}  // namespace rtlangle::app
