#include "dsp/chain.h"

#include "dsp/fir_design.h"

#include <numeric>

namespace rtlangle::dsp {
namespace {

int largest_divisor_up_to(std::uint32_t value, int limit) {
  for (int d = limit; d >= 2; --d) {
    if (value % static_cast<std::uint32_t>(d) == 0) return d;
  }
  return 0;
}

}  // namespace

std::vector<int> decimation_factors(std::uint32_t sample_rate_hz,
                                    std::uint32_t channel_rate_hz) {
  std::vector<int> out;
  if (channel_rate_hz == 0) return out;
  if (sample_rate_hz % channel_rate_hz != 0) return out;
  std::uint32_t ratio = sample_rate_hz / channel_rate_hz;
  if (ratio < 2 || ratio > 256) return out;

  while (ratio > 1) {
    int f = largest_divisor_up_to(ratio, 8);
    if (f == 0) f = largest_divisor_up_to(ratio, 16);
    if (f == 0) return {};   // a prime factor above 16: no stage can realise it
    out.push_back(f);
    ratio /= static_cast<std::uint32_t>(f);
    if (out.size() > 16) return {};
  }
  return out;
}

Latency Chain::latency() const {
  // Referring every stage's delay to the channel rate:
  //
  //   chain_latency_ch = sum over s of  d_s / prod_{j >= s} M_j
  //
  // The channel filter is the last stage, with M = 1. The sum is rational, and
  // is kept as one, because truncating each term is what reported 8 channel
  // samples where the answer was 8.125.
  std::int64_t den = 1;
  for (const FirDecimator& s : stages) den *= s.factor();
  if (den <= 0) den = 1;

  std::int64_t num = 0;
  std::int64_t suffix = den;   // prod_{j >= s} M_j for the current stage
  for (const FirDecimator& s : stages) {
    const std::int64_t d = static_cast<std::int64_t>(s.group_delay_input_samples());
    num += d * (den / suffix);
    suffix /= s.factor();
  }
  // The channel filter runs at the channel rate, so its delay contributes in
  // full.
  num += static_cast<std::int64_t>(channel.group_delay_input_samples()) * den;

  Latency out;
  const std::int64_t g = std::gcd(num, den);
  out.num = (g > 0) ? num / g : num;
  out.den = (g > 0) ? den / g : den;
  const double exact = static_cast<double>(out.num) / static_cast<double>(out.den);
  out.ceil_samples = static_cast<std::size_t>((out.num + out.den - 1) / out.den);
  out.residual = static_cast<double>(out.ceil_samples) - exact;
  if (out.residual < 0.0) out.residual = 0.0;
  return out;
}

std::size_t Chain::total_taps_at_channel_rate() const {
  // A stage's tap count referred to the channel rate scales the same way its
  // delay does.
  std::size_t den = 1;
  for (const FirDecimator& s : stages) den *= static_cast<std::size_t>(s.factor());
  if (den == 0) den = 1;

  double total = 0.0;
  std::size_t suffix = den;
  for (const FirDecimator& s : stages) {
    total += static_cast<double>(s.taps()) / static_cast<double>(suffix);
    suffix /= static_cast<std::size_t>(s.factor());
  }
  total += static_cast<double>(channel.taps());
  return static_cast<std::size_t>(total + 0.999);
}

std::size_t Chain::process(std::span<const std::complex<float>> raw,
                           std::vector<std::complex<float>>& channel_out,
                           std::vector<float>& audio_out) {
  // The mixer works in place, so the raw block is copied once per call. The
  // copy is what lets the caller keep its own buffer const.
  std::vector<std::complex<float>> a(raw.begin(), raw.end());
  mixer.process(a);

  std::vector<std::complex<float>> b;
  for (FirDecimator& stage : stages) {
    b.clear();
    b.reserve(a.size() / static_cast<std::size_t>(stage.factor()) + 1);
    stage.process(a, b);
    a.swap(b);
  }

  const std::size_t before = channel_out.size();
  channel.process(a, channel_out);
  const std::size_t produced = channel_out.size() - before;

  // The audio path is driven from the channel stream, so the two share every
  // stage above the channel filter and their sample indices differ only by the
  // audio filter's own delay (spec section 8.4).
  audio.process(std::span<const std::complex<float>>(channel_out).subspan(before, produced),
                audio_out);
  return produced;
}

void Chain::reset() {
  mixer.reset();
  for (FirDecimator& s : stages) s.reset();
  channel.reset();
  audio.reset();
}

Chain build_chain(const Config& cfg) {
  Chain chain;
  chain.channel_rate_hz = cfg.channel_rate_hz;
  chain.mixer = OffsetMixer(cfg.sample_rate_hz, cfg.offset_tune_hz);

  const auto factors = decimation_factors(cfg.sample_rate_hz, cfg.channel_rate_hz);
  double rate = static_cast<double>(cfg.sample_rate_hz);
  for (int f : factors) {
    const double out_rate = rate / static_cast<double>(f);
    // Spec section 8.2: passband 0.40*(R/M), stopband 0.50*(R/M), 60 dB.
    auto taps = design_lowpass(rate, 0.40 * out_rate, 0.50 * out_rate, 60.0);
    chain.stages.emplace_back(std::move(taps), f);
    rate = out_rate;
  }

  const double ch_rate = static_cast<double>(cfg.channel_rate_hz);
  const double pass = static_cast<double>(cfg.channel_bw_hz) / 2.0;
  const double stop = pass + 1000.0;
  chain.channel = ChannelFilter(design_lowpass(ch_rate, pass, stop, 60.0));
  chain.audio = AmDemodulator(cfg.channel_rate_hz);
  return chain;
}

}  // namespace rtlangle::dsp
