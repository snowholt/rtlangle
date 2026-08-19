# rtlangle Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a receive-only terminal application that measures AM airband reception quality at a sequence of manually-set dipole antenna angles and ranks those angles.

**Architecture:** A layered C++20 static library (`rtlangle_core`) plus a thin executable. Hardware, terminal, and storage sit behind four interfaces — `ISampleSource`, `IAngleProvider`, `ITerminalUi`, `SessionStore` — so `ExperimentController` and every DSP/metrics class is testable with zero hardware. Capture streams IQ through an offset mixer and two decimation stages down to a 32 kHz channel stream held in memory; all measurement runs on that buffered stream afterwards.

**Tech Stack:** C++20, CMake 3.24+, Ninja, librtlsdr (rtl-sdr-blog fork), FFTW3, nlohmann/json (header-only, FetchContent), doctest (header-only, FetchContent), POSIX termios.

**Spec:** `docs/superpowers/specs/2026-08-19-rtl-sdr-antenna-angle-design.md` — read it alongside this plan. Where this plan and the spec disagree, the spec wins; report the discrepancy.

## Global Constraints

- **C++20**, compiled with `-Wall -Wextra -Wpedantic`. No compiler-specific extensions.
- **The tool never transmits.** Never call `rtlsdr_set_bias_tee` unless `Config::bias_tee` is true, which requires the explicit `--bias-tee` flag.
- **AGC stays off.** Always call `rtlsdr_set_tuner_gain_mode(dev, 1)` (manual) and `rtlsdr_set_agc_mode(dev, 0)`. Gain is the experiment's controlled variable.
- **SNR is computed in the linear power domain, subtracting before dividing**: `SNR_lin = (P_event - N) / N`, then `10*log10`. Never `P_event / N`.
- **No placeholder implementations.** A function that cannot be completed is a blocker to report, not a stub to ship.
- Namespace for all library code: `rtlangle`. `core/`, `source/`, `angle/`, `persist/` and `experiment/` put their types directly in `rtlangle`; `dsp/`, `metrics/`, `ui/` and `app/` use the matching sub-namespace (`rtlangle::dsp`, `rtlangle::metrics`, `rtlangle::ui`, `rtlangle::app`). Every signature in this plan is written with its full namespace - follow them exactly.
- Headers use `#pragma once`. Public headers live next to their sources under `src/`; the `src/` directory is the single include root.
- Every task ends with a green build (`cmake --build build`) and green tests (`ctest --test-dir build --output-on-failure -LE hardware`) before its commit.
- Commit messages use Conventional Commits (`feat:`, `test:`, `docs:`, `build:`, `fix:`). Professional tone, no emoji, English only.
- **Fixed constants** (do not invent others): default channel rate 32000 Hz, frame length 1024 samples, frame hop 512 samples, byte-to-float conversion `(x - 127.4f) / 127.5f`, NCO table size 65536, JSON `schema_version` 1.

## File Structure

```
CMakeLists.txt                     top-level: options, deps, targets
cmake/Dependencies.cmake           find_package-first, FetchContent fallback
src/core/db.h|.cpp                 decibel helpers, guarded
src/core/statistics.h|.cpp         percentile, median, order-statistic CI
src/core/angle_sequence.h|.cpp     angle generation, validation, visit order
src/core/config.h|.cpp             Config aggregate + validate()
src/core/records.h|.cpp            EventRecord, Measurement, SessionMetadata, Summary
src/dsp/fir_design.h|.cpp          Kaiser-window low-pass design
src/dsp/offset_mixer.h|.cpp        integer-accumulator NCO
src/dsp/fir_decimator.h|.cpp       decimating FIR
src/dsp/channel_filter.h|.cpp      FIR low-pass at channel rate
src/dsp/am_demodulator.h|.cpp      envelope, DC block, audio shaping
src/dsp/chain.h|.cpp               build_chain(Config) -> Chain; Chain::process()
src/dsp/framer.h|.cpp              frame_powers(span, len, hop)
src/dsp/spectrum.h|.cpp            FFTW wrapper: carrier offset, Welch PSD
src/metrics/noise_floor.h|.cpp     NoiseFloorEstimator
src/metrics/event_detector.h|.cpp  EventDetector (hysteresis squelch)
src/metrics/snr_estimator.h|.cpp   SnrEstimator -> Measurement metrics
src/source/sample_source.h         ISampleSource, SourceInfo
src/source/synthetic_source.h|.cpp SyntheticSource
src/source/iq_file_source.h|.cpp   IqFileSource (.cu8)
src/source/rtlsdr_source.h|.cpp    RtlSdrSource (guarded by RTLANGLE_WITH_RTLSDR)
src/angle/angle_provider.h         IAngleProvider, AngleOutcome
src/angle/manual_angle_provider.h|.cpp
src/angle/fixed_angle_provider.h|.cpp
src/ui/terminal_ui.h               ITerminalUi, MenuItem, Table
src/ui/ansi_terminal_ui.h|.cpp     raw mode, colour, arrow-key menu
src/ui/scripted_terminal_ui.h|.cpp test double
src/ui/report_renderer.h|.cpp       summary table, ranking, warnings
src/persist/atomic_write.h|.cpp     write_atomic()
src/persist/csv.h|.cpp              RFC4180 escaping, CsvAppender
src/persist/session_store.h|.cpp    SessionStore (JSON + CSV)
src/persist/session_loader.h|.cpp   SessionLoader (resume)
src/experiment/visit_plan.h|.cpp    (round, angle) plan + completion diffing
src/experiment/capture_runner.h|.cpp source -> chain -> channel buffer
src/experiment/controller.h|.cpp    ExperimentController
src/experiment/summarizer.h|.cpp    per-angle aggregation, ranking, warnings
src/app/cli_parser.h|.cpp           argv + JSON config file -> Config
src/app/scan_command.h|.cpp         airband activity scan
src/app/device_check.h|.cpp         device/gain-table diagnostic
src/app/menus.h|.cpp                top-level interactive menus
src/app/main.cpp                    entry point
tests/*.cpp                         one file per task, doctest
tests/support/*.h|.cpp              synthetic signal builders shared by tests
docs/architecture.md
examples/session-synthetic/         committed example output
README.md
```

---

### Task 1: Build skeleton, dependencies, decibel helpers

Establishes the CMake project, both header-only dependencies, the test harness, and the first tested unit. Everything later depends on this compiling.

**Files:**
- Create: `CMakeLists.txt`, `cmake/Dependencies.cmake`
- Create: `src/core/db.h`, `src/core/db.cpp`
- Test: `tests/test_db.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: CMake targets `rtlangle_core` (STATIC), `rtlangle` (EXE), `rtlangle_tests` (EXE, registered with `add_test` and label-free; hardware tests later get `-L hardware`). Options `RTLANGLE_BUILD_TESTS` (default ON) and `RTLANGLE_WITH_RTLSDR` (default ON). Functions `rtlangle::to_db(double linear) -> double`, `rtlangle::from_db(double db) -> double`, `rtlangle::to_dbfs(double power) -> double`, constant `rtlangle::kDbFloor = -200.0`.

- [ ] **Step 1: Write the failing test**

`tests/test_db.cpp`:

```cpp
#include <doctest/doctest.h>
#include "core/db.h"

using namespace rtlangle;

TEST_CASE("to_db converts linear power to decibels") {
    CHECK(to_db(1.0) == doctest::Approx(0.0));
    CHECK(to_db(2.0) == doctest::Approx(3.010299957).epsilon(1e-6));
    CHECK(to_db(100.0) == doctest::Approx(20.0));
}

TEST_CASE("to_db floors non-positive input instead of returning -inf or NaN") {
    CHECK(to_db(0.0) == kDbFloor);
    CHECK(to_db(-1.0) == kDbFloor);
}

TEST_CASE("from_db is the inverse of to_db") {
    for (double db : {-30.0, -3.0, 0.0, 7.5, 42.0}) {
        CHECK(to_db(from_db(db)) == doctest::Approx(db).epsilon(1e-9));
    }
}

TEST_CASE("to_dbfs expresses power relative to a full-scale unit sinusoid") {
    // Full scale for our normalised complex samples is |x| == 1, power 1.0.
    CHECK(to_dbfs(1.0) == doctest::Approx(0.0));
    CHECK(to_dbfs(0.01) == doctest::Approx(-20.0));
}
```

- [ ] **Step 2: Write the build files**

`cmake/Dependencies.cmake`:

```cmake
include(FetchContent)

# nlohmann/json - prefer a system copy, fall back to a pinned download.
find_package(nlohmann_json 3.11 QUIET)
if(NOT nlohmann_json_FOUND)
  FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d)
  FetchContent_MakeAvailable(nlohmann_json)
endif()

if(RTLANGLE_BUILD_TESTS)
  find_package(doctest 2.4 QUIET)
  if(NOT doctest_FOUND)
    FetchContent_Declare(doctest
      URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.11.tar.gz
      URL_HASH SHA256=632ed2c05a7f53fa961381497bf8069093f0d6628c5f26286161fbd32a560186)
    FetchContent_MakeAvailable(doctest)
  endif()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFTW3 REQUIRED IMPORTED_TARGET fftw3)

if(RTLANGLE_WITH_RTLSDR)
  pkg_check_modules(RTLSDR IMPORTED_TARGET librtlsdr)
  if(NOT RTLSDR_FOUND)
    message(FATAL_ERROR
      "librtlsdr not found. Install it with:\n"
      "  sudo apt install librtlsdr-dev\n"
      "or build the rtl-sdr-blog fork (required for RTL-SDR Blog V4).\n"
      "To build without hardware support, configure with -DRTLANGLE_WITH_RTLSDR=OFF")
  endif()
endif()

