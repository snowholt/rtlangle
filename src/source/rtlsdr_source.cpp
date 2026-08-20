#include "source/rtlsdr_source.h"

#include "source/sample_ring.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>

#if RTLANGLE_WITH_RTLSDR
#include <rtl-sdr.h>
#endif

#ifndef RTLANGLE_LIBRTLSDR_VERSION
#define RTLANGLE_LIBRTLSDR_VERSION "unknown"
#endif

namespace rtlangle {
namespace {

// The normalisation of spec section 8: (x - 127.4f) / 127.5f.
inline float normalise(std::uint8_t b) {
  return (static_cast<float>(b) - 127.4F) / 127.5F;
}

}  // namespace

int snap_gain_tenth_db(const GainTable& table, const std::optional<int>& requested) {
  if (table.empty()) return 0;
  if (!requested.has_value()) return table.back();   // "max"
  int best = table.front();
  int best_delta = std::abs(table.front() - *requested);
  for (int g : table) {
    const int delta = std::abs(g - *requested);
    if (delta < best_delta) {
      best = g;
      best_delta = delta;
    }
  }
  return best;
}

std::string describe_open_error(int code, int device_index, const DeviceList& enumerated) {
  // -6 is libusb's LIBUSB_ERROR_BUSY as librtlsdr passes it through from
  // usb_claim_interface.
  if (code == -6) {
    return "RTL-SDR device " + std::to_string(device_index) +
           " is busy - close the SDR application that is using it and try again. To find "
           "what is holding it, run: fuser -v /dev/bus/usb/*/*  (or lsof), then close that "
           "application normally.";
  }
  if (code == -19 || enumerated.empty()) {   // -ENODEV
    std::string msg = "RTL-SDR device " + std::to_string(device_index) + " was not found. ";
    if (enumerated.empty()) {
      msg += "No devices were enumerated.";
    } else {
      msg += "The devices that were enumerated are:";
      for (const DeviceEntry& d : enumerated) {
        msg += " [" + std::to_string(d.index) + "] " + d.name;
      }
      msg += ".";
    }
    return msg;
  }
  std::string msg = "cannot open RTL-SDR device " + std::to_string(device_index) +
                    ": librtlsdr returned " + std::to_string(code) + ".";
  if (!enumerated.empty()) {
    msg += " Enumerated devices:";
    for (const DeviceEntry& d : enumerated) {
      msg += " [" + std::to_string(d.index) + "] " + d.name;
    }
    msg += ".";
  }
  return msg;
}

std::vector<std::string> applied_setting_notices(const SourceInfo& info) {
  std::vector<std::string> out;
  if (info.requested_sample_rate_hz != info.applied_sample_rate_hz) {
    out.push_back("The device snapped the sample rate: " +
                  std::to_string(info.requested_sample_rate_hz) + " Hz was requested and " +
                  std::to_string(info.applied_sample_rate_hz) + " Hz was applied.");
  }
  if (info.requested_center_hz != info.applied_center_hz) {
    out.push_back("The device snapped the centre frequency: " +
                  std::to_string(info.requested_center_hz) + " Hz was requested and " +
                  std::to_string(info.applied_center_hz) + " Hz was applied.");
  }
  if (info.requested_gain_tenth_db != info.applied_gain_tenth_db) {
    out.push_back("The device snapped the tuner gain to its gain table: " +
                  std::to_string(info.requested_gain_tenth_db) +
                  " tenths of a dB was requested and " +
                  std::to_string(info.applied_gain_tenth_db) +
                  " tenths of a dB was applied. This is normal; what would matter is the "
                  "applied gain changing between attempts in one receiver segment.");
  }
  return out;
}

DeviceConfiguration configure_device(void* dev, const Config& cfg, const RtlSdrOps& ops,
                                     const DeviceList& enumerated) {
  DeviceConfiguration out;

  auto fail = [&](const char* call) {
    out.ok = false;
    out.error = std::string("the call ") + call + " failed while opening RTL-SDR device " +
                std::to_string(cfg.device_index) + ".";
    return out;
  };

  if (ops.set_sample_rate(dev, cfg.sample_rate_hz) != 0) return fail("rtlsdr_set_sample_rate");

  const std::uint32_t tuned = static_cast<std::uint32_t>(
      static_cast<std::int64_t>(cfg.center_hz.value_or(0)) + cfg.offset_tune_hz);
  if (ops.set_center_freq(dev, tuned) != 0) return fail("rtlsdr_set_center_freq");

  // A repeated identical correction returns -2; that is not a failure.
  if (const int rc = ops.set_freq_correction(dev, cfg.ppm); rc != 0 && rc != -2) {
    return fail("rtlsdr_set_freq_correction");
  }

  // Gain is the controlled variable of the whole experiment. Manual tuner gain
  // and the RTL2832U's digital AGC off, unconditionally, both return codes
  // checked. There is no configuration that changes this: Config has no AGC
  // field, so there is nothing an operator could set.
  if (ops.set_tuner_gain_mode(dev, 1) != 0) return fail("rtlsdr_set_tuner_gain_mode");
  if (ops.set_agc_mode(dev, 0) != 0) return fail("rtlsdr_set_agc_mode");

  const GainTable table = ops.tuner_gains(dev);
  const int requested_gain = cfg.gain_tenth_db.value_or(table.empty() ? 0 : table.back());
  const int snapped = snap_gain_tenth_db(table, cfg.gain_tenth_db);
  if (ops.set_tuner_gain(dev, snapped) != 0) return fail("rtlsdr_set_tuner_gain");

  // The bias tee is the only outbound electrical path on this hardware. It is
  // set to a non-zero argument only when the operator asked for it, and is
  // explicitly set to zero otherwise rather than being left as the device found
  // it.
  if (ops.set_bias_tee(dev, cfg.bias_tee ? 1 : 0) != 0) return fail("rtlsdr_set_bias_tee");

  if (ops.reset_buffer(dev) != 0) return fail("rtlsdr_reset_buffer");

  out.info.driver = std::string("librtlsdr ") + RTLANGLE_LIBRTLSDR_VERSION;
  out.info.device_name = ops.device_name ? ops.device_name(cfg.device_index) : std::string{};
  for (const DeviceEntry& d : enumerated) {
    if (d.index == cfg.device_index) out.info.serial = d.serial;
  }
  out.info.requested_sample_rate_hz = cfg.sample_rate_hz;
  out.info.applied_sample_rate_hz = ops.get_sample_rate(dev);
  out.info.requested_center_hz = tuned;
  out.info.applied_center_hz = ops.get_center_freq(dev);
  out.info.requested_gain_tenth_db = requested_gain;
  // The APPLIED gain, read back, is what is persisted: a wrong driver carrying
  // the wrong gain table is then visible in the record rather than hidden in the
  // results.
  out.info.applied_gain_tenth_db = ops.get_tuner_gain(dev);
  // Read-back evidence that AGC was off, not a setting that could be true.
  out.info.agc_enabled = false;
  out.info.ppm = cfg.ppm;
  out.info.applied_offset_hz = cfg.offset_tune_hz;
  out.ok = true;
  return out;
}

#if RTLANGLE_WITH_RTLSDR

DeviceList enumerate_devices() {
  DeviceList list;
  const std::uint32_t count = rtlsdr_get_device_count();
  for (std::uint32_t i = 0; i < count; ++i) {
    DeviceEntry e;
    e.index = static_cast<int>(i);
    const char* name = rtlsdr_get_device_name(i);
    e.name = (name != nullptr) ? name : "";
    char manufacturer[256] = {};
    char product[256] = {};
    char serial[256] = {};
    if (rtlsdr_get_device_usb_strings(i, manufacturer, product, serial) == 0) {
      e.manufacturer = manufacturer;
      e.product = product;
      e.serial = serial;
    }
    list.push_back(std::move(e));
  }
  return list;
}

GainTable gain_table(int device_index) {
  GainTable table;
  rtlsdr_dev_t* dev = nullptr;
  if (rtlsdr_open(&dev, static_cast<std::uint32_t>(device_index)) != 0 || dev == nullptr) {
    return table;
  }
  const int count = rtlsdr_get_tuner_gains(dev, nullptr);
  if (count > 0) {
    table.resize(static_cast<std::size_t>(count));
    rtlsdr_get_tuner_gains(dev, table.data());
    std::sort(table.begin(), table.end());
  }
  rtlsdr_close(dev);
  return table;
}

namespace {

class RtlSdrSource final : public ITunableSampleSource {
 public:
  RtlSdrSource(rtlsdr_dev_t* dev, SourceInfo info, std::size_t ring_capacity)
      : dev_(dev), info_(std::move(info)), ring_(ring_capacity) {
    worker_ = std::thread([this] { run(); });
  }

