#pragma once

#include <string>
#include <string_view>

namespace rtlangle::ui {

// Control-character neutralisation, spec section 12.3.
//
// Every string that originates outside the program - session labels, operator
// notes, file paths, USB device descriptors, librtlsdr error text - passes
// through this before being rendered. C0, C1, and DEL become a visible \xNN
// form, so a crafted device string cannot reposition the cursor, clear the
// screen, or inject colour into the report.
//
// It applies AT RENDER TIME ONLY. The canonical record stores notes verbatim;
// mutating what was stored would lose the operator's actual words.
//
// UTF-8 above U+007F passes through unchanged, except the two-byte C1 sequences
// (0xC2 0x80 to 0xC2 0x9F), which are the C1 controls in UTF-8 and are
// neutralised as the bytes they are.
std::string safe_text(std::string_view);

}  // namespace rtlangle::ui
