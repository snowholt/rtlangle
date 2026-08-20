#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rtlangle {

// Angles are bearings of the marked dipole arm in the horizontal plane,
// measured clockwise from a physical 0 degree mark the operator chooses once
// (spec section 4.2). They are directions on a full 360 degree circle and are
// never folded modulo 180: theta and theta+180 are two distinct orientations,
// because the feedline, the mount, and the operator break the symmetry an ideal
// dipole would have.

// The single generation rule (spec section 4.3):
//
//   angles = { start + i*step : i = 0,1,2,... while start + i*step <= end + 1e-9 }
//
// The endpoint is included only when it lies on the grid, so 0:20:90 produces
// {0,20,40,60,80} and not {0,20,40,60,80,90}. Returns an empty vector when
// `step` is not finite and positive, or when `start` exceeds `end`; validation
// turns those into named errors.
std::vector<double> generate_angles(double start, double end, double step);

// Sorted ascending and de-duplicated within 1e-6. This is the index space every
// visit_id refers to (spec section 11.2), so it is computed once and reused.
std::vector<double> dedupe_and_sort_angles(std::span<const double> angles);

// Informational, not a warning and not an error: the requested end angle does
// not lie on the generation grid, so the last generated angle is lower than it.
// Empty when the endpoint was reached exactly.
std::optional<std::string> endpoint_notice(double start, double end, double step,
                                           std::span<const double> angles);

// Informational (spec section 4.4): the set contains a pair (theta, theta+180),
// which an ideal dipole would receive identically. Sampling both is a
// legitimate choice; the message exists so that a measured difference is read
// as environmental rather than as a fault. One message per pair.
std::vector<std::string> opposed_pair_notices(std::span<const double> angles);

// Circular geometry (spec section 4.5). Linear arithmetic is wrong near the
// wrap point: circular_distance_deg(350, 10) is 20, not 340.
double normalize_deg(double a);
double circular_distance_deg(double a, double b);
double circular_mean_deg(std::span<const double> xs);

// Circular standard deviation sqrt(-2 * ln(R)) in degrees, where R is the
// resultant vector length. Returns NaN when R is zero, because the spread is
// undefined there rather than infinite.
double circular_spread_deg(std::span<const double> xs);

enum class VisitOrder { Forward, Reverse, Alternating, Random };

std::optional<VisitOrder> parse_visit_order(std::string_view);
std::string_view to_string(VisitOrder);

// Returns angle INDICES into the sorted, de-duplicated angle list, not angles.
// Indices are what visit_id is built from (spec section 11.2) and what survives
// a resume unchanged; returning doubles would reintroduce the formatting
// collision the older angle-formatted identifier had.
//
// `round` is 1-based. Alternating ascends on odd rounds and descends on even
// ones. Random shuffles independently per round from (seed, round), so the
// realised order is reproducible from the recorded seed after the fact.
std::vector<std::size_t> order_for_round(std::size_t angle_count, VisitOrder order,
                                         int round, std::uint64_t seed);

}  // namespace rtlangle
