#pragma once

namespace rtlangle {

// The version string persisted in every session record (spec section 11.2).
inline constexpr const char* kToolVersion = "rtlangle 0.1.0";

// The session document schema this build reads and writes (spec section 11.2).
inline constexpr int kSchemaVersion = 1;

}  // namespace rtlangle
