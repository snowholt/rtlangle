#pragma once

#include "core/records.h"

#include <string>
#include <string_view>

namespace rtlangle {

// RFC 4180 quoting: a field containing a comma, a quote, a carriage return, or
// a line feed is quoted and its internal quotes are doubled.
std::string csv_escape(std::string_view field);

// The header of spec section 11.6.
//
// The column is `host_dropped_samples`, not any word meaning a device overrun:
// librtlsdr exposes no device-level overflow counter, and a CSV header is
// exactly the place a misleading name would outlive the explanation.
std::string csv_header();

// One row per attempt, in commit order. measurements.csv is a DERIVED export;
// session.json is the single source of truth and this can be regenerated from
// it at any time.
std::string csv_row(const AttemptRecord&);
std::string render_csv(const SessionRecord&);

// The number of data rows in an existing export, used to decide whether it
// disagrees with the JSON and must be regenerated. Returns -1 when the text is
// missing a header or is otherwise unusable, which is also a regeneration.
int csv_row_count(std::string_view text);

}  // namespace rtlangle