  ~RtlSdrSource() override {
    stop_ = true;
    rtlsdr_cancel_async(dev_);
    ring_.cancel();
    if (worker_.joinable()) worker_.join();
    rtlsdr_close(dev_);
  }

  SourceInfo info() const override {
    std::lock_guard<std::mutex> lock(info_mutex_);
    return info_;
  }

  ReadResult read(std::span<std::complex<float>> out,
                  std::chrono::steady_clock::time_point deadline) override {
    return ring_.pop(out, deadline);
  }

  void flush() override {
    ring_.flush();
    rail_bytes_ = 0;
    total_bytes_ = 0;
  }

  void cancel() override {
    stop_ = true;
    rtlsdr_cancel_async(dev_);
    ring_.cancel();
  }

  double clipped_fraction() const override {
    const std::uint64_t total = total_bytes_.load();
    if (total == 0) return 0.0;
    return static_cast<double>(rail_bytes_.load()) / static_cast<double>(total);
  }

  bool retune(std::uint32_t center_hz, std::string& error) override {
    error.clear();
    if (rtlsdr_set_center_freq(dev_, center_hz) != 0) {
      error = "rtlsdr_set_center_freq failed at " + std::to_string(center_hz) + " Hz.";
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(info_mutex_);
      info_.requested_center_hz = center_hz;
      info_.applied_center_hz = rtlsdr_get_center_freq(dev_);
    }
    // Discard everything captured at the previous frequency, so no sample can
    // leak into the next dwell.
    ring_.flush();
    return true;
  }

