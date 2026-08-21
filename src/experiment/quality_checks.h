#pragma once

#include "core/config.h"
#include "core/records.h"

#include <string_view>
#include <vector>

namespace rtlangle {

// The eight labelled data-quality warnings of spec section 10.4, and the
// comparability notes of section 10.5.
//
// These are WARNINGS ABOUT THE MEASUREMENT, in the sense that a compiler
// warning is about the code: each names something that makes a comparison less
// trustworthy, and NONE OF THEM COMBINE INTO A VERDICT. All eight are evaluated
// every time - there is no early return anywhere in the implementation, because
// a list of problems that stops at the first problem is not a list of problems,
// and a warning list that short-circuits is a gate ladder wearing a different
// name.
//
// The record is needed as well as the summary: W2 counts the rounds two angles
// actually shared, and W7 compares each attempt's applied settings against its
// own receiver segment's baseline. Neither quantity survives aggregation, so
// neither can be read off the summary alone.
//
// It appends to summary.warnings and stamps warning_ids on the angles each
// warning names. It writes no other field of AngleSummary, and it decides
// nothing.
void evaluate_warnings(SessionSummary& summary, const SessionRecord& record,
                       const Config& cfg);

// The identifiers spec section 10.4 defines, so a test can assert the set the
// suite exercises equals the set the specification names.
std::vector<std::string_view> warning_ids();

}  // namespace rtlangle
