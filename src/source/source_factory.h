#pragma once

#include "core/config.h"
#include "source/sample_source.h"

#include <memory>
#include <string>
#include <vector>

namespace rtlangle {

// Builds the source named by Config::source_spec: `rtlsdr`, `synthetic`, or
// `file:<path.cu8>`. Returns nullptr and sets `error` on failure.
//
// When the build is configured with -DRTLANGLE_WITH_RTLSDR=OFF the device
// translation unit is not compiled at all, and `rtlsdr` is refused with a
// message naming the CMake option rather than failing at link time.
std::unique_ptr<ISampleSource> make_source(const Config&, std::string& error);

// Any messages the constructed source wants reported once, such as a replay
// file that did not end on a whole I/Q pair.
std::vector<std::string> take_source_notices(const ISampleSource&);

}  // namespace rtlangle
