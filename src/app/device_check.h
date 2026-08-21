#pragma once

#include "core/config.h"
#include "source/rtlsdr_source.h"

#include <string>
#include <vector>

namespace rtlangle::app {

// What the `devices` command reports: the enumerated devices, each one's gain
// table, and the tuner the driver names. The report is built as text so that
// nothing in the source layer has to know about a terminal; the command wiring
// renders it.
struct DeviceReport {
  bool                     any_device = false;
  std::vector<std::string> lines;
};

DeviceReport describe_devices(const Config&);

// The gain table rendered as decibels, for the report.
std::string format_gain_table(const GainTable&);

}  // namespace rtlangle::app
