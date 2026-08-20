// WP7 — everything about the device path that can be tested with no device:
// the error mapping, gain snapping, requested-versus-applied reporting, and the
// configuration sequence that keeps AGC off. Spec sections 7.8 and 14.3,
// decision Q7.

#include <doctest/doctest.h>

#include "app/device_check.h"
#include "core/config.h"
#include "source/rtlsdr_source.h"
#include "source/source_factory.h"

#include <string>
#include <vector>

using namespace rtlangle;

namespace {

// The RTL-SDR Blog V4 gain table, in tenths of a dB.
GainTable blog_v4_gains() {
  return {0,   9,   14,  27,  37,  77,  87,  125, 144, 157, 166, 197, 207, 229, 254,
          280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496};
}

// Records every call and its arguments, so the AGC and gain-mode assertions are
// tests rather than a reading of the source.
struct RecordingShim {
  struct Call {
    std::string name;
    long        argument = 0;
  };
  std::vector<Call> calls;
  GainTable         gains = blog_v4_gains();
  std::uint32_t     applied_rate = 1024000;
  std::uint32_t     applied_center = 118600000;
  int               applied_gain = 496;
  std::string       fail_call;   // when set, that call returns a failure

  int result_for(const std::string& name) { return fail_call == name ? -1 : 0; }

  int count(const std::string& name) const {
    int n = 0;
    for (const Call& c : calls) {
      if (c.name == name) ++n;
    }
    return n;
  }
  bool called_with(const std::string& name, long argument) const {
    for (const Call& c : calls) {
      if (c.name == name && c.argument == argument) return true;
    }
    return false;
  }

  RtlSdrOps ops() {
    RtlSdrOps o;
    o.set_sample_rate = [this](void*, std::uint32_t v) {
      calls.push_back({"rtlsdr_set_sample_rate", static_cast<long>(v)});
      return result_for("rtlsdr_set_sample_rate");
    };
    o.get_sample_rate = [this](void*) { return applied_rate; };
    o.set_center_freq = [this](void*, std::uint32_t v) {
      calls.push_back({"rtlsdr_set_center_freq", static_cast<long>(v)});
      return result_for("rtlsdr_set_center_freq");
    };
    o.get_center_freq = [this](void*) { return applied_center; };
    o.set_freq_correction = [this](void*, int v) {
      calls.push_back({"rtlsdr_set_freq_correction", v});
      return fail_call == "rtlsdr_set_freq_correction" ? -1 : (v == 0 ? -2 : 0);
    };
    o.set_tuner_gain_mode = [this](void*, int v) {
      calls.push_back({"rtlsdr_set_tuner_gain_mode", v});
      return result_for("rtlsdr_set_tuner_gain_mode");
    };
    o.set_agc_mode = [this](void*, int v) {
      calls.push_back({"rtlsdr_set_agc_mode", v});
      return result_for("rtlsdr_set_agc_mode");
    };
    o.tuner_gains = [this](void*) { return gains; };
    o.set_tuner_gain = [this](void*, int v) {
      calls.push_back({"rtlsdr_set_tuner_gain", v});
      return result_for("rtlsdr_set_tuner_gain");
    };
    o.get_tuner_gain = [this](void*) { return applied_gain; };
    o.set_bias_tee = [this](void*, int v) {
      calls.push_back({"rtlsdr_set_bias_tee", v});
      return result_for("rtlsdr_set_bias_tee");
    };
    o.reset_buffer = [this](void*) {
      calls.push_back({"rtlsdr_reset_buffer", 0});
      return result_for("rtlsdr_reset_buffer");
    };
    o.device_name = [](int) { return std::string("Generic RTL2832U OEM"); };
    return o;
  }
};

Config device_config() {
  Config c;
  c.center_hz = 118350000;
  c.device_index = 0;
  return c;
}

DeviceList two_devices() {
  DeviceList d;
  d.push_back({0, "Generic RTL2832U OEM", "Realtek", "RTL2838UHIDIR", "00000001"});
  d.push_back({1, "RTL-SDR Blog V4", "RTLSDRBlog", "Blog V4", "00000002"});
  return d;
}

}  // namespace

