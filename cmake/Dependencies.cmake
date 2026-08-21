# Dependency resolution for rtlangle.
#
# Every dependency is resolved with find_package first so a distribution package
# is used when one exists, and falls back to FetchContent pinned by URL hash so
# a build is reproducible when one does not. The resolved origin and version of
# each dependency is echoed at configure time; spec section 3 requires that the
# record show which FFTW precision and which librtlsdr were actually used.

# The shared download and unpack cache must be chosen before FetchContent is
# included, since the module caches the variable on first inclusion. One cache
# keeps the ON, OFF, and sanitizer build trees from each re-downloading.
if(NOT DEFINED FETCHCONTENT_BASE_DIR OR FETCHCONTENT_BASE_DIR STREQUAL "")
  set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/.deps" CACHE PATH
      "Shared download and unpack cache for fetched dependencies")
endif()

include(FetchContent)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

find_package(PkgConfig REQUIRED)

# ---------------------------------------------------------------------------
# FFTW, single precision only.
#
# The implementation calls fftwf_* throughout. fftwf_plan_dft_1d is defined in
# libfftw3f and is absent from libfftw3, so linking the double-precision library
# is a link error rather than a silent precision change. The two are never
# mixed; see spec section 3.
# ---------------------------------------------------------------------------
pkg_check_modules(FFTW3F REQUIRED IMPORTED_TARGET fftw3f)
message(STATUS "  fftw3f               : ${FFTW3F_VERSION}")

if(RTLANGLE_WITH_RTLSDR)
  pkg_check_modules(LIBRTLSDR REQUIRED IMPORTED_TARGET librtlsdr)
  message(STATUS "  librtlsdr            : ${LIBRTLSDR_VERSION}")
endif()

# ---------------------------------------------------------------------------
# nlohmann/json 3.11.3
# ---------------------------------------------------------------------------
find_package(nlohmann_json 3.11.3 QUIET)
if(nlohmann_json_FOUND)
  message(STATUS "  nlohmann_json        : ${nlohmann_json_VERSION} (system)")
else()
  set(JSON_BuildTests   OFF CACHE INTERNAL "")
  set(JSON_Install      OFF CACHE INTERNAL "")
  FetchContent_Declare(nlohmann_json
    URL      https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz
    URL_HASH SHA256=0d8ef5af7f9794e3263480193c491549b2ba6cc74bb018906202ada498a79406
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)
  FetchContent_MakeAvailable(nlohmann_json)
  message(STATUS "  nlohmann_json        : 3.11.3 (fetched)")
endif()

# ---------------------------------------------------------------------------
# doctest 2.4.11
# ---------------------------------------------------------------------------
find_package(doctest 2.4.11 QUIET)
if(doctest_FOUND)
  message(STATUS "  doctest              : ${doctest_VERSION} (system)")
  if(DEFINED doctest_DIR)
    list(APPEND CMAKE_MODULE_PATH "${doctest_DIR}")
  endif()
else()
  set(DOCTEST_WITH_TESTS       OFF CACHE INTERNAL "")
  set(DOCTEST_NO_INSTALL       ON  CACHE INTERNAL "")
  FetchContent_Declare(doctest
    URL      https://github.com/doctest/doctest/archive/refs/tags/v2.4.11.tar.gz
    URL_HASH SHA256=632ed2c05a7f53fa961381497bf8069093f0d6628c5f26286161fbd32a560186
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)
  FetchContent_MakeAvailable(doctest)
  list(APPEND CMAKE_MODULE_PATH "${doctest_SOURCE_DIR}/scripts/cmake")
  message(STATUS "  doctest              : 2.4.11 (fetched)")
endif()

include(doctest OPTIONAL)
