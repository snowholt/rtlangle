#include "experiment/quality_checks.h"

#include "core/angle_math.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>

namespace rtlangle {
namespace {

std::string num(double v, int precision = 2) {
  std::ostringstream os;
  os.precision(precision);
  os << std::fixed << v;
  return os.str();
}

AngleSummary* find_angle(SessionSummary& summary, double planned_deg) {
  for (AngleSummary& a : summary.angles) {
    if (std::fabs(a.planned_deg - planned_deg) < 1e-9) return &a;
  }
  return nullptr;
}

void fire(SessionSummary& summary, std::string id, std::string message,
          std::vector<double> angles) {
  for (double a : angles) {
    if (AngleSummary* angle = find_angle(summary, a)) {
      if (std::find(angle->warning_ids.begin(), angle->warning_ids.end(), id) ==
          angle->warning_ids.end()) {
        angle->warning_ids.push_back(id);
      }
    }
  }
  summary.warnings.push_back(Warning{std::move(id), std::move(message), std::move(angles)});
}

const AngleSummary* ranked(const SessionSummary& summary, double planned_deg) {
  for (const AngleSummary& a : summary.angles) {
    if (std::fabs(a.planned_deg - planned_deg) < 1e-9) return &a;
  }
  return nullptr;
}

}  // namespace

std::vector<std::string_view> warning_ids() {
  return {"W1", "W2", "W3", "W4", "W5", "W6", "W7", "W8"};
}

void evaluate_warnings(SessionSummary& summary, const SessionRecord& record,
                       const Config& cfg) {
  const std::vector<double>& ranking = summary.ranking_channel;

  // ---- W1: how much data stands behind a median ---------------------------
  {
    std::vector<double> thin;
    for (double angle : ranking) {
      const AngleSummary* a = ranked(summary, angle);
      if (a != nullptr && a->n_captures < cfg.min_captures_advisory) thin.push_back(angle);
    }
    if (!thin.empty()) {
      std::ostringstream os;
      os << "W1  Fewer than " << cfg.min_captures_advisory
         << " accepted captures stand behind the median at ";
      for (std::size_t i = 0; i < thin.size(); ++i) {
        if (i > 0) os << ", ";
        const AngleSummary* a = ranked(summary, thin[i]);
        os << num(thin[i], 1) << " deg (" << (a != nullptr ? a->n_captures : 0) << ")";
      }
      os << ". Those positions in the ranking may move with one more round.";
      fire(summary, "W1", os.str(), thin);
    }
  }

  // ---- W2: were two angles measured under comparable traffic? -------------
  {
    // The rounds in which each angle has an accepted ok capture.
    std::map<double, std::set<int>> rounds_by_angle;
    std::set<int> all_rounds;
    for (const AttemptRecord& a : record.attempts) {
      all_rounds.insert(a.round);
      if (a.disposition != Disposition::Accepted || a.status != AttemptStatus::Ok) continue;
      rounds_by_angle[a.planned_deg].insert(a.round);
    }
    const std::size_t rounds_run = all_rounds.size();

    for (std::size_t i = 0; i + 1 < ranking.size() && rounds_run > 0; ++i) {
      for (std::size_t j = i + 1; j < ranking.size(); ++j) {
        const auto& first = rounds_by_angle[ranking[i]];
        const auto& second = rounds_by_angle[ranking[j]];
        std::vector<int> shared;
        std::set_intersection(first.begin(), first.end(), second.begin(), second.end(),
                              std::back_inserter(shared));
        if (shared.size() >= rounds_run) continue;
        std::ostringstream os;
        os << "W2  " << num(ranking[i], 1) << " deg and " << num(ranking[j], 1)
           << " deg both have an accepted capture in only " << shared.size() << " of the "
           << rounds_run
           << " rounds run, so part of the difference between them is when they were "
              "measured, not where the antenna pointed.";
        fire(summary, "W2", os.str(), {ranking[i], ranking[j]});
      }
    }
  }

  // ---- W3: is the gap worth acting on? ------------------------------------
  if (ranking.size() >= 2) {
    const AngleSummary* first = ranked(summary, ranking[0]);
    const AngleSummary* second = ranked(summary, ranking[1]);
    if (first != nullptr && second != nullptr && first->score_channel_db.has_value() &&
        second->score_channel_db.has_value()) {
      const double gap = *first->score_channel_db - *second->score_channel_db;
      if (gap < cfg.min_effect_db) {
        std::ostringstream os;
        os << "W3  The two highest-scoring angles differ by " << num(gap)
           << " dB, below the " << num(cfg.min_effect_db)
           << " dB threshold set for a gap worth acting on.";
        fire(summary, "W3", os.str(), {ranking[0], ranking[1]});
      }
    }
  }

  // ---- W4: do the two measurement paths tell the same story? --------------
  if (!summary.ranking_channel.empty() && !summary.ranking_audio.empty()) {
    if (std::fabs(summary.ranking_channel[0] - summary.ranking_audio[0]) > 1e-9) {
      std::ostringstream os;
      os << "W4  The channel ranking is led by " << num(summary.ranking_channel[0], 1)
         << " deg and the audio ranking by " << num(summary.ranking_audio[0], 1)
         << " deg. The two measurement paths are not telling the same story: the channel "
            "metric is a carrier-plus-sideband-to-noise ratio in the channel bandwidth, the "
            "audio metric is measured after demodulation.";
      fire(summary, "W4", os.str(), {summary.ranking_channel[0], summary.ranking_audio[0]});
    }
  }

  // ---- W5: did the noise environment change during the session? -----------
  {
    std::vector<double> floors;
    for (double angle : ranking) {
      const AngleSummary* a = ranked(summary, angle);
      if (a != nullptr && a->noise_floor_dbfs.has_value()) floors.push_back(*a->noise_floor_dbfs);
    }
    if (floors.size() >= 2) {
      const auto [lo, hi] = std::minmax_element(floors.begin(), floors.end());
      const double span = *hi - *lo;
      if (span > cfg.noise_drift_warn_db) {
        std::ostringstream os;
        os << "W5  The pooled noise floors of the ranked angles span " << num(span)
           << " dB, above the " << num(cfg.noise_drift_warn_db)
           << " dB threshold. The noise environment changed during the session, so angles "
              "measured at different times faced different conditions.";
        fire(summary, "W5", os.str(), ranking);
      }
    }
  }

  // ---- W6: the detection-selection effect of spec section 10.0 ------------
  if (!ranking.empty()) {
    const AngleSummary* top = ranked(summary, ranking[0]);
    double best_yield = 0.0;
    double best_yield_angle = 0.0;
    for (double angle : ranking) {
      const AngleSummary* a = ranked(summary, angle);
      if (a == nullptr || !a->yield_events_per_min.has_value()) continue;
      if (*a->yield_events_per_min > best_yield) {
        best_yield = *a->yield_events_per_min;
        best_yield_angle = angle;
      }
    }
    if (top != nullptr && top->yield_events_per_min.has_value() && best_yield > 0.0 &&
        *top->yield_events_per_min < cfg.yield_concordance_ratio * best_yield) {
      std::ostringstream os;
      os << "W6  The highest-scoring angle, " << num(ranking[0], 1) << " deg, heard "
         << num(*top->yield_events_per_min) << " events per minute while "
         << num(best_yield_angle, 1) << " deg heard " << num(best_yield)
         << " - a ratio of " << num(*top->yield_events_per_min / best_yield)
         << ", below the " << num(cfg.yield_concordance_ratio)
         << " expected of concordant angles. A higher score with a lower yield is what "
            "\"heard only the loud ones\" looks like: the reported SNR is conditioned on "
            "detection, and the detection threshold moves with the antenna.";
      fire(summary, "W6", os.str(), {ranking[0], best_yield_angle});
    }
  }

  // ---- W7: was the receiver held constant? --------------------------------
  {
    std::map<std::string, const ReceiverSegment*> segments;
    for (const ReceiverSegment& s : record.receiver_segments) segments[s.segment_id] = &s;

    std::vector<std::string> drifts;
    std::set<double> affected;
    for (const AttemptRecord& a : record.attempts) {
      if (a.disposition != Disposition::Accepted) continue;
      if (a.agc_enabled) {
        drifts.push_back("attempt " + std::to_string(a.attempt) + " of visit " + a.visit_id +
                         " reports AGC enabled");
        affected.insert(a.planned_deg);
        continue;
      }
      const auto it = segments.find(a.segment_id);
      if (it == segments.end()) continue;
      const SourceInfo& base = it->second->baseline;
      // Compared against the SEGMENT BASELINE, not against the request: a
      // device with a coarse gain table snaps every request, and comparing
      // against the request would fire on every capture of every real session.
      if (a.applied_gain_tenth_db != base.applied_gain_tenth_db) {
        drifts.push_back("the applied gain in segment " + a.segment_id + " moved from " +
                         std::to_string(base.applied_gain_tenth_db) + " to " +
                         std::to_string(a.applied_gain_tenth_db) + " tenths of a dB at visit " +
                         a.visit_id);
        affected.insert(a.planned_deg);
      }
      if (a.applied_sample_rate_hz != base.applied_sample_rate_hz) {
        drifts.push_back("the applied sample rate in segment " + a.segment_id + " moved from " +
                         std::to_string(base.applied_sample_rate_hz) + " to " +
                         std::to_string(a.applied_sample_rate_hz) + " Hz at visit " + a.visit_id);
        affected.insert(a.planned_deg);
      }
      if (a.applied_center_hz != base.applied_center_hz) {
        drifts.push_back("the applied centre frequency in segment " + a.segment_id +
                         " moved from " + std::to_string(base.applied_center_hz) + " to " +
                         std::to_string(a.applied_center_hz) + " Hz at visit " + a.visit_id);
        affected.insert(a.planned_deg);
      }
    }
    if (!drifts.empty()) {
      std::ostringstream os;
      os << "W7  The receiver was not held constant: ";
      for (std::size_t i = 0; i < drifts.size(); ++i) {
        if (i > 0) os << "; ";
        os << drifts[i];
      }
      os << ". The comparison is between two receivers as much as between two angles.";
      fire(summary, "W7", os.str(), std::vector<double>(affected.begin(), affected.end()));
    }
  }

  // ---- W8: named data-quality faults in the ranked data -------------------
  {
    std::vector<double> flagged;
    std::ostringstream detail;
    for (double angle : ranking) {
      const AngleSummary* a = ranked(summary, angle);
      if (a == nullptr || a->flags.empty()) continue;
      if (!flagged.empty()) detail << "; ";
      detail << num(angle, 1) << " deg: ";
      for (std::size_t i = 0; i < a->flags.size(); ++i) {
        if (i > 0) detail << ", ";
        detail << a->flags[i];
      }
      flagged.push_back(angle);
    }
    if (!flagged.empty()) {
      fire(summary, "W8",
           "W8  Named data-quality faults are present in the ranked data - " + detail.str() +
               ".",
           flagged);
    }
  }

  // ---- Spec section 10.5 comparability notes ------------------------------
  {
    int insufficient = 0;
    int unreliable = 0;
    int unidentifiable = 0;
    int clipped = 0;
    int failed = 0;
    int truncated_events = 0;
    bool retention_capped = false;
    for (const AttemptRecord& a : record.attempts) {
      if (a.disposition == Disposition::Superseded) continue;
      switch (a.status) {
        case AttemptStatus::InsufficientData: ++insufficient; break;
        case AttemptStatus::NoiseFloorUnreliable: ++unreliable; break;
        case AttemptStatus::NoiseFloorUnidentifiable: ++unidentifiable; break;
        case AttemptStatus::Clipped: ++clipped; break;
        case AttemptStatus::Timeout:
        case AttemptStatus::SourceError:
        case AttemptStatus::InsufficientSamples: ++failed; break;
        default: break;
      }
      truncated_events += a.truncated_event_count;
      if (a.events_total != a.events_retained) retention_capped = true;
    }
    auto note = [&summary](const std::string& text) { summary.notes.push_back(text); };

    if (insufficient + unreliable + unidentifiable + clipped + failed > 0) {
      std::ostringstream os;
      os << "Captures excluded from the ranking: " << insufficient << " with too few events, "
         << unreliable << " with an unreliable noise floor, " << unidentifiable
         << " with an unidentifiable one, " << clipped << " clipped, and " << failed
         << " that failed to capture.";
      note(os.str());
    }
    for (const AngleSummary& a : summary.angles) {
      if (a.n_captures_audio < a.n_captures) {
        note(num(a.planned_deg, 1) + " deg has " + std::to_string(a.n_captures_audio) +
             " audio-eligible captures against " + std::to_string(a.n_captures) +
             " channel-eligible ones, so the two rankings rest on different data there.");
      }
    }
    {
      int lowest = -1;
      int highest = -1;
      for (const AngleSummary& a : summary.angles) {
        if (a.n_captures == 0) continue;
        if (lowest < 0 || a.n_valid_events < lowest) lowest = a.n_valid_events;
        if (a.n_valid_events > highest) highest = a.n_valid_events;
      }
      if (lowest > 0 && highest > 3 * lowest) {
        note("Valid event counts across the ranked angles differ by more than a factor of "
             "three (" + std::to_string(lowest) + " to " + std::to_string(highest) +
             "), so the angles did not hear comparable amounts of traffic.");
      }
    }
    if (record.receiver_segments.size() > 1) {
      note("This session has " + std::to_string(record.receiver_segments.size()) +
           " receiver segments, because it was resumed and the device was reopened between "
           "them.");
    }
    if (!record.durability_warnings.empty()) {
      note("A commit was recorded as durable-in-the-record-but-not-on-disk " +
           std::to_string(record.durability_warnings.size()) +
           " time(s); every measurement is present, but a power loss at that moment could "
           "have lost the most recent one.");
    }
    if (record.plan.rounds == 1) {
      note("One round was run, so angle is fully confounded with time: each angle was "
           "measured once, in sequence.");
    }
    for (const AttemptRecord& a : record.attempts) {
      if (a.disposition != Disposition::Accepted) continue;
      if (a.angle_deviation_deg > cfg.angle_deviation_warn_deg) {
        note("Visit " + a.visit_id + " was accepted with the antenna " +
             num(a.angle_deviation_deg) + " degrees from its planned angle.");
      }
    }
    // A pair (theta, theta+180) whose scores differ: an ideal dipole would
    // receive identically at the two, so a difference is environmental.
    for (std::size_t i = 0; i < summary.angles.size(); ++i) {
      for (std::size_t j = i + 1; j < summary.angles.size(); ++j) {
        const AngleSummary& first = summary.angles[i];
        const AngleSummary& second = summary.angles[j];
        if (!first.score_channel_db.has_value() || !second.score_channel_db.has_value()) continue;
        if (std::fabs(circular_distance_deg(first.planned_deg, second.planned_deg) - 180.0) >
            1e-6) {
          continue;
        }
        const double gap = std::fabs(*first.score_channel_db - *second.score_channel_db);
        if (gap > cfg.noise_drift_warn_db) {
          note(num(first.planned_deg, 1) + " deg and " + num(second.planned_deg, 1) +
               " deg are 180 degrees apart and differ by " + num(gap) +
               " dB. An ideal dipole would receive identically at the two, so the "
               "environment - the feedline, the mount, the wall, where you stood - "
               "dominates the pattern rather than the antenna.");
        }
      }
    }
    if (truncated_events > 0) {
      note(std::to_string(truncated_events) +
           " event(s) were open at the first or last frame of their capture. Their true "
           "extent is unknown, so they are recorded for audit and excluded from every "
           "metric.");
    }
    if (retention_capped) {
      note("At least one capture found more events than the record retains; the metrics "
           "were computed over all of them and only the persisted event list was capped.");
    }
  }
}

}  // namespace rtlangle
