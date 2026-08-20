#pragma once

#include "core/config.h"
#include "source/sample_source.h"

#include <functional>
#include <memory>
#include <optional>
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

// Informational messages about a device that snapped a requested setting to
// what its hardware supports. Snapping is legitimate and expected; what must
// not happen is the applied values CHANGING part-way through a session, which
// is what warning W7 detects against the receiver-segment baseline rather than
// against the request (spec section 7.8).
std::vector<std::string> applied_setting_notices(const SourceInfo&);

// The librtlsdr calls the device-configuration sequence makes, behind one
// injectable seam.
//
// AGC being off is a non-negotiable product boundary, and a boundary needs
// evidence: this table lets an offline shim record every call and its
// arguments, so the assertions that set_tuner_gain_mode(dev, 1) and
// set_agc_mode(dev, 0) are always made, are never made with the opposite
// argument, and have their return codes checked, are tests rather than a
// reading of the source. `dev` is opaque here so the seam compiles with or
// without librtlsdr.
struct RtlSdrOps {
  std::function<int(void*, std::uint32_t)>          set_sample_rate;
  std::function<std::uint32_t(void*)>               get_sample_rate;
  std::function<int(void*, std::uint32_t)>          set_center_freq;
  std::function<std::uint32_t(void*)>               get_center_freq;
  std::function<int(void*, int)>                    set_freq_correction;
  std::function<int(void*, int)>                    set_tuner_gain_mode;
  std::function<int(void*, int)>                    set_agc_mode;
  std::function<GainTable(void*)>                   tuner_gains;
  std::function<int(void*, int)>                    set_tuner_gain;
  std::function<int(void*)>                         get_tuner_gain;
  std::function<int(void*, int)>                    set_bias_tee;
  std::function<int(void*)>                         reset_buffer;
  std::function<std::string(int)>                   device_name;
};

struct DeviceConfiguration {
  bool        ok = false;
  SourceInfo  info;
  std::string error;   // non-empty on failure, naming the failing call
};

// Applies Config to an opened device. Every return code is checked and a
// failure names the failing call. AGC is disabled unconditionally: there is no
// configuration that changes this, because Config has no AGC field.
DeviceConfiguration configure_device(void* dev, const Config&, const RtlSdrOps&,
                                     const DeviceList& enumerated);

#if RTLANGLE_WITH_RTLSDR
// Opens device `Config::device_index` and returns a running source, or nullptr
// with `error` set.
std::unique_ptr<ITunableSampleSource> make_rtlsdr_source(const Config&, std::string& error);
#endif

}  // namespace rtlangle
