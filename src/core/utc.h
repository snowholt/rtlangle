#pragma once

#include <chrono>
#include <string>

namespace rtlangle {

// ISO 8601 in UTC with a trailing Z, the form every timestamp in session.json
// uses (spec section 11.2).
std::string format_utc(std::chrono::system_clock::time_point);
std::string utc_now();

// The session directory stem: YYYYMMDD-HHMMSS. A label, when present, is
// appended by the caller after sanitisation.
std::string session_stamp(std::chrono::system_clock::time_point);

}  // namespace rtlangle