TEST_SUITE("rtlsdr_offline") {

TEST_CASE("a busy device is named as busy, without a broad pattern kill") {
  const std::string msg = describe_open_error(-6, 2, two_devices());
  CHECK(msg.find("device 2 is busy") != std::string::npos);
  CHECK(msg.find("close the SDR application") != std::string::npos);
  CHECK(msg.find("fuser -v /dev/bus/usb") != std::string::npos);
  // The documented instruction is to stop the specific holder, never a broad
  // pattern kill that can take unrelated processes with it.
  CHECK(msg.find("pkill") == std::string::npos);
}

TEST_CASE("a missing device names the devices that were enumerated") {
  const std::string with_others = describe_open_error(-19, 3, two_devices());
  CHECK(with_others.find("device 3 was not found") != std::string::npos);
  CHECK(with_others.find("Generic RTL2832U OEM") != std::string::npos);
  CHECK(with_others.find("RTL-SDR Blog V4") != std::string::npos);

  const std::string none = describe_open_error(-19, 0, {});
  CHECK(none.find("No devices were enumerated") != std::string::npos);
}

TEST_CASE("gain snapping picks the highest for max and the nearest otherwise") {
  const GainTable table = blog_v4_gains();
  CHECK(snap_gain_tenth_db(table, std::nullopt) == 496);
  CHECK(snap_gain_tenth_db(table, 496) == 496);
  CHECK(snap_gain_tenth_db(table, 0) == 0);
  CHECK(snap_gain_tenth_db(table, 300) == 297);
  CHECK(snap_gain_tenth_db(table, 400) == 402);
  CHECK(snap_gain_tenth_db(table, 1000) == 496);
  CHECK(snap_gain_tenth_db(table, -100) == 0);
  CHECK(snap_gain_tenth_db({}, 300) == 0);
}

TEST_CASE("opening a device always sets manual gain and disables AGC") {
  RecordingShim shim;
  Config cfg = device_config();
  const DeviceConfiguration result = configure_device(nullptr, cfg, shim.ops(), two_devices());

  REQUIRE_MESSAGE(result.ok, result.error);
  // Both calls are made, with the only arguments this tool ever passes.
  CHECK(shim.count("rtlsdr_set_tuner_gain_mode") == 1);
  CHECK(shim.called_with("rtlsdr_set_tuner_gain_mode", 1));
  CHECK(shim.count("rtlsdr_set_agc_mode") == 1);
  CHECK(shim.called_with("rtlsdr_set_agc_mode", 0));
  // And never with the opposite argument. There is no configuration that could
  // produce one, because Config has no AGC field.
  CHECK_FALSE(shim.called_with("rtlsdr_set_tuner_gain_mode", 0));
  CHECK_FALSE(shim.called_with("rtlsdr_set_agc_mode", 1));
  // The record carries read-back evidence that AGC was off.
  CHECK_FALSE(result.info.agc_enabled);
}

TEST_CASE("every configuration call has its return code checked") {
  for (const char* call :
       {"rtlsdr_set_sample_rate", "rtlsdr_set_center_freq", "rtlsdr_set_freq_correction",
        "rtlsdr_set_tuner_gain_mode", "rtlsdr_set_agc_mode", "rtlsdr_set_tuner_gain",
        "rtlsdr_set_bias_tee", "rtlsdr_reset_buffer"}) {
    RecordingShim shim;
    shim.fail_call = call;
    Config cfg = device_config();
    cfg.ppm = 7;   // so the frequency correction is exercised as a real call
    const DeviceConfiguration result = configure_device(nullptr, cfg, shim.ops(), two_devices());
    CHECK_MESSAGE(!result.ok, call);
    CHECK_MESSAGE(result.error.find(call) != std::string::npos, call);
  }
}

TEST_CASE("a repeated identical frequency correction is not a failure") {
  RecordingShim shim;
  Config cfg = device_config();
  cfg.ppm = 0;   // the shim returns -2 for this, as librtlsdr does
  const DeviceConfiguration result = configure_device(nullptr, cfg, shim.ops(), two_devices());
  CHECK(result.ok);
}

TEST_CASE("the bias tee is set to zero unless it was explicitly requested") {
  RecordingShim off;
  Config cfg = device_config();
  REQUIRE(configure_device(nullptr, cfg, off.ops(), two_devices()).ok);
  CHECK(off.called_with("rtlsdr_set_bias_tee", 0));
  CHECK_FALSE(off.called_with("rtlsdr_set_bias_tee", 1));

  RecordingShim on;
  cfg.bias_tee = true;
  REQUIRE(configure_device(nullptr, cfg, on.ops(), two_devices()).ok);
  CHECK(on.called_with("rtlsdr_set_bias_tee", 1));
}

TEST_CASE("the applied gain is the read-back value, not the request") {
  RecordingShim shim;
  shim.applied_gain = 480;   // the device settled one table entry lower
  Config cfg = device_config();
  cfg.gain_tenth_db = 490;

  const DeviceConfiguration result = configure_device(nullptr, cfg, shim.ops(), two_devices());
  REQUIRE(result.ok);
  CHECK(result.info.requested_gain_tenth_db == 490);
  CHECK(result.info.applied_gain_tenth_db == 480);
  // The request was snapped to the nearest table entry before being applied.
  CHECK(shim.called_with("rtlsdr_set_tuner_gain", 496));
}

TEST_CASE("a snapped setting produces an informational message naming both values") {
  RecordingShim shim;
  shim.applied_rate = 1000000;      // the device snapped the rate
  shim.applied_gain = 480;
  Config cfg = device_config();
  cfg.gain_tenth_db = 496;

  const DeviceConfiguration result = configure_device(nullptr, cfg, shim.ops(), two_devices());
  REQUIRE(result.ok);
  const auto notices = applied_setting_notices(result.info);
  REQUIRE(notices.size() >= 2);

  bool rate_named = false;
  bool gain_named = false;
  for (const std::string& n : notices) {
    if (n.find("1024000") != std::string::npos && n.find("1000000") != std::string::npos) {
      rate_named = true;
    }
    if (n.find("496") != std::string::npos && n.find("480") != std::string::npos) {
      gain_named = true;
    }
  }
  CHECK(rate_named);
  CHECK(gain_named);

  // A device that snapped nothing says nothing.
  SourceInfo exact;
  exact.requested_sample_rate_hz = exact.applied_sample_rate_hz = 1024000;
  exact.requested_center_hz = exact.applied_center_hz = 118600000;
  exact.requested_gain_tenth_db = exact.applied_gain_tenth_db = 496;
  CHECK(applied_setting_notices(exact).empty());
}

TEST_CASE("the tuned centre is the wanted frequency plus the offset") {
  RecordingShim shim;
  Config cfg = device_config();
  cfg.center_hz = 118350000;
  cfg.offset_tune_hz = 250000;
  const DeviceConfiguration result = configure_device(nullptr, cfg, shim.ops(), two_devices());
  REQUIRE(result.ok);
  CHECK(shim.called_with("rtlsdr_set_center_freq", 118600000));
  CHECK(result.info.applied_offset_hz == 250000);
  CHECK(result.info.requested_center_hz == 118600000);
}

TEST_CASE("the serial of the selected device is carried into the record") {
  RecordingShim shim;
  Config cfg = device_config();
  cfg.device_index = 1;
  const DeviceConfiguration result = configure_device(nullptr, cfg, shim.ops(), two_devices());
  REQUIRE(result.ok);
  CHECK(result.info.serial == "00000002");
  CHECK(result.info.driver.rfind("librtlsdr ", 0) == 0);
}

TEST_CASE("with device support disabled, the device source names the CMake option") {
#if RTLANGLE_WITH_RTLSDR
  // Deliberately not exercised with device support compiled in. `ctest -LE
  // hardware` must be genuinely hardware-free, and opening a device is a
  // hardware interaction even when it fails: how long it takes depends on what
  // else is holding the device. Everything about the device path that can be
  // decided without one - the error mapping, the gain snapping, the
  // configuration sequence - is tested above from its return codes.
#else
  Config cfg = device_config();
  cfg.source_spec = "rtlsdr";
  std::string error;
  const auto source = make_source(cfg, error);
  CHECK(source == nullptr);
  CHECK(error.find("RTLANGLE_WITH_RTLSDR=OFF") != std::string::npos);
  CHECK(error.find("--source synthetic") != std::string::npos);
#endif
}

TEST_CASE("the gain table renders as decibels") {
  CHECK(app::format_gain_table({}).find("no gain table") != std::string::npos);
  const std::string table = app::format_gain_table({0, 144, 496});
  CHECK(table.find("0.0") != std::string::npos);
  CHECK(table.find("14.4") != std::string::npos);
  CHECK(table.find("49.6 dB") != std::string::npos);
  // Whatever a device reports, the report never suggests a broad pattern kill.
  CHECK(table.find("pkill") == std::string::npos);

#if !RTLANGLE_WITH_RTLSDR
  // Without device support the report is buildable and says so; with it, the
  // report opens devices to read their gain tables and therefore belongs to the
  // hardware-labelled run rather than to this one.
  const app::DeviceReport report = app::describe_devices(device_config());
  CHECK_FALSE(report.lines.empty());
  CHECK(report.lines[0].rfind("rtlangle", 0) == 0);
  for (const std::string& line : report.lines) {
    CHECK(line.find("pkill") == std::string::npos);
  }
#endif
}

}  // TEST_SUITE