find_package(Threads REQUIRED)
```

Top-level `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.24)
project(rtlangle VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RTLANGLE_BUILD_TESTS "Build the test suite" ON)
option(RTLANGLE_WITH_RTLSDR "Build RTL-SDR hardware support" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(cmake/Dependencies.cmake)

add_library(rtlangle_core STATIC src/core/db.cpp)
target_include_directories(rtlangle_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(rtlangle_core PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(rtlangle_core PUBLIC nlohmann_json::nlohmann_json
                                            PkgConfig::FFTW3 Threads::Threads)
if(RTLANGLE_WITH_RTLSDR)
  target_link_libraries(rtlangle_core PUBLIC PkgConfig::RTLSDR)
  target_compile_definitions(rtlangle_core PUBLIC RTLANGLE_WITH_RTLSDR=1)
endif()
target_compile_definitions(rtlangle_core PUBLIC RTLANGLE_VERSION="${PROJECT_VERSION}")

# The executable is added in Task 16; until then rtlangle_core carries the work.

if(RTLANGLE_BUILD_TESTS)
  enable_testing()
  add_executable(rtlangle_tests tests/test_main.cpp tests/test_db.cpp)
  target_link_libraries(rtlangle_tests PRIVATE rtlangle_core doctest::doctest)
  target_compile_options(rtlangle_tests PRIVATE -Wall -Wextra -Wpedantic)
  include(CTest)
  add_test(NAME unit COMMAND rtlangle_tests)
endif()
```

`tests/test_main.cpp` (exactly this, so doctest's `main` is compiled once):

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cmake -S . -B build -G Ninja && cmake --build build`
Expected: FAIL — `core/db.h: No such file or directory`.

- [ ] **Step 4: Write the implementation**

`src/core/db.h`:

```cpp
#pragma once

namespace rtlangle {

/// Returned instead of -infinity when a decibel conversion has no valid input.
inline constexpr double kDbFloor = -200.0;

/// 10*log10(linear). Returns kDbFloor for linear <= 0 rather than -inf or NaN.
double to_db(double linear_power);

/// Inverse of to_db: 10^(db/10).
double from_db(double db);

/// Power relative to full scale, where a normalised sample of magnitude 1
/// has power 1.0. Identical arithmetic to to_db; the distinct name documents
/// the reference at every call site.
double to_dbfs(double power);

}  // namespace rtlangle
```

`src/core/db.cpp`:

```cpp
#include "core/db.h"

#include <cmath>

namespace rtlangle {

double to_db(double linear_power) {
    if (!(linear_power > 0.0)) return kDbFloor;  // also catches NaN
    return 10.0 * std::log10(linear_power);
}

double from_db(double db) { return std::pow(10.0, db / 10.0); }

double to_dbfs(double power) { return to_db(power); }

}  // namespace rtlangle
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS, 4 test cases.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt cmake src/core/db.h src/core/db.cpp tests/test_main.cpp tests/test_db.cpp
git commit -m "build: add CMake skeleton, dependencies and decibel helpers"
```

---

### Task 2: Statistics — percentile, median, order-statistic confidence interval

Pure numerics with no dependencies. The median confidence interval is the basis of the report's "difference not statistically resolved" warning, so it must be exactly right.

**Files:**
- Create: `src/core/statistics.h`, `src/core/statistics.cpp`
- Test: `tests/test_statistics.cpp`
- Modify: `CMakeLists.txt` (add sources to both targets)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `double rtlangle::percentile(std::vector<double> values, double p)` — `p` in `[0,100]`, linear interpolation between order statistics, takes its argument by value and sorts it internally.
  - `double rtlangle::median(std::vector<double> values)`
  - `struct rtlangle::MedianCi { double low; double high; };`
  - `std::optional<MedianCi> rtlangle::median_ci95(std::vector<double> values)` — distribution-free interval from binomial order statistics; `std::nullopt` when `values.size() < 6`.

- [ ] **Step 1: Write the failing test**

`tests/test_statistics.cpp`:

```cpp
#include <doctest/doctest.h>
#include "core/statistics.h"

#include <vector>

using namespace rtlangle;

TEST_CASE("percentile interpolates linearly between order statistics") {
    std::vector<double> v{1.0, 2.0, 3.0, 4.0};
    CHECK(percentile(v, 0.0)   == doctest::Approx(1.0));
    CHECK(percentile(v, 100.0) == doctest::Approx(4.0));
    CHECK(percentile(v, 50.0)  == doctest::Approx(2.5));
    // h = (n-1)*p/100 = 3*0.25 = 0.75 -> 1.0 + 0.75*(2.0-1.0)
    CHECK(percentile(v, 25.0)  == doctest::Approx(1.75));
}

TEST_CASE("percentile does not depend on input order") {
    std::vector<double> sorted{1.0, 2.0, 3.0, 4.0, 5.0};
    std::vector<double> shuffled{4.0, 1.0, 5.0, 2.0, 3.0};
    CHECK(percentile(shuffled, 20.0) == doctest::Approx(percentile(sorted, 20.0)));
}

TEST_CASE("median handles even and odd counts") {
    CHECK(median({3.0, 1.0, 2.0}) == doctest::Approx(2.0));
    CHECK(median({4.0, 1.0, 3.0, 2.0}) == doctest::Approx(2.5));
}

TEST_CASE("median_ci95 is unavailable below six samples") {
    // A 95% distribution-free interval needs coverage 1 - 2*P(X < k) >= 0.95
    // with X ~ Bin(n, 0.5). For n = 5 and k = 1 coverage is only 0.9375.
    for (std::size_t n = 0; n < 6; ++n) {
        std::vector<double> v(n, 1.0);
        CHECK_FALSE(median_ci95(v).has_value());
    }
}

TEST_CASE("median_ci95 picks the tightest interval that still covers 95 percent") {
    // n = 6: k = 1 gives coverage 1 - 2*(1/64) = 0.96875 >= 0.95.
    //        k = 2 gives coverage 1 - 2*(7/64) = 0.78125 < 0.95.
    // So the interval is [x_(1), x_(6)] - the full range.
    std::vector<double> six{10.0, 11.0, 12.0, 13.0, 14.0, 15.0};
    auto ci6 = median_ci95(six);
    REQUIRE(ci6.has_value());
    CHECK(ci6->low  == doctest::Approx(10.0));
    CHECK(ci6->high == doctest::Approx(15.0));

    // n = 10: k = 2 gives coverage 1 - 2*(1+10)/1024 = 0.9785 >= 0.95.
    //         k = 3 gives coverage 1 - 2*(1+10+45)/1024 = 0.8906 < 0.95.
    // So the interval is [x_(2), x_(9)].
    std::vector<double> ten{1,2,3,4,5,6,7,8,9,10};
    auto ci10 = median_ci95(ten);
    REQUIRE(ci10.has_value());
    CHECK(ci10->low  == doctest::Approx(2.0));
    CHECK(ci10->high == doctest::Approx(9.0));
}

TEST_CASE("median_ci95 brackets the median and tightens as n grows") {
    std::vector<double> small(9, 0.0), large(999, 0.0);
    for (std::size_t i = 0; i < small.size(); ++i) small[i] = static_cast<double>(i);
    for (std::size_t i = 0; i < large.size(); ++i) large[i] = static_cast<double>(i) / 111.0;

    auto cs = median_ci95(small);
    auto cl = median_ci95(large);
    REQUIRE(cs.has_value());
    REQUIRE(cl.has_value());
    CHECK(cs->low  <= median(small));
    CHECK(cs->high >= median(small));
    CHECK((cl->high - cl->low) < (cs->high - cs->low));
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: after adding `tests/test_statistics.cpp` to `rtlangle_tests` sources, `cmake --build build`
Expected: FAIL — `core/statistics.h: No such file or directory`.

- [ ] **Step 3: Write the implementation**

`src/core/statistics.h`:

```cpp
#pragma once

#include <cstddef>
#include <optional>
#include <vector>

namespace rtlangle {

/// Linear-interpolation percentile (the convention numpy calls "linear").
/// `p` is in [0, 100]. Returns NaN for an empty input.
double percentile(std::vector<double> values, double p);

double median(std::vector<double> values);

struct MedianCi {
    double low;
    double high;
};

/// Distribution-free 95% confidence interval for the median, built from
/// binomial order statistics: the interval is [x_(k), x_(n-k+1)] for the
/// largest k >= 1 whose coverage 1 - 2*P(Bin(n,0.5) < k) is still >= 0.95.
/// No such k exists for n < 6, so nullopt is returned and the caller should
/// report the interquartile range and flag the result low-confidence.
std::optional<MedianCi> median_ci95(std::vector<double> values);

}  // namespace rtlangle
```

`src/core/statistics.cpp`:

```cpp
#include "core/statistics.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace rtlangle {

double percentile(std::vector<double> values, double p) {
    if (values.empty()) return std::numeric_limits<double>::quiet_NaN();
    std::sort(values.begin(), values.end());
    if (values.size() == 1) return values.front();

    const double clamped = std::clamp(p, 0.0, 100.0);
    const double h = (static_cast<double>(values.size()) - 1.0) * clamped / 100.0;
    const std::size_t lo = static_cast<std::size_t>(std::floor(h));
    const std::size_t hi = static_cast<std::size_t>(std::ceil(h));
    if (lo == hi) return values[lo];
    return values[lo] + (h - static_cast<double>(lo)) * (values[hi] - values[lo]);
}

double median(std::vector<double> values) { return percentile(std::move(values), 50.0); }

namespace {
/// log of the binomial coefficient C(n, i).
double log_choose(std::size_t n, std::size_t i) {
    return std::lgamma(static_cast<double>(n) + 1.0)
         - std::lgamma(static_cast<double>(i) + 1.0)
         - std::lgamma(static_cast<double>(n - i) + 1.0);
}
}  // namespace

std::optional<MedianCi> median_ci95(std::vector<double> values) {
    const std::size_t n = values.size();
    if (n < 6) return std::nullopt;
    std::sort(values.begin(), values.end());

    const double log_half_pow = static_cast<double>(n) * std::log(2.0);
    double tail = 0.0;         // P(Bin(n, 0.5) < k), accumulated as k grows
    std::size_t best_k = 0;

    for (std::size_t k = 1; k <= n / 2; ++k) {
        tail += std::exp(log_choose(n, k - 1) - log_half_pow);   // add term i = k-1
        if (1.0 - 2.0 * tail >= 0.95) {
            best_k = k;                                          // still covers, keep tightening
        } else {
            break;                                               // coverage only falls from here
        }
    }
    if (best_k == 0) return std::nullopt;
    return MedianCi{values[best_k - 1], values[n - best_k]};
}

}  // namespace rtlangle
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/core/statistics.h src/core/statistics.cpp tests/test_statistics.cpp CMakeLists.txt
git commit -m "feat: add percentile, median and order-statistic median CI"
```

---

### Task 3: Angle sequence generation, validation and visit order

**Files:**
- Create: `src/core/angle_sequence.h`, `src/core/angle_sequence.cpp`
- Test: `tests/test_angle_sequence.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `enum class rtlangle::VisitOrder { Forward, Reverse, Alternating, Random };`
  - `std::optional<VisitOrder> rtlangle::parse_visit_order(std::string_view)`
  - `const char* rtlangle::to_string(VisitOrder)`
  - `struct rtlangle::AngleSpec { double start_deg = 0.0; double end_deg = 90.0; double step_deg = 15.0; std::vector<double> explicit_angles; };`
  - `struct rtlangle::AngleResult { std::vector<double> angles; std::vector<std::string> warnings; std::string error; bool ok() const { return error.empty(); } };`
  - `AngleResult rtlangle::generate_angles(const AngleSpec&)`
  - `std::vector<double> rtlangle::order_angles(const std::vector<double>& sorted_angles, VisitOrder, int round_index_1based, std::uint32_t seed)`

- [ ] **Step 1: Write the failing test**

`tests/test_angle_sequence.cpp`:

```cpp
#include <doctest/doctest.h>
#include "core/angle_sequence.h"

#include <algorithm>
#include <limits>
#include <string>

using namespace rtlangle;

TEST_CASE("default spec yields the documented seven angles") {
    AngleResult r = generate_angles(AngleSpec{});
    REQUIRE(r.ok());
    CHECK(r.angles == std::vector<double>{0, 15, 30, 45, 60, 75, 90});
    CHECK(r.warnings.empty());
}

TEST_CASE("angles are computed from the index so they do not accumulate error") {
    AngleSpec s; s.start_deg = 0; s.end_deg = 90; s.step_deg = 0.1;
    AngleResult r = generate_angles(s);
    REQUIRE(r.ok());
    CHECK(r.angles.size() == 901);
    CHECK(r.angles.back() == doctest::Approx(90.0).epsilon(1e-12));
}

TEST_CASE("a non-dividing step stops before overshooting the endpoint") {
    AngleSpec s; s.start_deg = 0; s.end_deg = 90; s.step_deg = 20;
    AngleResult r = generate_angles(s);
    REQUIRE(r.ok());
    CHECK(r.angles == std::vector<double>{0, 20, 40, 60, 80});
}

TEST_CASE("explicit angles override the range and are deduplicated and sorted") {
    AngleSpec s; s.start_deg = 0; s.end_deg = 90; s.step_deg = 15;
    s.explicit_angles = {90.0, 30.0, 0.0, 30.0000000001, 60.0};
    AngleResult r = generate_angles(s);
    REQUIRE(r.ok());
    CHECK(r.angles == std::vector<double>{0, 30, 60, 90});
}

TEST_CASE("each validation rule produces its own error") {
    SUBCASE("step must be positive") {
        AngleSpec s; s.step_deg = 0.0;
        CHECK(generate_angles(s).error.find("step") != std::string::npos);
    }
    SUBCASE("step must be finite") {
        AngleSpec s; s.step_deg = std::numeric_limits<double>::quiet_NaN();
        CHECK_FALSE(generate_angles(s).ok());
    }
    SUBCASE("end must not precede start") {
        AngleSpec s; s.start_deg = 90; s.end_deg = 0;
        CHECK(generate_angles(s).error.find("end") != std::string::npos);
    }
    SUBCASE("range must be within 0..360") {
        AngleSpec s; s.start_deg = -10;
        CHECK_FALSE(generate_angles(s).ok());
        AngleSpec t; t.end_deg = 400;
        CHECK_FALSE(generate_angles(t).ok());
    }
    SUBCASE("at least two angles are required") {
        AngleSpec s; s.start_deg = 10; s.end_deg = 10; s.step_deg = 5;
        CHECK(generate_angles(s).error.find("at least 2") != std::string::npos);
    }
    SUBCASE("at most 180 angles are allowed") {
        AngleSpec s; s.start_deg = 0; s.end_deg = 360; s.step_deg = 1;
        CHECK(generate_angles(s).error.find("180") != std::string::npos);
    }
    SUBCASE("explicit angles must be in range") {
        AngleSpec s; s.explicit_angles = {0.0, 400.0};
        CHECK_FALSE(generate_angles(s).ok());
    }
}

TEST_CASE("angles above 180 degrees warn about dipole symmetry but are allowed") {
    AngleSpec s; s.start_deg = 0; s.end_deg = 270; s.step_deg = 90;
    AngleResult r = generate_angles(s);
    REQUIRE(r.ok());
    REQUIRE(r.warnings.size() == 1);
    CHECK(r.warnings[0].find("symmetric") != std::string::npos);
}

TEST_CASE("visit order controls the per-round sequence") {
    const std::vector<double> a{0, 30, 60, 90};
    CHECK(order_angles(a, VisitOrder::Forward, 1, 7) == std::vector<double>{0, 30, 60, 90});
    CHECK(order_angles(a, VisitOrder::Forward, 2, 7) == std::vector<double>{0, 30, 60, 90});
    CHECK(order_angles(a, VisitOrder::Reverse, 1, 7) == std::vector<double>{90, 60, 30, 0});

    // Alternating breaks the correlation between angle and elapsed time.
    CHECK(order_angles(a, VisitOrder::Alternating, 1, 7) == std::vector<double>{0, 30, 60, 90});
    CHECK(order_angles(a, VisitOrder::Alternating, 2, 7) == std::vector<double>{90, 60, 30, 0});
    CHECK(order_angles(a, VisitOrder::Alternating, 3, 7) == std::vector<double>{0, 30, 60, 90});
}

TEST_CASE("random order is a reproducible permutation and varies by round") {
    const std::vector<double> a{0, 15, 30, 45, 60, 75, 90};
    auto r1 = order_angles(a, VisitOrder::Random, 1, 12345);
    auto r1_again = order_angles(a, VisitOrder::Random, 1, 12345);
    auto r2 = order_angles(a, VisitOrder::Random, 2, 12345);

    CHECK(r1 == r1_again);                       // same seed and round -> same order
    CHECK(r1 != r2);                             // different rounds differ
    auto sorted = r1; std::sort(sorted.begin(), sorted.end());
    CHECK(sorted == a);                          // still a permutation of the set
}

TEST_CASE("visit order parses from and prints to its CLI spelling") {
    CHECK(parse_visit_order("alternating").value() == VisitOrder::Alternating);
    CHECK(parse_visit_order("RANDOM").value() == VisitOrder::Random);
    CHECK_FALSE(parse_visit_order("sideways").has_value());
    CHECK(std::string(to_string(VisitOrder::Reverse)) == "reverse");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build`
Expected: FAIL — `core/angle_sequence.h: No such file or directory`.

- [ ] **Step 3: Write the implementation**

`src/core/angle_sequence.h`:

```cpp
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rtlangle {

enum class VisitOrder { Forward, Reverse, Alternating, Random };

std::optional<VisitOrder> parse_visit_order(std::string_view text);
const char* to_string(VisitOrder order);

struct AngleSpec {
    double start_deg = 0.0;
    double end_deg = 90.0;
    double step_deg = 15.0;
    /// When non-empty this replaces the range entirely.
    std::vector<double> explicit_angles;
};

struct AngleResult {
    std::vector<double> angles;             ///< sorted ascending, deduplicated
    std::vector<std::string> warnings;
    std::string error;                      ///< empty when valid
    bool ok() const { return error.empty(); }
};

AngleResult generate_angles(const AngleSpec& spec);

/// Returns the visiting sequence for one round. `round_index_1based` starts at 1.
std::vector<double> order_angles(const std::vector<double>& sorted_angles,
                                 VisitOrder order,
                                 int round_index_1based,
                                 std::uint32_t seed);

}  // namespace rtlangle
```

`src/core/angle_sequence.cpp` — the parts worth spelling out:

```cpp
#include "core/angle_sequence.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <random>

namespace rtlangle {
namespace {

constexpr double kDedupTolerance = 1e-6;
constexpr std::size_t kMaxAngles = 180;

bool finite(double v) { return std::isfinite(v); }

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

void dedup_sorted(std::vector<double>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end(),
                        [](double a, double b) { return std::fabs(a - b) < kDedupTolerance; }),
            v.end());
}

}  // namespace

std::optional<VisitOrder> parse_visit_order(std::string_view text) {
    const std::string t = lower(text);
    if (t == "forward")     return VisitOrder::Forward;
    if (t == "reverse")     return VisitOrder::Reverse;
    if (t == "alternating") return VisitOrder::Alternating;
    if (t == "random")      return VisitOrder::Random;
    return std::nullopt;
}

const char* to_string(VisitOrder order) {
    switch (order) {
        case VisitOrder::Forward:     return "forward";
        case VisitOrder::Reverse:     return "reverse";
        case VisitOrder::Alternating: return "alternating";
        case VisitOrder::Random:      return "random";
    }
    return "forward";
}

AngleResult generate_angles(const AngleSpec& spec) {
    AngleResult result;

    if (!spec.explicit_angles.empty()) {
        for (double a : spec.explicit_angles) {
            if (!finite(a) || a < 0.0 || a > 360.0) {
                result.error = "explicit angle " + std::to_string(a) +
                               " is outside the allowed range 0..360 degrees";
                return result;
            }
        }
        result.angles = spec.explicit_angles;
        dedup_sorted(result.angles);
    } else {
        if (!finite(spec.step_deg) || spec.step_deg <= 0.0) {
            result.error = "angle step must be a finite value greater than 0";
            return result;
        }
        if (!finite(spec.start_deg) || !finite(spec.end_deg) ||
            spec.start_deg < 0.0 || spec.start_deg > 360.0 ||
            spec.end_deg < 0.0 || spec.end_deg > 360.0) {
            result.error = "start and end angles must be finite and within 0..360 degrees";
            return result;
        }
        if (spec.end_deg < spec.start_deg) {
            result.error = "end angle must not be smaller than the start angle";
            return result;
        }
        // Index-based so repeated addition never accumulates rounding error.
        const double span = spec.end_deg - spec.start_deg;
        const std::size_t count = static_cast<std::size_t>(std::floor(span / spec.step_deg + 1e-9)) + 1;
        if (count > kMaxAngles + 1) {
            result.error = "the requested range produces more than 180 angles; "
                           "increase --step-deg or narrow the range";
            return result;
        }
        result.angles.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            result.angles.push_back(spec.start_deg + static_cast<double>(i) * spec.step_deg);
        }
        dedup_sorted(result.angles);
    }

    if (result.angles.size() < 2) {
        result.error = "an experiment needs at least 2 distinct angles";
        return result;
    }
    if (result.angles.size() > kMaxAngles) {
        result.error = "an experiment supports at most 180 angles";
        return result;
    }
    if (result.angles.back() > 180.0) {
        result.warnings.push_back(
            "angles above 180 degrees were requested; a dipole radiation pattern is "
            "symmetric about 180 degrees, so those angles duplicate coverage already "
            "measured below 180");
    }
    return result;
}

std::vector<double> order_angles(const std::vector<double>& sorted_angles,
                                 VisitOrder order, int round_index_1based,
                                 std::uint32_t seed) {
    std::vector<double> out = sorted_angles;
    switch (order) {
        case VisitOrder::Forward:
            break;
        case VisitOrder::Reverse:
            std::reverse(out.begin(), out.end());
            break;
        case VisitOrder::Alternating:
            if (round_index_1based % 2 == 0) std::reverse(out.begin(), out.end());
            break;
        case VisitOrder::Random: {
            // Mixing the round into the seed keeps each round reproducible while
            // still giving a different permutation per round.
            std::mt19937 rng(seed + static_cast<std::uint32_t>(round_index_1based) * 2654435761u);
            std::shuffle(out.begin(), out.end(), rng);
            break;
        }
    }
    return out;
}

}  // namespace rtlangle
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/core/angle_sequence.h src/core/angle_sequence.cpp tests/test_angle_sequence.cpp CMakeLists.txt
git commit -m "feat: add angle sequence generation, validation and visit order"
```

---

### Task 4: Config aggregate, validation and JSON round-trip

The single source of truth for every tunable. It is serialised verbatim into `session.json`, so its JSON round-trip must be lossless or resume silently changes the experiment.

**Files:**
- Create: `src/core/config.h`, `src/core/config.cpp`
- Test: `tests/test_config.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `AngleSpec`, `VisitOrder` (Task 3).
- Produces:
  - `enum class rtlangle::RankMetric { Channel, Audio, Both };` with `parse_rank_metric` / `to_string`.
  - `struct rtlangle::Config` with the fields listed below.
  - `struct rtlangle::ValidationResult { std::vector<std::string> errors; std::vector<std::string> warnings; bool ok() const { return errors.empty(); } };`
  - `ValidationResult rtlangle::validate(const Config&)`
  - `VisitOrder rtlangle::effective_order(const Config&)` — applies the "alternating when rounds > 1" default.
  - `void rtlangle::to_json(nlohmann::json&, const Config&)` and `void rtlangle::from_json(const nlohmann::json&, Config&)` (ADL hooks for nlohmann).
  - `bool rtlangle::parse_frequency(std::string_view text, double& out_hz)` — accepts `118100000`, `118.1M`, `121500k`, `118.35 MHz`.

`Config` fields, with defaults, exactly as in spec §7:

```cpp
struct Config {
    // Device
    int         device_index            = 0;
    double      center_hz               = 0.0;      // 0 means "not set yet"
    std::uint32_t sample_rate_hz        = 1024000;
    std::uint32_t channel_rate_hz       = 32000;
    int         requested_gain_tenth_db = -1;       // -1 means "use the maximum"
    bool        agc                     = false;
    bool        allow_agc               = false;
    int         ppm                     = 0;
    double      offset_tune_hz          = 250000.0;
    double      channel_bw_hz           = 8000.0;
    bool        bias_tee                = false;
    // Experiment
    double      duration_s              = 60.0;
    double      settle_s                = 3.0;
    AngleSpec   angles{};
    int         rounds                  = 1;
    std::optional<VisitOrder> order     = std::nullopt;   // nullopt -> derived
    std::uint32_t seed                  = 0;              // filled in at session start
    RankMetric  rank_metric             = RankMetric::Both;
    // Detection and metrics
    double      noise_percentile        = 20.0;
    double      open_db                 = 6.0;
    double      close_db                = 3.0;
    double      min_event_ms            = 300.0;
    double      merge_gap_ms            = 200.0;
    int         min_valid_events        = 3;
    double      max_active_fraction     = 0.70;
    double      noise_drift_warn_db     = 3.0;
    double      max_angle_deviation_deg = 30.0;
    // I/O
    std::string session_dir             = "sessions";
    std::string label;
    std::string source                  = "rtlsdr";   // "rtlsdr" | "synthetic" | "file:<path>"
    double      synthetic_snr_db        = 12.0;
    bool        save_iq                 = false;
    bool        no_color                = false;
    bool        non_interactive         = false;
};
```

- [ ] **Step 1: Write the failing test**

`tests/test_config.cpp`:

```cpp
#include <doctest/doctest.h>
#include "core/config.h"

#include <nlohmann/json.hpp>
#include <string>

using namespace rtlangle;

static bool has_error_mentioning(const ValidationResult& r, const std::string& needle) {
    for (const auto& e : r.errors) if (e.find(needle) != std::string::npos) return true;
    return false;
}

static Config valid_config() {
    Config c;
    c.center_hz = 118100000.0;
    return c;
}

TEST_CASE("the default configuration with a frequency is valid") {
    CHECK(validate(valid_config()).ok());
}

TEST_CASE("frequency parsing accepts the suffixes a user would type") {
    double hz = 0.0;
    CHECK(parse_frequency("118100000", hz));   CHECK(hz == doctest::Approx(118100000.0));
    CHECK(parse_frequency("118.1M", hz));      CHECK(hz == doctest::Approx(118100000.0));
    CHECK(parse_frequency("121500k", hz));     CHECK(hz == doctest::Approx(121500000.0));
    CHECK(parse_frequency("118.35 MHz", hz));  CHECK(hz == doctest::Approx(118350000.0));
    CHECK_FALSE(parse_frequency("banana", hz));
    CHECK_FALSE(parse_frequency("", hz));
}

TEST_CASE("sample rate must be one the RTL-SDR actually supports") {
    Config c = valid_config();
    c.sample_rate_hz = 500000;   // in the driver's unsupported gap
    CHECK(has_error_mentioning(validate(c), "sample rate"));
}

TEST_CASE("sample rate must be an integer multiple of the channel rate") {
    Config c = valid_config();
    c.sample_rate_hz = 1200000;  // 1200000 / 32000 = 37.5
    CHECK(has_error_mentioning(validate(c), "multiple"));
}

TEST_CASE("the offset tune must leave the channel inside the captured bandwidth") {
    Config c = valid_config();
    c.offset_tune_hz = 500000;   // beyond 0.9 * 1024000/2
    CHECK(has_error_mentioning(validate(c), "offset"));
}

TEST_CASE("disabling offset tuning warns about the DC spur instead of failing") {
    Config c = valid_config();
    c.offset_tune_hz = 0.0;
    ValidationResult r = validate(c);
    CHECK(r.ok());
    REQUIRE_FALSE(r.warnings.empty());
    bool mentions_dc = false;
    for (const auto& w : r.warnings) if (w.find("DC") != std::string::npos) mentions_dc = true;
    CHECK(mentions_dc);
}

TEST_CASE("AGC is refused without the explicit opt-in because it invalidates comparisons") {
    Config c = valid_config();
    c.agc = true;
    CHECK(has_error_mentioning(validate(c), "--allow-agc"));
    c.allow_agc = true;
    ValidationResult r = validate(c);
    CHECK(r.ok());
    CHECK_FALSE(r.warnings.empty());   // still warns loudly
}

TEST_CASE("squelch thresholds must form a real hysteresis") {
    Config c = valid_config();
    c.open_db = 3.0; c.close_db = 6.0;
    CHECK(has_error_mentioning(validate(c), "open"));
}

TEST_CASE("noise percentile must sit in the quiet half of the distribution") {
    Config c = valid_config();
    c.noise_percentile = 80.0;
    CHECK(has_error_mentioning(validate(c), "percentile"));
}

TEST_CASE("non-interactive mode cannot drive a manual angle provider") {
    Config c = valid_config();
    c.non_interactive = true;
    CHECK(has_error_mentioning(validate(c), "non-interactive"));
    c.source = "synthetic";
    CHECK(validate(c).ok());
}

TEST_CASE("effective order defaults to alternating only when there is more than one round") {
    Config c = valid_config();
    c.rounds = 1;  CHECK(effective_order(c) == VisitOrder::Forward);
    c.rounds = 3;  CHECK(effective_order(c) == VisitOrder::Alternating);
    c.order = VisitOrder::Reverse;
    CHECK(effective_order(c) == VisitOrder::Reverse);
}

TEST_CASE("config survives a JSON round trip unchanged") {
    Config a = valid_config();
    a.label = "balcony";
    a.rounds = 4;
    a.order = VisitOrder::Random;
    a.seed = 987654321u;
    a.rank_metric = RankMetric::Audio;
    a.angles.explicit_angles = {0.0, 45.0, 90.0};
    a.requested_gain_tenth_db = 402;

    nlohmann::json j = a;
    Config b = j.get<Config>();

    CHECK(b.label == a.label);
    CHECK(b.rounds == a.rounds);
    CHECK(b.order.value() == VisitOrder::Random);
    CHECK(b.seed == a.seed);
    CHECK(b.rank_metric == RankMetric::Audio);
    CHECK(b.angles.explicit_angles == a.angles.explicit_angles);
    CHECK(b.requested_gain_tenth_db == a.requested_gain_tenth_db);
    CHECK(b.center_hz == doctest::Approx(a.center_hz));
    CHECK(nlohmann::json(b) == j);   // lossless in both directions
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build`
Expected: FAIL — `core/config.h: No such file or directory`.

- [ ] **Step 3: Write the implementation**

Write `src/core/config.h` with the struct above plus the declared free functions, and `src/core/config.cpp`. The validation body, which is the substance of the task:

```cpp
ValidationResult validate(const Config& c) {
    ValidationResult r;
    auto err  = [&r](std::string m) { r.errors.push_back(std::move(m)); };
    auto warn = [&r](std::string m) { r.warnings.push_back(std::move(m)); };

    if (c.center_hz <= 0.0)
        err("no centre frequency set; pass --freq (for example --freq 118.1M)");
    else if (c.center_hz < 24e6 || c.center_hz > 1766e6)
        warn("centre frequency is outside the R820T/R828D tuning range of roughly "
             "24 MHz to 1.766 GHz");
    else if (c.center_hz < 118e6 || c.center_hz > 137e6)
        warn("centre frequency is outside the 118-137 MHz AM airband; the AM "
             "demodulator assumes airband channel spacing");

    const std::uint32_t sr = c.sample_rate_hz;
    const bool sr_supported = (sr >= 225001 && sr <= 300000) || (sr >= 900001 && sr <= 3200000);
    if (!sr_supported)
        err("sample rate " + std::to_string(sr) + " Hz is not supported by librtlsdr; "
            "use 225001-300000 or 900001-3200000 Hz (the default is 1024000)");

    if (c.channel_rate_hz == 0) {
        err("channel rate must be greater than 0");
    } else if (sr % c.channel_rate_hz != 0) {
        err("sample rate " + std::to_string(sr) + " must be an integer multiple of the "
            "channel rate " + std::to_string(c.channel_rate_hz) +
            "; try --sample-rate 1024000");
    } else if (sr / c.channel_rate_hz < 2) {
        err("sample rate must be at least twice the channel rate");
    }

    if (c.channel_bw_hz <= 0.0 || c.channel_bw_hz >= 0.9 * c.channel_rate_hz)
        err("channel bandwidth must be greater than 0 and below 90% of the channel rate");

    const double usable = 0.9 * static_cast<double>(sr) / 2.0;
    if (std::fabs(c.offset_tune_hz) + c.channel_bw_hz / 2.0 >= usable)
        err("offset tune of " + std::to_string(c.offset_tune_hz) +
            " Hz pushes the channel outside the usable bandwidth; keep "
            "|offset| + channel_bw/2 below " + std::to_string(usable) + " Hz");
    if (c.offset_tune_hz == 0.0)
        warn("offset tuning is disabled; the RTL-SDR DC spur sits exactly on the "
             "tuned centre frequency and will contaminate both the signal power "
             "and the noise floor estimate");

    if (c.agc && !c.allow_agc)
        err("automatic gain control changes gain between angles and invalidates the "
            "comparison; pass --allow-agc as well if you really want it");
    if (c.agc && c.allow_agc)
        warn("automatic gain control is enabled; angle-to-angle comparisons from this "
             "session are not valid measurements of antenna orientation");
    if (c.bias_tee)
        warn("bias tee enabled: DC power will be fed onto the coax centre conductor. "
             "Do not enable this with a passive antenna connected");

    if (!(c.duration_s > 0.0)) err("capture duration must be greater than 0 seconds");
    if (c.duration_s < 10.0)
        warn("capture durations below 10 seconds rarely catch enough airband traffic "
             "to reach the minimum valid event count");
    if (c.settle_s < 0.0)  err("settling delay cannot be negative");
    if (c.rounds < 1)      err("rounds must be at least 1");
    if (c.rounds == 1)
        warn("a single round visits each angle once in sequence, which confounds angle "
             "with time; use --rounds 2 or more for a comparable result");

    if (!(c.noise_percentile > 0.0 && c.noise_percentile <= 50.0))
        err("noise percentile must be greater than 0 and at most 50, so that it lands "
            "in the quiet part of the capture");
    if (!(c.open_db > c.close_db))
        err("squelch open threshold must be above the close threshold to form a "
            "hysteresis (defaults are open 6 dB, close 3 dB)");
    if (!(c.close_db > 0.0)) err("squelch close threshold must be above the noise floor");
    if (!(c.min_event_ms > 0.0)) err("minimum event duration must be greater than 0 ms");
    if (c.merge_gap_ms < 0.0)    err("merge gap cannot be negative");
    if (c.min_valid_events < 1)  err("minimum valid events must be at least 1");
    if (!(c.max_active_fraction > 0.0 && c.max_active_fraction < 1.0))
        err("maximum active fraction must be strictly between 0 and 1");
    if (!(c.max_angle_deviation_deg >= 0.0)) err("maximum angle deviation cannot be negative");

    if (c.non_interactive && c.source == "rtlsdr")
        err("non-interactive mode cannot prompt for manual antenna positioning; it is "
            "only valid with --source synthetic or --source file:<path>");

    AngleResult angles = generate_angles(c.angles);
    if (!angles.ok()) err(angles.error);
    for (auto& w : angles.warnings) warn(std::move(w));

    return r;
}
```

`effective_order`:

```cpp
VisitOrder effective_order(const Config& c) {
    if (c.order.has_value()) return *c.order;
    return c.rounds > 1 ? VisitOrder::Alternating : VisitOrder::Forward;
}
```

`parse_frequency` accepts an optional decimal number followed by optional whitespace and an optional `k`/`K`/`M`/`G` suffix, optionally followed by `Hz`/`hz`. Reject any trailing characters that do not match, and reject values that are not finite or not positive.

For JSON, define `to_json`/`from_json` for `VisitOrder` (as its string spelling), `RankMetric` (string), `AngleSpec` (object with `start_deg`, `end_deg`, `step_deg`, `explicit_angles`), and `Config` (one key per field, using the field name verbatim). `from_json` must use `j.value(key, default)` so that a `session.json` written by an older build still loads.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/core/config.h src/core/config.cpp tests/test_config.cpp CMakeLists.txt
git commit -m "feat: add configuration aggregate, validation and JSON round-trip"
```

---

### Task 5: DSP chain — filter design, offset mixer, decimator, channel filter

**Files:**
- Create: `src/dsp/fir_design.h|.cpp`, `src/dsp/offset_mixer.h|.cpp`, `src/dsp/fir_decimator.h|.cpp`, `src/dsp/channel_filter.h|.cpp`, `src/dsp/chain.h|.cpp`
- Create: `tests/support/signals.h`, `tests/support/signals.cpp`
- Test: `tests/test_dsp_chain.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `Config` (Task 4).
- Produces:
  - `std::vector<float> rtlangle::dsp::design_lowpass(double cutoff_hz, double sample_rate_hz, double transition_hz, double stopband_db)` — Kaiser window, odd length, unity DC gain.
  - `class rtlangle::dsp::OffsetMixer` — `explicit OffsetMixer(double shift_hz, std::uint32_t sample_rate_hz)`, `void process(std::span<std::complex<float>> inout)` (in place), `void reset()`.
  - `class rtlangle::dsp::FirDecimator` — `FirDecimator(std::vector<float> taps, std::size_t decimation)`, `void process(std::span<const std::complex<float>> in, std::vector<std::complex<float>>& out_append)`, `void reset()`.
  - `class rtlangle::dsp::ChannelFilter` — `ChannelFilter(std::vector<float> taps)`, same `process` shape with decimation 1.
  - `struct rtlangle::dsp::Chain { OffsetMixer mixer; FirDecimator stage1; FirDecimator stage2; ChannelFilter channel; std::uint32_t channel_rate_hz; void process(std::span<const std::complex<float>> raw, std::vector<std::complex<float>>& channel_out); void reset(); };`
  - `Chain rtlangle::dsp::build_chain(const Config&)`
  - `std::pair<std::size_t, std::size_t> rtlangle::dsp::split_decimation(std::size_t total)` — returns `{stage1, stage2}` with `stage1*stage2 == total`.
  - Test support: `rtlangle::test::make_am_burst_signal(...)` and `rtlangle::test::add_complex_awgn(...)` — signatures given in Step 3.

- [ ] **Step 1: Write the failing test**

`tests/test_dsp_chain.cpp`:

```cpp
#include <doctest/doctest.h>
#include "dsp/chain.h"
#include "dsp/fir_design.h"
#include "core/db.h"
#include "support/signals.h"

#include <cmath>
#include <complex>
#include <numeric>
#include <span>
#include <vector>

using namespace rtlangle;
using namespace rtlangle::dsp;

static double mean_power(std::span<const std::complex<float>> x) {
    double acc = 0.0;
    for (auto v : x) acc += static_cast<double>(std::norm(v));
    return x.empty() ? 0.0 : acc / static_cast<double>(x.size());
}

/// A unit-amplitude complex tone at `freq_hz`.
static std::vector<std::complex<float>> tone(double freq_hz, double fs, std::size_t n) {
    std::vector<std::complex<float>> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double ph = 2.0 * M_PI * freq_hz * static_cast<double>(i) / fs;
        out[i] = std::complex<float>(static_cast<float>(std::cos(ph)),
                                     static_cast<float>(std::sin(ph)));
    }
    return out;
}

TEST_CASE("design_lowpass produces unity DC gain and the requested stopband") {
    auto h = design_lowpass(4000.0, 32000.0, 1000.0, 60.0);
    CHECK(h.size() % 2 == 1);                                  // odd -> linear phase, integer delay
    CHECK(std::accumulate(h.begin(), h.end(), 0.0f) == doctest::Approx(1.0f).epsilon(1e-4));

    auto gain_at = [&h](double f, double fs) {
        std::complex<double> acc{0.0, 0.0};
        for (std::size_t n = 0; n < h.size(); ++n)
            acc += static_cast<double>(h[n]) *
                   std::exp(std::complex<double>(0.0, -2.0 * M_PI * f * static_cast<double>(n) / fs));
        return std::abs(acc);
    };
    CHECK(to_db(std::pow(gain_at(1000.0, 32000.0), 2)) > -0.5);   // passband
    CHECK(to_db(std::pow(gain_at(9000.0, 32000.0), 2)) < -55.0);  // stopband
}

TEST_CASE("OffsetMixer shifts a tone to DC and does not drift over a long capture") {
    const double fs = 256000.0;
    const double offset = 60000.0;
    const std::size_t n = 256000 * 6;                    // six seconds

    // Hardware tunes above the signal, so the signal lands at -offset.
    auto x = tone(-offset, fs, n);
    OffsetMixer mixer(offset, 256000);
    mixer.process(x);

    // After mixing it must be a constant at DC. Compare the first and last
    // blocks: any phase drift shows up as a rotation between them.
    const auto first = std::accumulate(x.begin(), x.begin() + 1024, std::complex<float>{},
                                       std::plus<std::complex<float>>{}) / 1024.0f;
    const auto last = std::accumulate(x.end() - 1024, x.end(), std::complex<float>{},
                                      std::plus<std::complex<float>>{}) / 1024.0f;
    CHECK(std::abs(first) == doctest::Approx(1.0).epsilon(0.01));
    CHECK(std::abs(last)  == doctest::Approx(1.0).epsilon(0.01));
    CHECK(std::abs(std::arg(last / first)) < 0.01);      // radians, over six seconds
}

TEST_CASE("split_decimation factors the ratio into two stages") {
    CHECK(split_decimation(32) == std::pair<std::size_t, std::size_t>{8, 4});
    CHECK(split_decimation(8)  == std::pair<std::size_t, std::size_t>{2, 4});
    CHECK(split_decimation(6)  == std::pair<std::size_t, std::size_t>{2, 3});
    CHECK(split_decimation(2)  == std::pair<std::size_t, std::size_t>{1, 2});
    for (std::size_t total : {2u, 4u, 6u, 8u, 16u, 32u, 64u}) {
        auto [a, b] = split_decimation(total);
        CHECK(a * b == total);
    }
}

TEST_CASE("FirDecimator emits one output per decimation factor and is chunk invariant") {
    auto taps = design_lowpass(3000.0, 32000.0, 1000.0, 60.0);
    auto x = tone(500.0, 32000.0, 32000);

    FirDecimator whole(taps, 4);
    std::vector<std::complex<float>> out_whole;
    whole.process(x, out_whole);
    CHECK(out_whole.size() == 8000);

    // Feeding the same input in ragged chunks must give an identical result.
    FirDecimator chunked(taps, 4);
    std::vector<std::complex<float>> out_chunked;
    for (std::size_t i = 0; i < x.size(); ) {
        const std::size_t take = std::min<std::size_t>(1 + (i % 997), x.size() - i);
        chunked.process(std::span(x).subspan(i, take), out_chunked);
        i += take;
    }
    REQUIRE(out_chunked.size() == out_whole.size());
    for (std::size_t i = 0; i < out_whole.size(); ++i)
        CHECK(std::abs(out_chunked[i] - out_whole[i]) < 1e-5f);
}

TEST_CASE("the full chain passes the channel and rejects an adjacent channel") {
    Config c; c.center_hz = 118.1e6; c.sample_rate_hz = 256000; c.channel_rate_hz = 32000;
    c.offset_tune_hz = 60000.0; c.channel_bw_hz = 8000.0;

    const std::size_t n = 256000 * 2;
    auto run = [&](double signal_offset_from_carrier_hz) {
        // Signal at (-offset + delta) in the raw stream: -offset is our channel centre.
        auto x = tone(-c.offset_tune_hz + signal_offset_from_carrier_hz, 256000.0, n);
        Chain chain = build_chain(c);
        std::vector<std::complex<float>> out;
        chain.process(x, out);
        REQUIRE(out.size() > 16000);
        // Skip the filter transient at the head.
        return mean_power(std::span(out).subspan(8000));
    };

    const double in_channel = run(1000.0);       // 1 kHz from the carrier: inside +/-4 kHz
    const double adjacent   = run(25000.0);      // the next airband channel up
    CHECK(to_db(in_channel) > -0.5);
    CHECK(to_db(adjacent / in_channel) < -55.0);
    CHECK(build_chain(c).channel_rate_hz == 32000);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build`
Expected: FAIL — `dsp/chain.h: No such file or directory`.

- [ ] **Step 3: Write the implementation**

`design_lowpass` — standard Kaiser window design:

```cpp
std::vector<float> design_lowpass(double cutoff_hz, double sample_rate_hz,
                                  double transition_hz, double stopband_db) {
    const double dw = 2.0 * M_PI * transition_hz / sample_rate_hz;   // transition in rad/sample
    const double A = stopband_db;
    double beta = 0.0;
    if (A > 50.0)      beta = 0.1102 * (A - 8.7);
    else if (A >= 21.0) beta = 0.5842 * std::pow(A - 21.0, 0.4) + 0.07886 * (A - 21.0);

    std::size_t n = static_cast<std::size_t>(std::ceil((A - 8.0) / (2.285 * dw))) + 1;
    if (n % 2 == 0) ++n;                       // odd length: linear phase, integer group delay
    n = std::max<std::size_t>(n, 11);

    const double m = static_cast<double>(n - 1) / 2.0;
    const double wc = 2.0 * M_PI * cutoff_hz / sample_rate_hz;
    const double i0_beta = bessel_i0(beta);

    std::vector<float> h(n);
    double sum = 0.0;
    for (std::size_t k = 0; k < n; ++k) {
        const double t = static_cast<double>(k) - m;
        const double ideal = (std::fabs(t) < 1e-12) ? wc / M_PI
                                                    : std::sin(wc * t) / (M_PI * t);
        const double r = t / m;
        const double win = bessel_i0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0_beta;
        const double v = ideal * win;
        h[k] = static_cast<float>(v);
        sum += v;
    }
    for (auto& v : h) v = static_cast<float>(v / sum);   // exact unity DC gain
    return h;
}
```

`bessel_i0` is the standard series `sum_{k>=0} ((x/2)^k / k!)^2`, iterated until the term falls below `1e-12` times the running sum.

`OffsetMixer` — exact integer phase accumulator, so a 60-second capture cannot drift:

```cpp
class OffsetMixer {
public:
    OffsetMixer(double shift_hz, std::uint32_t sample_rate_hz);
    void process(std::span<std::complex<float>> inout);
    void reset() { acc_ = 0; }
private:
    static constexpr std::size_t kTable = 65536;
    std::uint64_t fs_ = 0;
    std::uint64_t step_ = 0;                 // (shift mod fs), always in [0, fs)
    std::uint64_t acc_ = 0;
    bool identity_ = false;                  // shift == 0
    std::shared_ptr<const std::vector<std::complex<float>>> table_;  // exp(+j2*pi*i/kTable)
};
```

`process` does, per sample: `idx = acc_ * kTable / fs_`, multiply by `(*table_)[idx]`, then `acc_ = (acc_ + step_) % fs_`. Because `acc_` is exact modulo arithmetic, phase error never accumulates; the only error is the 65536-entry table quantisation, roughly 96 dB below the signal. The table is built once per distinct table (share a `static` const table via a function-local `static`).

Negative shifts map to `step_ = ((int64)shift % fs + fs) % fs`. `shift_hz` is rounded to the nearest integer hertz; a non-integer offset is rounded and the rounding is recorded by the caller.

`FirDecimator`:

```cpp
class FirDecimator {
public:
    FirDecimator(std::vector<float> taps, std::size_t decimation);
    void process(std::span<const std::complex<float>> in,
                 std::vector<std::complex<float>>& out_append);
    void reset();
    std::size_t decimation() const { return decim_; }
private:
    std::vector<float> taps_;
    std::size_t decim_;
    std::vector<std::complex<float>> hist_;  // circular, size == taps_.size()
    std::size_t head_ = 0;                   // next write position
    std::size_t phase_ = 0;                  // counts inputs since the last output
};
```

`process` pushes each input into the circular history, increments `phase_`, and when `phase_ == decim_` resets `phase_` to 0 and appends one output computed as the dot product of `taps_` against the history in newest-to-oldest order. Computing only one output per `decim_` inputs is exactly the operation count a polyphase structure achieves. `hist_` is zero-initialised, which is what makes the head of the output a filter transient — tests skip it.

`ChannelFilter` is `FirDecimator` with `decimation == 1`; implement it as a thin wrapper so the type name documents intent at call sites.

`split_decimation`:

```cpp
std::pair<std::size_t, std::size_t> split_decimation(std::size_t total) {
    // The second stage takes the largest small factor available; the first
    // stage takes whatever is left. Both orders are valid, this one is fixed
    // so that split_decimation is deterministic and testable.
    std::size_t stage2 = 1;
    if (total % 4 == 0)      stage2 = 4;
    else if (total % 3 == 0) stage2 = 3;
    else if (total % 2 == 0) stage2 = 2;
    return {total / stage2, stage2};
}
```

`build_chain`:

```cpp
Chain build_chain(const Config& c) {
    const double fs = static_cast<double>(c.sample_rate_hz);
    const std::size_t total = c.sample_rate_hz / c.channel_rate_hz;
    const auto [d1, d2] = split_decimation(total);

    const double fs1_out = fs / static_cast<double>(d1);
    const double fs2_out = fs1_out / static_cast<double>(d2);

    // Anti-alias each stage at 40% of its own output rate with a 10% transition.
    auto taps1 = (d1 == 1) ? std::vector<float>{1.0f}
                           : design_lowpass(0.40 * fs1_out, fs,      0.10 * fs1_out, 60.0);
    auto taps2 = (d2 == 1) ? std::vector<float>{1.0f}
                           : design_lowpass(0.40 * fs2_out, fs1_out, 0.10 * fs2_out, 60.0);
    auto taps_ch = design_lowpass(c.channel_bw_hz / 2.0, fs2_out,
                                  std::min(1000.0, c.channel_bw_hz / 4.0), 60.0);

    return Chain{OffsetMixer(c.offset_tune_hz, c.sample_rate_hz),
                 FirDecimator(std::move(taps1), d1),
                 FirDecimator(std::move(taps2), d2),
                 ChannelFilter(std::move(taps_ch)),
                 c.channel_rate_hz};
}
```

`Chain::process` copies the input into a scratch buffer, mixes in place, runs stage 1 into a second scratch buffer, stage 2 into a third, then the channel filter appending into `channel_out`. Scratch buffers are members so repeated calls do not reallocate.

`tests/support/signals.h` — shared generators used by this and later tasks:

```cpp
#pragma once
#include <complex>
#include <cstdint>
#include <span>
#include <vector>

namespace rtlangle::test {

struct BurstSpec { double start_s; double duration_s; };

/// A complex AM signal at `carrier_offset_hz` relative to DC, tone-modulated at
/// `mod_hz` with depth `mod_depth`, present only during `bursts` and exactly
/// zero elsewhere. `amplitude` is the unmodulated carrier amplitude.
std::vector<std::complex<float>> make_am_burst_signal(double sample_rate_hz,
                                                      double total_s,
                                                      double carrier_offset_hz,
                                                      double mod_hz,
                                                      double mod_depth,
                                                      double amplitude,
                                                      std::span<const BurstSpec> bursts);

/// Adds circularly-symmetric complex Gaussian noise with total power
/// `noise_power` (that is, E|w|^2 == noise_power) using a fixed seed.
void add_complex_awgn(std::span<std::complex<float>> inout,
                      double noise_power, std::uint32_t seed);

/// Same noise as add_complex_awgn would add, returned on its own.
std::vector<std::complex<float>> make_complex_awgn(std::size_t n,
                                                   double noise_power,
                                                   std::uint32_t seed);
}  // namespace rtlangle::test
```

`add_complex_awgn` draws each of the real and imaginary parts from `std::normal_distribution<double>(0.0, std::sqrt(noise_power / 2.0))` with `std::mt19937(seed)`, so `E|w|^2 == noise_power` exactly.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/dsp tests/test_dsp_chain.cpp tests/support CMakeLists.txt
git commit -m "feat: add offset mixer, decimating FIR chain and Kaiser filter design"
```

---

### Task 6: Framing, AM demodulation and spectrum

**Files:**
- Create: `src/dsp/framer.h|.cpp`, `src/dsp/am_demodulator.h|.cpp`, `src/dsp/spectrum.h|.cpp`
- Test: `tests/test_dsp_analysis.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `design_lowpass` (Task 5).
- Produces:
  - `inline constexpr std::size_t rtlangle::dsp::kFrameLen = 1024;` and `kFrameHop = 512;`
  - `std::vector<double> rtlangle::dsp::frame_powers(std::span<const std::complex<float>> x, std::size_t frame_len = kFrameLen, std::size_t hop = kFrameHop)` — `frame_powers(x)[k]` is the mean of `|x|^2` over frame `k`.
  - `std::vector<double> rtlangle::dsp::frame_powers_real(std::span<const float> x, std::size_t frame_len = kFrameLen, std::size_t hop = kFrameHop)`
  - `std::size_t rtlangle::dsp::frame_index_to_sample(std::size_t frame, std::size_t hop = kFrameHop)` and `double rtlangle::dsp::frame_index_to_seconds(std::size_t frame, double channel_rate_hz, std::size_t hop = kFrameHop)`
  - `class rtlangle::dsp::AmDemodulator` — `explicit AmDemodulator(double channel_rate_hz)`, `void process(std::span<const std::complex<float>> in, std::vector<float>& audio_append)`, `void reset()`.
  - `double rtlangle::dsp::estimate_carrier_offset_hz(std::span<const std::complex<float>> x, double channel_rate_hz, double search_hz)` — FFT peak within `+/- search_hz` of DC, parabolically interpolated on the log magnitude. Returns 0 when the input is shorter than the FFT size.
  - `std::vector<double> rtlangle::dsp::welch_psd(std::span<const std::complex<float>> x, std::size_t fft_size)` — Hann-windowed, averaged, power per bin, FFT-shifted so index 0 is `-fs/2`.

The AM audio path is, in order: envelope `|x|`; a DC-blocking single-pole high-pass at about 25 Hz, which is what removes the carrier term; an FIR low-pass at 3400 Hz (transition 600 Hz, 60 dB); a single-pole high-pass at 300 Hz. That pair of high-passes plus the low-pass is the 300-3400 Hz speech band.

- [ ] **Step 1: Write the failing test**

`tests/test_dsp_analysis.cpp`:

```cpp
#include <doctest/doctest.h>
#include "dsp/framer.h"
#include "dsp/am_demodulator.h"
#include "dsp/spectrum.h"
#include "core/db.h"
#include "support/signals.h"

#include <cmath>
#include <span>
#include <vector>

using namespace rtlangle;
using namespace rtlangle::dsp;

TEST_CASE("frame_powers reports mean magnitude squared per frame") {
    std::vector<std::complex<float>> x(kFrameLen * 3, std::complex<float>(2.0f, 0.0f));
    auto p = frame_powers(x);
    // 3072 samples, 1024-long frames stepping by 512 -> frames start at 0,512,1024,1536,2048
    REQUIRE(p.size() == 5);
    for (double v : p) CHECK(v == doctest::Approx(4.0));
}

TEST_CASE("frame_powers drops the trailing partial frame") {
    std::vector<std::complex<float>> x(kFrameLen + kFrameHop + 7, std::complex<float>(1.0f, 0.0f));
    CHECK(frame_powers(x).size() == 2);
    CHECK(frame_powers(std::vector<std::complex<float>>(100)).empty());
}

TEST_CASE("frame index maps back to samples and seconds") {
    CHECK(frame_index_to_sample(3) == 1536);
    CHECK(frame_index_to_seconds(3, 32000.0) == doctest::Approx(0.048));
}

TEST_CASE("AM demodulator recovers the modulating tone and rejects the carrier") {
    const double fs = 32000.0;
    const double mod_hz = 1000.0, depth = 0.7, amp = 0.5;
    test::BurstSpec whole{0.0, 1.0};
    auto x = test::make_am_burst_signal(fs, 1.0, 0.0, mod_hz, depth, amp, std::span(&whole, 1));

    AmDemodulator demod(fs);
    std::vector<float> audio;
    demod.process(x, audio);
    REQUIRE(audio.size() == x.size());

    // Skip the filter transient, then measure. The carrier is DC and must be gone,
    // leaving only the tone of amplitude amp*depth -> power (amp*depth)^2 / 2.
    std::span<const float> tail(audio.data() + 8000, audio.size() - 8000);
    double dc = 0.0, ac = 0.0;
    for (float v : tail) dc += v;
    dc /= static_cast<double>(tail.size());
    for (float v : tail) ac += (v - dc) * (v - dc);
    ac /= static_cast<double>(tail.size());

    CHECK(std::fabs(dc) < 0.01 * amp);                                  // carrier removed
    CHECK(ac == doctest::Approx(std::pow(amp * depth, 2) / 2.0).epsilon(0.15));
}

TEST_CASE("AM demodulator attenuates content outside the speech band") {
    const double fs = 32000.0;
    test::BurstSpec whole{0.0, 1.0};
    auto in_band  = test::make_am_burst_signal(fs, 1.0, 0.0, 1000.0, 0.7, 0.5, std::span(&whole, 1));
    auto too_high = test::make_am_burst_signal(fs, 1.0, 0.0, 7000.0, 0.7, 0.5, std::span(&whole, 1));

    auto audio_power = [fs](const std::vector<std::complex<float>>& x) {
        AmDemodulator d(fs);
        std::vector<float> a;
        d.process(x, a);
        double acc = 0.0;
        for (std::size_t i = 8000; i < a.size(); ++i) acc += static_cast<double>(a[i]) * a[i];
        return acc / static_cast<double>(a.size() - 8000);
    };
    CHECK(to_db(audio_power(too_high) / audio_power(in_band)) < -30.0);
}

TEST_CASE("carrier offset estimation finds a small frequency error") {
    const double fs = 32000.0;
    test::BurstSpec whole{0.0, 0.5};
    for (double true_offset : {-800.0, -50.0, 0.0, 137.0, 900.0}) {
        auto x = test::make_am_burst_signal(fs, 0.5, true_offset, 1000.0, 0.5, 0.5,
                                            std::span(&whole, 1));
        CHECK(estimate_carrier_offset_hz(x, fs, 2000.0) == doctest::Approx(true_offset).epsilon(0.0)
              .scale(1.0));
        CHECK(std::fabs(estimate_carrier_offset_hz(x, fs, 2000.0) - true_offset) < 10.0);
    }
}

TEST_CASE("welch_psd puts a tone in the right bin") {
    const double fs = 32000.0;
    const std::size_t n = 1 << 15;
    std::vector<std::complex<float>> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double ph = 2.0 * M_PI * 4000.0 * static_cast<double>(i) / fs;
        x[i] = std::complex<float>(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
    }
    const std::size_t fft = 1024;
    auto psd = welch_psd(x, fft);
    REQUIRE(psd.size() == fft);
    const std::size_t peak = static_cast<std::size_t>(
        std::max_element(psd.begin(), psd.end()) - psd.begin());
    // Shifted layout: bin 0 is -fs/2, so +4000 Hz sits at fft/2 + 4000/(fs/fft).
    CHECK(peak == fft / 2 + 128);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build`
Expected: FAIL — `dsp/framer.h: No such file or directory`.

- [ ] **Step 3: Write the implementation**

`frame_powers`:

```cpp
std::vector<double> frame_powers(std::span<const std::complex<float>> x,
                                 std::size_t frame_len, std::size_t hop) {
    std::vector<double> out;
    if (x.size() < frame_len || frame_len == 0 || hop == 0) return out;
    out.reserve((x.size() - frame_len) / hop + 1);
    for (std::size_t start = 0; start + frame_len <= x.size(); start += hop) {
        double acc = 0.0;
        for (std::size_t i = 0; i < frame_len; ++i) acc += static_cast<double>(std::norm(x[start + i]));
        out.push_back(acc / static_cast<double>(frame_len));
    }
    return out;
}
```

`frame_powers_real` is identical with `v*v` instead of `std::norm`.

`AmDemodulator` holds three pieces of state: two single-pole high-pass filters and one `FirDecimator` with decimation 1 for the 3400 Hz low-pass. The single-pole high-pass is `y[n] = a*(y[n-1] + x[n] - x[n-1])` with `a = exp(-2*pi*fc/fs)`; use `fc = 25.0` for the DC block and `fc = 300.0` for the speech-band high-pass. `process` writes `|in[i]|` into a scratch buffer, runs the DC block, the low-pass, then the 300 Hz high-pass, appending exactly `in.size()` samples so the audio stream stays sample-aligned with the channel stream — the SNR estimator relies on that alignment to frame both identically.

`estimate_carrier_offset_hz` uses an FFTW `fftwf_plan_dft_1d` of size 4096 (or the largest power of two that fits when the input is shorter, minimum 256), a Hann window, and averages the magnitude spectra of up to 8 non-overlapping blocks. It searches the bins within `+/- search_hz` of DC for the maximum, then refines with parabolic interpolation on `log(magnitude)` of the three bins around the peak:

```cpp
const double y0 = std::log(mag[peak - 1]), y1 = std::log(mag[peak]), y2 = std::log(mag[peak + 1]);
const double delta = 0.5 * (y0 - y2) / (y0 - 2.0 * y1 + y2);   // in bins, within +/-0.5
const double bin_hz = channel_rate_hz / static_cast<double>(fft_size);
return (static_cast<double>(signed_bin) + delta) * bin_hz;
```

Guard the denominator against zero and clamp `delta` to `[-0.5, 0.5]`. FFTW plans are created once per `(fft_size)` and cached in a function-local `static` protected by a `std::mutex`, because FFTW's planner is not thread-safe.

`welch_psd` averages `|FFT|^2 / (fft_size * window_power)` over Hann-windowed 50%-overlapped blocks and returns the FFT-shifted result.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/dsp/framer.h src/dsp/framer.cpp src/dsp/am_demodulator.h src/dsp/am_demodulator.cpp src/dsp/spectrum.h src/dsp/spectrum.cpp tests/test_dsp_analysis.cpp CMakeLists.txt
git commit -m "feat: add framing, AM demodulation and FFT spectrum helpers"
```

---

### Task 7: Noise floor estimation and event detection

**Files:**
- Create: `src/metrics/noise_floor.h|.cpp`, `src/metrics/event_detector.h|.cpp`
- Test: `tests/test_event_detection.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `percentile` (Task 2), `Config` (Task 4), `kFrameLen`/`kFrameHop` (Task 6).
- Produces:
  - `struct rtlangle::metrics::NoiseFloor { double power = 0.0; double active_fraction = 0.0; bool reliable = true; };`
  - `NoiseFloor rtlangle::metrics::estimate_noise_floor(const std::vector<double>& frame_powers, double percentile_p, double open_db, double max_active_fraction)`
  - `struct rtlangle::metrics::FrameSpan { std::size_t first_frame; std::size_t last_frame; };` (inclusive)
  - `std::vector<FrameSpan> rtlangle::metrics::detect_events(const std::vector<double>& frame_powers, double noise_power, const EventDetectorParams&)`
  - `struct rtlangle::metrics::EventDetectorParams { double open_db = 6.0; double close_db = 3.0; double min_event_ms = 300.0; double merge_gap_ms = 200.0; double channel_rate_hz = 32000.0; std::size_t hop = dsp::kFrameHop; };`
  - `EventDetectorParams rtlangle::metrics::params_from(const Config&)`

The noise floor is the `percentile_p`-th percentile of **all** frame powers of the capture. `active_fraction` is the fraction of frames above `noise * from_db(open_db)`, computed against that same floor; when it exceeds `max_active_fraction` there are too few quiet frames for the percentile to mean anything and `reliable` is false.

Event detection is a hysteresis squelch: a candidate opens when power rises above `noise * from_db(open_db)` and closes when it falls below `noise * from_db(close_db)`. Candidates separated by a gap shorter than `merge_gap_ms` are merged, then candidates shorter than `min_event_ms` are discarded. Merging happens before the duration filter so that a transmission broken by a brief pause is not discarded as two short fragments.

Frame `k` covers samples `[k*hop, k*hop + frame_len)`. A span of frames `[a, b]` therefore has duration `((b - a) * hop + frame_len) / channel_rate_hz` seconds, and the gap between spans `[a1,b1]` and `[a2,b2]` is `(a2 - b1 - 1) * hop / channel_rate_hz`.

- [ ] **Step 1: Write the failing test**

`tests/test_event_detection.cpp`:

```cpp
#include <doctest/doctest.h>
#include "metrics/event_detector.h"
#include "metrics/noise_floor.h"
#include "core/db.h"

#include <vector>

using namespace rtlangle;
using namespace rtlangle::metrics;

/// Builds a frame-power sequence: `noise` everywhere, `noise * gain` inside each
/// [first, last] frame span given.
static std::vector<double> powers(std::size_t n, double noise, double gain,
                                  std::vector<std::pair<std::size_t, std::size_t>> bursts) {
    std::vector<double> p(n, noise);
    for (auto [a, b] : bursts)
        for (std::size_t i = a; i <= b && i < n; ++i) p[i] = noise * gain;
    return p;
}

static EventDetectorParams default_params() {
    EventDetectorParams q;               // open 6 dB, close 3 dB, min 300 ms, gap 200 ms
    q.channel_rate_hz = 32000.0;         // hop 512 -> 16 ms per frame step
    return q;
}

TEST_CASE("noise floor is the low percentile of all frame powers") {
    // 80% of frames at 1.0, 20% at 100.0. The 20th percentile must land at 1.0.
    std::vector<double> p(1000, 1.0);
    for (std::size_t i = 800; i < 1000; ++i) p[i] = 100.0;
    NoiseFloor nf = estimate_noise_floor(p, 20.0, 6.0, 0.70);
    CHECK(nf.power == doctest::Approx(1.0));
    CHECK(nf.active_fraction == doctest::Approx(0.20));
    CHECK(nf.reliable);
}

TEST_CASE("a capture with too few quiet frames is flagged unreliable") {
    std::vector<double> p(1000, 100.0);
    for (std::size_t i = 0; i < 100; ++i) p[i] = 1.0;      // only 10% quiet
    NoiseFloor nf = estimate_noise_floor(p, 20.0, 6.0, 0.70);
    CHECK(nf.active_fraction > 0.70);
    CHECK_FALSE(nf.reliable);
}

TEST_CASE("detect_events finds a single burst with the right boundaries") {
    // Frames 100..199 loud. Duration = (99*512 + 1024)/32000 = 1.616 s.
    auto p = powers(500, 1.0, 100.0, {{100, 199}});
    auto ev = detect_events(p, 1.0, default_params());
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].first_frame == 100);
    CHECK(ev[0].last_frame  == 199);
}

TEST_CASE("bursts shorter than the minimum duration are discarded") {
    // 10 frames = (9*512 + 1024)/32000 = 0.176 s, under the 300 ms minimum.
    auto p = powers(500, 1.0, 100.0, {{100, 109}});
    CHECK(detect_events(p, 1.0, default_params()).empty());
}

TEST_CASE("a gap shorter than merge_gap_ms joins two bursts into one") {
    // Gap of 5 frames = 5*512/32000 = 80 ms, under the 200 ms merge gap.
    auto p = powers(500, 1.0, 100.0, {{100, 149}, {155, 199}});
    auto ev = detect_events(p, 1.0, default_params());
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].first_frame == 100);
    CHECK(ev[0].last_frame  == 199);
}

