#pragma once

#include "core/records.h"

namespace rtlangle {

// Levels one and two of spec section 10.1, plus the detection yield of section
// 9.5.
//
// THE EXPERIMENTAL UNIT IS THE CAPTURE, NOT THE EVENT. Events inside one
// capture share a receiver configuration, a minute of propagation, an antenna
// position, and often a single transmitter, and two frames inside one event are
// 50 percent overlapped by construction. Treating events as independent
// replicates inflates n and makes a spread look tighter than the experiment
// earned, so each accepted `ok` capture contributes exactly one vote to each
// ranking it is eligible for.
//
// The two metrics qualify separately (spec section 9.4.1): an angle with no
// audio-eligible capture is absent from the audio ranking and present in the
// channel one, and n_captures_audio is never assumed equal to n_captures.
//
// It produces no decision. There is no best angle, no resolution, and no test
// statistic anywhere in the result.
SessionSummary aggregate(const SessionRecord&);

}  // namespace rtlangle
