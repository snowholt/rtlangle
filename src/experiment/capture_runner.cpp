#include "experiment/capture_runner.h"

#include <algorithm>
#include <cmath>

namespace rtlangle {

std::string_view to_string(CaptureOutcome::Status s) {
  switch (s) {
    case CaptureOutcome::Status::Ok: return "ok";
    case CaptureOutcome::Status::Timeout: return "timeout";
    case CaptureOutcome::Status::SourceError: return "source_error";
    case CaptureOutcome::Status::Clipped: return "clipped";
    case CaptureOutcome::Status::InsufficientSamples: return "insufficient_samples";
    case CaptureOutcome::Status::Cancelled: return "cancelled";
  }
  return "ok";
}

CaptureRunner::CaptureRunner(ISampleSource& source, dsp::Chain& chain, const Config& cfg)
    : source_(source), chain_(chain), cfg_(cfg) {}

CaptureOutcome CaptureRunner::run(const std::function<void(double)>& on_progress) {
  CaptureOutcome out;

  // Rule 7: the applied settings are a snapshot taken at capture start, so a
  // source whose info() changes mid-capture leaves the record at the value that
  // was actually in force when the capture began.
  out.applied = source_.info();
  out.audio_latency_samples = chain_.audio.latency_samples();

  const std::size_t wanted =
      static_cast<std::size_t>(std::llround(cfg_.duration_s *
                                            static_cast<double>(cfg_.channel_rate_hz)));

  // Rule 4: the transient trim uses the CEILING of the exact rational chain
  // latency plus twice the chain's total FIR length at the channel rate, so it
  // can only ever over-trim and never leave part of the transient behind.
  const std::size_t trim =
      chain_.latency_samples() + 2 * chain_.total_taps_at_channel_rate();

  const std::uint64_t dropped_at_start = [&] {
    // A zero-length read is not defined on the interface, so the starting count
    // is taken from the first real read below instead.
    return std::uint64_t{0};
  }();
  bool have_start_drop = false;
  std::uint64_t drop_base = dropped_at_start;

  // Rule 3: the read deadline is duration * factor + slack. Exceeding it is a
  // Timeout, not a hang.
  const double budget_s = cfg_.duration_s * cfg_.read_timeout_factor + cfg_.read_timeout_slack_s;

  // Rule 1: started_utc is sampled BEFORE the first read.
  out.started_utc = std::chrono::system_clock::now();
  const auto steady_start = std::chrono::steady_clock::now();
  const auto deadline =
      steady_start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                         std::chrono::duration<double>(budget_s));

  const std::size_t block = std::max<std::size_t>(
      1024, static_cast<std::size_t>(cfg_.sample_rate_hz) / 16);
  std::vector<std::complex<float>> raw(block);

  std::vector<std::complex<float>> channel;
  std::vector<float> audio;
  channel.reserve(wanted + trim + block);
  audio.reserve(wanted + trim + block);

  CaptureOutcome::Status terminal = CaptureOutcome::Status::Ok;
  std::string terminal_detail;

  while (channel.size() < wanted + trim) {
    const ReadResult r = source_.read(raw, deadline);
    if (!have_start_drop) {
      // Rule 2: the per-capture drop count is the delta across this capture.
      drop_base = r.host_dropped_samples;
      have_start_drop = true;
    }
    if (r.samples > 0) {
      chain_.process(std::span<const std::complex<float>>(raw).subspan(0, r.samples), channel,
                     audio);
    }
    out.host_dropped_samples =
        r.host_dropped_samples >= drop_base ? r.host_dropped_samples - drop_base : 0;

    if (on_progress) {
      const double fraction =
          wanted == 0 ? 1.0
                      : std::min(1.0, static_cast<double>(channel.size()) /
                                          static_cast<double>(wanted + trim));
      on_progress(fraction);
    }

    if (r.status == ReadStatus::Ok) continue;
    if (r.status == ReadStatus::Timeout) {
      terminal = CaptureOutcome::Status::Timeout;
      terminal_detail = "the source produced no samples within " +
                        std::to_string(static_cast<int>(budget_s)) + " s.";
      break;
    }
    if (r.status == ReadStatus::Error) {
      terminal = CaptureOutcome::Status::SourceError;
      terminal_detail = r.error;
      break;
    }
    if (r.status == ReadStatus::EndOfStream) {
      terminal = CaptureOutcome::Status::InsufficientSamples;
      terminal_detail = "the sample stream ended after " + std::to_string(channel.size()) +
                        " of " + std::to_string(wanted + trim) + " channel samples.";
      break;
    }
    if (r.status == ReadStatus::Cancelled) {
      terminal = CaptureOutcome::Status::Cancelled;
      terminal_detail = "the capture was cancelled.";
      break;
    }
  }

  const auto steady_end = std::chrono::steady_clock::now();
  out.wall_duration_s = std::chrono::duration<double>(steady_end - steady_start).count();
  out.clipped_fraction = source_.clipped_fraction();

  if (terminal != CaptureOutcome::Status::Ok) {
    out.status = terminal;
    out.detail = terminal_detail;
    return out;
  }

  // Rule 6: clipping is checked before the sample-count rule, because a clipped
  // capture is a front-end fault whatever its length.
  if (out.clipped_fraction > cfg_.max_clipped_fraction) {
    out.status = CaptureOutcome::Status::Clipped;
    out.detail = "the clipped sample fraction was " + std::to_string(out.clipped_fraction) +
                 ", above the limit of " + std::to_string(cfg_.max_clipped_fraction) +
                 ". Restart the experiment at a lower fixed gain; the gain is never "
                 "changed part-way through a session.";
    return out;
  }

  // Rule 5: after trimming, exactly round(duration_s * channel_rate_hz) channel
  // samples are collected. Fewer means the capture is short, not that it is a
  // shorter measurement.
  if (channel.size() < wanted + trim) {
    out.status = CaptureOutcome::Status::InsufficientSamples;
    out.detail = "only " + std::to_string(channel.size()) + " of " +
                 std::to_string(wanted + trim) + " channel samples were captured.";
    return out;
  }

  out.channel.assign(channel.begin() + static_cast<std::ptrdiff_t>(trim),
                     channel.begin() + static_cast<std::ptrdiff_t>(trim + wanted));
  out.audio.assign(audio.begin() + static_cast<std::ptrdiff_t>(trim),
                   audio.begin() + static_cast<std::ptrdiff_t>(trim + wanted));
  out.status = CaptureOutcome::Status::Ok;
  return out;
}

}  // namespace rtlangle
