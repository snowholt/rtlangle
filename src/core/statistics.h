#pragma once

#include <span>

namespace rtlangle {

// Every helper below is TOTAL over its declared domain: for any input a valid
// Config can produce it returns a defined value or a flagged absence, never
// undefined behaviour and never a silent NaN.
//
// Spec decision Q1 removed the inferential helpers entirely. There is no
// t-quantile, no paired confidence bound, and no permutation test, because
// Phase 1 runs no statistical test. This is the whole numeric surface, and its
// totality is checkable by hand.

// A statistic that may not exist. `valid == false` carries no value.
struct Stat {
  double value = 0.0;
  bool   valid = false;
  // True when the requested percentile lay outside [0, 100] and was clamped
  // into it. The clamp is reported rather than silently reinterpreted.
  bool   clamped = false;
};

// Linear-interpolated percentile over the finite entries of `xs`.
//
// Non-finite entries carry no information a percentile could use, so they are
// excluded; when nothing finite remains the result is `valid == false`. `pct`
// is clamped to [0, 100] and the clamp is reported. The input is not modified:
// the function sorts a copy.
Stat percentile(std::span<const double> xs, double pct);

// The fiftieth percentile.
Stat median(std::span<const double> xs);

// Descriptive spread only (spec section 10.2): min-max below four contributing
// values, interquartile at four or more. The type name says what it is,
// because it is NOT a confidence interval and spec section 15.2 forbids
// presenting it as one.
struct DescriptiveSpread {
  double low = 0.0;
  double high = 0.0;
  int    n = 0;            // how many finite values contributed
  bool   is_iqr = false;   // false means low..high is the min and the max
  bool   valid = false;
};

DescriptiveSpread descriptive_spread(std::span<const double> xs);

}  // namespace rtlangle