TEST_CASE("a gap longer than merge_gap_ms keeps two separate events") {
    // Gap of 30 frames = 480 ms, above the 200 ms merge gap. Both halves are
    // long enough on their own (50 frames = 0.816 s).
    auto p = powers(500, 1.0, 100.0, {{100, 149}, {180, 229}});
    auto ev = detect_events(p, 1.0, default_params());
    REQUIRE(ev.size() == 2);
    CHECK(ev[0].first_frame == 100);
    CHECK(ev[1].first_frame == 180);
}

TEST_CASE("hysteresis stops a burst hovering between the thresholds from fragmenting") {
    // Power alternates between +8 dB (above open) and +4 dB (above close but
    // below open). With hysteresis this is one event; with a single threshold
    // at +6 dB it would shatter into 50 fragments, all too short to survive.
    std::vector<double> p(500, 1.0);
    for (std::size_t i = 100; i < 200; ++i) p[i] = from_db((i % 2 == 0) ? 8.0 : 4.0);
    auto ev = detect_events(p, 1.0, default_params());
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].first_frame == 100);
}

TEST_CASE("an event still open at the end of the capture is closed at the last frame") {
    auto p = powers(500, 1.0, 100.0, {{400, 499}});
    auto ev = detect_events(p, 1.0, default_params());
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].last_frame == 499);
}

