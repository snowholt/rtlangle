#include "app/scan_command.h"

#include "core/db.h"
#include "core/statistics.h"
#include "dsp/spectrum.h"
#include "source/source_factory.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <sstream>

namespace rtlangle::app {
namespace {

constexpr std::size_t kScanFftSize = 1024;
constexpr std::size_t kScanSegments = 8;

std::string mhz(std::uint32_t hz) {
  std::ostringstream os;
  os.precision(4);
  os << std::fixed << static_cast<double>(hz) / 1e6;
  return os.str();
}

}  // namespace

DwellPlan plan_dwells(const Config& cfg, double step_override) {
  DwellPlan plan;
  plan.usable_half_hz = cfg.scan_usable_fraction * static_cast<double>(cfg.sample_rate_hz) / 2.0;
  plan.dc_exclusion_hz = static_cast<double>(cfg.scan_dc_exclusion_hz);
  plan.feasible = plan.usable_half_hz >= 3.0 * plan.dc_exclusion_hz;

  const double u = plan.usable_half_hz;
  const double d = plan.dc_exclusion_hz;
  plan.step_hz = step_override > 0.0 ? step_override : (u - d);
  if (!(plan.step_hz > 0.0)) return plan;

  const double first = static_cast<double>(cfg.scan_start_hz) + u;
  const double last_needed = static_cast<double>(cfg.scan_end_hz) + d;

  for (int i = 0; i < 100000; ++i) {
    const double center = first + static_cast<double>(i) * plan.step_hz;
    plan.centers.push_back(static_cast<std::uint32_t>(center + 0.5));
    if (center >= last_needed) break;
  }
  // At least two dwells are always generated: the hole of a single dwell would
  // otherwise have no neighbour to cover it.
  while (plan.centers.size() < 2) {
    plan.centers.push_back(static_cast<std::uint32_t>(
        static_cast<double>(plan.centers.back()) + plan.step_hz + 0.5));
  }
  return plan;
}

std::vector<std::uint32_t> scan_channels(const Config& cfg) {
  std::vector<std::uint32_t> channels;
  if (cfg.scan_channel_hz == 0 || cfg.scan_end_hz < cfg.scan_start_hz) return channels;
  for (std::uint64_t hz = cfg.scan_start_hz; hz <= cfg.scan_end_hz;
       hz += cfg.scan_channel_hz) {
    channels.push_back(static_cast<std::uint32_t>(hz));
    if (channels.size() > 100000) break;
  }
  return channels;
}

std::vector<ScanChannel> run_scan(ITunableSampleSource& source, const Config& cfg,
                                  ui::ITerminalUi& terminal, std::string& error) {
  error.clear();

  const DwellPlan plan = plan_dwells(cfg);
  const auto channel_grid = scan_channels(cfg);

  std::vector<ScanChannel> channels;
  channels.reserve(channel_grid.size());
  for (std::uint32_t hz : channel_grid) channels.push_back(ScanChannel{hz, 0, 0, std::nullopt});

  std::ostringstream plan_line;
  plan_line << plan.centers.size() << " dwells of " << cfg.scan_dwell_ms << " ms over "
            << cfg.scan_passes << " pass(es), stepping "
            << static_cast<std::uint64_t>(plan.step_hz + 0.5) << " Hz with a usable half-span of "
            << static_cast<std::uint64_t>(plan.usable_half_hz + 0.5)
            << " Hz and a DC exclusion of "
            << static_cast<std::uint64_t>(plan.dc_exclusion_hz + 0.5) << " Hz.";
  terminal.info(plan_line.str());

  dsp::Spectrum spectrum(kScanFftSize, kScanSegments);
  const std::size_t dwell_samples = std::max<std::size_t>(
      kScanFftSize * kScanSegments,
      static_cast<std::size_t>(cfg.scan_dwell_ms / 1000.0 *
                               static_cast<double>(cfg.sample_rate_hz)));
  std::vector<std::complex<float>> buffer(dwell_samples);

  const double bin_hz = static_cast<double>(cfg.sample_rate_hz) / static_cast<double>(kScanFftSize);
  const double hit_threshold = from_db(cfg.open_db);
  const int total_dwells = static_cast<int>(plan.centers.size()) * cfg.scan_passes;
  int done = 0;

  for (int pass = 0; pass < cfg.scan_passes; ++pass) {
    for (std::uint32_t center : plan.centers) {
      if (!source.retune(center, error)) {
        error = "the scan could not retune to " + std::to_string(center) + " Hz: " + error;
        return channels;
      }
      source.flush();

      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      std::size_t got = 0;
      while (got < buffer.size()) {
        const ReadResult r =
            source.read(std::span<std::complex<float>>(buffer).subspan(got, buffer.size() - got),
                        deadline);
        got += r.samples;
        if (r.status == ReadStatus::Ok) continue;
        if (r.status == ReadStatus::Error) {
          error = "the scan source failed at " + std::to_string(center) + " Hz: " + r.error;
          return channels;
        }
        break;
      }
      if (got < buffer.size()) {
        error = "the scan source produced only " + std::to_string(got) + " of " +
                std::to_string(buffer.size()) + " samples at " + std::to_string(center) + " Hz.";
        return channels;
      }

      const auto psd = spectrum.welch_psd(buffer);
      if (psd.empty()) continue;

      // The power in each channel that this dwell can see, and the dwell's own
      // band noise floor: the median of the in-span channel powers.
      std::map<std::size_t, double> powers;
      std::vector<double> in_span;
      for (std::size_t i = 0; i < channels.size(); ++i) {
        const double offset =
            static_cast<double>(channels[i].center_hz) - static_cast<double>(center);
        if (std::fabs(offset) > plan.usable_half_hz) continue;
        // A channel inside the DC exclusion gains nothing from THIS dwell: it
        // is neither a hit nor a miss there.
        if (std::fabs(offset) < plan.dc_exclusion_hz) continue;

        const double bin = offset / bin_hz + static_cast<double>(kScanFftSize) / 2.0;
        const auto low = static_cast<std::ptrdiff_t>(
            std::floor(bin - static_cast<double>(cfg.scan_channel_hz) / 2.0 / bin_hz));
        const auto high = static_cast<std::ptrdiff_t>(
            std::ceil(bin + static_cast<double>(cfg.scan_channel_hz) / 2.0 / bin_hz));
        double power = 0.0;
        for (std::ptrdiff_t k = std::max<std::ptrdiff_t>(0, low);
             k <= std::min<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(psd.size()) - 1, high);
             ++k) {
          power += psd[static_cast<std::size_t>(k)];
        }
        powers[i] = power;
        in_span.push_back(power);
        ++channels[i].exposures;
      }

      const Stat floor = median(in_span);
      if (!floor.valid || !(floor.value > 0.0)) continue;
      for (const auto& [index, power] : powers) {
        if (power > floor.value * hit_threshold) ++channels[index].hits;
      }

      ++done;
      terminal.progress("Scanning", static_cast<double>(done) / std::max(1, total_dwells));
    }
  }

