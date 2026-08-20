#include "core/angle_math.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>
#include <random>
#include <sstream>
#include <string_view>

namespace rtlangle {
namespace {

constexpr double kGridTolerance = 1e-9;
constexpr double kDedupeTolerance = 1e-6;
// The normalised resultant length below which the mean direction and the spread
// are treated as undefined. In exact arithmetic a balanced set such as
// {0,90,180,270} has a resultant of exactly zero; in floating point it has a
// resultant of order 1e-17, which would otherwise be reported as a precise mean
// direction and an enormous finite spread. Both are undefined there, and the
// report says so rather than printing a number derived from rounding noise.
constexpr double kResultantEpsilon = 1e-12;
constexpr double kDegPerRad = 180.0 / std::numbers::pi_v<double>;
constexpr double kRadPerDeg = std::numbers::pi_v<double> / 180.0;

// Angles are printed with enough precision to distinguish entries that
// de-duplication keeps apart (1e-6 degrees), with trailing zeros trimmed so the
// common whole-degree case reads naturally.
std::string format_deg(double a) {
  std::ostringstream os;
  os.precision(6);
  os << std::fixed << a;
  std::string s = os.str();
  const auto dot = s.find('.');
  if (dot != std::string::npos) {
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
  }
  return s;
}

}  // namespace

std::vector<double> generate_angles(double start, double end, double step) {
  std::vector<double> out;
  if (!std::isfinite(start) || !std::isfinite(end) || !std::isfinite(step)) return out;
  if (!(step > 0.0)) return out;
  if (end < start) return out;

  // A bound computed once, so the loop cannot run away on a tiny step, and
  // `start + i*step` rather than repeated addition, so rounding does not
  // accumulate across the sequence.
  const double span = end - start;
  const double max_i = std::floor(span / step) + 2.0;
  if (!std::isfinite(max_i)) return out;
  const long long limit = static_cast<long long>(std::min(max_i, 1.0e6));

  for (long long i = 0; i <= limit; ++i) {
    const double a = start + static_cast<double>(i) * step;
    if (a > end + kGridTolerance) break;
    out.push_back(a);
  }
  return out;
}

std::vector<double> dedupe_and_sort_angles(std::span<const double> angles) {
  std::vector<double> v(angles.begin(), angles.end());
  std::sort(v.begin(), v.end());
  std::vector<double> out;
  out.reserve(v.size());
  for (double a : v) {
    if (out.empty() || std::fabs(a - out.back()) > kDedupeTolerance) out.push_back(a);
  }
  return out;
}

std::optional<std::string> endpoint_notice(double start, double end, double step,
                                           std::span<const double> angles) {
  if (angles.empty()) return std::nullopt;
  if (!std::isfinite(end)) return std::nullopt;
  const double last = angles.back();
  if (std::fabs(last - end) <= kGridTolerance) return std::nullopt;
  std::ostringstream os;
  os << "The end angle " << format_deg(end) << " deg does not lie on the grid "
     << format_deg(start) << " + i*" << format_deg(step)
     << " deg, so the last generated angle is " << format_deg(last)
     << " deg. Choose a step that divides the range if you want the end angle sampled.";
  return os.str();
}

std::vector<std::string> opposed_pair_notices(std::span<const double> angles) {
  std::vector<std::string> out;
  for (std::size_t i = 0; i < angles.size(); ++i) {
    for (std::size_t j = i + 1; j < angles.size(); ++j) {
      if (std::fabs(circular_distance_deg(angles[i], angles[j]) - 180.0) > kDedupeTolerance) {
        continue;
      }
      std::ostringstream os;
      os << "Angles " << format_deg(angles[i]) << " deg and " << format_deg(angles[j])
         << " deg are 180 deg apart. An ideal dipole in free space would receive "
            "identically at the two, so any measured difference between them is "
            "environmental - the feedline, the mount, the wall, or where you stand - "
            "rather than a property of the antenna pattern. Both angles are kept and "
            "measured separately.";
      out.push_back(os.str());
    }
  }
  return out;
}

double normalize_deg(double a) {
  if (!std::isfinite(a)) return a;
  double r = std::fmod(a, 360.0);
  if (r < 0.0) r += 360.0;
  // fmod of a value just below a multiple of 360 can round up to exactly 360.
  if (r >= 360.0) r = 0.0;
  return r;
}

double circular_distance_deg(double a, double b) {
  if (!std::isfinite(a) || !std::isfinite(b)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double d = normalize_deg(std::fabs(a - b));
  return std::min(d, 360.0 - d);
}

double circular_mean_deg(std::span<const double> xs) {
  double s = 0.0;
  double c = 0.0;
  std::size_t n = 0;
  for (double x : xs) {
    if (!std::isfinite(x)) continue;
    s += std::sin(x * kRadPerDeg);
    c += std::cos(x * kRadPerDeg);
    ++n;
  }
  if (n == 0) return std::numeric_limits<double>::quiet_NaN();
  const double r = std::hypot(s, c) / static_cast<double>(n);
  if (!(r > kResultantEpsilon)) return std::numeric_limits<double>::quiet_NaN();
  return normalize_deg(std::atan2(s, c) * kDegPerRad);
}

double circular_spread_deg(std::span<const double> xs) {
  double s = 0.0;
  double c = 0.0;
  std::size_t n = 0;
  for (double x : xs) {
    if (!std::isfinite(x)) continue;
    s += std::sin(x * kRadPerDeg);
    c += std::cos(x * kRadPerDeg);
    ++n;
  }
  if (n == 0) return std::numeric_limits<double>::quiet_NaN();
  const double r = std::hypot(s, c) / static_cast<double>(n);
  if (!(r > kResultantEpsilon)) return std::numeric_limits<double>::quiet_NaN();
  const double clamped = std::min(r, 1.0);
  return std::sqrt(-2.0 * std::log(clamped)) * kDegPerRad;
}

std::optional<VisitOrder> parse_visit_order(std::string_view s) {
  if (s == "forward") return VisitOrder::Forward;
  if (s == "reverse") return VisitOrder::Reverse;
  if (s == "alternating") return VisitOrder::Alternating;
  if (s == "random") return VisitOrder::Random;
  return std::nullopt;
}

std::string_view to_string(VisitOrder o) {
  switch (o) {
    case VisitOrder::Forward: return "forward";
    case VisitOrder::Reverse: return "reverse";
    case VisitOrder::Alternating: return "alternating";
    case VisitOrder::Random: return "random";
  }
  return "forward";
}

std::vector<std::size_t> order_for_round(std::size_t angle_count, VisitOrder order,
                                         int round, std::uint64_t seed) {
  std::vector<std::size_t> idx(angle_count);
  std::iota(idx.begin(), idx.end(), std::size_t{0});

  switch (order) {
    case VisitOrder::Forward:
      break;
    case VisitOrder::Reverse:
      std::reverse(idx.begin(), idx.end());
      break;
    case VisitOrder::Alternating:
      // Round 1 ascends, round 2 descends, alternating onward. This balances
      // the linear component of a time trend across angles. It does not remove
      // the angle-time confound (spec section 4.6).
      if (round % 2 == 0) std::reverse(idx.begin(), idx.end());
      break;
    case VisitOrder::Random: {
      // Independently shuffled per round, and reproducible after the fact from
      // the seed recorded in session metadata.
      std::seed_seq seq{static_cast<std::uint32_t>(seed & 0xFFFFFFFFU),
                        static_cast<std::uint32_t>((seed >> 32) & 0xFFFFFFFFU),
                        static_cast<std::uint32_t>(round)};
      std::mt19937 rng(seq);
      std::shuffle(idx.begin(), idx.end(), rng);
      break;
    }
  }
  return idx;
}

}  // namespace rtlangle
