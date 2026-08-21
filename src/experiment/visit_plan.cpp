#include "experiment/visit_plan.h"

namespace rtlangle {

std::vector<Visit> build_visit_plan(const Config& cfg) {
  std::vector<Visit> visits;
  const std::vector<double> angles = resolved_angles(cfg);
  if (angles.empty()) return visits;

  for (int round = 1; round <= cfg.rounds; ++round) {
    const auto order = order_for_round(angles.size(), cfg.order, round, cfg.seed);
    for (std::size_t index : order) {
      Visit v;
      v.visit_id = make_visit_id(round, index);
      v.round = round;
      v.angle_index = index;
      v.planned_deg = angles[index];
      visits.push_back(std::move(v));
    }
  }
  return visits;
}

}  // namespace rtlangle