  for (ScanChannel& c : channels) {
    if (c.exposures > 0) {
      c.activity = static_cast<double>(c.hits) / static_cast<double>(c.exposures);
    }
  }
  return channels;
}

int scan_command(const Config& cfg, ui::ITerminalUi& terminal) {
  std::string error;
  auto source = make_source(cfg, error);
  if (source == nullptr) {
    terminal.error(error);
    return 3;   // device not found, busy, or refused
  }
  auto* tunable = dynamic_cast<ITunableSampleSource*>(source.get());
  if (tunable == nullptr) {
    terminal.error(
        "the configured source cannot retune, so it cannot sweep a band. Use --source rtlsdr "
        "or --source synthetic.");
    return 2;
  }

  terminal.heading("Airband scan");
  auto channels = run_scan(*tunable, cfg, terminal, error);
  if (!error.empty()) {
    terminal.error(error);
    return 1;
  }

  std::stable_sort(channels.begin(), channels.end(),
                   [](const ScanChannel& a, const ScanChannel& b) {
                     return a.activity.value_or(-1.0) > b.activity.value_or(-1.0);
                   });

  ui::Table table;
  table.title = "Busiest channels";
  table.headers = {"frequency MHz", "activity", "hits", "exposures"};
  int shown = 0;
  for (const ScanChannel& c : channels) {
    if (shown++ >= cfg.scan_top_n) break;
    std::ostringstream activity;
    if (c.activity.has_value()) {
      activity.precision(1);
      activity << std::fixed << (*c.activity * 100.0) << " %";
    } else {
      // Never 0 percent: this channel was never observed.
      activity << "-";
    }
    table.rows.push_back({mhz(c.center_hz), activity.str(), std::to_string(c.hits),
                          std::to_string(c.exposures)});
  }
  table.notes.push_back(
      "Activity is hits divided by exposures. A channel that no dwell could see reports \"-\" "
      "rather than 0 %.");
  terminal.table(table);

  if (!channels.empty() && channels.front().activity.has_value()) {
    terminal.info("Run the experiment on the busiest channel with:");
    terminal.info("  rtlangle run --freq " + std::to_string(channels.front().center_hz));
  }
  // Scan writes nothing under the session root.
  return 0;
}

}  // namespace rtlangle::app
