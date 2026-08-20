#include "experiment/aggregator.h"

#include "core/angle_math.h"
#include "core/statistics.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace rtlangle {
namespace {

// The attempts that count: exactly one per completed visit, and only the ones
// the operator accepted. A superseded attempt is never pooled into its
// successor - that contamination path is what made a failed capture raise the
// angle it was retried at.
bool contributes(const AttemptRecord& a) {
  return a.disposition == Disposition::Accepted && a.status == AttemptStatus::Ok;
}

bool accepted_at_all(const AttemptRecord& a) {
  return a.disposition == Disposition::Accepted;
}

void add_flag(std::vector<std::string>& flags, const std::string& flag) {
  if (std::find(flags.begin(), flags.end(), flag) == flags.end()) flags.push_back(flag);
}

// Ranks the angles that have a score for this metric, highest first. Angles
// without one are absent rather than ranked last: absence is not a low score.
std::vector<double> rank_by(const std::vector<AngleSummary>& angles,
                            const std::optional<double> AngleSummary::*member) {
  std::vector<const AngleSummary*> eligible;
  for (const AngleSummary& a : angles) {
    if ((a.*member).has_value()) eligible.push_back(&a);
  }
  std::stable_sort(eligible.begin(), eligible.end(),
                   [member](const AngleSummary* x, const AngleSummary* y) {
                     return *(x->*member) > *(y->*member);
                   });
  std::vector<double> out;
  out.reserve(eligible.size());
  for (const AngleSummary* a : eligible) out.push_back(a->planned_deg);
  return out;
}

}  // namespace

SessionSummary aggregate(const SessionRecord& rec) {
  SessionSummary summary;
  summary.report_metric = rec.config.report_metric;

  // The angle list of the plan is the index space; grouping by index rather
  // than by a floating-point angle is what keeps two angles a thousandth of a
  // degree apart distinct.
  const std::vector<double>& angles = rec.plan.angles_deg;
  std::map<std::size_t, std::vector<const AttemptRecord*>> by_index;
  for (const AttemptRecord& a : rec.attempts) {
    if (!accepted_at_all(a)) continue;
    by_index[a.angle_index].push_back(&a);
  }

  for (std::size_t index = 0; index < angles.size(); ++index) {
    AngleSummary out;
    out.planned_deg = angles[index];

    std::vector<double> channel_scores;
    std::vector<double> audio_scores;
    std::vector<double> yields;
    std::vector<double> detected;
    std::vector<double> floors;
    std::vector<double> actual_angles;

    const auto it = by_index.find(index);
    if (it != by_index.end()) {
      for (const AttemptRecord* a : it->second) {
        actual_angles.push_back(a->actual_deg);

        if (!contributes(*a)) {
          ++out.n_excluded;
          // Spec section 10.4 warning W8: named data-quality faults present in
          // the ranked data.
          switch (a->status) {
            case AttemptStatus::Clipped: add_flag(out.flags, "clipped"); break;
            case AttemptStatus::NoiseFloorUnreliable:
              add_flag(out.flags, "noise_floor_unreliable");
              break;
            case AttemptStatus::NoiseFloorUnidentifiable:
              add_flag(out.flags, "noise_floor_unidentifiable");
              break;
            case AttemptStatus::InsufficientData:
              add_flag(out.flags, "insufficient_data");
              break;
            case AttemptStatus::Timeout: add_flag(out.flags, "timeout"); break;
            case AttemptStatus::SourceError: add_flag(out.flags, "source_error"); break;
            case AttemptStatus::InsufficientSamples:
              add_flag(out.flags, "insufficient_samples");
              break;
            default: break;
          }
          continue;
        }

        out.n_valid_events += a->valid_event_count;
        if (a->host_dropped_samples > 0) add_flag(out.flags, "host_dropped_samples");
        if (a->clipped_fraction > 0.0) add_flag(out.flags, "clipping_present");
        if (a->audio_insufficient) add_flag(out.flags, "audio_insufficient");
        if (a->angle_deviation_deg > rec.config.angle_deviation_warn_deg) {
          add_flag(out.flags, "angle_deviation");
        }

        if (a->capture_score_channel_db.has_value()) {
          channel_scores.push_back(*a->capture_score_channel_db);
        }
        if (a->capture_score_audio_db.has_value()) {
          audio_scores.push_back(*a->capture_score_audio_db);
        }
        yields.push_back(a->events_per_minute);
        detected.push_back(a->detected_fraction);
        if (a->noise_floor_dbfs.has_value()) floors.push_back(*a->noise_floor_dbfs);
      }
    }

    out.n_captures = static_cast<int>(channel_scores.size());
    out.n_captures_audio = static_cast<int>(audio_scores.size());

    if (!channel_scores.empty()) {
      out.score_channel_db = median(channel_scores).value;
      out.spread_channel = descriptive_spread(channel_scores);
    }
    if (!audio_scores.empty()) {
      out.score_audio_db = median(audio_scores).value;
      out.spread_audio = descriptive_spread(audio_scores);
    }
    if (!yields.empty()) out.yield_events_per_min = median(yields).value;
    if (!detected.empty()) out.detected_fraction = median(detected).value;
    if (!floors.empty()) {
      out.noise_floor_dbfs = median(floors).value;
      const auto [lo, hi] = std::minmax_element(floors.begin(), floors.end());
      out.noise_floor_spread_db = *hi - *lo;
    }

    // Circular statistics for the entered actual angles: a mean of {350, 10} is
    // near 0, not 180, and angles are never folded modulo 180.
    out.actual_mean_deg = circular_mean_deg(actual_angles);
    out.actual_spread_deg = circular_spread_deg(actual_angles);

    if (out.n_captures > 0) {
      out.status = "ranked";
    } else if (out.n_excluded > 0) {
      out.status = "no eligible capture";
    } else {
      out.status = "not measured";
    }

    summary.angles.push_back(std::move(out));
  }

  summary.ranking_channel = rank_by(summary.angles, &AngleSummary::score_channel_db);
  summary.ranking_audio = rank_by(summary.angles, &AngleSummary::score_audio_db);
  summary.ranking_yield = rank_by(summary.angles, &AngleSummary::yield_events_per_min);
  return summary;
}

}  // namespace rtlangle
