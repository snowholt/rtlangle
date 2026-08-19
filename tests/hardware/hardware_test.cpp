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
// passed. The receive-path body lands in WP7; until then no device is opened,
// so the only honest outcome is `skipped`.

#include <cstdio>

int main() {
  std::puts("RTLANGLE_HARDWARE=skipped");
  return 77;
}
