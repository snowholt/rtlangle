#include "core/db.h"

#include <cmath>

namespace rtlangle {

double to_db(double linear) {
  if (!(linear > 0.0)) return kDecibelFloor;  // false for NaN as well as for <= 0
  const double db = 10.0 * std::log10(linear);
  return std::isfinite(db) ? db : kDecibelFloor;
}

double from_db(double db) {
  if (std::isnan(db)) return 0.0;
  return std::pow(10.0, db / 10.0);
}

double minimum_detectable_snr_db(double open_db) {
  return to_db(from_db(open_db) - 1.0);
}

}  // namespace rtlangle