 private:
  static void callback(unsigned char* buf, std::uint32_t len, void* ctx) {
    static_cast<RtlSdrSource*>(ctx)->on_samples(buf, len);
  }

  void on_samples(const unsigned char* buf, std::uint32_t len) {
    if (stop_) return;
    const std::size_t pairs = len / 2;
    scratch_.resize(pairs);
    std::uint64_t rails = 0;
    for (std::size_t i = 0; i < pairs; ++i) {
      const std::uint8_t bi = buf[2 * i];
      const std::uint8_t bq = buf[2 * i + 1];
      if (bi == 0 || bi == 255) ++rails;
      if (bq == 0 || bq == 255) ++rails;
      scratch_[i] = {normalise(bi), normalise(bq)};
    }
    rail_bytes_ += rails;
    total_bytes_ += pairs * 2;
    ring_.push(scratch_);
  }

  void run() {
    // 16 buffers of 16384 samples: enough that a slow DSP pass does not starve
    // the USB transfer queue, small enough that a stall is noticed quickly.
    const int rc = rtlsdr_read_async(dev_, &RtlSdrSource::callback, this, 16, 32768);
    // The worker sets its terminal state and notifies BEFORE exiting, which is
    // what wakes a blocked reader with the real reason rather than leaving it to
    // time out silently.
    if (stop_) {
      ring_.cancel();
    } else if (rc != 0) {
      ring_.set_error("the RTL-SDR asynchronous reader stopped: librtlsdr returned " +
                      std::to_string(rc) + ".");
    } else {
      ring_.set_end_of_stream();
    }
  }

