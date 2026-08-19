#pragma once

namespace rtlangle {

// The value to_db() returns for an argument that has no decibel representation:
// zero, a negative power, or a NaN. It is a large negative sentinel rather than
// -infinity or NaN so that arithmetic on it stays defined and a missing
// measurement can never be mistaken for a measurement of zero. Callers test for
// it explicitly; nothing formats it as a number for the operator.
inline constexpr double kDecibelFloor = -999.0;

// 10*log10(linear). Power ratios, not amplitude ratios.
double to_db(double linear);

// 10^(db/10). The inverse of to_db over the domain where to_db is defined.
double from_db(double db);

// The weakest SNR the squelch can admit, spec section 9.3:
//
//   10*log10(from_db(open_db) - 1)
//
// An event is only detected once its total in-channel power exceeds the noise
// floor by open_db, and the reported SNR subtracts that floor, so nothing below
// this value is ever reported. It is about 4.7407 dB at the default open_db of
// 6.0. Returns kDecibelFloor when open_db leaves nothing above the floor.
double minimum_detectable_snr_db(double open_db);

}  // namespace rtlangle
