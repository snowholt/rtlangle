#pragma once

#include "core/records.h"
#include "ui/terminal_ui.h"

#include <string>
#include <string_view>

namespace rtlangle::ui {

// Spec section 10.6. The report's closing headline, printed in every session
// without exception, in the terminal, in report.txt, and quoted in the README.
//
// There is no second headline and no condition under which a different one is
// printed. It may not be reworded, abbreviated, split, or coloured away, and it
// may not appear before the ranking tables.
inline constexpr std::string_view kResultHeadline =
    "Exploratory ranking only. These are the reception qualities measured at each\n"
    "angle under the traffic that happened to occur. This tool does not determine\n"
    "which angle is best, and it is NOT a measurement of the transmitter's physical\n"
    "direction.";

// A pure function of its arguments. It never computes a warning and never
// re-aggregates, which is what makes the report-metric comparison a byte
// comparison rather than a judgement call.
std::string render_report(const SessionRecord&, const SessionSummary&, bool color);

// Lets an interactive operator change which ranking is listed FIRST. Both are
// always printed in full, and nothing downstream consumes the choice, so it
// cannot influence any number in either table.
//
// It mutates only report_metric and report_metric_source.
void choose_report_metric(SessionSummary&, ITerminalUi&);

}  // namespace rtlangle::ui
