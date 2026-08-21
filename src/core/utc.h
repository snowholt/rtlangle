#pragma once

#include <chrono>
#include <functional>
#include <string>

namespace rtlangle {

// The one clock every timestamp in a session record comes from.
//
// It is injectable for exactly one reason: spec section 16 requires a committed
// example session that the tool GENERATES, and WP12 asserts byte equality
// between a regenerated copy and the committed one. Wall-clock timestamps in
// session.json, in every attempt, in every event, and in the directory name
// make that impossible unless the clock can be pinned. Nothing in production
// sets it; the default is std::chrono::system_clock::now.
std::chrono::system_clock::time_point now();
void set_clock(std::function<std::chrono::system_clock::time_point()>);
void reset_clock();

// ISO 8601 in UTC with a trailing Z, the form every timestamp in session.json
// uses (spec section 11.2).
std::string format_utc(std::chrono::system_clock::time_point);
std::string utc_now();

// The session directory stem: YYYYMMDD-HHMMSS. A label, when present, is
// appended by the caller after sanitisation.
std::string session_stamp(std::chrono::system_clock::time_point);

}  // namespace rtlangle
