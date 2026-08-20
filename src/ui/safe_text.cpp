#include "ui/safe_text.h"

#include <cstdio>

namespace rtlangle::ui {
namespace {

void append_escaped(std::string& out, unsigned char c) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "\\x%02X", c);
  out += buf;
}

}  // namespace

std::string safe_text(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    const auto c = static_cast<unsigned char>(text[i]);

    // C0 and DEL.
    if (c < 0x20 || c == 0x7F) {
      append_escaped(out, c);
      continue;
    }

    // The C1 controls, which in UTF-8 are 0xC2 followed by 0x80 to 0x9F.
    if (c == 0xC2 && i + 1 < text.size()) {
      const auto next = static_cast<unsigned char>(text[i + 1]);
      if (next >= 0x80 && next <= 0x9F) {
        append_escaped(out, c);
        append_escaped(out, next);
        ++i;
        continue;
      }
    }

    out.push_back(text[i]);
  }
  return out;
}

}  // namespace rtlangle::ui
