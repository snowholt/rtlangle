#pragma once

#include "angle/angle_provider.h"
#include "core/config.h"
#include "core/records.h"
#include "dsp/chain.h"
#include "dsp/spectrum.h"
#include "experiment/capture_runner.h"
#include "metrics/noise_floor.h"
#include "persist/session_store.h"
#include "source/sample_source.h"
#include "ui/terminal_ui.h"

#include <string>

namespace rtlangle {

// The visit loop: position, settle, flush, capture, measure, ask, commit.
//
// `segment_id` is created by the APP layer via
// ISessionStore::begin_receiver_segment and passed in. The controller stamps it
// on every attempt and never creates one, because opening the device is the app
// layer's job on both the new and the resume path.
//
// The controller calls commit_visit(), and - only in response to
// CommitOutcome::Indeterminate - reload_from_disk(). It calls none of pause(),
// finalize(), or abort(): all three take a SessionSummary, and computing one is
// the aggregator's job, above this layer. IT THEREFORE ALWAYS LEAVES THE
// SESSION STATE AT `running`, and signals which terminal or paused state the
// app layer should write by its return value.
class ExperimentController {
 public:
  ExperimentController(ISampleSource& source, IAngleProvider& provider,
                       ui::ITerminalUi& terminal, ISessionStore& store, dsp::Chain& chain,
                       const Config& cfg, std::string segment_id);

  enum class Result { Completed, QuitRequested, Failed };

  Result run();

  // Why the run ended, for the abort reason the app layer records.
  const std::string& failure_detail() const { return failure_detail_; }

 private:
  enum class VisitStep { Advance, Quit, Failed };

  VisitStep run_visit(const Visit& visit, int attempt);

  // Spec section 11.3's mapping from what the capture loop did to what the
  // visit produced. It is total, and only the `ok` row can rank.
  AttemptStatus map_status(const CaptureOutcome&, const metrics::NoiseFloor& floor,
                           int valid_events) const;

  // Applies a CommitResult, returning what the caller should do next.
  VisitStep apply_commit(const VisitCommit&, const CommitResult&);

  bool retry_offered() const;

  ISampleSource&   source_;
  IAngleProvider&  provider_;
  ui::ITerminalUi& terminal_;
  ISessionStore&   store_;
  dsp::Chain&      chain_;
  Config           cfg_;
  std::string      segment_id_;
  dsp::Spectrum    spectrum_;
  std::string      failure_detail_;
};

}  // namespace rtlangle
