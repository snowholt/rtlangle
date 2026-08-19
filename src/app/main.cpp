#include "core/version.h"

#include <cstdio>
#include <cstring>

namespace {

void print_usage(std::FILE* out) {
  std::fprintf(out, "%s\n", rtlangle::kToolVersion);
  std::fprintf(out, "\nUsage:\n");
  std::fprintf(out, "  rtlangle --version    print the tool version\n");
  std::fprintf(out, "  rtlangle --help       print this message\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    std::printf("%s\n", rtlangle::kToolVersion);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
    print_usage(stdout);
    return 0;
  }
  print_usage(stderr);
  return 2;  // usage error, spec section 12.1
}