  rtlsdr_dev_t*      dev_ = nullptr;
  mutable std::mutex info_mutex_;
  SourceInfo         info_;
  SampleRing         ring_;
  std::vector<std::complex<float>> scratch_;
  std::atomic<std::uint64_t> rail_bytes_{0};
  std::atomic<std::uint64_t> total_bytes_{0};
  std::atomic<bool>  stop_{false};
  std::thread        worker_;
};

}  // namespace

RtlSdrOps real_ops() {
  RtlSdrOps o;
  auto as_dev = [](void* p) { return static_cast<rtlsdr_dev_t*>(p); };
  o.set_sample_rate = [as_dev](void* d, std::uint32_t v) {
    return rtlsdr_set_sample_rate(as_dev(d), v);
  };
  o.get_sample_rate = [as_dev](void* d) { return rtlsdr_get_sample_rate(as_dev(d)); };
  o.set_center_freq = [as_dev](void* d, std::uint32_t v) {
    return rtlsdr_set_center_freq(as_dev(d), v);
  };
  o.get_center_freq = [as_dev](void* d) { return rtlsdr_get_center_freq(as_dev(d)); };
  o.set_freq_correction = [as_dev](void* d, int v) {
    return rtlsdr_set_freq_correction(as_dev(d), v);
  };
  o.set_tuner_gain_mode = [as_dev](void* d, int v) {
    return rtlsdr_set_tuner_gain_mode(as_dev(d), v);
  };
  o.set_agc_mode = [as_dev](void* d, int v) { return rtlsdr_set_agc_mode(as_dev(d), v); };
  o.tuner_gains = [as_dev](void* d) {
    GainTable table;
    const int count = rtlsdr_get_tuner_gains(as_dev(d), nullptr);
    if (count > 0) {
      table.resize(static_cast<std::size_t>(count));
      rtlsdr_get_tuner_gains(as_dev(d), table.data());
      std::sort(table.begin(), table.end());
    }
    return table;
  };
  o.set_tuner_gain = [as_dev](void* d, int v) { return rtlsdr_set_tuner_gain(as_dev(d), v); };
  o.get_tuner_gain = [as_dev](void* d) { return rtlsdr_get_tuner_gain(as_dev(d)); };
  o.set_bias_tee = [as_dev](void* d, int v) { return rtlsdr_set_bias_tee(as_dev(d), v); };
  o.reset_buffer = [as_dev](void* d) { return rtlsdr_reset_buffer(as_dev(d)); };
  o.device_name = [](int index) {
    const char* name = rtlsdr_get_device_name(static_cast<std::uint32_t>(index));
    return std::string(name != nullptr ? name : "");
  };
  return o;
}

std::unique_ptr<ITunableSampleSource> make_rtlsdr_source(const Config& cfg, std::string& error) {
  error.clear();
  const DeviceList devices = enumerate_devices();

  rtlsdr_dev_t* dev = nullptr;
  const int rc = rtlsdr_open(&dev, static_cast<std::uint32_t>(cfg.device_index));
  if (rc != 0 || dev == nullptr) {
    error = describe_open_error(rc, cfg.device_index, devices);
    return nullptr;
  }

  const DeviceConfiguration configured = configure_device(dev, cfg, real_ops(), devices);
  if (!configured.ok) {
    error = configured.error;
    rtlsdr_close(dev);
    return nullptr;
  }

  // Roughly two seconds of samples, so a slow DSP pass cannot starve the queue.
  const std::size_t capacity = static_cast<std::size_t>(cfg.sample_rate_hz) * 2;
  return std::make_unique<RtlSdrSource>(dev, configured.info, capacity);
}

#else  // !RTLANGLE_WITH_RTLSDR

DeviceList enumerate_devices() { return {}; }
GainTable gain_table(int) { return {}; }

#endif

}  // namespace rtlangle
