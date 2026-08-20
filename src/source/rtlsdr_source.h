#pragma once

#include "core/config.h"
#include "source/sample_source.h"

#include <memory>
#include <string>
#include <vector>

namespace rtlangle {

// Device enumeration, for the `devices` command and for the not-found message.
struct DeviceEntry {
  int         index = 0;
  std::string name;
  std::string manufacturer;
  std::string product;
  std::string serial;
};
using DeviceList = std::vector<DeviceEntry>;
DeviceList enumerate_devices();

// The device's supported gain settings, in tenths of a dB, ascending.
using GainTable = std::vector<int>;
GainTable gain_table(int device_index);

// Snaps a requested gain to the nearest supported entry; `max` (an absent
// request) selects the highest. Returns 0 for an empty table.
int snap_gain_tenth_db(const GainTable&, const std::optional<int>& requested);

// Maps a librtlsdr return code from opening a device to an operator-facing
// message. Tested directly with the error codes, so no device is required.
//
// The busy message never tells the operator to run a broad pattern kill: the
// documented instruction is to close the holding application normally, or to
// identify it with fuser or lsof and stop that specific process.
std::string describe_open_error(int code, int device_index, const DeviceList& enumerated);

#if RTLANGLE_WITH_RTLSDR
// Opens device `Config::device_index` and returns a running source, or nullptr
// with `error` set. AGC is disabled unconditionally and the tuner gain mode is
// set to manual; both return codes are checked (spec section 7.8, decision Q7).
std::unique_ptr<ITunableSampleSource> make_rtlsdr_source(const Config&, std::string& error);
#endif

}  // namespace rtlangle
