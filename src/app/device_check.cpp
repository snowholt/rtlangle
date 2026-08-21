#include "app/device_check.h"

#include "core/version.h"

#include <sstream>

namespace rtlangle::app {

std::string format_gain_table(const GainTable& table) {
  if (table.empty()) return "(the driver reported no gain table)";
  std::ostringstream os;
  os.precision(1);
  os << std::fixed;
  for (std::size_t i = 0; i < table.size(); ++i) {
    if (i > 0) os << ", ";
    os << static_cast<double>(table[i]) / 10.0;
  }
  os << " dB";
  return os.str();
}

DeviceReport describe_devices(const Config& cfg) {
  DeviceReport report;
  report.lines.push_back(std::string(kToolVersion));

#if RTLANGLE_WITH_RTLSDR
  const DeviceList devices = enumerate_devices();
  if (devices.empty()) {
    report.lines.push_back(
        "No RTL-SDR device was enumerated. If one is attached, another application may "
        "hold it: close that application normally, or identify it with "
        "`fuser -v /dev/bus/usb/*/*` (or lsof) and stop that specific process.");
    return report;
  }

  report.any_device = true;
  for (const DeviceEntry& d : devices) {
    report.lines.push_back("[" + std::to_string(d.index) + "] " + d.name);
    if (!d.manufacturer.empty() || !d.product.empty() || !d.serial.empty()) {
      report.lines.push_back("      manufacturer: " + d.manufacturer +
                             "   product: " + d.product + "   serial: " + d.serial);
    }
    const GainTable table = gain_table(d.index);
    report.lines.push_back("      gain table:   " + format_gain_table(table));
    if (!table.empty()) {
      report.lines.push_back(
          "      --gain max selects " +
          std::to_string(static_cast<double>(table.back()) / 10.0) + " dB");
    } else if (d.index == cfg.device_index) {
      report.lines.push_back(
          "      the gain table could not be read, which usually means the device is held "
          "by another application.");
    }
  }
#else
  (void)cfg;
  report.lines.push_back(
      "This build has no RTL-SDR support: it was configured with "
      "-DRTLANGLE_WITH_RTLSDR=OFF. Reconfigure with -DRTLANGLE_WITH_RTLSDR=ON to "
      "enumerate devices.");
#endif

  return report;
}

}  // namespace rtlangle::app