TEST_CASE("a capture with no signal produces no events") {
    std::vector<double> p(500, 1.0);
    CHECK(detect_events(p, 1.0, default_params()).empty());
}

TEST_CASE("params_from maps the configuration onto the detector") {
    Config c; c.open_db = 9.0; c.close_db = 4.5; c.min_event_ms = 500.0;
    c.merge_gap_ms = 100.0; c.channel_rate_hz = 48000;
    EventDetectorParams q = params_from(c);
    CHECK(q.open_db == doctest::Approx(9.0));
    CHECK(q.close_db == doctest::Approx(4.5));
    CHECK(q.min_event_ms == doctest::Approx(500.0));
    CHECK(q.merge_gap_ms == doctest::Approx(100.0));
    CHECK(q.channel_rate_hz == doctest::Approx(48000.0));
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build`
Expected: FAIL — `metrics/event_detector.h: No such file or directory`.

- [ ] **Step 3: Write the implementation**

```cpp
NoiseFloor estimate_noise_floor(const std::vector<double>& p, double percentile_p,
                                double open_db, double max_active_fraction) {
    NoiseFloor nf;
    if (p.empty()) { nf.reliable = false; return nf; }
    nf.power = percentile(p, percentile_p);
    const double threshold = nf.power * from_db(open_db);
    std::size_t active = 0;
    for (double v : p) if (v > threshold) ++active;
    nf.active_fraction = static_cast<double>(active) / static_cast<double>(p.size());
    nf.reliable = nf.power > 0.0 && nf.active_fraction <= max_active_fraction;
    return nf;
}

std::vector<FrameSpan> detect_events(const std::vector<double>& p, double noise_power,
                                     const EventDetectorParams& q) {
    std::vector<FrameSpan> spans;
    if (p.empty() || !(noise_power > 0.0)) return spans;

    const double open_thr  = noise_power * from_db(q.open_db);
    const double close_thr = noise_power * from_db(q.close_db);

    bool in_event = false;
    std::size_t start = 0;
    for (std::size_t k = 0; k < p.size(); ++k) {
        if (!in_event && p[k] > open_thr) { in_event = true; start = k; }
        else if (in_event && p[k] < close_thr) { spans.push_back({start, k - 1}); in_event = false; }
    }
    if (in_event) spans.push_back({start, p.size() - 1});

    // Merge first, so a transmission split by a brief pause is not thrown away
    // as two fragments that are each too short.
    const double frames_per_second = q.channel_rate_hz / static_cast<double>(q.hop);
    const double merge_gap_frames = q.merge_gap_ms / 1000.0 * frames_per_second;
    std::vector<FrameSpan> merged;
    for (const auto& s : spans) {
        if (!merged.empty()) {
            const double gap = static_cast<double>(s.first_frame - merged.back().last_frame - 1);
            if (gap < merge_gap_frames) { merged.back().last_frame = s.last_frame; continue; }
        }
        merged.push_back(s);
    }

    const double min_event_s = q.min_event_ms / 1000.0;
    std::vector<FrameSpan> kept;
    for (const auto& s : merged) {
        const double duration_s =
            (static_cast<double>(s.last_frame - s.first_frame) * static_cast<double>(q.hop)
             + static_cast<double>(dsp::kFrameLen)) / q.channel_rate_hz;
        if (duration_s >= min_event_s) kept.push_back(s);
    }
    return kept;
}
```

Also provide `double rtlangle::metrics::span_duration_s(const FrameSpan&, const EventDetectorParams&)` and `double span_start_s(const FrameSpan&, const EventDetectorParams&)` using the same arithmetic, so Task 8 does not duplicate it.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/metrics tests/test_event_detection.cpp CMakeLists.txt
git commit -m "feat: add noise floor estimation and hysteresis event detection"
```

---

### Task 8: Records and the SNR estimator — the heart of the tool

This is the task the whole experiment depends on. If the estimator is wrong, every number the tool prints is wrong in a way that looks plausible. Write the tests first and do not weaken a tolerance to make one pass.

**Files:**
- Create: `src/core/records.h`, `src/core/records.cpp`
- Create: `src/metrics/snr_estimator.h`, `src/metrics/snr_estimator.cpp`
- Test: `tests/test_snr_estimator.cpp`
- Modify: `tests/support/signals.h|.cpp` (add `mean_power_over`), `CMakeLists.txt`

**Interfaces:**
- Consumes: everything from Tasks 2, 4, 5, 6, 7.
- Produces:
  - `enum class rtlangle::MeasurementStatus { Ok, InsufficientData, NoiseFloorUnreliable, Skipped };` with `to_string` / `parse_measurement_status`.
  - `struct rtlangle::SnrDistribution { bool valid = false; double median = 0, p10 = 0, p25 = 0, p75 = 0, p90 = 0, ci95_low = 0, ci95_high = 0; std::string ci_method = "none"; };` — `ci_method` is `"order-statistic"`, `"iqr"` (fewer than 6 events) or `"none"`.
  - `struct rtlangle::EventRecord { double start_s = 0, duration_s = 0; std::string utc; double carrier_offset_hz = 0, signal_power_dbfs = 0, channel_snr_db = 0, audio_snr_db = 0; bool audio_snr_valid = false; };`
  - `struct rtlangle::Measurement { int index = 0; int round = 1; double planned_deg = 0, actual_deg = 0; std::string started_utc; double duration_s = 0; MeasurementStatus status = MeasurementStatus::InsufficientData; std::string skip_reason; std::size_t frames_total = 0, frames_active = 0; double active_fraction = 0; double noise_floor_dbfs = kDbFloor, audio_noise_floor_dbfs = kDbFloor; std::uint64_t sample_overruns = 0; std::size_t valid_event_count = 0; SnrDistribution channel_snr_db, audio_snr_db; double signal_power_dbfs = kDbFloor; std::vector<EventRecord> events; std::string note; };` — spec §8.1 plus `audio_noise_floor_dbfs`, with `to_json`/`from_json`.
  - `struct rtlangle::AngleSummary` and `struct rtlangle::SessionSummary` — the aggregate shapes with `to_json`/`from_json`, declared here so `SessionStore` (Task 11) can serialise them; their fields are listed in Task 15, which populates them.
  - `class rtlangle::metrics::SnrEstimator` — `explicit SnrEstimator(const Config&)`, `Measurement analyze(std::span<const std::complex<float>> channel, std::span<const float> audio, std::chrono::system_clock::time_point capture_start) const`. Fills only the measurement's metric fields; the controller fills `index`, `round`, angles and `note`.

**Documented limitation this task must encode:** an event is only detected when its power exceeds the noise floor by `open_db`, so with the default 6 dB threshold the lowest measurable channel SNR is about 4.7 dB (`10*log10(from_db(6) - 1)`). Add `double rtlangle::metrics::minimum_detectable_snr_db(double open_db)` returning exactly that, use it in the README, and print it in the session header.

- [ ] **Step 1: Write the failing test**

`tests/test_snr_estimator.cpp`:

```cpp
#include <doctest/doctest.h>
#include "metrics/snr_estimator.h"
#include "dsp/chain.h"
#include "dsp/am_demodulator.h"
#include "core/db.h"
#include "support/signals.h"

#include <chrono>
#include <cmath>
#include <span>
#include <vector>

using namespace rtlangle;
using namespace rtlangle::metrics;

static const auto kEpoch = std::chrono::system_clock::time_point{};

/// A configuration whose squelch is low enough to detect the low end of the sweep.
static Config sweep_config() {
    Config c;
    c.center_hz = 118.1e6;
    c.sample_rate_hz = 256000;
    c.channel_rate_hz = 32000;
    c.offset_tune_hz = 60000.0;
    c.channel_bw_hz = 8000.0;
    // Defaults would refuse to detect anything below about 4.7 dB SNR, and the
    // sweep deliberately includes 0 dB. Lower the squelch for these tests only.
    c.open_db = 1.0;
    c.close_db = 0.5;
    c.min_valid_events = 3;
    return c;
}

static std::vector<float> demodulate(std::span<const std::complex<float>> channel, double rate) {
    dsp::AmDemodulator d(rate);
    std::vector<float> audio;
    d.process(channel, audio);
    return audio;
}

TEST_CASE("minimum detectable SNR follows from the squelch threshold") {
    CHECK(minimum_detectable_snr_db(6.0) == doctest::Approx(4.7).epsilon(0.02));
    CHECK(minimum_detectable_snr_db(3.0) == doctest::Approx(0.0).epsilon(0.05));
}

// ---------------------------------------------------------------------------
// Test A: the estimator in isolation, at the channel rate. Tolerance 0.5 dB.
// ---------------------------------------------------------------------------
TEST_CASE("Test A: estimator recovers a known SNR from channel-rate frames") {
    const Config c = sweep_config();
    const double fs = 32000.0;
    const std::size_t n = static_cast<std::size_t>(fs * 6.0);
    const std::vector<test::BurstSpec> bursts{{0.5, 0.8}, {2.0, 0.8}, {3.5, 0.8}, {5.0, 0.8}};

    for (double true_snr_db : {5.0, 10.0, 20.0, 30.0}) {
        const double noise_power = 1e-4;
        // AM with unit modulation index 0.7: mean power over the burst is
        // amp^2 * (1 + depth^2/2). Solve for amp to hit the requested SNR.
        const double depth = 0.7;
        const double want_signal_power = noise_power * from_db(true_snr_db);
        const double amp = std::sqrt(want_signal_power / (1.0 + depth * depth / 2.0));

        auto x = test::make_am_burst_signal(fs, 6.0, 0.0, 1000.0, depth, amp, bursts);
        test::add_complex_awgn(x, noise_power, 424242u);
        auto audio = demodulate(x, fs);

        SnrEstimator est(c);
        Measurement m = est.analyze(x, audio, kEpoch);

        CAPTURE(true_snr_db);
        CHECK(m.status == MeasurementStatus::Ok);
        CHECK(m.valid_event_count == 4);
        CHECK(m.channel_snr_db.median == doctest::Approx(true_snr_db).epsilon(0.0).scale(1.0));
        CHECK(std::fabs(m.channel_snr_db.median - true_snr_db) < 0.5);
        CHECK(m.noise_floor_dbfs == doctest::Approx(to_dbfs(noise_power)).epsilon(0.0).scale(1.0));
        CHECK(std::fabs(m.noise_floor_dbfs - to_dbfs(noise_power)) < 0.5);
    }
}

// ---------------------------------------------------------------------------
// Test B: the full DSP chain, with the reference SNR obtained by measurement.
// Tolerance 1.5 dB absolute, 1.0 dB on the differences between sweep points.
// ---------------------------------------------------------------------------
TEST_CASE("Test B: estimator recovers a known SNR through the whole DSP chain") {
    const Config c = sweep_config();
    const double fs = static_cast<double>(c.sample_rate_hz);
    const double total_s = 6.0;
    const double depth = 0.7;
    const std::vector<test::BurstSpec> bursts{{0.5, 0.7}, {2.0, 0.7}, {3.5, 0.7}, {5.0, 0.7}};
    // The signal sits at -offset in the raw stream; the mixer moves it to DC.
    const double raw_carrier_hz = -c.offset_tune_hz;

    std::vector<double> true_db, measured_db;

    for (double input_snr_db : {0.0, 5.0, 10.0, 20.0}) {
        const double noise_power = 1e-4;
        const double want_signal_power = noise_power * from_db(input_snr_db);
        const double amp = std::sqrt(want_signal_power / (1.0 + depth * depth / 2.0));

        auto signal_only = test::make_am_burst_signal(fs, total_s, raw_carrier_hz, 1000.0,
                                                      depth, amp, bursts);
        auto noise_only  = test::make_complex_awgn(signal_only.size(), noise_power, 99001u);

        std::vector<std::complex<float>> both(signal_only.size());
        for (std::size_t i = 0; i < both.size(); ++i) both[i] = signal_only[i] + noise_only[i];

        auto run_chain = [&c](std::span<const std::complex<float>> in) {
            dsp::Chain chain = dsp::build_chain(c);
            std::vector<std::complex<float>> out;
            chain.process(in, out);
            return out;
        };
        auto sig_out   = run_chain(signal_only);
        auto noise_out = run_chain(noise_only);
        auto both_out  = run_chain(both);

        // Reference: direct mean power on the separated components through the
        // identical chain. This measures the cascade's noise-equivalent
        // bandwidth instead of deriving it by hand, and it shares no code with
        // the estimator's framing / squelch / percentile / median path.
        const double margin_s = 0.05;   // stay clear of filter edge transients
        const double p_sig   = test::mean_power_over(sig_out, c.channel_rate_hz, bursts, margin_s);
        const double p_noise = test::mean_power_over(noise_out, c.channel_rate_hz,
                                                     {{0.2, total_s - 0.4}}, 0.0);
        const double snr_true_db = to_db(p_sig / p_noise);

        SnrEstimator est(c);
        Measurement m = est.analyze(both_out, demodulate(both_out, c.channel_rate_hz), kEpoch);

        CAPTURE(input_snr_db);
        CAPTURE(snr_true_db);
        CHECK(m.status == MeasurementStatus::Ok);
        CHECK(m.valid_event_count == 4);
        CHECK(std::fabs(m.channel_snr_db.median - snr_true_db) < 1.5);

        true_db.push_back(snr_true_db);
        measured_db.push_back(m.channel_snr_db.median);
    }

    // A constant bias would pass the absolute check above; this catches a scale
    // error by requiring the measured steps to track the true steps.
    for (std::size_t i = 1; i < true_db.size(); ++i) {
        const double d_true = true_db[i] - true_db[i - 1];
        const double d_meas = measured_db[i] - measured_db[i - 1];
        CAPTURE(i);
        CHECK(std::fabs(d_meas - d_true) < 1.0);
    }
}

// ---------------------------------------------------------------------------
// Test C: negative control.
// ---------------------------------------------------------------------------
TEST_CASE("Test C: pure noise yields no events and insufficient data, never a number") {
    const Config c = sweep_config();
    auto x = test::make_complex_awgn(static_cast<std::size_t>(32000 * 6), 1e-4, 7u);
    SnrEstimator est(c);
    Measurement m = est.analyze(x, demodulate(x, 32000.0), kEpoch);

    CHECK(m.status == MeasurementStatus::InsufficientData);
    CHECK(m.valid_event_count == 0);
    CHECK(m.events.empty());
    CHECK_FALSE(m.channel_snr_db.valid);
    CHECK_FALSE(m.audio_snr_db.valid);
}

// ---------------------------------------------------------------------------
// Test D: guards the subtraction. This test fails loudly if anyone ever
// "simplifies" (P - N)/N into P/N.
// ---------------------------------------------------------------------------
TEST_CASE("Test D: SNR is (P-N)/N, not P/N") {
    const Config c = sweep_config();
    const double fs = 32000.0;
    const double noise_power = 1e-4, depth = 0.7;
    const double amp = std::sqrt(noise_power / (1.0 + depth * depth / 2.0));   // exactly 0 dB
    const std::vector<test::BurstSpec> bursts{{0.5, 0.8}, {2.0, 0.8}, {3.5, 0.8}, {5.0, 0.8}};

    auto x = test::make_am_burst_signal(fs, 6.0, 0.0, 1000.0, depth, amp, bursts);
    test::add_complex_awgn(x, noise_power, 31337u);

    SnrEstimator est(c);
    Measurement m = est.analyze(x, demodulate(x, fs), kEpoch);

    REQUIRE(m.status == MeasurementStatus::Ok);
    CHECK(std::fabs(m.channel_snr_db.median - 0.0) < 1.0);
    // 10*log10(P/N) with P = 2N would report 3.01 dB. It must not.
    CHECK(std::fabs(m.channel_snr_db.median - 3.0103) > 0.5);
}

// ---------------------------------------------------------------------------
// Test E: audio SNR.
// ---------------------------------------------------------------------------
TEST_CASE("Test E: audio SNR tracks the channel SNR and is invalid without audio content") {
    const Config c = sweep_config();
    const double fs = 32000.0, noise_power = 1e-4, depth = 0.7;
    const std::vector<test::BurstSpec> bursts{{0.5, 0.8}, {2.0, 0.8}, {3.5, 0.8}, {5.0, 0.8}};

    SUBCASE("a modulated carrier produces a valid, monotonically improving audio SNR") {
        double previous = -1e9;
        for (double snr_db : {10.0, 20.0, 30.0}) {
            const double amp = std::sqrt(noise_power * from_db(snr_db) / (1.0 + depth * depth / 2.0));
            auto x = test::make_am_burst_signal(fs, 6.0, 0.0, 1000.0, depth, amp, bursts);
            test::add_complex_awgn(x, noise_power, 555u);
            SnrEstimator est(c);
            Measurement m = est.analyze(x, demodulate(x, fs), kEpoch);
            REQUIRE(m.status == MeasurementStatus::Ok);
            REQUIRE(m.audio_snr_db.valid);
            CAPTURE(snr_db);
            CHECK(m.audio_snr_db.median > previous);
            previous = m.audio_snr_db.median;
        }
    }

    SUBCASE("an unmodulated carrier is a valid channel event with no valid audio SNR") {
        // depth == 0: strong in-channel power, but the speech band holds only noise.
        const double amp = std::sqrt(noise_power * from_db(25.0));
        auto x = test::make_am_burst_signal(fs, 6.0, 0.0, 1000.0, 0.0, amp, bursts);
        test::add_complex_awgn(x, noise_power, 606u);
        SnrEstimator est(c);
        Measurement m = est.analyze(x, demodulate(x, fs), kEpoch);
        REQUIRE(m.status == MeasurementStatus::Ok);
        CHECK(m.channel_snr_db.valid);
        CHECK(m.channel_snr_db.median > 20.0);
        CHECK_FALSE(m.audio_snr_db.valid);
        for (const auto& e : m.events) CHECK_FALSE(e.audio_snr_valid);
    }
}

TEST_CASE("too few events is insufficient data even when each event is strong") {
    Config c = sweep_config();
    c.min_valid_events = 5;
    const double fs = 32000.0, noise_power = 1e-4, depth = 0.7;
    const double amp = std::sqrt(noise_power * from_db(25.0) / (1.0 + depth * depth / 2.0));
    const std::vector<test::BurstSpec> bursts{{0.5, 0.8}, {2.0, 0.8}, {3.5, 0.8}};

    auto x = test::make_am_burst_signal(fs, 6.0, 0.0, 1000.0, depth, amp, bursts);
    test::add_complex_awgn(x, noise_power, 808u);
    SnrEstimator est(c);
    Measurement m = est.analyze(x, demodulate(x, fs), kEpoch);

    CHECK(m.valid_event_count == 3);
    CHECK(m.status == MeasurementStatus::InsufficientData);
}

TEST_CASE("a capture with almost no quiet frames is flagged rather than measured") {
    Config c = sweep_config();
    const double fs = 32000.0, noise_power = 1e-4, depth = 0.7;
    const double amp = std::sqrt(noise_power * from_db(20.0) / (1.0 + depth * depth / 2.0));
    const std::vector<test::BurstSpec> bursts{{0.05, 5.9}};   // carrier up almost the whole time

    auto x = test::make_am_burst_signal(fs, 6.0, 0.0, 1000.0, depth, amp, bursts);
    test::add_complex_awgn(x, noise_power, 909u);
    SnrEstimator est(c);
    Measurement m = est.analyze(x, demodulate(x, fs), kEpoch);

    CHECK(m.active_fraction > c.max_active_fraction);
    CHECK(m.status == MeasurementStatus::NoiseFloorUnreliable);
}

TEST_CASE("distribution percentiles and the CI method follow the event count") {
    const Config c = sweep_config();
    const double fs = 32000.0, noise_power = 1e-4, depth = 0.7;
    const double amp = std::sqrt(noise_power * from_db(20.0) / (1.0 + depth * depth / 2.0));

    std::vector<test::BurstSpec> many;
    for (int i = 0; i < 10; ++i) many.push_back({0.4 + 1.1 * i, 0.6});
    auto x = test::make_am_burst_signal(fs, 12.0, 0.0, 1000.0, depth, amp, many);
    test::add_complex_awgn(x, noise_power, 111u);
    SnrEstimator est(c);
    Measurement m = est.analyze(x, demodulate(x, fs), kEpoch);

    REQUIRE(m.valid_event_count == 10);
    CHECK(m.channel_snr_db.ci_method == "order-statistic");
    CHECK(m.channel_snr_db.p10 <= m.channel_snr_db.p25);
    CHECK(m.channel_snr_db.p25 <= m.channel_snr_db.median);
    CHECK(m.channel_snr_db.median <= m.channel_snr_db.p75);
    CHECK(m.channel_snr_db.p75 <= m.channel_snr_db.p90);
    CHECK(m.channel_snr_db.ci95_low <= m.channel_snr_db.median);
    CHECK(m.channel_snr_db.ci95_high >= m.channel_snr_db.median);
}

TEST_CASE("measurement survives a JSON round trip") {
    Measurement a;
    a.index = 3; a.round = 2; a.planned_deg = 45.0; a.actual_deg = 47.5;
    a.started_utc = "2026-08-19T14:30:07Z"; a.duration_s = 60.0;
    a.status = MeasurementStatus::Ok; a.note = "note with a comma, a \"quote\" and a\nnewline";
    a.valid_event_count = 2;
    a.channel_snr_db.valid = true; a.channel_snr_db.median = 18.2;
    a.channel_snr_db.ci_method = "iqr";
    a.events.push_back(EventRecord{3.42, 2.15, "2026-08-19T14:30:10Z", -213.4,
                                   -44.0, 18.4, 14.9, true});
    nlohmann::json j = a;
    Measurement b = j.get<Measurement>();
    CHECK(nlohmann::json(b) == j);
    CHECK(b.note == a.note);
    CHECK(b.events.size() == 1);
    CHECK(b.events[0].carrier_offset_hz == doctest::Approx(-213.4));
}
```

Add to `tests/support/signals.h`:

```cpp
/// Mean of |x|^2 over the union of the given time windows, each shrunk by
/// `margin_s` at both ends so filter edge transients are excluded.
double mean_power_over(std::span<const std::complex<float>> x, double sample_rate_hz,
                       std::span<const BurstSpec> windows, double margin_s);
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build`
Expected: FAIL — `metrics/snr_estimator.h: No such file or directory`.

- [ ] **Step 3: Write the implementation**

The core of `SnrEstimator::analyze`:

```cpp
Measurement SnrEstimator::analyze(std::span<const std::complex<float>> channel,
                                  std::span<const float> audio,
                                  std::chrono::system_clock::time_point capture_start) const {
    Measurement m;
    const double rate = static_cast<double>(cfg_.channel_rate_hz);
    m.duration_s = channel.empty() ? 0.0
                                   : static_cast<double>(channel.size()) / rate;
    m.started_utc = format_utc(capture_start);

    const std::vector<double> p = dsp::frame_powers(channel);
    m.frames_total = p.size();
    if (p.empty()) { m.status = MeasurementStatus::InsufficientData; return m; }

    const NoiseFloor nf = estimate_noise_floor(p, cfg_.noise_percentile,
                                               cfg_.open_db, cfg_.max_active_fraction);
    m.noise_floor_dbfs = to_dbfs(nf.power);
    m.active_fraction = nf.active_fraction;
    m.frames_active = static_cast<std::size_t>(
        std::llround(nf.active_fraction * static_cast<double>(p.size())));

    const std::vector<double> a = dsp::frame_powers_real(audio);
    const double audio_noise = a.empty() ? 0.0 : percentile(a, cfg_.noise_percentile);
    m.audio_noise_floor_dbfs = to_dbfs(audio_noise);

    const EventDetectorParams q = params_from(cfg_);
    const std::vector<FrameSpan> spans = detect_events(p, nf.power, q);

    std::vector<double> channel_snrs, audio_snrs, event_powers;
    for (const FrameSpan& s : spans) {
        // Median frame power inside the event, not the mean: a single loud
        // frame from a nearby transient must not move the result.
        const double p_event = median(std::vector<double>(p.begin() + s.first_frame,
                                                          p.begin() + s.last_frame + 1));

        // (P - N) / N in the linear power domain, then convert to dB.
        // P / N would be SNR + 1 - the total-channel-power quantity that is
        // explicitly not SNR. Events at or below the floor are discarded
        // rather than reported as a negative or infinite number.
        if (!(p_event > nf.power)) continue;
        const double snr_lin = (p_event - nf.power) / nf.power;

        EventRecord e;
        e.start_s    = span_start_s(s, q);
        e.duration_s = span_duration_s(s, q);
        e.utc = format_utc(capture_start + std::chrono::milliseconds(
                               static_cast<long long>(e.start_s * 1000.0)));
        e.signal_power_dbfs = to_dbfs(p_event);
        e.channel_snr_db = to_db(snr_lin);

        if (!a.empty() && audio_noise > 0.0 && s.last_frame < a.size()) {
            const double s_a = median(std::vector<double>(a.begin() + s.first_frame,
                                                          a.begin() + s.last_frame + 1));
            if (s_a > audio_noise) {
                e.audio_snr_db = to_db((s_a - audio_noise) / audio_noise);
                e.audio_snr_valid = true;
                audio_snrs.push_back(e.audio_snr_db);
            }
        }

        const std::size_t first = dsp::frame_index_to_sample(s.first_frame);
        const std::size_t last  = std::min(channel.size(),
                                           dsp::frame_index_to_sample(s.last_frame) + dsp::kFrameLen);
        e.carrier_offset_hz = dsp::estimate_carrier_offset_hz(
            channel.subspan(first, last - first), rate, 2000.0);

        channel_snrs.push_back(e.channel_snr_db);
        event_powers.push_back(p_event);
        m.events.push_back(std::move(e));
    }

    m.valid_event_count = m.events.size();
    m.channel_snr_db = summarise(channel_snrs);
    m.audio_snr_db   = summarise(audio_snrs);
    m.signal_power_dbfs = event_powers.empty() ? kDbFloor : to_dbfs(median(event_powers));

    // Order matters: an unreliable noise floor makes every SNR above suspect,
    // so it outranks the event-count check.
    if (!nf.reliable)                                          m.status = MeasurementStatus::NoiseFloorUnreliable;
    else if (static_cast<int>(m.valid_event_count) < cfg_.min_valid_events)
                                                               m.status = MeasurementStatus::InsufficientData;
    else                                                       m.status = MeasurementStatus::Ok;
    return m;
}
```

`summarise` builds an `SnrDistribution`:

```cpp
SnrDistribution summarise(const std::vector<double>& v) {
    SnrDistribution d;
    if (v.empty()) return d;                      // valid stays false, ci_method "none"
    d.valid = true;
    d.median = median(v);
    d.p10 = percentile(v, 10.0); d.p25 = percentile(v, 25.0);
    d.p75 = percentile(v, 75.0); d.p90 = percentile(v, 90.0);
    if (auto ci = median_ci95(v)) {
        d.ci95_low = ci->low; d.ci95_high = ci->high; d.ci_method = "order-statistic";
    } else {
        d.ci95_low = d.p25; d.ci95_high = d.p75; d.ci_method = "iqr";
    }
    return d;
}

double minimum_detectable_snr_db(double open_db) { return to_db(from_db(open_db) - 1.0); }
```

`format_utc` renders a `system_clock::time_point` as `YYYY-MM-DDTHH:MM:SSZ` using `gmtime_r`.

Write `to_json`/`from_json` for `SnrDistribution`, `EventRecord`, `MeasurementStatus` (as its lower-case-with-underscores string) and `Measurement`, using `j.value(key, default)` on the reading side.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: PASS. If Test B fails by a constant offset, the bug is in the estimator or the chain — do not widen the tolerance. If it fails only at 0 dB, check that `open_db` really is 1.0 in `sweep_config`.

- [ ] **Step 5: Commit**

```bash
git add src/core/records.h src/core/records.cpp src/metrics/snr_estimator.h src/metrics/snr_estimator.cpp tests/test_snr_estimator.cpp tests/support CMakeLists.txt
git commit -m "feat: add measurement records and the linear-domain SNR estimator"
```

---

### Task 9: Sample sources — interface, synthetic and file replay

**Files:**
- Create: `src/source/sample_source.h`, `src/source/synthetic_source.h|.cpp`, `src/source/iq_file_source.h|.cpp`
- Test: `tests/test_sources.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `Config` (Task 4), test signal helpers (Task 5).
- Produces:
  - `struct rtlangle::SourceInfo { std::string driver, device_name, serial; std::uint32_t sample_rate_hz = 0; double tuned_center_hz = 0; int requested_gain_tenth_db = 0, applied_gain_tenth_db = 0; bool agc_enabled = false; int ppm = 0; std::uint64_t overruns = 0; };` with `to_json`/`from_json`.
  - `class rtlangle::ISampleSource { public: virtual ~ISampleSource() = default; virtual SourceInfo info() const = 0; virtual bool read(std::span<std::complex<float>> out, std::string& error) = 0; virtual void flush() = 0; };`
  - `class rtlangle::SyntheticSource : public ISampleSource` — `SyntheticSource(const Config&, double snr_db, std::uint32_t seed)`, plus `void set_snr_db(double)` so the end-to-end test can vary quality per angle. It generates at `Config::sample_rate_hz` with the carrier at `-offset_tune_hz`, AM at 1 kHz with depth 0.7, in bursts of 1.5 s separated by 3.5 s of silence, plus AWGN — a talk/silence pattern close to real airband traffic.
  - `class rtlangle::IqFileSource : public ISampleSource` — reads interleaved unsigned 8-bit IQ (`.cu8`), converting with `(x - 127.4f)/127.5f`; `bool open(const std::string& path, std::string& error)`; returns `false` from `read` at end of file with the error `"IQ file exhausted"`.

- [ ] **Step 1: Write the failing test**

`tests/test_sources.cpp`:

```cpp
#include <doctest/doctest.h>
#include "source/synthetic_source.h"
#include "source/iq_file_source.h"
#include "dsp/chain.h"
#include "metrics/snr_estimator.h"
#include "dsp/am_demodulator.h"
#include "core/db.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace rtlangle;

static Config synth_config() {
    Config c;
    c.center_hz = 118.1e6;
    c.sample_rate_hz = 256000; c.channel_rate_hz = 32000;
    c.offset_tune_hz = 60000.0; c.source = "synthetic";
    c.open_db = 1.0; c.close_db = 0.5;
    return c;
}

TEST_CASE("SyntheticSource reports itself honestly and never claims to be hardware") {
    SyntheticSource s(synth_config(), 15.0, 1u);
    CHECK(s.info().driver == "synthetic");
    CHECK(s.info().sample_rate_hz == 256000);
    CHECK_FALSE(s.info().agc_enabled);
}

TEST_CASE("SyntheticSource is chunk invariant and continuous across reads") {
    SyntheticSource a(synth_config(), 15.0, 5u), b(synth_config(), 15.0, 5u);
    std::vector<std::complex<float>> whole(100000), part1(40000), part2(60000);
    std::string err;
    REQUIRE(a.read(whole, err));
    REQUIRE(b.read(part1, err));
    REQUIRE(b.read(part2, err));
    for (std::size_t i = 0; i < part1.size(); ++i) CHECK(std::abs(whole[i] - part1[i]) < 1e-6f);
    for (std::size_t i = 0; i < part2.size(); ++i)
        CHECK(std::abs(whole[part1.size() + i] - part2[i]) < 1e-6f);
}

TEST_CASE("SyntheticSource produces a stream the estimator measures near the requested SNR") {
    Config c = synth_config();
    for (double want : {10.0, 20.0}) {
        SyntheticSource src(c, want, 42u);
        std::vector<std::complex<float>> raw(static_cast<std::size_t>(c.sample_rate_hz * 12));
        std::string err;
        REQUIRE(src.read(raw, err));

        dsp::Chain chain = dsp::build_chain(c);
        std::vector<std::complex<float>> ch;
        chain.process(raw, ch);
        dsp::AmDemodulator demod(c.channel_rate_hz);
        std::vector<float> audio;
        demod.process(ch, audio);

        metrics::SnrEstimator est(c);
        Measurement m = est.analyze(ch, audio, {});
        CAPTURE(want);
        CHECK(m.status == MeasurementStatus::Ok);
        CHECK(m.valid_event_count >= 2);
        CHECK(std::fabs(m.channel_snr_db.median - want) < 2.0);
    }
}

TEST_CASE("set_snr_db changes the measured quality in the expected direction") {
    Config c = synth_config();
    auto measure = [&c](double snr) {
        SyntheticSource src(c, snr, 9u);
        std::vector<std::complex<float>> raw(static_cast<std::size_t>(c.sample_rate_hz * 12));
        std::string err; src.read(raw, err);
        dsp::Chain chain = dsp::build_chain(c);
        std::vector<std::complex<float>> ch; chain.process(raw, ch);
        dsp::AmDemodulator d(c.channel_rate_hz); std::vector<float> a; d.process(ch, a);
        return metrics::SnrEstimator(c).analyze(ch, a, {}).channel_snr_db.median;
    };
    CHECK(measure(22.0) > measure(12.0) + 5.0);
}

TEST_CASE("IqFileSource round-trips unsigned 8-bit IQ and reports exhaustion") {
    const auto path = std::filesystem::temp_directory_path() / "rtlangle_test.cu8";
    {
        std::ofstream f(path, std::ios::binary);
        const unsigned char bytes[] = {127, 127, 255, 0, 0, 255};   // 3 complex samples
        f.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
    }
    IqFileSource src(synth_config());
    std::string err;
    REQUIRE(src.open(path.string(), err));

    std::vector<std::complex<float>> out(3);
    REQUIRE(src.read(out, err));
    CHECK(out[0].real() == doctest::Approx((127.0f - 127.4f) / 127.5f));
    CHECK(out[1].real() == doctest::Approx((255.0f - 127.4f) / 127.5f));
    CHECK(out[1].imag() == doctest::Approx((0.0f - 127.4f) / 127.5f));

    std::vector<std::complex<float>> more(3);
    CHECK_FALSE(src.read(more, err));
    CHECK(err.find("exhausted") != std::string::npos);
    std::filesystem::remove(path);
}

TEST_CASE("opening a missing IQ file reports the path rather than throwing") {
    IqFileSource src(synth_config());
    std::string err;
    CHECK_FALSE(src.open("/nonexistent/definitely-not-here.cu8", err));
    CHECK(err.find("definitely-not-here.cu8") != std::string::npos);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build` — Expected: FAIL, missing `source/synthetic_source.h`.

- [ ] **Step 3: Write the implementation**

`SyntheticSource` keeps a `std::uint64_t sample_counter_` so successive `read` calls continue the same waveform — that is what makes it chunk invariant. It computes, per sample, the elapsed seconds `t = counter / fs`, decides whether `t` falls inside a burst (`fmod(t, 5.0) < 1.5`), and emits `amp*(1 + 0.7*cos(2*pi*1000*t)) * exp(-j*2*pi*offset*t)` during bursts and zero otherwise, then adds AWGN drawn from a `std::mt19937` seeded once at construction. `amp` is derived from `snr_db` exactly as in the Task 8 tests, against a fixed noise power of `1e-4`. `flush()` is a no-op. `set_snr_db` recomputes `amp` only, leaving the counter and RNG untouched.

`IqFileSource` reads into a `std::vector<unsigned char>` staging buffer of `2 * out.size()` bytes; a short read means exhaustion.

- [ ] **Step 4: Run the tests to verify they pass** — `cmake --build build && ctest --test-dir build --output-on-failure`
- [ ] **Step 5: Commit**

```bash
git add src/source tests/test_sources.cpp CMakeLists.txt
git commit -m "feat: add sample source interface with synthetic and IQ file sources"
```

---

### Task 10: RTL-SDR hardware source

**Files:**
- Create: `src/source/rtlsdr_source.h|.cpp`
- Test: `tests/test_rtlsdr_hardware.cpp` (CTest label `hardware`)
- Modify: `CMakeLists.txt` (add `add_test(NAME hardware_smoke ...)` with `set_tests_properties(hardware_smoke PROPERTIES LABELS hardware)`)

**Interfaces:**
- Consumes: `ISampleSource`, `Config`.
- Produces:
  - `class rtlangle::RtlSdrSource : public ISampleSource` with `bool open(std::string& error)`, `void close()`, destructor closing cleanly.
  - `struct rtlangle::RtlSdrDeviceInfo { int index; std::string name, manufacturer, product, serial; };`
  - `std::vector<RtlSdrDeviceInfo> rtlangle::enumerate_rtlsdr_devices()`
  - `std::vector<int> rtlangle::supported_gains_tenth_db(int device_index, std::string& error)`
  - `int rtlangle::snap_gain(int requested_tenth_db, const std::vector<int>& supported)` — `requested < 0` means "use the maximum".
  - `std::unique_ptr<ISampleSource> rtlangle::make_source(const Config&, std::string& error)` — dispatches on `Config::source`; returns `RtlSdrSource` only when `RTLANGLE_WITH_RTLSDR` is defined, and otherwise an error naming the build option. Put it in `src/source/sample_source.cpp`.

Open sequence, in this exact order: `rtlsdr_open` → `rtlsdr_set_sample_rate` → `rtlsdr_set_center_freq(center + offset)` → `rtlsdr_set_freq_correction(ppm)` (skipped when `ppm == 0`, since librtlsdr returns an error for a no-op change) → `rtlsdr_set_tuner_gain_mode(dev, 1)` → `rtlsdr_set_tuner_gain(snapped)` → `rtlsdr_set_agc_mode(dev, agc ? 1 : 0)` → `rtlsdr_set_bias_tee(dev, bias_tee ? 1 : 0)` → `rtlsdr_reset_buffer`. Then read back `rtlsdr_get_tuner_gain` and `rtlsdr_get_center_freq` and store the **applied** values in `SourceInfo`.

Error mapping, each with the exact user-facing text:

| Condition | Message |
|---|---|
| `rtlsdr_open` returns `-6` (or `errno == EBUSY`) | `RTL-SDR device 0 is busy - close SDR++, gqrx, dump1090 or any other SDR software using it and try again.` |
| device count is 0 | `No RTL-SDR device found. Check the USB connection and that the DVB-T kernel driver is blacklisted (see README).` |
| index out of range | `RTL-SDR device index N does not exist; N devices were found: ...` (lists them) |
| any other non-zero return | `librtlsdr call <name> failed with code <rc>` |

Reading uses `rtlsdr_read_async` on a `std::thread` pushing into a lock-free-enough bounded ring buffer guarded by a mutex and condition variable; when the buffer is full the callback drops the block and increments `overruns_`, which surfaces in `SourceInfo::overruns` and lands in `Measurement::sample_overruns`. `flush()` empties the ring buffer so the settling delay's stale samples never enter a measurement. `close()` calls `rtlsdr_cancel_async`, joins the thread, then `rtlsdr_close`.

- [ ] **Step 1: Write the hardware test**

`tests/test_rtlsdr_hardware.cpp`:

```cpp
#include <doctest/doctest.h>
#include "source/rtlsdr_source.h"

#include <cmath>
#include <complex>
#include <vector>

using namespace rtlangle;

TEST_CASE("snap_gain picks the nearest supported entry and treats -1 as maximum") {
    const std::vector<int> gains{0, 9, 140, 297, 386, 496};
    CHECK(snap_gain(300, gains) == 297);
    CHECK(snap_gain(400, gains) == 386);
    CHECK(snap_gain(-1, gains) == 496);
    CHECK(snap_gain(10000, gains) == 496);
    CHECK(snap_gain(0, gains) == 0);
    CHECK(snap_gain(250, {}) == 250);        // no table: pass the request through
}

TEST_CASE("hardware receive smoke test" * doctest::skip(false)) {
    auto devices = enumerate_rtlsdr_devices();
    if (devices.empty()) {
        MESSAGE("No RTL-SDR device present - hardware validation is PENDING, not passed.");
        return;
    }
    MESSAGE("Found device: " << devices[0].name << " serial " << devices[0].serial);

    Config c;
    c.center_hz = 118.1e6;
    c.sample_rate_hz = 1024000;
    c.bias_tee = false;                      // receive only, never feed DC up the coax

    RtlSdrSource src(c);
    std::string err;
    if (!src.open(err)) {
        MESSAGE("Could not open the device: " << err);
        MESSAGE("Hardware validation is PENDING, not passed.");
        return;
    }

    CHECK(src.info().agc_enabled == false);
    CHECK(src.info().applied_gain_tenth_db > 0);
    CHECK(src.info().tuned_center_hz == doctest::Approx(118.1e6 + c.offset_tune_hz).epsilon(1e-5));

    std::vector<std::complex<float>> buf(1024000);   // one second
    REQUIRE_MESSAGE(src.read(buf, err), err);

    // A live receiver produces varying samples. A constant buffer means the
    // device returned zeros or the conversion is broken.
    double mean = 0.0, var = 0.0;
    for (auto v : buf) mean += std::norm(v);
    mean /= static_cast<double>(buf.size());
    for (auto v : buf) var += std::pow(std::norm(v) - mean, 2);
    var /= static_cast<double>(buf.size());
    CHECK(mean > 0.0);
    CHECK(var > 0.0);
    CHECK(mean < 1.0);                        // not railed against full scale
    src.close();
}
```

- [ ] **Step 2: Verify the build fails** — `cmake --build build`, Expected: missing `source/rtlsdr_source.h`.
- [ ] **Step 3: Write the implementation** as described above.
- [ ] **Step 4: Run the tests**

```bash
cmake --build build
ctest --test-dir build --output-on-failure -LE hardware    # must pass
ctest --test-dir build --output-on-failure -L hardware     # requires the device to be free
```

If the second command reports the device is busy, close SDR++ (`pkill sdrpp` or quit it from its window) and re-run. **If it still cannot run, record in the final report that hardware validation is pending — never describe an untested receive path as verified.**

- [ ] **Step 5: Commit**

```bash
git add src/source/rtlsdr_source.h src/source/rtlsdr_source.cpp tests/test_rtlsdr_hardware.cpp CMakeLists.txt
git commit -m "feat: add RTL-SDR sample source with gain snapping and named errors"
```

---

### Task 11: Persistence — atomic JSON, CSV, session store and resume

**Files:**
- Create: `src/persist/atomic_write.h|.cpp`, `src/persist/csv.h|.cpp`, `src/persist/session_store.h|.cpp`, `src/persist/session_loader.h|.cpp`
- Test: `tests/test_persistence.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `Config`, `Measurement`, `SourceInfo`.
- Produces:
  - `bool rtlangle::persist::write_atomic(const std::filesystem::path&, std::string_view data, std::string& error)`
  - `std::string rtlangle::persist::csv_escape(std::string_view)` — RFC 4180.
  - `struct rtlangle::SessionMetadata { int schema_version = 1; std::string session_id, started_utc, finished_utc, tool_version; Config config; SourceInfo source; std::string angle_provider_name; bool angle_provider_automated = false; std::vector<double> plan_angles; int rounds = 1; std::string order; std::uint32_t seed = 0; };`
  - (`AngleSummary` and `SessionSummary` are declared in `src/core/records.h` by Task 8 and populated by Task 15; `SessionStore::finalize` only serialises what it is handed.)
  - `class rtlangle::SessionStore` — `static std::unique_ptr<SessionStore> create(const Config&, SessionMetadata, std::string& error)` (makes the directory, writes the header and the CSV header), `static std::unique_ptr<SessionStore> reopen(const std::filesystem::path&, std::string& error)`, `bool append(const Measurement&, std::string& error)`, `bool finalize(const SessionSummary&, const std::string& report_text, std::string& error)`, `const SessionMetadata& metadata() const`, `const std::vector<Measurement>& measurements() const`, `std::filesystem::path directory() const`.
  - `struct rtlangle::CompletedKey { int round; double planned_deg; bool operator<(const CompletedKey&) const; }` and `std::set<CompletedKey> rtlangle::completed_keys(const std::vector<Measurement>&)` — a measurement counts as complete when its status is anything other than a retry, that is `Ok`, `InsufficientData`, `NoiseFloorUnreliable` or `Skipped`. Angles compare with a 1e-6 tolerance via rounding to 1e-6 before comparison.
  - `std::unique_ptr<SessionStore> rtlangle::SessionLoader::load(const std::filesystem::path&, std::string& error)` — thin wrapper over `reopen`, plus `bool rtlangle::reject_conflicting_overrides(const Config& stored, const Config& cli, std::string& error)`.

The directory name is `<session_dir>/<YYYYMMDD-HHMMSS>[-<label>]`. `append` pushes onto the in-memory vector, serialises the whole session, `write_atomic`s `session.json`, then appends one CSV row and flushes. `write_atomic` writes `<target>.tmp`, `fsync`s the file, `rename`s it over the target, then `fsync`s the containing directory.

- [ ] **Step 1: Write the failing test**

`tests/test_persistence.cpp`:

```cpp
#include <doctest/doctest.h>
#include "persist/session_store.h"
#include "persist/session_loader.h"
#include "persist/csv.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

using namespace rtlangle;
namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    TempDir() : path(fs::temp_directory_path() /
                     ("rtlangle_test_" + std::to_string(::getpid()) + "_" +
                      std::to_string(reinterpret_cast<std::uintptr_t>(this)))) {
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

static SessionMetadata meta_for(const Config& c) {
    SessionMetadata m;
    m.session_id = "20260819-143000-test";
    m.started_utc = "2026-08-19T14:30:00Z";
    m.tool_version = "rtlangle test";
    m.config = c;
    m.angle_provider_name = "manual";
    m.plan_angles = {0.0, 45.0, 90.0};
    m.rounds = 2;
    m.order = "alternating";
    return m;
}

static Measurement measurement_at(int index, int round, double angle, MeasurementStatus st) {
    Measurement m;
    m.index = index; m.round = round; m.planned_deg = angle; m.actual_deg = angle;
    m.started_utc = "2026-08-19T14:30:07Z"; m.duration_s = 60.0; m.status = st;
    m.valid_event_count = (st == MeasurementStatus::Ok) ? 5 : 0;
    m.channel_snr_db.valid = (st == MeasurementStatus::Ok);
    m.channel_snr_db.median = 15.0 + angle / 10.0;
    return m;
}

TEST_CASE("csv_escape follows RFC 4180") {
    CHECK(csv_escape("plain") == "plain");
    CHECK(csv_escape("has,comma") == "\"has,comma\"");
    CHECK(csv_escape("has\"quote") == "\"has\"\"quote\"");
    CHECK(csv_escape("has\nnewline") == "\"has\nnewline\"");
    CHECK(csv_escape("") == "");
}

TEST_CASE("session.json is valid after every single append and leaves no temp file") {
    TempDir tmp;
    Config c; c.center_hz = 118.1e6; c.session_dir = tmp.path.string();
    std::string err;
    auto store = SessionStore::create(c, meta_for(c), err);
    REQUIRE_MESSAGE(store != nullptr, err);

    for (int i = 0; i < 6; ++i) {
        REQUIRE(store->append(measurement_at(i, 1 + i / 3, 45.0 * (i % 3),
                                             MeasurementStatus::Ok), err));
        std::ifstream f(store->directory() / "session.json");
        REQUIRE(f.good());
        nlohmann::json j;
        REQUIRE_NOTHROW(f >> j);                                     // never truncated
        CHECK(j.at("measurements").size() == static_cast<std::size_t>(i + 1));
        CHECK_FALSE(fs::exists(store->directory() / "session.json.tmp"));
    }
}

TEST_CASE("the CSV gets one header and one row per measurement") {
    TempDir tmp;
    Config c; c.center_hz = 118.1e6; c.session_dir = tmp.path.string();
    std::string err;
    auto store = SessionStore::create(c, meta_for(c), err);
    REQUIRE(store != nullptr);
    for (int i = 0; i < 3; ++i)
        REQUIRE(store->append(measurement_at(i, 1, 45.0 * i, MeasurementStatus::Ok), err));

    std::ifstream f(store->directory() / "measurements.csv");
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(f, line)) lines.push_back(line);
    REQUIRE(lines.size() == 4);
    CHECK(lines[0].rfind("index,round,planned_deg,", 0) == 0);
}

TEST_CASE("notes containing commas, quotes and newlines survive both formats") {
    TempDir tmp;
    Config c; c.center_hz = 118.1e6; c.session_dir = tmp.path.string();
    std::string err;
    auto store = SessionStore::create(c, meta_for(c), err);
    REQUIRE(store != nullptr);

    Measurement m = measurement_at(0, 1, 0.0, MeasurementStatus::Ok);
    m.note = "cable, \"loose\"\nre-seated";
    REQUIRE(store->append(m, err));

    auto reloaded = SessionStore::reopen(store->directory(), err);
    REQUIRE_MESSAGE(reloaded != nullptr, err);
    REQUIRE(reloaded->measurements().size() == 1);
    CHECK(reloaded->measurements()[0].note == m.note);

    std::ifstream f(store->directory() / "measurements.csv");
    std::stringstream all; all << f.rdbuf();
    CHECK(all.str().find("\"cable, \"\"loose\"\"\nre-seated\"") != std::string::npos);
}

TEST_CASE("reopen restores the metadata and every measurement") {
    TempDir tmp;
    Config c; c.center_hz = 118.1e6; c.session_dir = tmp.path.string();
    c.rounds = 2; c.label = "balcony";
    std::string err;
    fs::path dir;
    {
        auto store = SessionStore::create(c, meta_for(c), err);
        REQUIRE(store != nullptr);
        dir = store->directory();
        REQUIRE(store->append(measurement_at(0, 1, 0.0, MeasurementStatus::Ok), err));
        REQUIRE(store->append(measurement_at(1, 1, 45.0, MeasurementStatus::InsufficientData), err));
    }
    auto reloaded = SessionStore::reopen(dir, err);
    REQUIRE_MESSAGE(reloaded != nullptr, err);
    CHECK(reloaded->metadata().config.rounds == 2);
    CHECK(reloaded->metadata().config.center_hz == doctest::Approx(118.1e6));
    CHECK(reloaded->metadata().plan_angles.size() == 3);
    REQUIRE(reloaded->measurements().size() == 2);
    CHECK(reloaded->measurements()[1].status == MeasurementStatus::InsufficientData);
}

TEST_CASE("appending to a reopened session extends rather than truncating") {
    TempDir tmp;
    Config c; c.center_hz = 118.1e6; c.session_dir = tmp.path.string();
    std::string err;
    fs::path dir;
    {
        auto store = SessionStore::create(c, meta_for(c), err);
        REQUIRE(store != nullptr);
        dir = store->directory();
        REQUIRE(store->append(measurement_at(0, 1, 0.0, MeasurementStatus::Ok), err));
    }
    {
        auto store = SessionStore::reopen(dir, err);
        REQUIRE(store != nullptr);
        REQUIRE(store->append(measurement_at(1, 1, 45.0, MeasurementStatus::Ok), err));
    }
    auto final_store = SessionStore::reopen(dir, err);
    REQUIRE(final_store != nullptr);
    CHECK(final_store->measurements().size() == 2);

    std::ifstream f(dir / "measurements.csv");
    std::string line; int rows = 0;
    while (std::getline(f, line)) ++rows;
    CHECK(rows == 3);                                     // header plus two rows
}

TEST_CASE("completed_keys treats every terminal status as done and ignores nothing else") {
    std::vector<Measurement> ms{
        measurement_at(0, 1,  0.0, MeasurementStatus::Ok),
        measurement_at(1, 1, 45.0, MeasurementStatus::InsufficientData),
        measurement_at(2, 1, 90.0, MeasurementStatus::Skipped),
        measurement_at(3, 2,  0.0, MeasurementStatus::NoiseFloorUnreliable),
    };
    auto keys = completed_keys(ms);
    CHECK(keys.size() == 4);
    CHECK(keys.count(CompletedKey{1, 45.0}) == 1);
    CHECK(keys.count(CompletedKey{2, 45.0}) == 0);        // round 2 at 45 is still pending
}

TEST_CASE("resume rejects CLI overrides that would change the experiment") {
    Config stored; stored.center_hz = 118.1e6; stored.requested_gain_tenth_db = 402;
    std::string err;

    Config same = stored;
    CHECK(reject_conflicting_overrides(stored, same, err));

    Config different_gain = stored; different_gain.requested_gain_tenth_db = 200;
    CHECK_FALSE(reject_conflicting_overrides(stored, different_gain, err));
    CHECK(err.find("gain") != std::string::npos);

    Config different_freq = stored; different_freq.center_hz = 121.5e6;
    CHECK_FALSE(reject_conflicting_overrides(stored, different_freq, err));
    CHECK(err.find("frequency") != std::string::npos);
}

TEST_CASE("reopening a directory without a session file reports the path") {
    TempDir tmp;
    std::string err;
    CHECK(SessionStore::reopen(tmp.path, err) == nullptr);
    CHECK(err.find(tmp.path.string()) != std::string::npos);
}
```

- [ ] **Step 2: Verify the build fails** — Expected: missing `persist/session_store.h`.
- [ ] **Step 3: Write the implementation.** `write_atomic`:

```cpp
bool write_atomic(const std::filesystem::path& target, std::string_view data, std::string& error) {
    const std::filesystem::path tmp = target.string() + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { error = "cannot create " + tmp.string() + ": " + std::strerror(errno); return false; }
    std::size_t written = 0;
    while (written < data.size()) {
        const ssize_t n = ::write(fd, data.data() + written, data.size() - written);
        if (n <= 0) { ::close(fd); ::unlink(tmp.c_str());
                      error = "write to " + tmp.string() + " failed: " + std::strerror(errno);
                      return false; }
        written += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) { ::close(fd); ::unlink(tmp.c_str());
                            error = "fsync failed: " + std::string(std::strerror(errno)); return false; }
    ::close(fd);
    std::error_code ec;
    std::filesystem::rename(tmp, target, ec);     // atomic within one directory
    if (ec) { std::filesystem::remove(tmp, ec);
              error = "rename to " + target.string() + " failed: " + ec.message(); return false; }
    const int dfd = ::open(target.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) { ::fsync(dfd); ::close(dfd); }  // make the rename itself durable
    return true;
}
```

`reject_conflicting_overrides` compares the fields that would invalidate the comparison — `center_hz`, `sample_rate_hz`, `channel_rate_hz`, `requested_gain_tenth_db`, `agc`, `ppm`, `offset_tune_hz`, `channel_bw_hz`, `duration_s`, the angle spec, `rounds`, `order`, and every detection threshold — and returns `false` with a message naming the first difference in plain words (`"frequency"`, `"gain"`, `"sample rate"`, ...).

- [ ] **Step 4: Run the tests** — `cmake --build build && ctest --test-dir build --output-on-failure -LE hardware`
- [ ] **Step 5: Commit**

```bash
git add src/persist tests/test_persistence.cpp CMakeLists.txt
git commit -m "feat: add atomic session persistence, CSV output and resume support"
```

---

### Task 12: Terminal interface — the interactive coloured menu

**Files:**
- Create: `src/ui/terminal_ui.h`, `src/ui/scripted_terminal_ui.h|.cpp`, `src/ui/ansi_terminal_ui.h|.cpp`, `src/ui/style.h|.cpp`
- Test: `tests/test_terminal_ui.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `struct rtlangle::ui::MenuItem { std::string label; std::string hint; bool enabled = true; };`
  - `struct rtlangle::ui::Table { std::vector<std::string> headers; std::vector<std::vector<std::string>> rows; std::vector<bool> right_align; };`
  - `class rtlangle::ui::ITerminalUi` exactly as in spec §3.6.
  - `class rtlangle::ui::ScriptedTerminalUi : public ITerminalUi` — `void push_menu_choice(int)`, `void push_line(std::string)`, `void push_cancel()` (queues a cancelled prompt, that is `std::nullopt`), `void push_confirm(bool)`, `const std::vector<std::string>& output() const`, `bool output_contains(std::string_view) const`, `bool exhausted() const`. Popping from an empty queue throws `std::runtime_error` so a test that under-scripts fails loudly rather than hanging.
  - `class rtlangle::ui::AnsiTerminalUi : public ITerminalUi` — `explicit AnsiTerminalUi(bool force_no_color)`.
  - `bool rtlangle::ui::color_enabled(bool force_no_color)` — false when `force_no_color`, when `NO_COLOR` is set in the environment, or when `isatty(STDOUT_FILENO)` is false.
  - `class rtlangle::ui::RawMode` — RAII; constructor saves `termios` and switches off `ICANON` and `ECHO` with `VMIN=1, VTIME=0`; destructor restores. A file-scope pointer to the saved `termios` is installed for the signal handler.
  - `void rtlangle::ui::install_terminal_signal_handlers()` — installs `SIGINT` and `SIGTERM` handlers that call `tcsetattr(STDIN_FILENO, TCSANOW, saved)`, `write(STDOUT_FILENO, "\x1b[?25h\n", 7)`, then re-raise the signal with the default disposition. Only async-signal-safe calls.

Menu rendering, matching the layout the user approved:

```
  Select an action

  ❯ ▸ Start new experiment
      Resume session
      Device check / gain table
      Replay synthetic session
      Quit

  ↑/↓ move   ⏎ select   q quit
```

The cursor line is bold and coloured; disabled items are dim and skipped by the cursor. Keys: `Up`/`k` previous, `Down`/`j` next, `Enter` select, `q` or `Esc` cancel (returns -1), `Home`/`End` first/last. Escape sequences arrive as `ESC [ A` / `ESC [ B`; a lone `ESC` is distinguished by switching to `VMIN=0, VTIME=1` for the follow-up read. When colour is disabled, `❯` becomes `>` and `▸` is dropped, so a piped log stays readable.

- [ ] **Step 1: Write the failing test**

`tests/test_terminal_ui.cpp`:

```cpp
#include <doctest/doctest.h>
#include "ui/scripted_terminal_ui.h"
#include "ui/style.h"

#include <array>
#include <cstdlib>
#include <unistd.h>

using namespace rtlangle::ui;

TEST_CASE("ScriptedTerminalUi returns queued answers in order") {
    ScriptedTerminalUi ui;
    ui.push_menu_choice(2);
    ui.push_line("47.5");
    ui.push_confirm(true);

    const std::array<MenuItem, 3> items{MenuItem{"a", "", true}, MenuItem{"b", "", true},
                                        MenuItem{"c", "", true}};
    CHECK(ui.menu("pick", items, 0) == 2);
    CHECK(ui.prompt_line("angle", "45").value() == "47.5");
    CHECK(ui.confirm("ready?", true) == true);
    CHECK(ui.exhausted());
}

TEST_CASE("ScriptedTerminalUi records everything printed so tests can assert on it") {
    ScriptedTerminalUi ui;
    ui.heading("Angle 45 degrees");
    ui.info("capturing");
    ui.warn("noise floor drifted");
    ui.error("device busy");
    CHECK(ui.output_contains("Angle 45 degrees"));
    CHECK(ui.output_contains("noise floor drifted"));
    CHECK(ui.output().size() == 4);
}

TEST_CASE("ScriptedTerminalUi renders tables into its output") {
    ScriptedTerminalUi ui;
    Table t;
    t.headers = {"angle", "median SNR"};
    t.rows = {{"0", "12.4"}, {"45", "18.9"}};
    t.right_align = {false, true};
    ui.table(t);
    CHECK(ui.output_contains("median SNR"));
    CHECK(ui.output_contains("18.9"));
}

TEST_CASE("an under-scripted UI fails loudly instead of hanging") {
    ScriptedTerminalUi ui;
    const std::array<MenuItem, 1> items{MenuItem{"only", "", true}};
    CHECK_THROWS_AS(ui.menu("pick", items, 0), std::runtime_error);
}

TEST_CASE("prompt_line returning nullopt models the user cancelling") {
    ScriptedTerminalUi ui;
    ui.push_cancel();
    CHECK_FALSE(ui.prompt_line("angle", "45").has_value());
}

TEST_CASE("colour is disabled by the flag, by NO_COLOR and when stdout is not a terminal") {
    CHECK_FALSE(color_enabled(true));
    ::setenv("NO_COLOR", "1", 1);
    CHECK_FALSE(color_enabled(false));
    ::unsetenv("NO_COLOR");
    // Depends on how the binary was launched, so assert the rule rather than a
    // fixed answer: with nothing forcing it off, colour follows isatty.
    CHECK(color_enabled(false) == (::isatty(STDOUT_FILENO) != 0));
}
```

- [ ] **Step 2: Verify the build fails.**
- [ ] **Step 3: Write the implementation.** `AnsiTerminalUi::menu` core loop:

```cpp
int AnsiTerminalUi::menu(std::string_view title, std::span<const MenuItem> items, int initial) {
    if (items.empty()) return -1;
    RawMode raw;                                   // restores termios on every exit path
    int cursor = clamp_to_enabled(items, initial);
    hide_cursor();
    for (;;) {
        render_menu(title, items, cursor);
        switch (read_key()) {
            case Key::Up:    cursor = previous_enabled(items, cursor); break;
            case Key::Down:  cursor = next_enabled(items, cursor);     break;
            case Key::Home:  cursor = first_enabled(items);            break;
            case Key::End:   cursor = last_enabled(items);             break;
            case Key::Enter: show_cursor(); finish_menu(); return cursor;
            case Key::Quit:  show_cursor(); finish_menu(); return -1;
            default: break;
        }
        move_cursor_up(rendered_line_count(title, items));   // redraw in place
    }
}
```

`render_menu` writes the block, remembers how many lines it emitted, and the loop moves the cursor back up by that many lines before redrawing, so the menu updates in place without clearing the scrollback above it. `finish_menu` moves past the block so subsequent output starts on a fresh line.

`prompt_line` is not raw-mode: it prints `label [default]: ` and uses `std::getline`, so line editing and paste work normally. An empty line returns the default. EOF returns `std::nullopt`.

`style.h` exposes `constexpr` ANSI strings (`kReset`, `kBold`, `kDim`, `kCyan`, `kGreen`, `kYellow`, `kRed`) and a `Style` object that returns `""` for each when colour is off, so call sites never branch.

- [ ] **Step 4: Run the tests.**
- [ ] **Step 5: Manually verify the real menu once** (it cannot be unit tested):

```bash
cmake --build build && ./build/rtlangle --help   # after Task 17 this shows the menu; for now:
printf 'j\nj\n\n' | ./build/rtlangle             # confirm no terminal corruption after exit
stty -a | head -2                                # confirm echo and icanon are restored
```

- [ ] **Step 6: Commit**

```bash
git add src/ui tests/test_terminal_ui.cpp CMakeLists.txt
git commit -m "feat: add terminal UI interface with ANSI menu and scripted test double"
```

---

### Task 13: Angle providers

**Files:**
- Create: `src/angle/angle_provider.h`, `src/angle/manual_angle_provider.h|.cpp`, `src/angle/fixed_angle_provider.h|.cpp`
- Test: `tests/test_angle_provider.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `ITerminalUi` (Task 12), `Config` (Task 4).
- Produces: `AngleOutcome` and `IAngleProvider` exactly as in spec §3.5, plus `ManualAngleProvider(ui, config)` and `FixedAngleProvider()`.

`ManualAngleProvider::request` sequence:

1. `ui.heading("Angle <planned> degrees")`.
2. `ui.info` with the positioning instruction, naming the planned angle.
3. A menu: `Capture at this angle` / `Retry the previous angle` / `Skip this angle` / `Add a note` / `Quit and save`. `Add a note` prompts and then re-shows the menu, so a note can be attached before capturing.
4. On `Capture`: `ui.prompt_line("Estimated actual angle (degrees)", "<planned>")`. An empty line accepts the planned angle. A non-numeric entry warns and re-prompts. An entry deviating by more than `max_angle_deviation_deg` triggers `ui.confirm("... is <d> degrees from the planned <p>. Accept it?")`; declining re-prompts.
5. Returns `Proceed` with the accepted angle and any note.

`settle` sleeps for the requested duration; the controller is responsible for telling the user it is doing so.

- [ ] **Step 1: Write the failing test**

`tests/test_angle_provider.cpp`:

```cpp
#include <doctest/doctest.h>
#include "angle/manual_angle_provider.h"
#include "angle/fixed_angle_provider.h"
#include "ui/scripted_terminal_ui.h"

using namespace rtlangle;
using namespace rtlangle::ui;

// Menu indices used by ManualAngleProvider.
static constexpr int kCapture = 0, kRetry = 1, kSkip = 2, kNote = 3, kQuit = 4;

TEST_CASE("pressing Enter at the angle prompt accepts the planned angle") {
    ScriptedTerminalUi ui;
    Config c; c.center_hz = 118.1e6;
    ui.push_menu_choice(kCapture);
    ui.push_line("");                      // Enter -> accept the planned angle
    ManualAngleProvider p(ui, c);
    AngleOutcome o = p.request(45.0);
    CHECK(o.cmd == AngleOutcome::Cmd::Proceed);
    CHECK(o.actual_deg == doctest::Approx(45.0));
    CHECK_FALSE(o.note.has_value());
}

TEST_CASE("a typed angle within tolerance is accepted without a confirmation") {
    ScriptedTerminalUi ui;
    Config c; c.center_hz = 118.1e6; c.max_angle_deviation_deg = 30.0;
    ui.push_menu_choice(kCapture);
    ui.push_line("47.5");
    ManualAngleProvider p(ui, c);
    CHECK(p.request(45.0).actual_deg == doctest::Approx(47.5));
    CHECK(ui.exhausted());                 // no confirm was consumed
}

TEST_CASE("a wildly different angle must be confirmed, which catches a typo") {
    ScriptedTerminalUi ui;
    Config c; c.center_hz = 118.1e6; c.max_angle_deviation_deg = 30.0;
    ui.push_menu_choice(kCapture);
    ui.push_line("9");                     // typo for 90
    ui.push_confirm(false);                // "no, that was wrong"
    ui.push_line("90");                    // corrected
    ManualAngleProvider p(ui, c);
    AngleOutcome o = p.request(90.0);
    CHECK(o.actual_deg == doctest::Approx(90.0));
    CHECK(ui.output_contains("planned"));
}

TEST_CASE("non-numeric input re-prompts instead of being silently coerced to zero") {
    ScriptedTerminalUi ui;
    Config c; c.center_hz = 118.1e6;
    ui.push_menu_choice(kCapture);
    ui.push_line("about ninety");
    ui.push_line("90");
    ManualAngleProvider p(ui, c);
    CHECK(p.request(90.0).actual_deg == doctest::Approx(90.0));
}

TEST_CASE("retry, skip and quit are surfaced as commands") {
    Config c; c.center_hz = 118.1e6;
    {
        ScriptedTerminalUi ui; ui.push_menu_choice(kRetry);
        CHECK(ManualAngleProvider(ui, c).request(45.0).cmd == AngleOutcome::Cmd::Retry);
    }
    {
        ScriptedTerminalUi ui; ui.push_menu_choice(kSkip);
        ui.push_line("blocked by the balcony wall");     // skip reason
        AngleOutcome o = ManualAngleProvider(ui, c).request(45.0);
        CHECK(o.cmd == AngleOutcome::Cmd::Skip);
        CHECK(o.note.value() == "blocked by the balcony wall");
    }
    {
        ScriptedTerminalUi ui; ui.push_menu_choice(kQuit);
        ui.push_confirm(true);
        CHECK(ManualAngleProvider(ui, c).request(45.0).cmd == AngleOutcome::Cmd::Quit);
    }
}

TEST_CASE("a note can be attached before capturing and comes back with the outcome") {
    ScriptedTerminalUi ui;
    Config c; c.center_hz = 118.1e6;
    ui.push_menu_choice(kNote);
    ui.push_line("cable routed along the balcony rail");
    ui.push_menu_choice(kCapture);
    ui.push_line("");
    ManualAngleProvider p(ui, c);
    AngleOutcome o = p.request(0.0);
    CHECK(o.cmd == AngleOutcome::Cmd::Proceed);
    CHECK(o.note.value() == "cable routed along the balcony rail");
}

TEST_CASE("the manual provider reports itself as not automated") {
    ScriptedTerminalUi ui;
    Config c; c.center_hz = 118.1e6;
    ManualAngleProvider p(ui, c);
    CHECK(p.name() == "manual");
    CHECK_FALSE(p.is_automated());
}

TEST_CASE("FixedAngleProvider proceeds without any terminal interaction") {
    FixedAngleProvider p;
    AngleOutcome o = p.request(60.0);
    CHECK(o.cmd == AngleOutcome::Cmd::Proceed);
    CHECK(o.actual_deg == doctest::Approx(60.0));
    CHECK(p.name() == "fixed");
    CHECK_FALSE(p.is_automated());
}
```

- [ ] **Step 2: Verify the build fails.**
- [ ] **Step 3: Write the implementation** as described above. Both providers implement `settle` with `std::this_thread::sleep_for`; `FixedAngleProvider::settle` ignores the duration so tests run instantly.
- [ ] **Step 4: Run the tests.**
- [ ] **Step 5: Commit**

```bash
git add src/angle tests/test_angle_provider.cpp CMakeLists.txt
git commit -m "feat: add angle provider interface with manual and fixed implementations"
```

---

### Task 14: Visit plan, capture runner and the experiment controller

**Files:**
- Create: `src/experiment/visit_plan.h|.cpp`, `src/experiment/capture_runner.h|.cpp`, `src/experiment/controller.h|.cpp`
- Test: `tests/test_controller.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: every interface from Tasks 9-13.
- Produces:
  - `struct rtlangle::VisitStep { int round; double planned_deg; };`
  - `std::vector<VisitStep> rtlangle::build_visit_plan(const std::vector<double>& angles, int rounds, VisitOrder, std::uint32_t seed)`
  - `std::vector<VisitStep> rtlangle::remaining_steps(const std::vector<VisitStep>& full_plan, const std::set<CompletedKey>& done)`
  - `struct rtlangle::CaptureResult { std::vector<std::complex<float>> channel; std::vector<float> audio; std::uint64_t overruns = 0; std::string error; bool ok() const { return error.empty(); } };`
  - `class rtlangle::CaptureRunner` — `CaptureRunner(const Config&)`, `CaptureResult capture(ISampleSource&, double duration_s, const std::function<void(double)>& progress)`. Reads in blocks of 65536 raw samples, runs the chain and the demodulator incrementally, calls `progress` with the completed fraction at most 20 times per capture, and stops once the channel buffer holds `duration_s * channel_rate_hz` samples.
  - `class rtlangle::ExperimentController` — `ExperimentController(const Config&, ISampleSource&, IAngleProvider&, ui::ITerminalUi&, SessionStore&)`, `enum class RunOutcome { Completed, QuitByUser, Failed };`, `RunOutcome run(std::string& error)`.

`run` walks `remaining_steps`, and for each step:

```
ui.heading("Round R of N - angle A degrees  (step S of T)")
outcome = provider.request(A)
  Quit  -> stop, return QuitByUser (the caller still finalises the session)
  Skip  -> append a Skipped measurement carrying the reason, continue
  Retry -> repeat this same step without appending anything
  Proceed:
    ui.info("Settling for X s ..."); provider.settle(settle)
    source.flush()
    result = runner.capture(source, duration, progress)
    m = estimator.analyze(result.channel, result.audio, now)
    m.index/round/planned_deg/actual_deg/note/sample_overruns filled here
    ui: render the per-measurement block
    store.append(m)
    if m.status != Ok and ui.confirm("Retry this angle?", true) -> repeat the step
```

A `Retry` never appends a measurement, so `completed_keys` stays correct and a resumed session repeats exactly the steps that were never recorded. Guard against an unbounded retry loop with a per-step retry counter of 10, after which the controller warns and moves on.

- [ ] **Step 1: Write the failing test**

`tests/test_controller.cpp`:

```cpp
#include <doctest/doctest.h>
#include "experiment/controller.h"
#include "experiment/visit_plan.h"
#include "source/synthetic_source.h"
#include "angle/fixed_angle_provider.h"
#include "ui/scripted_terminal_ui.h"
#include "persist/session_store.h"

#include <filesystem>
#include <set>

using namespace rtlangle;
namespace fs = std::filesystem;

TEST_CASE("build_visit_plan interleaves rounds in the requested order") {
    const std::vector<double> a{0, 45, 90};
    auto plan = build_visit_plan(a, 2, VisitOrder::Alternating, 0);
    REQUIRE(plan.size() == 6);
    CHECK(plan[0].round == 1); CHECK(plan[0].planned_deg == doctest::Approx(0.0));
    CHECK(plan[2].planned_deg == doctest::Approx(90.0));
    CHECK(plan[3].round == 2); CHECK(plan[3].planned_deg == doctest::Approx(90.0));
    CHECK(plan[5].planned_deg == doctest::Approx(0.0));
}

TEST_CASE("remaining_steps drops exactly what has already been recorded") {
    auto plan = build_visit_plan({0, 45, 90}, 2, VisitOrder::Forward, 0);
    std::set<CompletedKey> done{CompletedKey{1, 0.0}, CompletedKey{1, 45.0}};
    auto rest = remaining_steps(plan, done);
    REQUIRE(rest.size() == 4);
    CHECK(rest[0].round == 1);
    CHECK(rest[0].planned_deg == doctest::Approx(90.0));
    CHECK(rest[1].round == 2);
}
```

The rest of the controller's behaviour is covered by the end-to-end test in Task 16, which exercises the same code with real persistence and reporting. Keep this file to the plan-arithmetic tests so the two are not duplicated.

- [ ] **Step 2: Verify the build fails.**
- [ ] **Step 3: Write the implementation.**
- [ ] **Step 4: Run the tests.**
- [ ] **Step 5: Commit**

```bash
git add src/experiment/visit_plan.h src/experiment/visit_plan.cpp src/experiment/capture_runner.h src/experiment/capture_runner.cpp src/experiment/controller.h src/experiment/controller.cpp tests/test_controller.cpp CMakeLists.txt
git commit -m "feat: add visit plan, capture runner and experiment controller"
```

---

### Task 15: Summary, ranking, warnings and the report

**Files:**
- Create: `src/experiment/summarizer.h|.cpp`, `src/ui/report_renderer.h|.cpp`
- Test: `tests/test_summary.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `Measurement`, `Config`, `statistics`, `ITerminalUi`.
- Produces:
  - `struct rtlangle::AngleSummary` (declared in `core/records.h` in Task 8) has the fields: `{ double planned_deg = 0; double mean_actual_deg = 0, max_actual_deviation_deg = 0; int captures = 0; double total_duration_s = 0; std::size_t valid_event_count = 0; SnrDistribution channel_snr_db, audio_snr_db; double signal_power_dbfs = kDbFloor, noise_floor_dbfs = kDbFloor; MeasurementStatus status = MeasurementStatus::InsufficientData; bool low_confidence = false; std::vector<std::string> notes; };`
  - `struct rtlangle::SessionSummary` has the fields: `{ std::vector<AngleSummary> angles; std::vector<double> ranking_channel_deg, ranking_audio_deg; std::optional<double> best_angle_deg; std::string best_metric; bool rankings_agree = false; std::vector<std::string> warnings; std::string headline; };`
  - `SessionSummary rtlangle::summarize(const std::vector<Measurement>&, const Config&)`
  - `std::string rtlangle::ui::render_report(const SessionSummary&, const SessionMetadata&)` — the plain-text report, no ANSI codes, written to `report.txt`.
  - `void rtlangle::ui::print_report(ITerminalUi&, const SessionSummary&, const SessionMetadata&)` — the coloured terminal version.

Aggregation pools every event from every round at a given planned angle, then re-summarises. An angle is `Ok` only when it has at least `min_valid_events` events and at least one capture whose own status was `Ok`. Ranking sorts `Ok` angles by median SNR descending; `insufficient_data` and `noise_floor_unreliable` angles are listed but never ranked. `best_angle_deg` is set only when the channel and audio rankings agree on the top angle, or when `rank_metric` names a single metric.

The headline is exactly:

```
Highest measured reception quality under this experiment.
This is NOT a measurement of the transmitter's physical direction.
```

Warnings are generated for every condition in spec §6.8, each as a complete sentence naming the affected angles.

- [ ] **Step 1: Write the failing test**

`tests/test_summary.cpp`:

```cpp
#include <doctest/doctest.h>
#include "experiment/summarizer.h"
#include "ui/report_renderer.h"

#include <vector>

using namespace rtlangle;

/// A measurement with `n` events at the given SNR, spread by +/- 1 dB.
static Measurement make(int round, double angle, double snr_db, std::size_t n,
                        MeasurementStatus st = MeasurementStatus::Ok,
                        double noise_dbfs = -60.0) {
    Measurement m;
    m.round = round; m.planned_deg = angle; m.actual_deg = angle;
    m.duration_s = 60.0; m.status = st; m.noise_floor_dbfs = noise_dbfs;
    for (std::size_t i = 0; i < n; ++i) {
        EventRecord e;
        e.channel_snr_db = snr_db + (static_cast<double>(i % 3) - 1.0);
        e.audio_snr_db = e.channel_snr_db - 4.0;
        e.audio_snr_valid = true;
        e.duration_s = 2.0;
        m.events.push_back(e);
    }
    m.valid_event_count = m.events.size();
    return m;
}

static Config cfg() { Config c; c.center_hz = 118.1e6; c.rounds = 2; c.min_valid_events = 3; return c; }

TEST_CASE("events are pooled across rounds for each angle") {
    std::vector<Measurement> ms{make(1, 0.0, 10.0, 4), make(2, 0.0, 10.0, 5)};
    SessionSummary s = summarize(ms, cfg());
    REQUIRE(s.angles.size() == 1);
    CHECK(s.angles[0].valid_event_count == 9);
    CHECK(s.angles[0].captures == 2);
    CHECK(s.angles[0].total_duration_s == doctest::Approx(120.0));
}

TEST_CASE("ranking puts the strongest angle first and both metrics agree") {
    std::vector<Measurement> ms{
        make(1,  0.0,  8.0, 6), make(2,  0.0,  8.0, 6),
        make(1, 45.0, 20.0, 6), make(2, 45.0, 20.0, 6),
        make(1, 90.0, 12.0, 6), make(2, 90.0, 12.0, 6)};
    SessionSummary s = summarize(ms, cfg());
    REQUIRE(s.ranking_channel_deg.size() == 3);
    CHECK(s.ranking_channel_deg[0] == doctest::Approx(45.0));
    CHECK(s.ranking_audio_deg[0] == doctest::Approx(45.0));
    CHECK(s.rankings_agree);
    REQUIRE(s.best_angle_deg.has_value());
    CHECK(s.best_angle_deg.value() == doctest::Approx(45.0));
}

TEST_CASE("disagreeing rankings refuse to name a single best angle and say so") {
    std::vector<Measurement> ms{make(1, 0.0, 10.0, 6), make(1, 45.0, 12.0, 6)};
    // Invert the audio ordering so the two metrics point at different angles.
    for (auto& e : ms[0].events) e.audio_snr_db = 30.0;
    for (auto& e : ms[1].events) e.audio_snr_db = 5.0;

    SessionSummary s = summarize(ms, cfg());
    CHECK(s.ranking_channel_deg[0] == doctest::Approx(45.0));
    CHECK(s.ranking_audio_deg[0] == doctest::Approx(0.0));
    CHECK_FALSE(s.rankings_agree);
    CHECK_FALSE(s.best_angle_deg.has_value());
    bool warned = false;
    for (const auto& w : s.warnings) if (w.find("disagree") != std::string::npos) warned = true;
    CHECK(warned);
}

TEST_CASE("an angle below the minimum event count is listed but never ranked") {
    std::vector<Measurement> ms{
        make(1,  0.0, 10.0, 6),
        make(1, 45.0, 99.0, 1, MeasurementStatus::InsufficientData),
        make(1, 90.0, 12.0, 6)};
    SessionSummary s = summarize(ms, cfg());
    CHECK(s.angles.size() == 3);
    CHECK(s.ranking_channel_deg.size() == 2);
    for (double a : s.ranking_channel_deg) CHECK(a != doctest::Approx(45.0));
    bool warned = false;
    for (const auto& w : s.warnings)
        if (w.find("45") != std::string::npos && w.find("insufficient") != std::string::npos)
            warned = true;
    CHECK(warned);
}

TEST_CASE("a noise floor that drifted across angles is warned about") {
    std::vector<Measurement> ms{
        make(1,  0.0, 10.0, 6, MeasurementStatus::Ok, -60.0),
        make(1, 90.0, 12.0, 6, MeasurementStatus::Ok, -50.0)};   // 10 dB apart
    SessionSummary s = summarize(ms, cfg());
    bool warned = false;
    for (const auto& w : s.warnings)
        if (w.find("noise floor") != std::string::npos) warned = true;
    CHECK(warned);
}

TEST_CASE("overlapping confidence intervals are reported as unresolved") {
    // Two angles a fraction of a dB apart with wide spreads.
    std::vector<Measurement> ms{make(1, 0.0, 12.0, 10), make(1, 45.0, 12.3, 10)};
    SessionSummary s = summarize(ms, cfg());
    bool warned = false;
    for (const auto& w : s.warnings)
        if (w.find("not statistically resolved") != std::string::npos) warned = true;
    CHECK(warned);
}

TEST_CASE("a single round always warns that angle is confounded with time") {
    Config c = cfg(); c.rounds = 1;
    std::vector<Measurement> ms{make(1, 0.0, 10.0, 6), make(1, 90.0, 12.0, 6)};
    SessionSummary s = summarize(ms, c);
    bool warned = false;
    for (const auto& w : s.warnings)
        if (w.find("single") != std::string::npos && w.find("time") != std::string::npos)
            warned = true;
    CHECK(warned);
}

TEST_CASE("an actual angle far from the planned one is warned about") {
    std::vector<Measurement> ms{make(1, 0.0, 10.0, 6), make(1, 90.0, 12.0, 6)};
    ms[1].actual_deg = 78.0;                                    // 12 degrees off
    SessionSummary s = summarize(ms, cfg());
    bool warned = false;
    for (const auto& w : s.warnings) if (w.find("78") != std::string::npos) warned = true;
    CHECK(warned);
}

TEST_CASE("the report states its result as measured quality, not transmitter direction") {
    std::vector<Measurement> ms{make(1, 0.0, 10.0, 6), make(1, 45.0, 20.0, 6)};
    SessionSummary s = summarize(ms, cfg());
    SessionMetadata meta;
    meta.session_id = "20260819-143000-test";
    std::string text = ui::render_report(s, meta);

    CHECK(text.find("Highest measured reception quality under this experiment") != std::string::npos);
    CHECK(text.find("NOT a measurement of the transmitter's physical direction") != std::string::npos);
    CHECK(text.find("45") != std::string::npos);
    CHECK(text.find("\x1b[") == std::string::npos);             // no ANSI codes in report.txt
}

TEST_CASE("a summary with no usable data ranks nothing and says why") {
    std::vector<Measurement> ms{
        make(1, 0.0, 0.0, 0, MeasurementStatus::InsufficientData),
        make(1, 90.0, 0.0, 0, MeasurementStatus::InsufficientData)};
    SessionSummary s = summarize(ms, cfg());
    CHECK(s.ranking_channel_deg.empty());
    CHECK_FALSE(s.best_angle_deg.has_value());
    SessionMetadata meta;
    std::string text = ui::render_report(s, meta);
    CHECK(text.find("no angle") != std::string::npos);
}
```

- [ ] **Step 2: Verify the build fails.**
- [ ] **Step 3: Write the implementation.**
- [ ] **Step 4: Run the tests.**
- [ ] **Step 5: Commit**

```bash
git add src/experiment/summarizer.h src/experiment/summarizer.cpp src/ui/report_renderer.h src/ui/report_renderer.cpp tests/test_summary.cpp CMakeLists.txt
git commit -m "feat: add per-angle summary, ranking, comparability warnings and report"
```

---

### Task 16: End-to-end integration test

Runs the real controller, real persistence and real report over a synthetic AM-voice signal, with no hardware and no terminal.

**Files:**
- Create: `tests/test_end_to_end.cpp`
- Modify: `src/source/synthetic_source.h|.cpp` if the per-angle SNR hook needs adjusting; `CMakeLists.txt`

**Interfaces:**
- Consumes: everything.
- Produces: no production interfaces. Add a small helper `class AngleVaryingSource : public ISampleSource` **inside the test file** that wraps a `SyntheticSource` and switches its SNR when the test tells it to — it must not leak into the library.

- [ ] **Step 1: Write the failing test**

`tests/test_end_to_end.cpp`:

```cpp
#include <doctest/doctest.h>
#include "experiment/controller.h"
#include "experiment/summarizer.h"
#include "ui/report_renderer.h"
#include "ui/scripted_terminal_ui.h"
#include "source/synthetic_source.h"
#include "angle/manual_angle_provider.h"
#include "persist/session_store.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>

using namespace rtlangle;
namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    TempDir() : path(fs::temp_directory_path() /
                     ("rtlangle_e2e_" + std::to_string(::getpid()))) {
        std::error_code ec; fs::remove_all(path, ec); fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

/// Feeds a different SNR depending on the angle the controller is about to
/// measure, so the test knows which angle must win.
class AngleVaryingSource : public ISampleSource {
public:
    AngleVaryingSource(const Config& c, std::map<double, double> snr_by_angle)
        : inner_(c, 10.0, 4242u), snr_by_angle_(std::move(snr_by_angle)) {}
    void set_angle(double deg) { inner_.set_snr_db(snr_by_angle_.at(deg)); }
    SourceInfo info() const override { return inner_.info(); }
    bool read(std::span<std::complex<float>> out, std::string& e) override { return inner_.read(out, e); }
    void flush() override { inner_.flush(); }
private:
    SyntheticSource inner_;
    std::map<double, double> snr_by_angle_;
};

static Config e2e_config(const fs::path& dir) {
    Config c;
    c.center_hz = 118.1e6;
    c.sample_rate_hz = 256000;
    c.channel_rate_hz = 32000;
    c.offset_tune_hz = 60000.0;
    c.duration_s = 12.0;
    c.settle_s = 0.0;
    c.angles.explicit_angles = {0.0, 45.0, 90.0};
    c.rounds = 2;
    c.order = VisitOrder::Alternating;
    c.open_db = 1.0;                    // the sweep includes low-SNR angles
    c.close_db = 0.5;
    c.min_valid_events = 2;
    c.session_dir = dir.string();
    c.label = "e2e";
    c.source = "synthetic";
    return c;
}

TEST_CASE("end to end: six measurements, correct ranking, consistent artefacts") {
    TempDir tmp;
    Config c = e2e_config(tmp.path);
    AngleVaryingSource source(c, {{0.0, 8.0}, {45.0, 22.0}, {90.0, 13.0}});
    ui::ScriptedTerminalUi ui;
    ManualAngleProvider provider(ui, c);

    // Six steps, each: choose "Capture", then press Enter to accept the angle.
    for (int i = 0; i < 6; ++i) { ui.push_menu_choice(0); ui.push_line(""); }

    SessionMetadata meta;
    meta.session_id = "e2e";
    meta.started_utc = "2026-08-19T14:30:00Z";
    meta.config = c;
    meta.plan_angles = {0.0, 45.0, 90.0};
    meta.rounds = 2;
    meta.order = "alternating";
    meta.angle_provider_name = "manual";

    std::string err;
    auto store = SessionStore::create(c, meta, err);
    REQUIRE_MESSAGE(store != nullptr, err);

    // The controller tells the source which angle is next through this hook,
    // which stands in for the antenna actually being moved.
    ExperimentController controller(c, source, provider, ui, *store);
    controller.set_pre_capture_hook([&source](double angle) { source.set_angle(angle); });

    REQUIRE(controller.run(err) == ExperimentController::RunOutcome::Completed);

    SessionSummary summary = summarize(store->measurements(), c);
    std::string report = ui::render_report(summary, store->metadata());
    REQUIRE(store->finalize(summary, report, err));

    // --- the measurements themselves ---
    REQUIRE(store->measurements().size() == 6);
    for (const auto& m : store->measurements()) {
        CAPTURE(m.planned_deg);
        CHECK(m.status == MeasurementStatus::Ok);
        CHECK(m.valid_event_count >= 2);
    }

    // --- the ranking must find the angle the test made strongest ---
    REQUIRE(summary.ranking_channel_deg.size() == 3);
    CHECK(summary.ranking_channel_deg[0] == doctest::Approx(45.0));
    CHECK(summary.ranking_channel_deg[2] == doctest::Approx(0.0));
    REQUIRE(summary.best_angle_deg.has_value());
    CHECK(summary.best_angle_deg.value() == doctest::Approx(45.0));

    // --- the three artefacts exist and agree with each other ---
    const fs::path dir = store->directory();
    REQUIRE(fs::exists(dir / "session.json"));
    REQUIRE(fs::exists(dir / "measurements.csv"));
    REQUIRE(fs::exists(dir / "report.txt"));

    nlohmann::json j;
    { std::ifstream f(dir / "session.json"); f >> j; }
    CHECK(j.at("schema_version") == 1);
    CHECK(j.at("measurements").size() == 6);
    CHECK(j.at("config").at("rounds") == 2);
    CHECK(j.at("summary").at("best_angle_deg") == doctest::Approx(45.0));

    int csv_rows = 0;
    { std::ifstream f(dir / "measurements.csv"); std::string l;
      while (std::getline(f, l)) ++csv_rows; }
    CHECK(csv_rows == 7);                                        // header plus six rows

    std::string report_text;
    { std::ifstream f(dir / "report.txt"); std::stringstream ss; ss << f.rdbuf();
      report_text = ss.str(); }
    CHECK(report_text.find("Highest measured reception quality under this experiment")
          != std::string::npos);
    CHECK(report_text.find("NOT a measurement of the transmitter's physical direction")
          != std::string::npos);
}

TEST_CASE("end to end: quitting halfway still produces a valid report from what completed") {
    TempDir tmp;
    Config c = e2e_config(tmp.path);
    AngleVaryingSource source(c, {{0.0, 8.0}, {45.0, 22.0}, {90.0, 13.0}});
    ui::ScriptedTerminalUi ui;
    ManualAngleProvider provider(ui, c);

    ui.push_menu_choice(0); ui.push_line("");        // measure step 1
    ui.push_menu_choice(0); ui.push_line("");        // measure step 2
    ui.push_menu_choice(4); ui.push_confirm(true);   // quit at step 3

    SessionMetadata meta; meta.session_id = "e2e-quit"; meta.config = c;
    meta.plan_angles = {0.0, 45.0, 90.0}; meta.rounds = 2; meta.order = "alternating";
    std::string err;
    auto store = SessionStore::create(c, meta, err);
    REQUIRE(store != nullptr);

    ExperimentController controller(c, source, provider, ui, *store);
    controller.set_pre_capture_hook([&source](double a) { source.set_angle(a); });
    CHECK(controller.run(err) == ExperimentController::RunOutcome::QuitByUser);

    CHECK(store->measurements().size() == 2);
    SessionSummary summary = summarize(store->measurements(), c);
    std::string report = ui::render_report(summary, store->metadata());
    REQUIRE(store->finalize(summary, report, err));
    CHECK(fs::exists(store->directory() / "report.txt"));
    CHECK(report.find("Highest measured reception quality") != std::string::npos);
}

TEST_CASE("end to end: resume continues exactly where the quit left off") {
    TempDir tmp;
    Config c = e2e_config(tmp.path);
    fs::path dir;
    {
        AngleVaryingSource source(c, {{0.0, 8.0}, {45.0, 22.0}, {90.0, 13.0}});
        ui::ScriptedTerminalUi ui;
        ManualAngleProvider provider(ui, c);
        ui.push_menu_choice(0); ui.push_line("");
        ui.push_menu_choice(0); ui.push_line("");
        ui.push_menu_choice(4); ui.push_confirm(true);
        SessionMetadata meta; meta.session_id = "e2e-resume"; meta.config = c;
        meta.plan_angles = {0.0, 45.0, 90.0}; meta.rounds = 2; meta.order = "alternating";
        std::string err;
        auto store = SessionStore::create(c, meta, err);
        REQUIRE(store != nullptr);
        dir = store->directory();
        ExperimentController controller(c, source, provider, ui, *store);
        controller.set_pre_capture_hook([&source](double a) { source.set_angle(a); });
        controller.run(err);
        CHECK(store->measurements().size() == 2);
    }
    {
        std::string err;
        auto store = SessionStore::reopen(dir, err);
        REQUIRE_MESSAGE(store != nullptr, err);
        AngleVaryingSource source(c, {{0.0, 8.0}, {45.0, 22.0}, {90.0, 13.0}});
        ui::ScriptedTerminalUi ui;
        ManualAngleProvider provider(ui, c);
        for (int i = 0; i < 4; ++i) { ui.push_menu_choice(0); ui.push_line(""); }
        ExperimentController controller(c, source, provider, ui, *store);
        controller.set_pre_capture_hook([&source](double a) { source.set_angle(a); });
        REQUIRE(controller.run(err) == ExperimentController::RunOutcome::Completed);
        CHECK(store->measurements().size() == 6);            // 2 + 4, nothing repeated
        CHECK(ui.exhausted());                               // exactly four steps remained
    }
}
```

- [ ] **Step 2: Verify the build fails** — `set_pre_capture_hook` does not exist yet.
- [ ] **Step 3: Add `void ExperimentController::set_pre_capture_hook(std::function<void(double)>)`,** called with the planned angle immediately before `source.flush()`. In production nothing sets it; it exists so a test source can model the antenna moving. Document that in the header.
- [ ] **Step 4: Run the tests** — `cmake --build build && ctest --test-dir build --output-on-failure -LE hardware`
- [ ] **Step 5: Commit**

```bash
git add tests/test_end_to_end.cpp src/experiment/controller.h src/experiment/controller.cpp CMakeLists.txt
git commit -m "test: add end-to-end synthetic session, quit and resume integration tests"
```

---

### Task 17: The application — CLI, menus, scan and device check

**Files:**
- Create: `src/app/cli_parser.h|.cpp`, `src/app/scan_command.h|.cpp`, `src/app/device_check.h|.cpp`, `src/app/menus.h|.cpp`, `src/app/main.cpp`
- Test: `tests/test_cli_parser.cpp`
- Modify: `CMakeLists.txt` (add the `rtlangle` executable)

**Interfaces:**
- Consumes: everything.
- Produces:
  - `struct rtlangle::app::CliResult { Config config; bool show_help = false; bool show_version = false; std::string command; std::string error; bool ok() const { return error.empty(); } };` — `command` is `""` (menu), `"run"`, `"scan"`, `"devices"` or `"resume"`.
  - `CliResult rtlangle::app::parse_cli(int argc, char** argv)`
  - `bool rtlangle::app::apply_config_file(const std::string& path, Config&, std::string& error)`
  - `std::string rtlangle::app::help_text()`
  - `int rtlangle::app::run_scan(const Config&, ui::ITerminalUi&)`
  - `int rtlangle::app::run_device_check(const Config&, ui::ITerminalUi&)`
  - `int rtlangle::app::run_menu(Config, ui::ITerminalUi&)`

Precedence is enforced by parsing in this order: defaults, then `--config` if present, then the remaining flags. That means `--config` may appear anywhere on the line and flags always win.

The scan command sweeps 118-137 MHz in steps of 0.7 MHz, dwelling `--scan-dwell-ms` (default 60) per tune, repeating `--scan-passes` (default 20) times. For each tune it computes a Welch PSD and assigns power to each 25 kHz channel by summing the bins within +/-4 kHz of the channel centre; channels falling within +/-50 kHz of the tuned centre are skipped for that tune because of the DC spur, and the 0.7 MHz step guarantees every channel is covered by a tune where it is not near DC. The per-tune noise floor is the 20th percentile across that tune's channels. A channel counts as active in a pass when its power exceeds that floor by `open_db`. Results are printed sorted by activity, with the frequency, activity percentage, and mean power above the floor, plus the exact `rtlangle run --freq <best>` line to copy.

`run_menu` renders the approved top-level menu and dispatches. Starting an experiment when `Config::center_hz` is zero prompts for the frequency first and suggests running the scan.

`main` installs the terminal signal handlers, constructs an `AnsiTerminalUi`, and returns 0 on success, 1 on a configuration error, 2 on a device error.

- [ ] **Step 1: Write the failing test**

`tests/test_cli_parser.cpp`:

```cpp
#include <doctest/doctest.h>
#include "app/cli_parser.h"

#include <filesystem>
#include <fstream>
#include <vector>

using namespace rtlangle;
using namespace rtlangle::app;

static CliResult parse(std::vector<const char*> args) {
    args.insert(args.begin(), "rtlangle");
    return parse_cli(static_cast<int>(args.size()), const_cast<char**>(args.data()));
}

TEST_CASE("no arguments means the interactive menu") {
    CliResult r = parse({});
    CHECK(r.ok());
    CHECK(r.command.empty());
}

TEST_CASE("flags map onto the configuration") {
    CliResult r = parse({"run", "--freq", "118.35M", "--gain", "402", "--duration", "90",
                         "--rounds", "3", "--order", "random", "--seed", "77",
                         "--angles", "0,30,60,90", "--label", "balcony"});
    REQUIRE_MESSAGE(r.ok(), r.error);
    CHECK(r.command == "run");
    CHECK(r.config.center_hz == doctest::Approx(118.35e6));
    CHECK(r.config.requested_gain_tenth_db == 402);
    CHECK(r.config.duration_s == doctest::Approx(90.0));
    CHECK(r.config.rounds == 3);
    CHECK(r.config.order.value() == VisitOrder::Random);
    CHECK(r.config.seed == 77u);
    CHECK(r.config.angles.explicit_angles == std::vector<double>{0, 30, 60, 90});
    CHECK(r.config.label == "balcony");
}

TEST_CASE("--gain max is the default and is spelled out") {
    CliResult r = parse({"run", "--freq", "118.1M", "--gain", "max"});
    REQUIRE(r.ok());
    CHECK(r.config.requested_gain_tenth_db == -1);
}

TEST_CASE("an unknown flag is an error naming the flag, not a silent ignore") {
    CliResult r = parse({"run", "--freq", "118.1M", "--turbo"});
    CHECK_FALSE(r.ok());
    CHECK(r.error.find("--turbo") != std::string::npos);
}

TEST_CASE("a flag missing its value is an error") {
    CliResult r = parse({"run", "--freq"});
    CHECK_FALSE(r.ok());
    CHECK(r.error.find("--freq") != std::string::npos);
}

TEST_CASE("a non-numeric value is an error naming the flag and the value") {
    CliResult r = parse({"run", "--freq", "118.1M", "--duration", "soon"});
    CHECK_FALSE(r.ok());
    CHECK(r.error.find("--duration") != std::string::npos);
    CHECK(r.error.find("soon") != std::string::npos);
}

TEST_CASE("command line flags override the config file regardless of flag order") {
    const auto path = std::filesystem::temp_directory_path() / "rtlangle_cli_test.json";
    { std::ofstream f(path);
      f << R"({"center_hz": 118100000, "duration_s": 30, "rounds": 5, "label": "from-file"})"; }

    CliResult before = parse({"run", "--config", path.c_str(), "--duration", "90"});
    REQUIRE_MESSAGE(before.ok(), before.error);
    CHECK(before.config.duration_s == doctest::Approx(90.0));
    CHECK(before.config.rounds == 5);
    CHECK(before.config.label == "from-file");

    CliResult after = parse({"run", "--duration", "90", "--config", path.c_str()});
    REQUIRE_MESSAGE(after.ok(), after.error);
    CHECK(after.config.duration_s == doctest::Approx(90.0));   // flag still wins
    std::filesystem::remove(path);
}

TEST_CASE("a malformed config file reports the path and the parse problem") {
    const auto path = std::filesystem::temp_directory_path() / "rtlangle_bad.json";
    { std::ofstream f(path); f << "{not json"; }
    CliResult r = parse({"run", "--config", path.c_str()});
    CHECK_FALSE(r.ok());
    CHECK(r.error.find("rtlangle_bad.json") != std::string::npos);
    std::filesystem::remove(path);
}

TEST_CASE("--agc without --allow-agc is rejected by validation, not by the parser") {
    CliResult r = parse({"run", "--freq", "118.1M", "--agc"});
    REQUIRE(r.ok());                            // the parser accepts it
    CHECK_FALSE(validate(r.config).ok());       // validation refuses it
}

TEST_CASE("help and version short-circuit") {
    CHECK(parse({"--help"}).show_help);
    CHECK(parse({"--version"}).show_version);
    CHECK(help_text().find("--freq") != std::string::npos);
    CHECK(help_text().find("--rank-metric") != std::string::npos);
}

TEST_CASE("subcommands are recognised") {
    CHECK(parse({"scan"}).command == "scan");
    CHECK(parse({"devices"}).command == "devices");
    CliResult r = parse({"resume", "sessions/20260819-143000-balcony"});
    CHECK(r.command == "resume");
    CHECK(r.config.resume_dir == "sessions/20260819-143000-balcony");
}
```

- [ ] **Step 2: Verify the build fails.**
- [ ] **Step 3: Write the implementation** and add the executable:

```cmake
add_executable(rtlangle
  src/app/main.cpp src/app/cli_parser.cpp src/app/scan_command.cpp
  src/app/device_check.cpp src/app/menus.cpp)
target_link_libraries(rtlangle PRIVATE rtlangle_core)
target_compile_options(rtlangle PRIVATE -Wall -Wextra -Wpedantic)
```

- [ ] **Step 4: Run the tests and exercise the binary by hand**

```bash
cmake --build build && ctest --test-dir build --output-on-failure -LE hardware
./build/rtlangle --help
./build/rtlangle devices
./build/rtlangle run --source synthetic --non-interactive --freq 118.1M \
    --duration 12 --settle 0 --rounds 1 --angles 0,45,90 \
    --open-db 1 --close-db 0.5 --min-valid-events 2 --session-dir /tmp/rtlangle-demo
```

Expected: the last command completes without a terminal, writes a session under `/tmp/rtlangle-demo`, and prints a ranked table.

- [ ] **Step 5: Commit**

```bash
git add src/app tests/test_cli_parser.cpp CMakeLists.txt
git commit -m "feat: add CLI parser, interactive menus, airband scan and device check"
```

---

### Task 18: Documentation and the example session

**Files:**
- Create: `README.md`, `docs/architecture.md`, `examples/session-synthetic/` (`session.json`, `measurements.csv`, `report.txt`)
- Modify: `.gitignore` (the `sessions/` rule must not swallow `examples/session-synthetic/`)

- [ ] **Step 1: Generate the example session from the real tool**

```bash
./build/rtlangle run --source synthetic --non-interactive --freq 118.1M \
    --duration 12 --settle 0 --rounds 2 --angles 0,15,30,45,60,75,90 \
    --open-db 1 --close-db 0.5 --min-valid-events 2 \
    --session-dir examples --label synthetic
mv examples/*-synthetic examples/session-synthetic
```

The example must be generated, never hand-written, so it cannot drift from the real output shape.

- [ ] **Step 2: Write `README.md`** covering, in this order:
  1. What the tool does in three sentences, including that it is receive-only and that RTL-SDR hardware has no transmit path.
  2. **The result wording**: it reports the highest measured reception quality under the experiment, not the transmitter's direction.
  3. Dependencies with the exact Ubuntu install line, and the note that an RTL-SDR Blog V4 requires the rtl-sdr-blog fork of librtlsdr, not the stock package.
  4. Build: `cmake -S . -B build -G Ninja && cmake --build build`.
  5. Test: `ctest --test-dir build --output-on-failure -LE hardware` and the separate `-L hardware` line.
  6. Blacklisting the DVB-T kernel driver, and the "device busy" fix (close SDR++/gqrx/dump1090).
  7. **Running the experiment**, step by step, including finding a frequency with `rtlangle scan`.
  8. **The exact first-experiment command** (see Step 4).
  9. Worked CLI examples: multi-round alternating, randomised order, resume, synthetic replay, non-interactive.
  10. **The SNR definition, spelled out**: `SNR_lin = (P_event - N) / N` then `10*log10`, with `N` the 20th percentile of in-channel frame powers, `P_event` the median in-channel power over the event's frames, both measured over the +/-4 kHz channel. State plainly that `channel_snr_db` is a carrier-plus-sideband-to-noise ratio (a CNR), and that `audio_snr_db` is the post-demodulation speech-band figure.
  11. **Limitations**, each stated plainly: the squelch threshold sets a floor of about 4.7 dB on the measurable SNR at default settings; airband traffic is intermittent and changes between angles, which is why multiple rounds and alternating order exist; the operator's entered angle is an estimate; the tool measures reception quality, not direction; and the state of hardware validation.
  12. A short "what Phase 1 does not include" list: no GUI, no servo or ESP32 control, no denoising.

- [ ] **Step 3: Write `docs/architecture.md`** — the layer diagram from this plan's File Structure section, one paragraph per module explaining what it does and what it depends on, the four interfaces with their signatures, and a section titled "Adding a servo angle provider" showing that only a new `IAngleProvider` implementation and one line of wiring in `main.cpp` are required.

- [ ] **Step 4: Put the first-experiment command in the README verbatim**

```bash
# 1. Close any other SDR software first (SDR++, gqrx, dump1090).
pkill -f sdrpp

# 2. Confirm the device is visible and free.
./build/rtlangle devices

# 3. Find an airband channel that actually carries traffic near you (about 30 s).
./build/rtlangle scan

# 4. Run the 0-90 degree experiment on the frequency the scan recommended.
#    Two rounds in alternating order, 60 s per angle, 5 s to settle after moving it.
./build/rtlangle run \
    --freq <FREQUENCY_FROM_SCAN> \
    --start-deg 0 --end-deg 90 --step-deg 15 \
    --rounds 2 --order alternating \
    --duration 60 --settle 5 \
    --gain max \
    --label first-experiment
```

- [ ] **Step 5: Verify the documented commands actually work**

```bash
grep -n 'rtlangle ' README.md    # then run each example with --help or a synthetic source
cmake --build build && ctest --test-dir build --output-on-failure -LE hardware
```

- [ ] **Step 6: Commit**

```bash
git add README.md docs/architecture.md examples .gitignore
git commit -m "docs: add README, architecture notes and a generated example session"
```

---

## Final verification

Run all of this and record the actual output; do not summarise from memory.

```bash
rm -rf build
cmake -S . -B build -G Ninja
cmake --build build 2>&1 | tail -20
ctest --test-dir build --output-on-failure -LE hardware
ctest --test-dir build --output-on-failure -L hardware      # needs the device to be free
cmake -S . -B build-nohw -G Ninja -DRTLANGLE_WITH_RTLSDR=OFF
cmake --build build-nohw && ctest --test-dir build-nohw --output-on-failure -LE hardware
git status --short
git log --oneline
```

Then report: files changed, commands executed, build and test results, **whether real RTL-SDR hardware was detected and tested or whether validation is pending**, known limitations, and the exact first-experiment command from Task 18 Step 4.

## Spec coverage

| Spec section | Task |
|---|---|
| §1.2 transmit safety, bias tee default off | 4 (validation warning), 10 (open sequence), 18 (README) |
| §3.1 core types | 1, 2, 3, 4, 8 |
| §3.2 ISampleSource, named errors | 9, 10 |
| §3.3 dsp | 5, 6 |
| §3.4 metrics | 7, 8 |
| §3.5 IAngleProvider, Manual, Fixed | 13 |
| §3.6 ITerminalUi, ANSI, scripted | 12 |
| §3.7 ExperimentController state machine | 14 |
| §3.8 atomic persistence | 11 |
| §4 angle generation, validation, order | 3 |
| §4.2 actual-angle entry and deviation guard | 13 |
| §5 signal chain, offset tuning | 5 |
| §6.1-6.5 framing, noise floor, events, SNR | 6, 7, 8 |
| §6.6 rank metric selection | 15 |
| §6.7 aggregation, percentiles, median CI | 2, 15 |
| §6.8 comparability warnings | 15 |
| §6.9 result wording | 15, 18 |
| §7 configuration and precedence | 4, 17 |
| §7.1 gain snapping and read-back | 10 |
| §8 session layout, JSON, CSV, resume | 11, 16 |
| §9 terminal interface, scan, device check | 12, 17 |
| §10.1-10.5 test suite | 3, 7, 8, 11, 16 |
| §10.6 hardware smoke test | 10 |
| §11 build | 1 |
| §12 deliverables | 18 |
