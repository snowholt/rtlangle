#pragma once

#include <cstddef>
#include <vector>

namespace rtlangle::dsp {

// Kaiser-window low-pass design from three explicit parameters. There is
// deliberately no single "cutoff" argument: a cutoff is read as -3 dB, -6 dB,
// or the band edge depending on the reader, and spec section 8.2 fixes the
// convention as (passband edge, stopband edge, stopband attenuation).
//
// The returned tap count is always ODD, so the filter has an integer group
// delay of (T-1)/2 at its own input rate - the property the chain latency
// arithmetic of spec section 8.4 depends on. The taps are normalised to unity
// gain at DC.
//
// Returns an empty vector when the edges are not ordered, are not finite, or
// lie outside (0, sample_rate/2).
std::vector<float> design_lowpass(double sample_rate_hz, double passband_edge_hz,
                                  double stopband_edge_hz, double stopband_atten_db);

// The Kaiser shape parameter for a given stopband attenuation, exposed so the
// design can be checked against the textbook formula rather than only against
// its own output.
double kaiser_beta(double stopband_atten_db);

}  // namespace rtlangle::dsp
