// Hardware evidence, spec section 14.3.
//
// A separate executable, registered with CTest under the label `hardware` and
// with SKIP_RETURN_CODE 77, because CTest labels apply to registered tests and
// not to cases inside a binary. That separation is what makes
// `ctest -LE hardware` a genuinely hardware-free run.
//
// Exactly one machine-readable line is printed:
//
//   RTLANGLE_HARDWARE=passed    exit 0   a device was opened and the assertions held
//   RTLANGLE_HARDWARE=skipped   exit 77  no device, or the device was busy
//   RTLANGLE_HARDWARE=failed    exit 1   a device was opened and an assertion failed
//
// A skipped result is hardware validation PENDING. It is never reported as
// passed, and any completion report must parse this line rather than infer
// success from a green aggregate CTest run.
//
// The test never enables the bias tee and never transmits. The hardware has no
// transmit path at all; the bias tee is the single outbound electrical path,
// and Config::bias_tee is left false here.

#include "core/config.h"
#include "core/db.h"
#include "source/rtlsdr_source.h"
#include "source/sample_source.h"

#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int report(const char* outcome, int code, const std::string& detail) {
  if (!detail.empty()) std::fprintf(stderr, "%s\n", detail.c_str());
  std::printf("RTLANGLE_HARDWARE=%s\n", outcome);
  return code;
}

}  // namespace

int main() {
#if !RTLANGLE_WITH_RTLSDR
  return report("skipped", 77,
                "this build was configured with -DRTLANGLE_WITH_RTLSDR=OFF, so no device "
                "can be opened.");
#else
  using namespace rtlangle;

  const DeviceList devices = enumerate_devices();
  if (devices.empty()) {
    return report("skipped", 77, "no RTL-SDR device was enumerated.");
  }

  Config cfg;
  cfg.device_index = 0;
  cfg.center_hz = 118350000;
  cfg.sample_rate_hz = 1024000;
  cfg.gain_tenth_db.reset();   // "max": the highest entry in the device's table
  cfg.bias_tee = false;        // never enabled here, and never transmits

  const GainTable table = gain_table(cfg.device_index);
  if (table.empty()) {
    // The gain table can only be read by opening the device, so an empty table
    // means the device is busy. Busy is SKIPPED, not FAILED: validation remains
    // pending rather than being recorded as a failure of the tool.
    return report("skipped", 77,
                  "RTL-SDR device 0 could not be opened to read its gain table; it is most "
                  "likely held by another application. Close that application normally, or "
                  "identify it with `fuser -v /dev/bus/usb/*/*` and stop that process.");
  }

  std::string error;
  auto source = make_rtlsdr_source(cfg, error);
  if (source == nullptr) {
    const bool busy = error.find("is busy") != std::string::npos;
    const bool absent = error.find("was not found") != std::string::npos;
    if (busy || absent) return report("skipped", 77, error);
    return report("failed", 1, error);
  }

  const SourceInfo info = source->info();

  // AGC is off, unconditionally, and the record says so.
  if (info.agc_enabled) {
    return report("failed", 1,
                  "the device reported AGC enabled; it is disabled unconditionally and there "
                  "is no configuration that turns it on.");
  }

  // The applied gain is the snapped request, read back from the device.
  const int expected_gain = snap_gain_tenth_db(table, cfg.gain_tenth_db);
  if (info.applied_gain_tenth_db != expected_gain) {
    return report("failed", 1,
                  "the applied tuner gain is " + std::to_string(info.applied_gain_tenth_db) +
                      " tenths of a dB, but the snapped request was " +
                      std::to_string(expected_gain) + ".");
  }

  // One second of samples.
  const std::size_t wanted = cfg.sample_rate_hz;
  std::vector<std::complex<float>> buffer(wanted);
  source->flush();
  std::size_t got = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (got < wanted) {
    const ReadResult r = source->read(
        std::span<std::complex<float>>(buffer).subspan(got, wanted - got), deadline);
    got += r.samples;
    if (r.status == ReadStatus::Ok) continue;
    if (r.status == ReadStatus::Timeout) {
      return report("failed", 1, "the device produced only " + std::to_string(got) + " of " +
                                     std::to_string(wanted) + " samples within 10 s.");
    }
    return report("failed", 1, "the read ended with status " +
                                   std::string(to_string(r.status)) + ": " + r.error);
  }

  // A plausible, non-constant sample distribution: a device delivering a
  // constant is not receiving.
  double sum = 0.0;
  double sum_sq = 0.0;
  double peak = 0.0;
  for (const std::complex<float>& x : buffer) {
    const double p = std::norm(x);
    sum += p;
    sum_sq += p * p;
    peak = std::max(peak, p);
  }
  const double mean = sum / static_cast<double>(buffer.size());
  const double variance = sum_sq / static_cast<double>(buffer.size()) - mean * mean;
  if (!(mean > 0.0)) {
    return report("failed", 1, "every sample was zero; the device is not receiving.");
  }
  if (!(variance > 0.0)) {
    return report("failed", 1,
                  "the sample power has zero variance; the device is delivering a constant.");
  }

  const double clipped = source->clipped_fraction();
  if (clipped > cfg.max_clipped_fraction) {
    return report("failed", 1,
                  "the clipped sample fraction was " + std::to_string(clipped) +
                      ", above the limit of " + std::to_string(cfg.max_clipped_fraction) +
                      ". Re-run at a lower fixed gain.");
  }

  source->cancel();
  source.reset();   // closes the device

  return report("passed", 0, {});
#endif
}
