#include "core/statistics.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace rtlangle {
namespace {

// The finite entries of `xs`, ascending. Non-finite entries are dropped rather
// than propagated, which is what keeps a valid result finite.
std::vector<double> finite_sorted(std::span<const double> xs) {
  std::vector<double> v;
  v.reserve(xs.size());
  for (double x : xs) {
    if (std::isfinite(x)) v.push_back(x);
  }
  std::sort(v.begin(), v.end());
  return v;
}

// Linear interpolation at rank `pct/100 * (n-1)` over an ascending vector.
double interpolate(const std::vector<double>& v, double pct) {
  const std::size_t n = v.size();
  if (n == 1) return v[0];
  const double rank = pct / 100.0 * static_cast<double>(n - 1);
  const double lo_f = std::floor(rank);
  std::size_t lo = static_cast<std::size_t>(lo_f);
  if (lo >= n - 1) return v[n - 1];
  const double frac = rank - lo_f;
  return v[lo] + frac * (v[lo + 1] - v[lo]);
}

}  // namespace

Stat percentile(std::span<const double> xs, double pct) {
  Stat out;
  out.clamped = (pct < 0.0) || (pct > 100.0);
  const double p = std::clamp(pct, 0.0, 100.0);

  const std::vector<double> v = finite_sorted(xs);
  if (v.empty()) return out;  // valid stays false; the clamp is still reported

  out.value = interpolate(v, p);
  out.valid = true;
  return out;
}

Stat median(std::span<const double> xs) { return percentile(xs, 50.0); }

DescriptiveSpread descriptive_spread(std::span<const double> xs) {
  DescriptiveSpread out;
  const std::vector<double> v = finite_sorted(xs);
  if (v.empty()) return out;

  out.n = static_cast<int>(v.size());
  out.valid = true;
  if (v.size() >= 4) {
    out.is_iqr = true;
    out.low = interpolate(v, 25.0);
    out.high = interpolate(v, 75.0);
  } else {
    out.is_iqr = false;
    out.low = v.front();
    out.high = v.back();
  }
  return out;
}

}  // namespace rtlangle
