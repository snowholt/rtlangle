cmake_minimum_required(VERSION 3.24)

project(rtlangle
        VERSION 0.1.0
        DESCRIPTION "Antenna-angle reception-quality experiments for RTL-SDR"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR
          "rtlangle Phase 1 is Linux-only: it assumes termios, renameat, fsync, "
          "and O_NOFOLLOW. Building on ${CMAKE_SYSTEM_NAME} is unsupported.")
endif()

option(RTLANGLE_WITH_RTLSDR "Build with librtlsdr device support" ON)
option(RTLANGLE_WERROR      "Treat project-target warnings as errors" ON)

include(CTest)
include(cmake/Dependencies.cmake)

# ---------------------------------------------------------------------------
# Warning flags. Project targets only: fetched dependencies are attached as
# SYSTEM include directories so their headers never produce a diagnostic here.
# ---------------------------------------------------------------------------
add_library(rtlangle_warnings INTERFACE)
target_compile_options(rtlangle_warnings INTERFACE
                       -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
if(RTLANGLE_WERROR)
  target_compile_options(rtlangle_warnings INTERFACE -Werror)
endif()

# ---------------------------------------------------------------------------
# rtlangle_core
# ---------------------------------------------------------------------------
set(RTLANGLE_CORE_SOURCES
    src/app/device_check.cpp
    src/core/angle_math.cpp
    src/core/config.cpp
    src/core/records.cpp
    src/core/utc.cpp
    src/dsp/am_demodulator.cpp
    src/dsp/chain.cpp
    src/dsp/fir_decimator.cpp
    src/dsp/fir_design.cpp
    src/dsp/framer.cpp
    src/dsp/offset_mixer.cpp
    src/dsp/spectrum.cpp
    src/experiment/capture_runner.cpp
    src/experiment/controller.cpp
    src/experiment/visit_plan.cpp
    src/metrics/event_detector.cpp
    src/metrics/noise_floor.cpp
    src/metrics/snr_estimator.cpp
    src/persist/atomic_write.cpp
    src/persist/csv.cpp
    src/persist/json_session_store.cpp
    src/persist/path_safety.cpp
    src/persist/session_loader.cpp
    src/angle/manual_angle_provider.cpp
    src/ui/ansi_terminal_ui.cpp
    src/ui/safe_text.cpp
    src/ui/scripted_terminal_ui.cpp
    src/source/iq_file_source.cpp
    src/source/rtlsdr_source.cpp
    src/source/sample_ring.cpp
    src/source/source_factory.cpp
    src/source/synthetic_source.cpp
    src/core/db.cpp
    src/core/statistics.cpp
)

add_library(rtlangle_core STATIC ${RTLANGLE_CORE_SOURCES})
target_include_directories(rtlangle_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(rtlangle_core
                      PUBLIC  nlohmann_json::nlohmann_json
                              PkgConfig::FFTW3F
                              Threads::Threads
                      PRIVATE rtlangle_warnings)

if(RTLANGLE_WITH_RTLSDR)
  target_compile_definitions(rtlangle_core PUBLIC RTLANGLE_WITH_RTLSDR=1)
  target_compile_definitions(rtlangle_core PRIVATE
                             RTLANGLE_LIBRTLSDR_VERSION="${LIBRTLSDR_VERSION}")
  target_link_libraries(rtlangle_core PUBLIC PkgConfig::LIBRTLSDR)
else()
  target_compile_definitions(rtlangle_core PUBLIC RTLANGLE_WITH_RTLSDR=0)
endif()

# ---------------------------------------------------------------------------
# rtlangle — the CLI executable
# ---------------------------------------------------------------------------
add_executable(rtlangle src/app/main.cpp)
target_link_libraries(rtlangle PRIVATE rtlangle_core rtlangle_warnings)

# ---------------------------------------------------------------------------
# rtlangle_tests — every hardware-free test
# ---------------------------------------------------------------------------
set(RTLANGLE_TEST_SOURCES
    tests/test_main.cpp
    tests/test_fftw_link.cpp
    tests/test_core_numeric.cpp
    tests/test_angle_math.cpp
    tests/test_config.cpp
    tests/test_records.cpp
    tests/test_dsp.cpp
    tests/test_sources.cpp
    tests/test_capture_runner.cpp
    tests/test_event_detector.cpp
    tests/test_noise_floor.cpp
    tests/test_snr_estimator.cpp
    tests/test_persist.cpp
    tests/test_resume.cpp
    tests/test_rtlsdr_offline.cpp
    tests/test_ui.cpp
    tests/test_angle_provider.cpp
    tests/test_terminal_pty.cpp
    tests/test_visit_plan.cpp
    tests/test_controller.cpp
)

add_executable(rtlangle_tests ${RTLANGLE_TEST_SOURCES})
target_include_directories(rtlangle_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rtlangle_tests
                      PRIVATE rtlangle_core doctest::doctest rtlangle_warnings util)

if(BUILD_TESTING)
  if(COMMAND doctest_discover_tests)
    doctest_discover_tests(rtlangle_tests ADD_LABELS 0 PROPERTIES LABELS unit)
  else()
    add_test(NAME rtlangle_tests COMMAND rtlangle_tests)
    set_tests_properties(rtlangle_tests PROPERTIES LABELS unit)
  endif()
endif()

# ---------------------------------------------------------------------------
# rtlangle_hardware_test — a separate executable, so that `ctest -LE hardware`
# is a genuinely hardware-free run. CTest labels apply to registered tests,
# not to cases inside a binary.
# ---------------------------------------------------------------------------
add_executable(rtlangle_hardware_test tests/hardware/hardware_test.cpp)
target_link_libraries(rtlangle_hardware_test PRIVATE rtlangle_core rtlangle_warnings)

if(BUILD_TESTING)
  add_test(NAME hardware COMMAND rtlangle_hardware_test)
  set_tests_properties(hardware PROPERTIES LABELS hardware SKIP_RETURN_CODE 77)
endif()

message(STATUS "rtlangle ${PROJECT_VERSION}")
message(STATUS "  RTLANGLE_WITH_RTLSDR : ${RTLANGLE_WITH_RTLSDR}")
message(STATUS "  RTLANGLE_WERROR      : ${RTLANGLE_WERROR}")
