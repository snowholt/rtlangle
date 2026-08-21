// WP4 — the source contract: determinism, deadlines, cancellation, worker-error
// wakeup, and the .cu8 replay metadata and lifecycle. Spec sections 6.1, 7.5,
// and 8.5.

#include <doctest/doctest.h>

#include "core/config.h"
#include "persist/path_safety.h"
#include "source/iq_file_source.h"
#include "source/sample_source.h"
#include "source/source_factory.h"
#include "source/synthetic_source.h"
#include "tests/support/fake_sources.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace rtlangle;
using namespace std::chrono_literals;

namespace {

namespace fs = std::filesystem;

// A temporary directory that removes itself, so a failing assertion cannot
// leave files behind for the next run to trip over.
class TempDir {
 public:
  TempDir() {
    static std::atomic<int> counter{0};
    path_ = fs::temp_directory_path() /
            ("rtlangle-test-" + std::to_string(::getpid()) + "-" +
             std::to_string(counter.fetch_add(1)));
    fs::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  const fs::path& path() const { return path_; }
  std::string file(const std::string& name) const { return (path_ / name).string(); }

 private:
  fs::path path_;
};

void write_bytes(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

void write_text(const std::string& path, const std::string& text) {
  std::ofstream out(path);
  out << text;
}

Config replay_config() {
  Config c;
  c.center_hz = 118350000;
  c.sample_rate_hz = 1024000;
  c.gain_tenth_db = 496;
  return c;
}

std::vector<std::uint8_t> ramp_bytes(std::size_t pairs) {
  std::vector<std::uint8_t> b(pairs * 2);
  for (std::size_t i = 0; i < b.size(); ++i) b[i] = static_cast<std::uint8_t>(i % 256);
  return b;
}

}  // namespace

TEST_SUITE("sources") {

TEST_CASE("the synthetic source is reproducible for a seed") {
  SyntheticParams p;
  p.seed = 20260819;
  SyntheticSource a(p);
  SyntheticSource b(p);

  std::vector<std::complex<float>> va(4096);
  std::vector<std::complex<float>> vb(4096);
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  CHECK(a.read(va, deadline).samples == va.size());
  CHECK(b.read(vb, deadline).samples == vb.size());
  for (std::size_t i = 0; i < va.size(); ++i) REQUIRE(va[i] == vb[i]);

  SyntheticParams q = p;
  q.seed = 999;
  SyntheticSource c(q);
  std::vector<std::complex<float>> vc(4096);
  c.read(vc, deadline);
  bool differs = false;
  for (std::size_t i = 0; i < va.size(); ++i) {
    if (va[i] != vc[i]) differs = true;
  }
  CHECK(differs);
}

TEST_CASE("a settable SNR takes effect only at the next flush boundary") {
  SyntheticParams p;
  p.snr_db = 0.0;
  SyntheticSource s(p);
  const auto deadline = std::chrono::steady_clock::now() + 5s;

  std::vector<std::complex<float>> before(8192);
  s.read(before, deadline);

  s.set_snr_db(30.0);
  std::vector<std::complex<float>> during(8192);
  s.read(during, deadline);   // still generated at the old SNR

  s.flush();
  std::vector<std::complex<float>> after(8192);
  s.read(after, deadline);

  auto power = [](const std::vector<std::complex<float>>& v) {
    double sum = 0.0;
    for (const auto& x : v) sum += std::norm(x);
    return sum / static_cast<double>(v.size());
  };
  CHECK(power(after) > power(during) * 10.0);
}

TEST_CASE("a read whose deadline has already passed returns Timeout without blocking") {
  test::RingBackedSource src(test::RingBackedSource::Behaviour::Stall, 0ms);
  std::vector<std::complex<float>> buf(1024);

  const auto start = std::chrono::steady_clock::now();
  const auto r = src.read(buf, start - 1s);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  CHECK(r.status == ReadStatus::Timeout);
  CHECK(r.samples == 0);
  CHECK(elapsed < 100ms);
}

TEST_CASE("a stalled source times out at its deadline rather than hanging") {
  test::RingBackedSource src(test::RingBackedSource::Behaviour::Stall, 0ms);
  std::vector<std::complex<float>> buf(1024);
  const auto start = std::chrono::steady_clock::now();
  const auto r = src.read(buf, start + 200ms);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  CHECK(r.status == ReadStatus::Timeout);
  CHECK(elapsed >= 150ms);
  CHECK(elapsed < 2s);
}

TEST_CASE("cancel from another thread wakes a blocked read") {
  test::RingBackedSource src(test::RingBackedSource::Behaviour::Stall, 0ms);
  std::vector<std::complex<float>> buf(1024);

  std::thread canceller([&] {
    std::this_thread::sleep_for(50ms);
    src.cancel();
  });
  const auto start = std::chrono::steady_clock::now();
  const auto r = src.read(buf, start + 10s);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  canceller.join();

  CHECK(r.status == ReadStatus::Cancelled);
  CHECK(elapsed < 200ms);

  // Every later read reports Cancelled too.
  CHECK(src.read(buf, std::chrono::steady_clock::now() + 1s).status == ReadStatus::Cancelled);
}

TEST_CASE("a failing worker wakes a blocked reader with Error, not Timeout") {
  test::RingBackedSource src(test::RingBackedSource::Behaviour::FailAfterDelay, 50ms);
  std::vector<std::complex<float>> buf(1024);
  const auto start = std::chrono::steady_clock::now();
  // The deadline is far away: if the worker did not wake the reader, this would
  // return Timeout after ten seconds instead.
  const auto r = src.read(buf, start + 10s);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  CHECK(r.status == ReadStatus::Error);
  CHECK_FALSE(r.error.empty());
  CHECK(elapsed < 1s);
}

TEST_CASE("a feeding source returns Ok and the ring reports host-side drops") {
  test::RingBackedSource src(test::RingBackedSource::Behaviour::Feed, 0ms, 1024);
  std::vector<std::complex<float>> buf(256);
  const auto r = src.read(buf, std::chrono::steady_clock::now() + 5s);
  CHECK(r.status == ReadStatus::Ok);
  CHECK(r.samples == buf.size());

  // Leave the worker running without reading, so the bounded buffer fills and
  // the discarded samples are counted. They are host-side drops, not device
  // overruns, and nothing here calls them one.
  std::this_thread::sleep_for(200ms);
  const auto r2 = src.read(buf, std::chrono::steady_clock::now() + 5s);
  CHECK(r2.host_dropped_samples > 0);
}

TEST_CASE("every ReadStatus value is produced by at least one source") {
  // Ok, Timeout, Error, and Cancelled are covered by the cases above;
  // EndOfStream is covered here and by the replay lifecycle tests.
  test::ShortSource src(100);
  std::vector<std::complex<float>> buf(256);
  const auto r = src.read(buf, std::chrono::steady_clock::now() + 1s);
  CHECK(r.status == ReadStatus::EndOfStream);
  CHECK(r.samples == 100);
}

TEST_CASE("replay yields the exact expected complex values for a known byte pattern") {
  TempDir dir;
  const std::string path = dir.file("capture.cu8");
  write_bytes(path, {0, 255, 127, 128, 64, 192});

  std::string error;
  IqFileSource src(path, replay_config(), error);
  REQUIRE_MESSAGE(src.ok(), error);

  std::vector<std::complex<float>> out(3);
  const auto r = src.read(out, std::chrono::steady_clock::now() + 1s);
  CHECK(r.samples == 3);
  auto norm = [](int b) { return (static_cast<float>(b) - 127.4F) / 127.5F; };
  CHECK(out[0].real() == doctest::Approx(norm(0)));
  CHECK(out[0].imag() == doctest::Approx(norm(255)));
  CHECK(out[1].real() == doctest::Approx(norm(127)));
  CHECK(out[1].imag() == doctest::Approx(norm(128)));
  CHECK(out[2].real() == doctest::Approx(norm(64)));
  CHECK(out[2].imag() == doctest::Approx(norm(192)));
}

TEST_CASE("replay reports the configuration as both requested and applied") {
  TempDir dir;
  const std::string path = dir.file("airband.cu8");
  write_bytes(path, ramp_bytes(64));

  std::string error;
  const Config cfg = replay_config();
  IqFileSource src(path, cfg, error);
  REQUIRE(src.ok());

  const SourceInfo i = src.info();
  CHECK(i.driver == "iq-file");
  CHECK(i.device_name == "airband.cu8");
  // A file has no hardware to snap anything, so the two are equal by
  // construction.
  CHECK(i.requested_sample_rate_hz == cfg.sample_rate_hz);
  CHECK(i.applied_sample_rate_hz == cfg.sample_rate_hz);
  CHECK(i.requested_center_hz == i.applied_center_hz);
  CHECK(i.requested_gain_tenth_db == 496);
  CHECK(i.applied_gain_tenth_db == 496);
  CHECK_FALSE(i.agc_enabled);
  CHECK(src.notices().empty());
}

TEST_CASE("a sidecar that agrees is accepted and one that disagrees names both values") {
  TempDir dir;
  const std::string path = dir.file("capture.cu8");
  write_bytes(path, ramp_bytes(64));
  const Config cfg = replay_config();

  SUBCASE("agreeing") {
    write_text(path + ".json", R"({"sample_rate_hz":1024000,"center_hz":118350000})");
    std::string error;
    IqFileSource src(path, cfg, error);
    CHECK_MESSAGE(src.ok(), error);
  }
  SUBCASE("disagreeing on the sample rate") {
    write_text(path + ".json", R"({"sample_rate_hz":2048000,"center_hz":118350000})");
    std::string error;
    IqFileSource src(path, cfg, error);
    CHECK_FALSE(src.ok());
    CHECK(error.find("2048000") != std::string::npos);
    CHECK(error.find("1024000") != std::string::npos);
  }
  SUBCASE("disagreeing on the centre frequency") {
    write_text(path + ".json", R"({"sample_rate_hz":1024000,"center_hz":121500000})");
    std::string error;
    IqFileSource src(path, cfg, error);
    CHECK_FALSE(src.ok());
    CHECK(error.find("121500000") != std::string::npos);
  }
  SUBCASE("incomplete") {
    write_text(path + ".json", R"({"sample_rate_hz":1024000})");
    std::string error;
    IqFileSource src(path, cfg, error);
    CHECK_FALSE(src.ok());
    CHECK(error.find("center_hz") != std::string::npos);
  }
  SUBCASE("missing, which is not an error") {
    std::string error;
    IqFileSource src(path, cfg, error);
    CHECK_MESSAGE(src.ok(), error);
  }
}

TEST_CASE("a symlinked replay path or sidecar is refused with the path named") {
  TempDir dir;
  const std::string real = dir.file("real.cu8");
  write_bytes(real, ramp_bytes(64));
  const std::string link = dir.file("link.cu8");
  fs::create_symlink(real, link);

  std::string error;
  IqFileSource src(link, replay_config(), error);
  CHECK_FALSE(src.ok());
  CHECK(error.find("link.cu8") != std::string::npos);
  CHECK(error.find("symbolic link") != std::string::npos);

  // And a symlinked sidecar beside a real file.
  const std::string other = dir.file("meta.json");
  write_text(other, R"({"sample_rate_hz":1024000,"center_hz":118350000})");
  fs::create_symlink(other, real + ".json");
  std::string error2;
  IqFileSource src2(real, replay_config(), error2);
  CHECK_FALSE(src2.ok());
  CHECK(error2.find("symbolic link") != std::string::npos);
}

TEST_CASE("a sidecar above the size limit is refused") {
  TempDir dir;
  const std::string path = dir.file("capture.cu8");
  write_bytes(path, ramp_bytes(64));
  write_text(path + ".json", std::string(kMaxSidecarBytes + 16, 'x'));

  std::string error;
  IqFileSource src(path, replay_config(), error);
  CHECK_FALSE(src.ok());
  CHECK(error.find("above the limit") != std::string::npos);
}

TEST_CASE("the replay stream is consumed sequentially across captures") {
  TempDir dir;
  const std::string path = dir.file("capture.cu8");
  const auto bytes = ramp_bytes(256);
  write_bytes(path, bytes);

  std::string error;
  IqFileSource src(path, replay_config(), error);
  REQUIRE(src.ok());
  const auto deadline = std::chrono::steady_clock::now() + 1s;

  std::vector<std::complex<float>> first(64);
  std::vector<std::complex<float>> second(64);
  CHECK(src.read(first, deadline).samples == 64);
  src.flush();   // between captures; the position must NOT rewind
  CHECK(src.read(second, deadline).samples == 64);

  auto norm = [](std::uint8_t b) { return (static_cast<float>(b) - 127.4F) / 127.5F; };
  // The second capture continues at pair 64, checked against the file bytes.
  CHECK(second[0].real() == doctest::Approx(norm(bytes[128])));
  CHECK(second[0].imag() == doctest::Approx(norm(bytes[129])));
  CHECK(first[0] != second[0]);
}

TEST_CASE("exhausting the replay file ends the stream and never loops") {
  TempDir dir;
  const std::string path = dir.file("short.cu8");
  write_bytes(path, ramp_bytes(32));

  std::string error;
  IqFileSource src(path, replay_config(), error);
  REQUIRE(src.ok());
  const auto deadline = std::chrono::steady_clock::now() + 1s;

  std::vector<std::complex<float>> out(64);
  const auto first = src.read(out, deadline);
  CHECK(first.samples == 32);
  CHECK(first.status == ReadStatus::EndOfStream);

  // Every subsequent read reports end of stream rather than replaying the file,
  // which would fabricate replicates that do not exist.
  for (int i = 0; i < 3; ++i) {
    const auto again = src.read(out, deadline);
    CHECK(again.samples == 0);
    CHECK(again.status == ReadStatus::EndOfStream);
  }
}

TEST_CASE("an odd trailing byte is dropped and reported exactly once") {
  TempDir dir;
  const std::string path = dir.file("odd.cu8");
  auto bytes = ramp_bytes(16);
  bytes.push_back(7);
  write_bytes(path, bytes);

  std::string error;
  IqFileSource src(path, replay_config(), error);
  REQUIRE(src.ok());
  CHECK(src.total_samples() == 16);
  REQUIRE(src.notices().size() == 1);
  CHECK(src.notices()[0].find("trailing byte") != std::string::npos);

  // Reading the whole file does not repeat the message.
  std::vector<std::complex<float>> out(32);
  src.read(out, std::chrono::steady_clock::now() + 1s);
  src.read(out, std::chrono::steady_clock::now() + 1s);
  CHECK(src.notices().size() == 1);
}

TEST_CASE("the clipping census counts rail bytes exactly") {
  TempDir dir;
  const std::string path = dir.file("rails.cu8");
  // 1000 bytes, of which exactly 3 are at a rail.
  std::vector<std::uint8_t> bytes(1000, 100);
  bytes[10] = 0;
  bytes[20] = 255;
  bytes[30] = 0;
  write_bytes(path, bytes);

  std::string error;
  IqFileSource src(path, replay_config(), error);
  REQUIRE(src.ok());
  std::vector<std::complex<float>> out(500);
  src.read(out, std::chrono::steady_clock::now() + 1s);
  CHECK(src.clipped_fraction() == doctest::Approx(0.003));

  // flush() restarts the census, which is what makes it a per-capture figure.
  src.flush();
  CHECK(src.clipped_fraction() == doctest::Approx(0.0));
}

TEST_CASE("make_source builds each source kind and names what it refuses") {
  Config cfg = replay_config();
  std::string error;

  cfg.source_spec = "synthetic";
  CHECK(make_source(cfg, error) != nullptr);
  CHECK(error.empty());

  TempDir dir;
  const std::string path = dir.file("capture.cu8");
  write_bytes(path, ramp_bytes(64));
  cfg.source_spec = "file:" + path;
  auto file_source = make_source(cfg, error);
  CHECK(file_source != nullptr);
  CHECK(take_source_notices(*file_source).empty());

  cfg.source_spec = "file:" + dir.file("missing.cu8");
  CHECK(make_source(cfg, error) == nullptr);
  CHECK(error.find("missing.cu8") != std::string::npos);

  cfg.source_spec = "usrp";
  CHECK(make_source(cfg, error) == nullptr);
  CHECK(error.find("usrp") != std::string::npos);

#if RTLANGLE_WITH_RTLSDR
  // Deliberately NOT exercised here. `ctest -LE hardware` must be genuinely
  // hardware-free, and opening a device is a hardware interaction even when it
  // fails: how long it takes depends on what else is holding the device. The
  // device path's error mapping is tested directly in the offline suite, from
  // the return codes, with no device involved.
#else
  cfg.source_spec = "rtlsdr";
  const auto device = make_source(cfg, error);
  CHECK(device == nullptr);
  CHECK(error.find("RTLANGLE_WITH_RTLSDR=OFF") != std::string::npos);
#endif
}

TEST_CASE("the synthetic source retunes and reports the applied centre") {
  SyntheticParams p;
  p.band_transmitters = {{118500000, 0.5}, {119200000, 0.25}};
  SyntheticSource src(p);
  std::string error;
  CHECK(src.retune(119000000, error));
  CHECK(error.empty());
  CHECK(src.info().applied_center_hz == 119000000U + 250000U);
}

}  // TEST_SUITE
