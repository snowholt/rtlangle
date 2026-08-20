#pragma once

#include "core/config.h"
#include "core/records.h"

#include <vector>

namespace rtlangle {

// The visit sequence: every angle, once per round, in the configured order.
//
// The identifier is r<round>-i<angle index padded to three digits>. It is
// index-based rather than angle-formatted because an angle-formatted id rounds,
// while the angle list de-duplicates only within 1e-6: --angles "45.001,45.002"
// would then produce two planned angles sharing one identifier, and on resume
// the second visit would be treated as the first - a silent loss of one
// measurement per collision. The full-precision planned_deg is carried
// separately and is what the aggregator groups by.
std::vector<Visit> build_visit_plan(const Config&);

}  // namespace rtlangle
