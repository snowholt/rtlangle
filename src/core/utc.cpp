#include "core/utc.h"

#include <ctime>

namespace rtlangle {
namespace {

std::string format(std::chrono::system_clock::time_point tp, const char* pattern) {
  const std::time_t t = std::chrono::system_clock::to_time_t(tp);
  std::tm tm{};
  ::gmtime_r(&t, &tm);
  char buf[64];
  std::strftime(buf, sizeof(buf), pattern, &tm);
  return buf;
}

}  // namespace

std::string format_utc(std::chrono::system_clock::time_point tp) {
  return format(tp, "%Y-%m-%dT%H:%M:%SZ");
}

std::string utc_now() { return format_utc(std::chrono::system_clock::now()); }

std::string session_stamp(std::chrono::system_clock::time_point tp) {
  return format(tp, "%Y%m%d-%H%M%S");
}

}  // namespace rtlangle
