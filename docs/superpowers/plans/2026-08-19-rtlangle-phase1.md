# rtlangle Phase 1 Implementation Plan

Date: 2026-08-19
Revision: 4 (descriptive-only rework)
Status: Approved for implementation — independently revalidated 2026-08-19
Spec: `docs/superpowers/specs/2026-08-19-rtl-sdr-antenna-angle-design.md` (revision 4)

**Goal:** A receive-only Linux terminal application that measures AM airband
reception quality at a sequence of manually-set dipole antenna angles, ranks
those angles descriptively, and reports every reason the ranking might mislead.
It does not decide which angle is best (spec §2, decision Q1).

**Architecture:** A layered C++20 static library (`rtlangle_core`) plus a thin
CLI. Five interfaces — `ISampleSource`, `ITunableSampleSource`,
`IAngleProvider`, `ITerminalUi`, `ISessionStore` — keep every measurement,
persistence, and orchestration path testable with no hardware.

**Tech stack:** C++20, CMake 3.24+, Ninja, librtlsdr (rtl-sdr-blog fork),
FFTW3 **single precision** (`fftw3f`), nlohmann/json, doctest, POSIX termios.

Revision 2 replaced the original 18-task sequence with dependency-ordered work
packages; revision 3 corrected seven release-blocking defects in it. Revision 4
follows the product decision that Phase 1 is **descriptive only**. WP10 shrinks
from a decision engine to an aggregator plus a warning list, WP1 loses its
inferential statistics entirely, and WP6 grows to carry commit-outcome semantics
and a directory-fd write protocol. Seven further implementation contradictions
are closed; the R-table at the end maps each to its package and its test.


## How to execute this plan

Work the packages in order. Within a package, write the listed failing tests
first, watch them fail for the stated reason, then implement until they pass.
Each package ends with **one** commit whose message is given; do not create that
commit until every acceptance criterion in the package holds and the
verification commands pass.

Where a package says a value must be *calibrated* (WP5's carrier-prominence
threshold), the number in the spec is a starting point: run the measurement,
use what the measurement says, and record the measured rates in the test file.
Do not adjust a tolerance to make a failing test pass — that is a stop
condition, not a fix.

This plan does not require subagents, worktrees, or any external skill. It is
written to be executed inline by one developer with a review gate at each
package boundary.

## Global constraints

Every package's requirements implicitly include these. Values are copied
verbatim from the spec; where a package needs a different value, the spec is
wrong and must be fixed first.

- **C++20**, `-Wall -Wextra -Wpedantic -Wconversion -Wshadow` on project
  targets. No compiler extensions. `std::numbers::pi_v<double>`, never `M_PI`.
- **Linux only.** termios, POSIX `rename`/`fsync`, `O_NOFOLLOW` are assumed.
- **The tool never transmits.** `rtlsdr_set_bias_tee` is called with a non-zero
  argument only when `Config::bias_tee` is true.
- **AGC stays off, and there is no flag that could turn it on.** Always
  `rtlsdr_set_tuner_gain_mode(dev, 1)` and `rtlsdr_set_agc_mode(dev, 0)`, both
  return codes checked. Gain is the controlled variable and never changes during
  a session. `Config` has no `agc` and no `allow_agc` field (spec decision Q7);
  `SourceInfo::agc_enabled` exists only as read-back evidence that it was off.
- **Nothing decides.** There is no `resolved_best_angle`, no
  `resolution_method`, no `resolution_strength`, no `resolution_reason`, no
  confidence interval, no p-value, and no `primary_metric`. `SessionSummary`
  carries no member whose name contains `resolved`, `resolution`, or `winner`,
  and WP10 asserts that structurally. Spec §15.2 lists the wording that may
  never appear; WP10 greps the rendered report for it.
- **`rename()` is the commit point.** A commit returns a `CommitResult` with one
  of four outcomes (spec §11.1.1); it never throws to signal them, because
  "committed but not durable" and "not committed" are different facts and an
  exception can carry only one.
- **A score is never printed without its yield.** Spec §10.0 makes
  `angle_score` uninterpretable on its own; every table, report, and CSV that
  carries a score carries `events_per_minute` beside it (spec §9.5).
- **Dropped samples are host-side.** The field, the column, and every message
  say `host_dropped_samples`. `librtlsdr` exposes no device overrun counter, so
  calling it one would be false (spec §6.1).
- **One visit outcome is one commit.** The store exposes a single mutator,
  `commit_visit(VisitCommit)`. Nothing may write an attempt, its disposition,
  and its retry intent as separate commits (spec §6.6, §11.4).
- **SNR subtracts before dividing**: `SNR_lin = (P_event - N) / N`, then
  `10*log10`. Never `P_event / N`.
- **FFTW single precision only**: `fftwf_*` with `PkgConfig::FFTW3F`. Never mix
  precisions.
- **Production code never includes anything from `tests/`.** The synthetic
  generator is production code under `src/source/`.
- **No placeholder implementations.** A function that cannot be completed is a
  blocker to report, not a stub to ship.
- Namespace `rtlangle`; sub-namespaces `rtlangle::dsp`, `::metrics`, `::ui`,
  `::app`. `core/`, `source/`, `angle/`, `persist/`, `experiment/` put their
  types directly in `rtlangle`.
- Headers use `#pragma once`; `src/` is the single include root.
- Fixed constants, not to be reinvented: channel rate 32000 Hz, frame 1024
  samples, hop 512 samples, byte conversion `(x - 127.4f) / 127.5f`, NCO table
  65536 entries, `schema_version` 1, hardware-test skip exit code 77.
- Commit messages use Conventional Commits, professional tone, English, no
  emoji.

## File structure

```
CMakeLists.txt                       targets, options, warnings, CTest
cmake/Dependencies.cmake             find_package-first, FetchContent fallback
src/core/db.h|.cpp                   to_db, from_db, minimum_detectable_snr_db
src/core/statistics.h|.cpp           percentile, median, t-quantile, paired CI
src/core/angle_math.h|.cpp           generation, validation, circular distance/mean, visit order
src/core/config.h|.cpp               Config, validate(), JSON round-trip, resume classification
src/core/records.h|.cpp              EventRecord, AttemptRecord, SessionRecord, SessionSummary
src/core/version.h                   tool version string
src/dsp/fir_design.h|.cpp            Kaiser design from (fp, fs, atten)
src/dsp/offset_mixer.h|.cpp          integer-accumulator NCO
src/dsp/fir_decimator.h|.cpp         polyphase decimating FIR
src/dsp/channel_filter.h|.cpp        channel-rate FIR low-pass
src/dsp/am_demodulator.h|.cpp        envelope, DC block, 300-3400 Hz shaping
src/dsp/framer.h|.cpp                frame_powers(span, len, hop)
src/dsp/spectrum.h|.cpp              fftwf Welch PSD, carrier offset, peak prominence
src/dsp/chain.h|.cpp                 build_chain(Config), Chain::process, latency_samples
src/metrics/noise_floor.h|.cpp       two-stage probe floor, persistent-carrier guard
src/metrics/event_detector.h|.cpp    hysteresis squelch, merge, discard, truncation
src/metrics/snr_estimator.h|.cpp     per-event channel and audio SNR -> AttemptRecord
src/source/sample_source.h           ISampleSource, ITunableSampleSource, ReadResult, SourceInfo
src/source/synthetic_source.h|.cpp   production AM-voice generator + tunable synthetic band
src/source/iq_file_source.h|.cpp     .cu8 replay
src/source/rtlsdr_source.h|.cpp      RtlSdrSource (guarded by RTLANGLE_WITH_RTLSDR)
src/source/source_factory.h|.cpp     make_source(Config)
src/angle/angle_provider.h           IAngleProvider, AngleOutcome
src/angle/manual_angle_provider.h|.cpp
src/angle/fixed_angle_provider.h|.cpp
src/ui/terminal_ui.h                 ITerminalUi, MenuItem, Table
src/ui/safe_text.h|.cpp              control-character neutralisation
src/ui/ansi_terminal_ui.h|.cpp       raw mode, signals, colour, arrow-key menu
src/ui/scripted_terminal_ui.h|.cpp   test double
src/ui/report_renderer.h|.cpp        rankings, warnings, resolution wording
src/persist/path_safety.h|.cpp       slug, O_NOFOLLOW open, symlink refusal, size limits
src/persist/atomic_write.h|.cpp      tmp -> fsync -> rename -> dir fsync
src/persist/csv.h|.cpp               RFC 4180 export
src/persist/session_store.h          ISessionStore
src/persist/json_session_store.h|.cpp
src/persist/session_loader.h|.cpp    load, migrate, reconcile, resume diff
src/experiment/visit_plan.h|.cpp     visits, visit_id, completion diffing
src/experiment/capture_runner.h|.cpp source -> chain -> CaptureOutcome
src/experiment/controller.h|.cpp     ExperimentController state machine
src/experiment/aggregator.h|.cpp     capture scores, angle scores, yields, descriptive rankings
src/experiment/quality_checks.h|.cpp warnings W1-W8; no decision, no test statistic
src/app/cli_parser.h|.cpp            argv + JSON file -> Config
src/app/run_command.h|.cpp
src/app/scan_command.h|.cpp
src/app/device_check.h|.cpp
src/app/menus.h|.cpp
src/app/main.cpp
tests/*.cpp                          one file per work package
tests/support/*.h|.cpp               assertions, fixtures, and test doubles only
tests/support/fault_store.h          fault-injecting ISessionStore decorator
tests/support/scripted_capture_source.h   ISampleSource double: production
                                     SyntheticSource driven at a per-visit SNR
tests/support/scripted_angle_provider.h   IAngleProvider double that tells the
                                     scripted source which visit is next
tests/hardware/hardware_test.cpp     separate executable
docs/architecture.md
examples/session-synthetic/          generated example output
README.md
```

---

## WP0 — Requirements reconciliation and acceptance gates

**Purpose.** Freeze the contradictions out of the documents before any code
exists, and write down the gates that decide whether Phase 1 is done.
**Depends on.** Nothing.
**Role / effort.** Product owner with DSP lead — 0.5 day.

**Deliverables.** No source code. A checklist file
`docs/acceptance-gates.md` and a confirmed-clean review of both documents.

**Acceptance criteria.**

1. Both documents state exactly one rule for angle endpoint inclusion, and it is
   `start + i*step <= end + 1e-9`.
2. Neither document claims alternating order "breaks" the angle–time
   correlation.
3. Both documents name five interfaces, including `ISessionStore`.
4. No document claims carrier offset identifies a transmitter or aircraft.
5. No document contains a current SDR++ PID, a `pkill -f` instruction, `M_PI`,
   a bare `fftw3` link target, `--save-iq`, `completed_keys`, or "Approved for
   implementation".
6. **Removed constructs.** No document contains, as a *usage*: `--agc`,
   `allow_agc`, `selected_metric`, `sample_overruns`, a `r1-a<angle>` visit-id
   format, `append_attempt`, `set_disposition`, `record_retry_intent`, or
   `begin_visit` (revision 3 removals); nor `resolved_best_angle`,
   `resolution_method`, `resolution_strength`, `resolution_reason`,
   `primary_metric`, `family_alpha`, `min_captures_for_resolution`,
   `paired_ci95`, `paired_lower_bound`, `t_quantile`, `signflip`, "gate G",
   "statistically resolved", or "Highest measured reception quality"
   (revision 4 removals under decision Q1).
7. **Positive checks.** Both documents contain: an explicit estimand (spec
   §10.0); detection yield printed beside every score (spec §9.5, warning W6);
   the warning list W1–W8 described as *not* a decision ladder; `commit_visit`
   as the store's single mutator returning a `CommitResult`; `rename()` named as
   the commit point with four outcomes; a session `state` of
   `running | paused | completed | aborted` with the controller never writing a
   terminal state; `valid_audio_event_count` and the §9.4.2 three-row floor
   table; reserved retry capacity; the dot-run collapse in label sanitisation;
   the `O_NOFOLLOW` final-component scoping with the `dirfd` protocol; the
   dwell-step rule `2D <= S <= U - D` with its feasibility condition
   `U >= 3D`; and the corrected `A(A+1)/2` persistence arithmetic.
8. `docs/acceptance-gates.md` lists the six Phase 1 completion conditions from
   §1 of `docs/artifacts/2026-08-19-rtlangle-revalidated-plan.md`, each with the
   command that produces its evidence and the three-state outcome vocabulary
   (passed / failed / pending).

**Verification.**

The stale terms appear in both documents *as prohibitions* — "must not contain
`pkill -f`", "never `M_PI`", "there is no `--agc`". A bare grep therefore matches
by design; the check is that every match is a prohibition and none is a usage.
The one exception is `Approved for implementation`: exactly two usages are
permitted, the `Status` line of each document, which records the independent
revalidation of 2026-08-19 that spec §15.2's last entry required. A third
occurrence, or either `Status` line reverting, fails the gate. Review each
hit:

```bash
grep -nE 'pkill|M_PI|save-iq|completed_keys|Approved for implementation|four interfaces' \
  docs/superpowers/specs/*.md docs/superpowers/plans/*.md
grep -nE '\-\-agc|allow_agc|selected_metric|sample_overruns|r1-a[0-9]|append_attempt|set_disposition|record_retry_intent|begin_visit' \
  docs/superpowers/specs/*.md docs/superpowers/plans/*.md
grep -nE 'resolved_best_angle|resolution_(method|strength|reason)|primary_metric|family_alpha|min_captures_for_resolution|paired_(ci95|lower_bound)|t_quantile|signflip|statistically resolved|Highest measured' \
  docs/superpowers/specs/*.md docs/superpowers/plans/*.md
```

The positive checks are the complement, and each must produce at least one hit
in each document:

```bash
grep -nE 'estimand|events_per_minute|commit_visit|CommitOutcome|aborted|W6|valid_audio_event_count|U >= 3D|A\(A\+1\)/2' \
  docs/superpowers/specs/*.md docs/superpowers/plans/*.md
```

The prohibited terms must produce **zero** matches once source and README exist;
that check belongs to WP12 and is written there against `src/` and `README.md`.

**Stop condition.** Any contradiction that cannot be resolved without a product
decision goes back to the owner; do not pick one reading and proceed.

**Commit.** `docs: add Phase 1 acceptance gates`

---


## WP1 — Build system, dependencies, CI, and numeric primitives

**Purpose.** A four-target build that links the correct FFTW precision, builds
with and without RTL-SDR, and runs clean under sanitizers — plus the two
numeric modules everything else depends on.
**Depends on.** WP0.
**Role / effort.** C++ platform engineer — 1 day. (Revision 4 removed the
inferential statistics this package used to carry.)

**Files.** `CMakeLists.txt`, `cmake/Dependencies.cmake`, `src/core/db.*`,
`src/core/statistics.*`, `src/core/version.h`, `tests/test_core_numeric.cpp`,
`tests/hardware/hardware_test.cpp` (skeleton returning 77).

**Interfaces produced.**

```cpp
namespace rtlangle {
double to_db(double linear);                    // 10*log10; !(x>0) -> -HUGE_VAL sentinel, documented
double from_db(double db);                      // 10^(db/10)
double minimum_detectable_snr_db(double open_db); // 10*log10(from_db(open_db) - 1)

// Every helper below is TOTAL over its declared domain: for any input a valid
// Config can produce, it returns a defined value or a named error, never
// undefined behaviour and never a silent NaN. Spec decision Q1 removed the
// inferential helpers entirely - there is no t_quantile, no paired confidence
// bound, and no sign-flip permutation test, because Phase 1 runs no test - so
// this is the whole numeric surface and its totality is checkable by hand.

struct Stat { double value; bool valid; };      // valid == false carries no value

// Linear-interpolated percentile. pct is clamped to [0,100] and the clamp is
// reported; empty input returns valid == false. Never reads out of range.
Stat percentile(std::span<const double> sorted_or_not, double pct);
Stat median(std::span<const double> xs);        // == percentile(xs, 50)

// Descriptive spread only. min-max for n < 4, interquartile for n >= 4
// (spec 10.2). Not a confidence interval, and the type name says so.
struct DescriptiveSpread { double low, high; int n; bool is_iqr, valid; };
DescriptiveSpread descriptive_spread(std::span<const double> xs);
}
```

**Failing tests first** (`tests/test_core_numeric.cpp`):

- `to_db(1.0) == 0`, `to_db(2.0) ≈ 3.0103`, `from_db(to_db(x)) ≈ x` over a
  decade sweep, `to_db(0.0)` and `to_db(-1.0)` return the documented sentinel
  and do not produce NaN.
- `minimum_detectable_snr_db(6.0) ≈ 4.7407` within 1e-3. This number appears in
  the report and the README; pin it here.
- `percentile` against a hand-computed 9-element vector at 0, 20, 50, 95, 100;
  unsorted input gives the same answer as sorted input; empty input is an error,
  not a silent zero.
- **Totality.** A table-driven test drives `percentile`, `median`, and
  `descriptive_spread` with: empty input; one element; two identical elements;
  `pct` at 0, 50, 100, −1, and 101; a vector containing `+inf`, `−inf`, and
  `NaN`; and 10 000 random vectors. Every call returns without UB, and every
  returned `valid == true` carries a finite value. Run under
  `-fsanitize=address,undefined`, which is what makes "no undefined behaviour"
  an assertion rather than a hope. Out-of-range `pct` is clamped and the clamp
  is visible to the caller; it is never silently reinterpreted.
- `percentile` against a hand-computed 9-element vector at 0, 20, 50, 95, 100;
  unsorted input gives the same answer as sorted input.
- `descriptive_spread` on 3 elements reports `is_iqr == false` and the min–max;
  on 4 it reports `is_iqr == true` and the interquartile range; on 0 elements
  `valid == false`.
- **No inferential helper exists.** Asserted by grep in WP12 and by the absence
  of any header declaring one: `grep -rn 't_quantile\|paired_ci\|signflip' src/`
  is part of WP12's stale-term sweep.

**Implementation notes.**

- Colour and terminal helpers do **not** belong here.
- `pkg_check_modules(FFTW3F REQUIRED IMPORTED_TARGET fftw3f)` and link
  `PkgConfig::FFTW3F`. A link smoke test (a translation unit calling
  `fftwf_plan_dft_1d` and `fftwf_destroy_plan`) is part of `rtlangle_tests` so a
  precision regression fails at build time, not at runtime.
- `option(RTLANGLE_WITH_RTLSDR "" ON)`; when OFF, `rtlsdr_source.cpp` is not
  compiled and `make_source` rejects `rtlsdr` with a message naming the option.
- Register `rtlangle_hardware_test` with
  `set_tests_properties(hardware PROPERTIES LABELS hardware SKIP_RETURN_CODE 77)`.
  At this stage it prints `RTLANGLE_HARDWARE=skipped` and returns 77.
- Warnings apply to project targets only; fetched dependencies are included as
  SYSTEM.

**Acceptance criteria.** Both `-DRTLANGLE_WITH_RTLSDR=ON` and `=OFF` configure
and build clean with zero warnings. `ctest -LE hardware` runs and excludes the
hardware executable. `ctest -L hardware` reports **Skipped**, not Passed. The
ASan/UBSan build runs the suite clean.

**Verification.**

```bash
cmake -S . -B build -G Ninja && cmake --build build 2>&1 | grep -i warning   # expect none
ctest --test-dir build -LE hardware --output-on-failure
ctest --test-dir build -L  hardware --output-on-failure                      # expect Skipped
cmake -S . -B build-nohw -G Ninja -DRTLANGLE_WITH_RTLSDR=OFF && cmake --build build-nohw
nm -C build/librtlangle_core.a 2>/dev/null | grep -c fftw_plan_dft_1d          # expect 0
```

**Stop condition.** If `fftw3f` is not installed, stop and report the required
package (`libfftw3-dev` provides both precisions on Ubuntu 24.04); do not fall
back to double precision silently.

**Commit.** `build: add four-target CMake build with fftw3f, sanitizers, and numeric primitives`

---

## WP2 — Configuration, angle mathematics, records, and resource validation

**Purpose.** Every field the CLI, persistence, resume, and reports will use,
defined once, validated once, and round-tripped through JSON — before any
consumer exists. Spec §4, §7, §11.2.
**Depends on.** WP1.
**Role / effort.** Application engineer — 2.5 days.

**Files.** `src/core/angle_math.*`, `src/core/config.*`, `src/core/records.*`,
`tests/test_angle_math.cpp`, `tests/test_config.cpp`.

**Interfaces produced.**

```cpp
namespace rtlangle {
std::vector<double> generate_angles(double start, double end, double step);
double circular_distance_deg(double a, double b);
double circular_mean_deg(std::span<const double> xs);
double circular_spread_deg(std::span<const double> xs);   // NaN when undefined
enum class VisitOrder { Forward, Reverse, Alternating, Random };
// Returns angle INDICES into the sorted, de-duplicated angle list, not angles.
// Indices are what visit_id is built from (spec 11.2) and what survives a
// resume unchanged; returning doubles would reintroduce the formatting
// collision the "r1-a45.00" identifier had.
std::vector<std::size_t> order_for_round(std::size_t angle_count,
                                         VisitOrder order, int round,
                                         std::uint64_t seed);

struct Config { /* every field of spec 7.1-7.6, with the stated defaults */ };
struct ValidationError { std::string field, message; };
std::vector<ValidationError> validate(const Config&);      // includes cross-field rules
void to_json(nlohmann::json&, const Config&);
void from_json(const nlohmann::json&, Config&);

enum class FieldClass { ExperimentDefining, ReportOrOperational, PathDetermined };
FieldClass resume_class(std::string_view field);   // exhaustive; see spec 11.5

// Spec 7.9. Every (field, command) pair is Required, Used, or Refused; a flag
// given to a command that Refuses it is a usage error, not a silent no-op.
enum class Command { Run, Scan, Devices, Report };
enum class FieldScope { Required, Used, Refused };
FieldScope field_scope(std::string_view field, Command);   // exhaustive
std::vector<ValidationError> validate_for_command(const Config& explicitly_set,
                                                  Command);

struct ResourceEstimate {
  int           planned_visits    = 0;
  int           attempt_ceiling   = 0;      // spec 7.7, currently 250
  double        capture_seconds   = 0.0;
  std::uint64_t attempt_bytes     = 0;      // 1.2 KiB + max_retained_events * 0.2 KiB
  std::uint64_t final_json_bytes  = 0;      // attempt_bytes * attempt_ceiling
  std::uint64_t cumulative_bytes  = 0;      // attempt_bytes * A(A+1)/2   <- spec 11.1
  bool          needs_confirmation = false; // capture_seconds > 3600
};
ResourceEstimate estimate_resources(const Config&);
}
```

`records.h` declares `EventRecord`, `AttemptRecord`, `SessionRecord`,
`ReceiverSegment`, `SessionState` (`Running | Paused | Completed | Aborted`),
`PendingRetry`, `VisitCommit`, `CommitOutcome`, `CommitResult`, `AngleSummary`,
`SessionSummary`, `Disposition`, `AttemptStatus`, and `VisitKey`, exactly
matching spec §6.6 and §11.2–§11.3. There is no `AttemptKey`: an attempt is
written once with its final disposition, so nothing ever needs to address an
already-committed attempt. They are declared here, before WP5, WP6, and WP9 need
them, so no later package invents a divergent shape.

`AttemptRecord` carries the applied receiver settings and the `segment_id` that
warning W7 compares them against; the yield fields `events_total`,
`events_retained`, `events_per_minute`, `detected_fraction`; and the audio
eligibility fields `valid_audio_event_count` and `audio_insufficient`
(spec §9.4.1). **Both capture scores are `std::optional<double>`**, because
either may legitimately be absent while the other is present — an `int` or a
sentinel `0.0` here would silently become a measurement of zero.

`SessionSummary` carries no member whose name contains `resolved`,
`resolution`, or `winner`. A test asserts that over its JSON serialisation
(spec §14.2 Test K), so the removal is enforced by the suite and not only by
review.

**Failing tests first.**

- `generate_angles(0,90,15)` equals `{0,15,30,45,60,75,90}`.
- `generate_angles(0,90,20)` equals `{0,20,40,60,80}` — the endpoint is **not**
  appended. This is the single rule; a test asserting the old behaviour is a
  bug in the test.
- `circular_distance_deg(350,10) == 20`, `(10,350) == 20`, `(0,180) == 180`,
  `(0,181) == 179`.
- `circular_mean_deg({350,10})` ≈ 0; `circular_mean_deg({0,90,180,270})` has
  `circular_spread_deg` NaN (undefined resultant).
- **Angles are never folded modulo 180.** `generate_angles(0,180,180)` keeps
  both `0` and `180` as distinct entries and emits the spec §4.4 informational
  message; a test asserts the message text mentions that a difference between
  them is environmental, and asserts the two are **not** merged.
- `order_for_round` with `Alternating` gives ascending indices on round 1,
  descending on round 2; `Random` with a fixed seed is reproducible and is a
  permutation of `0..angle_count-1`.
- Every validation rule in spec §7 produces exactly one `ValidationError` naming
  its field: a table-driven test with one row per rule, asserting the field name
  and that the message contains the offending value.
- **Cross-field rules each produce their own error naming both fields**:
  `channel_bw_hz = 25000` with `channel_rate_hz = 16000`;
  `channel_bw_hz = 2000`; a `sample_rate_hz / channel_rate_hz` ratio of 17;
  a ratio of 1; `|offset_tune_hz| + channel_bw_hz/2 >= 0.45 * sample_rate_hz`;
  and `scan_usable_fraction * sample_rate_hz < 6 * scan_dc_exclusion_hz`. The
  last one is the spec §12.5.1 feasibility condition `U >= 3D`, and the message
  names the minimum `scan_usable_fraction` that would work.
- **`Config` has no AGC field and no decision field.** Asserted directly:
  `to_json(Config{})` contains none of `agc`, `allow_agc`, `primary_metric`,
  `family_alpha`, or `min_captures_for_resolution`, and each is an unknown-flag
  error in WP11. `min_captures_advisory` is present with default 4.
- `to_json`/`from_json` round-trips a fully-populated `Config` with byte-equal
  re-serialisation; an unknown key is rejected with the key named; a JSON file
  over 1 MiB is rejected.
- `resume_class` returns the class spec §11.5 step 6 assigns, for every field
  in that table — table-driven over the full list. Additionally, a test
  enumerates the keys produced by `to_json(Config)` and asserts `resume_class`
  has an entry for each, so a field added later without a classification fails
  the build's tests rather than silently defaulting to overridable.
  **Every §7.4 field is `ReportOrOperational`, with no exception** — asserted
  explicitly as a set equality against §7.4's field list, because §7.4 now holds
  only reporting thresholds and a field that crept back into
  `ExperimentDefining` would mean something in there had become a measurement
  parameter again.
- `field_scope` is likewise exhaustive over `to_json(Config)` × the four
  commands, so a new field with no command scope fails the suite.
  `validate_for_command` reports `--gain` on `report` and any `--scan-*` on
  `run` as errors naming both the flag and the command.
- `estimate_resources` reports the reserved retry capacity of spec §7.7:
  `attempt_ceiling - planned_visits`, which is 236 for the default 14-visit
  experiment and 50 at the 200-visit cap. A table-driven test walks the retry
  boundary at `attempts_committed + remaining_visits` equal to
  `attempt_ceiling - 1`, `attempt_ceiling`, and `attempt_ceiling + 1`, and
  asserts the retry option is offered only in the first case.
- `estimate_resources` for 7 angles × 2 rounds × 60 s reports 14 planned visits
  and 882 s. At `max_retained_events = 32` it reports `attempt_bytes` ≈ 7.6 KiB,
  `final_json_bytes` ≈ 1.9 MiB, and `cumulative_bytes` ≈ 232 MiB, matching the
  spec §11.1 table — the test pins the arithmetic so the number cannot drift
  back into prose. At `max_retained_events = 500` the projection exceeds the
  16 MiB final limit and `validate` returns an error naming both values. Above
  3600 s of planned capture, `needs_confirmation` is set. `rounds * angles`
  above 200 is an error.

**Implementation notes.**

- Generation uses `start + i*step` with an integer `i`, never repeated addition.
- `circular_mean_deg` is `atan2(mean sin, mean cos)` normalised into `[0,360)`.
- Frequency parsing accepts `118.1M`, `121500k`, and bare Hz; reject anything
  else with the token echoed.
- `--angles` overrides the range flags; dedupe within 1e-6 and sort ascending.
  The de-duplicated, sorted list is the index space every `visit_id` refers to.
- `validate_for_command` needs to know which fields were *explicitly set* rather
  than defaulted, so the CLI parser (WP11) records a set of touched keys and
  passes it through. A field left at its default is never a scope error.

**Acceptance criteria.** No field is consumed anywhere in the codebase that is
not present in `Config`, in the JSON round-trip test, in `resume_class`, and in
`field_scope`. A single test enumerates `Config`'s serialised fields once and
drives all three tables from that enumeration.

**Stop condition.** If a field is needed later that is not here, come back and
add it here first, with its bounds, its resume class, its command scope, and its
tests.

**Commit.** `feat: add configuration, angle mathematics, and record schemas`

---


## WP3 — DSP primitives: design, mixing, decimation, filtering, framing

**Purpose.** The signal path, with explicit filter edges, measured latency, and
chunk invariance. Spec §8.
**Depends on.** WP2.
**Role / effort.** DSP engineer — 2.5 days.

**Files.** `src/dsp/fir_design.*`, `offset_mixer.*`, `fir_decimator.*`,
`channel_filter.*`, `am_demodulator.*`, `framer.*`, `spectrum.*`, `chain.*`,
`tests/test_dsp.cpp`.

**Interfaces produced.**

```cpp
namespace rtlangle::dsp {
// Kaiser design from explicit edges. Never a single ambiguous "cutoff".
std::vector<float> design_lowpass(double sample_rate_hz, double passband_edge_hz,
                                  double stopband_edge_hz, double stopband_atten_db);

class OffsetMixer {                       // exp(+j 2 pi f_off t), integer accumulator
public: OffsetMixer(std::uint32_t sample_rate_hz, std::int64_t offset_hz);
        std::int64_t applied_offset_hz() const;
        void process(std::span<std::complex<float>> inout);
        void reset();
};
class FirDecimator {
public: FirDecimator(std::vector<float> taps, int factor);   // taps.size() must be odd
        std::size_t process(std::span<const std::complex<float>> in,
                            std::vector<std::complex<float>>& out);
        // EXACT input-rate delay, always an integer because the tap count is odd.
        // The old group_delay_output_samples() returned (taps-1)/(2*factor) in
        // integer arithmetic and silently truncated: 131 taps at factor 8 is
        // 8.125 output samples, not 8. Referring stage delays to a common rate
        // is Chain's job, and Chain keeps the result rational (spec 8.4).
        std::size_t group_delay_input_samples() const;    // (taps-1)/2
        int         factor() const;
        void reset();
};
class ChannelFilter { /* same shape, factor 1 */ };
class AmDemodulator {
public: AmDemodulator(std::uint32_t channel_rate_hz);
        void process(std::span<const std::complex<float>> in, std::vector<float>& audio_out);
        std::size_t latency_samples() const;   // audio low-pass FIR group delay
        void reset();
};
std::vector<double> frame_powers(std::span<const std::complex<float>> x,
                                 std::size_t len, std::size_t hop);
std::vector<double> frame_powers(std::span<const float> x, std::size_t len, std::size_t hop);

// spec 8.4: the chain latency referred to the channel rate is a RATIONAL
// number. Keep it exact and let callers choose how to round.
struct Latency {
  std::int64_t num = 0, den = 1;   // exact latency in channel samples = num/den
  std::size_t  ceil_samples = 0;   // ceil(num/den); the transient trim uses this
  double       residual = 0.0;     // ceil_samples - num/den, in [0,1)
};

struct Chain {
  OffsetMixer mixer; std::vector<FirDecimator> stages; ChannelFilter channel; AmDemodulator audio;
  Latency     latency() const;              // spec 8.4 formula, exact
  std::size_t latency_samples() const;      // == latency().ceil_samples
  std::size_t process(std::span<const std::complex<float>> raw,
                      std::vector<std::complex<float>>& channel_out,
                      std::vector<float>& audio_out);
  void reset();
};
Chain build_chain(const Config&);
// Deterministic factorisation of sample_rate/channel_rate into factors in [2,16];
// returns empty when no such factorisation exists, which spec 7.1 makes a
// configuration error rather than a runtime surprise.
std::vector<int> decimation_factors(std::uint32_t sample_rate_hz,
                                    std::uint32_t channel_rate_hz);

class Spectrum {                            // fftwf, Welch averaging
public: Spectrum(std::size_t fft_size, std::size_t segments);
        std::vector<double> welch_psd(std::span<const std::complex<float>> x);
        double peak_prominence_db(std::span<const double> psd, double half_band_bins);
        double carrier_offset_hz(std::span<const double> psd, double bin_hz); // parabolic interp
};
}
```

**Failing tests first.**

- `design_lowpass(32000, 4000, 5000, 60)`: measured response is within 0.5 dB of
  unity below 4000 Hz and below −60 dB above 5000 Hz, evaluated by direct DTFT
  at 200 frequencies. Assert the tap count is odd (linear phase, integer group
  delay).
- **Chunk invariance**, one test per stateful block and one for the whole
  `Chain`: processing 1 000 000 samples in one call and in pseudo-random chunks
  of 1..8192 produces bit-identical output. This is normative because the
  capture loop delivers whatever the device yields.
- **Odd tap counts**: every filter `design_lowpass` returns has an odd length,
  asserted for the decimation stages, the channel filter, and the audio
  low-pass, because `Latency` assumes an integer per-stage delay.
- **Group delay, exact**: an impulse through each block peaks at
  `group_delay_input_samples()`; `Chain::latency()` returns a rational whose
  `ceil_samples` and `residual` are mutually consistent
  (`residual == ceil_samples - double(num)/den`, `0 <= residual < 1`); and the
  measured impulse peak at the channel rate equals `floor(num/den)` **or**
  `ceil(num/den)`. The assertion is a two-value set on purpose: a fractional
  latency cannot land on one integer, and demanding that it does is what makes
  a correct implementation look broken and invites a widened tolerance.
- **A fractional case is constructed deliberately**: a chain whose stage tap
  counts make `num/den` non-integral (for example 131 taps at factor 8 feeding
  65 taps at factor 4) is built in the test, and `residual > 0` is asserted, so
  the rational path is exercised rather than accidentally integral.
- `decimation_factors(1024000, 32000)` is `{8,4}`; `(1024000, 64000)` is `{8,2}`;
  a ratio of 17 returns empty; the result is deterministic across calls.
- **Mixer**: a complex tone at `-offset` lands within one FFT bin of DC; after
  60 s at 1.024 MHz the accumulator phase error is below 1e-6 cycles (compare
  against exact `int64` arithmetic); a non-representable requested offset yields
  an `applied_offset_hz` differing by less than one accumulator step.
- **Decimation aliasing**: a tone placed in the stopband of stage 1 appears at
  the channel rate at least 55 dB below an in-band tone of equal amplitude.
- **Byte conversion**: `(0-127.4)/127.5`, `(127-127.4)/127.5`,
  `(255-127.4)/127.5` exactly as computed in double then cast, at the three
  rails and the midpoint.
- **Framer**: `frame_powers` of a unit-magnitude constant is all 1.0; frame
  count for `N` samples is `1 + (N-len)/hop`; a shorter-than-one-frame input
  yields an empty vector, not a partial frame.
- **Spectrum**: a single tone's `carrier_offset_hz` is recovered within 0.1 bin
  by parabolic interpolation; `welch_psd` of white noise has a peak-to-median
  ratio whose distribution is recorded (this feeds WP5's calibration).

**Implementation notes.**

- Decimation stages: passband `0.40*(R/M)`, stopband `0.50*(R/M)`, 60 dB.
  Factors for 1.024 MHz → 32 kHz are `{8, 4}`; the factorisation helper picks
  the largest available small factor for the first stage and is deterministic.
- `AmDemodulator`: envelope `|x|`; 25 Hz single-pole high-pass removes the
  carrier DC term; 3400 Hz FIR low-pass; 300 Hz single-pole high-pass. Only the
  FIR contributes to `latency_samples()`; the IIR residual is absorbed by WP5's
  guard interval, and a test asserts the residual is under 50 ms.
- `Spectrum` owns its `fftwf_plan` and is not copyable. Plan creation is not
  thread-safe in FFTW; construct plans on one thread.

**Acceptance criteria.** All of the above pass. `Chain::latency_samples()` is
used by WP6's transient trim and by WP5's audio alignment; it must be
measurement-verified here, not asserted from a formula alone.

**Stop condition.** If chunk invariance fails, the bug is in filter state
handling; do not paper over it by requiring fixed-size blocks in the capture
loop.

**Commit.** `feat: add DSP chain with explicit filter edges, measured latency, and chunk invariance`

---

## WP4 — Sources and timeout-aware capture

**Purpose.** The source contract with deadlines, cancellation, worker-error
wakeup, and the capture runner's timing and health rules. Spec §6.1, §6.3, §8.5.
**Depends on.** WP3.
**Role / effort.** C++ engineer — 2 days.

**Files.** `src/source/sample_source.h`, `synthetic_source.*`,
`iq_file_source.*`, `source_factory.*`, `src/experiment/capture_runner.*`,
`tests/test_sources.cpp`, `tests/test_capture_runner.cpp`.

**Interfaces produced.** `ISampleSource`, `ITunableSampleSource`, `ReadResult`,
`ReadStatus`, `SourceInfo`, `CaptureOutcome` exactly as in spec §6.1–§6.3, plus:

```cpp
namespace rtlangle {
// Production generator. Lives in src/ because SyntheticSource is a production
// source type; tests may call it, but production never includes tests/.
struct SyntheticParams {
  double snr_db = 12.0, duty = 0.25, talk_s = 3.0, gap_s = 9.0;
  std::uint64_t seed = 1;
  std::vector<std::pair<std::uint32_t,double>> band_transmitters; // for scan tests
};
class SyntheticSource final : public ITunableSampleSource {
public:
  explicit SyntheticSource(SyntheticParams);
  // Production API used by the WP12 test double to vary SNR per visit without
  // the controller or any production type knowing that varying is possible.
  // Changing it takes effect at the next flush() boundary, so a capture is
  // never generated at two different SNRs.
  void set_snr_db(double);
  /* ISampleSource / ITunableSampleSource ... */
};

struct IqFileMetadata {              // spec 7.5, from an optional <path>.cu8.json
  std::optional<std::uint32_t> sample_rate_hz, center_hz;
};
class IqFileSource   final : public ISampleSource { /* ... */ };
std::unique_ptr<ISampleSource> make_source(const Config&, std::string& error);

class CaptureRunner {
public: CaptureRunner(ISampleSource&, dsp::Chain&, const Config&);
        CaptureOutcome run(std::function<void(double)> on_progress);
};
}
```

**Failing tests first.**

- `SyntheticSource` with a fixed seed is byte-reproducible across two
  constructions; changing the seed changes the output.
- A read whose deadline has passed returns `Timeout` with `samples == 0` and
  **does not block** — assert wall time under 100 ms with a deadline in the past.
- `cancel()` from another thread wakes a blocked read within 200 ms and it
  returns `Cancelled`.
- A source whose worker thread fails returns `Error` with non-empty `error`,
  and a reader blocked at the time of failure is woken rather than left to time
  out — assert the return is `Error`, not `Timeout`.
- `IqFileSource` on a known 64-byte `.cu8` fixture yields the exact expected
  complex values; a symlinked path is refused (uses WP6's `path_safety`, so this
  test is written here and enabled when WP6 lands — note the dependency in the
  test name).
- **`.cu8` metadata and lifecycle** (spec §7.5), one test each:
  - with no sidecar, `info()` reports the `Config`'s `sample_rate_hz`,
    `center_hz`, `gain_tenth_db`, and `ppm` as **both** `requested_*` and
    `applied_*`, `driver == "iq-file"`, and `device_name` equal to the basename;
  - a `<path>.cu8.json` agreeing with the config is accepted; one disagreeing on
    `sample_rate_hz` is a configuration error naming both values; one over 1 MiB
    or reached through a symlink is refused;
  - **the stream is consumed sequentially across captures**: two successive
    captures from one file return different samples, and the second continues
    where the first stopped — asserted by comparing against the file bytes
    directly. A rewind between captures is the failure this pins.
  - exhausting the file yields `EndOfStream`, the capture in progress ends
    `InsufficientSamples`, and **every subsequent capture does too** rather than
    the file looping;
  - a file whose length is not a whole number of I/Q pairs drops the odd
    trailing byte and reports it exactly once, not once per read.
- `clipped_fraction` counts bytes at 0 and 255 exactly: a fixture with 3 rail
  bytes in 1000 reports 0.003.
- **CaptureRunner timing**: `started_utc` is captured before the first read —
  assert with a source that sleeps 300 ms before its first return and check
  `started_utc` precedes the read's completion.
- **Host drops are per-capture deltas**: a source reporting cumulative
  `host_dropped_samples` of `{5, 9}` across two captures yields `5` then `4`,
  not `5` then `9`. A separate test asserts the field is named
  `host_dropped_samples` everywhere it surfaces and that no message calls it a
  device overrun (spec §6.1, §15.2).
- **Applied settings are snapshotted**: `CaptureOutcome::applied` equals
  `source.info()` as it was at capture start; a source whose `info()` changes
  mid-capture leaves the recorded snapshot at the start value, which is what
  warning W7 needs to compare against the segment baseline.
- **Transient trim**: with a chain of known latency, the first
  `Chain::latency_samples() + 2*taps_total` channel samples are absent from the
  output — the **ceiling** of the rational latency (spec §8.4), so the trim can
  only over-remove; assert by feeding a step function and checking the output
  begins in steady state.
- **Sample count**: exactly `round(duration_s * channel_rate)` channel samples,
  and `InsufficientSamples` when the source ends early.
- **Clipping**: `clipped_fraction` above `max_clipped_fraction` yields status
  `Clipped` and no metrics are computed.
- **Timeout**: a source that stalls yields `Timeout` after
  `duration_s * 1.5 + 5 s` and not later.

**Implementation notes.**

- `RtlSdrSource` is **not** in this package; the async worker and named device
  errors are WP7. Everything here must be exercisable with no hardware.
- The bounded ring buffer between worker and reader uses one mutex and one
  condition variable; the worker sets an error flag and notifies before exiting,
  which is what makes the wakeup test pass.
- `make_source` parses `rtlsdr` / `synthetic` / `file:<path>` and rejects
  `rtlsdr` with a clear message when `RTLANGLE_WITH_RTLSDR=OFF`.

**Acceptance criteria.** Every `ReadStatus` value is produced by at least one
test. No test in this package requires hardware.

**Stop condition.** A test that hangs is a contract violation, not a slow test.
Fix the deadline handling rather than raising the CTest timeout.

**Commit.** `feat: add sample sources and timeout-aware capture runner`

---

## WP5 — Noise-floor reliability, event detection, and SNR

**Purpose.** The measurement itself: the two-stage probe floor that replaces the
circular check, hysteresis detection with correct ordering and truncation, the
subtracting SNR, latency-compensated audio, and the detection yield that makes
the SNR interpretable. Spec §9.
**Depends on.** WP2 (records), WP3 (chain, framer, spectrum), **WP4**
(`CaptureOutcome`, which `estimate_snr` takes by reference — revision 2 listed
only WP2 and WP3 and could not have compiled).
**Role / effort.** DSP engineer with a statistics reviewer — 3 days. This is the
package that decides whether the tool measures what it claims; do not compress
it.

**Files.** `src/metrics/noise_floor.*`, `event_detector.*`, `snr_estimator.*`,
`tests/test_noise_floor.cpp`, `tests/test_event_detector.cpp`,
`tests/test_snr_estimator.cpp`.

**Interfaces produced.**

```cpp
namespace rtlangle::metrics {
enum class FloorStatus { Reliable, Unreliable, Unidentifiable };
struct NoiseFloor {
  FloorStatus status = FloorStatus::Reliable;
  double power = 0.0;             // linear, at the configured percentile
  double probe_power = 0.0;       // linear, at the probe percentile
  double active_probe_fraction = 0.0;
  double dynamic_range_db = 0.0;
  double carrier_persistence = 0.0;
};
NoiseFloor estimate_noise_floor(std::span<const double> frame_powers,
                                std::span<const std::complex<float>> channel,
                                const Config&, dsp::Spectrum&);

struct Event {
  std::size_t first_frame = 0, last_frame = 0;
  bool truncated = false, valid = false;
};
std::vector<Event> detect_events(std::span<const double> frame_powers,
                                 const NoiseFloor&, const Config&);

// Fills the metric fields of an AttemptRecord. Never invents a value when a
// floor is not Reliable or when an event is invalid. Also fills the spec 9.5
// yield fields, computed from the same event list and frame count:
//   events_total       = number of valid, non-truncated events (ALL of them)
//   events_retained    = min(events_total, cfg.max_retained_events)
//   events_per_minute  = events_total / (duration_s / 60)
//   detected_fraction  = frames inside a valid event / frames_total
// Percentiles and the capture score are computed over ALL valid events; only
// the persisted `events` vector is truncated (spec 11.3).
//
// The two metrics qualify SEPARATELY (spec 9.4.1). `channel_floor` and
// `audio_floor` are estimated independently and either may be unreliable.
//   valid_event_count       -> capture_score_channel_db  (needs >= min_valid_events)
//   valid_audio_event_count -> capture_score_audio_db    (needs >= min_valid_events)
// Whichever count falls short leaves ITS score as std::nullopt; a short audio
// count additionally sets audio_insufficient. Attempt status describes the
// CHANNEL path only.
void estimate_snr(const CaptureOutcome&, const NoiseFloor& channel_floor,
                  const NoiseFloor& audio_floor, std::span<const Event>,
                  const Config&, AttemptRecord& out);
}
```

**Failing tests first.**

The order here matters: write Test D before the estimator exists, so the first
implementation cannot accidentally ship `P/N`.

**Read this before writing Test B or Test D.** Both exercise a true SNR of
0 dB. At 0 dB the *total* in-channel power is `10*log10(1+1) = 3.01 dB` above
the noise floor, which is **below** the production default `open_db = 6.0`, so
the squelch never opens, no event is ever detected, and there is no median to
assert on. Revision 2 wrote both tests against the default configuration and
neither could have passed. Every low-SNR case therefore uses a test-only
squelch:

```cpp
Config low_squelch = cfg;        // legal under spec 7.3 bounds: open > close > 0
low_squelch.open_db  = 1.0;      // ~6.7 sigma above the mean noise frame power
low_squelch.close_db = 0.5;      // for 1024-sample frames, so false opens are
                                 // negligible over a 60 s capture
```

and every one of them is paired with a **production-default** case asserting the
documented behaviour at `open_db = 6.0`: a 3 dB signal produces zero events, and
the reported reason names the `minimum_detectable_snr_db` limit rather than
claiming the channel was quiet. Do not raise `open_db` back to the default in a
low-SNR case to "make it pass", and do not lower the production default to make
the test simpler — the two settings answer two different questions.

- **Test D — the forbidden metric.** With `low_squelch`, construct frames with
  `P_event = 2N` exactly, i.e. a true SNR of 0 dB. Assert the reported median
  channel SNR is within 1.0 dB of 0.0, and assert explicitly that it is **not**
  within 0.5 dB of 3.0103 dB:

  ```cpp
  CHECK(std::fabs(rec.channel_snr_db.median - 0.0) < 1.0);
  // 10*log10(P/N) with P = 2N reports 3.0103 dB. It must not.
  CHECK(std::fabs(rec.channel_snr_db.median - 3.0103) > 0.5);
  ```

  Paired case, production default: the same frames under `cfg` yield zero valid
  events and `AttemptStatus::InsufficientData`.

- **Test A — estimator in isolation, tolerance 0.5 dB.** Build channel-rate
  frames from complex Gaussian noise of known variance with a known signal added
  over a known frame subset. Derive the reference **by measurement** from the
  separated signal-only and noise-only streams
  (`test::mean_power_over(...)`), not from the nominal input SNR. Asserting
  against the nominal value would put the test inside the estimator's own bias
  budget: the 20th-percentile floor reads about 0.12 dB low for 1024-sample
  frames, which biases the reported SNR high by the same amount before anything
  is actually wrong.
- **Test B — full chain, tolerance 1.5 dB.** Generate at 1.024 MHz: complex AM
  carrier at the tuning offset, 1 kHz tone modulation at known depth, plus
  complex AWGN. Push through the real `build_chain(cfg)`. Reference = the same
  signal-only and noise-only streams through the *identical* chain, mean output
  powers. Sweep 0, 5, 10, 20 dB **with `low_squelch`**, since the 0 dB and 5 dB
  points sit at or below the default squelch limit. Additionally assert
  adjacent-step differences track the true differences within 1.0 dB, so a
  constant bias cannot hide a scale error. Paired case, production default: at
  `open_db = 6.0` the 20 dB point is detected and the 0 dB point is not.
- **Test C — negative control.** Pure noise: zero valid events,
  `AttemptStatus::InsufficientData`, no SNR emitted, and `FloorStatus::Reliable`
  (pure noise is a perfectly good floor; it must not be flagged unreliable).
- **Test E — reliability Monte Carlo, ≥200 seeded trials per case.**
  - pure noise → `Unreliable` in under 1 % of trials;
  - 90 % occupancy → `Unreliable` in over 99 % of trials. Construct this by
    generating frame powers where 90 % of frames are 12 dB above the noise
    level; the superseded algorithm reported 0 % activity here, so this test is
    the direct regression guard;
  - constant unmodulated carrier at 6 dB → `Unidentifiable`, and
    `estimate_snr` emits no SNR;
  - record the measured false-positive and detection rates in a comment in the
    test file, and set `carrier_prominence_db` to the value that achieves them.
    The spec's 10.0 dB is a starting point, not a verified constant.
- **Test F — audio SNR, absolute, tolerance 2.0 dB.** Known modulation depth and
  noise level through `AmDemodulator`; reference measured from separated
  streams; sweep at least three input levels. A monotonicity-only assertion does
  not satisfy this test.
- **Test G — audio alignment.** A 400 ms burst: assert the guarded audio window
  lies entirely inside the transmission after shifting by
  `audio_latency_samples`; assert an event whose guarded window falls below
  `min_audio_window_ms` yields `audio_snr_valid == false` while remaining valid
  for the channel metric; assert residual misalignment from the uncompensated
  IIR sections is below `audio_guard_ms`.
- **Test H — event detection.** Exact count and boundaries for bursts at known
  frames; a gap under `merge_gap_ms` merges two bursts into one; a burst under
  `min_event_ms` is discarded **after** merging (assert both orderings
  explicitly: a 200 ms + 100 ms gap + 200 ms pattern survives as one 500 ms
  event); hysteresis prevents fragmentation for a burst hovering between open
  and close; an event open at frame 0 or at the last frame is `truncated` and
  `valid == false`.
- **Minimum detectable SNR.** A signal at 3 dB true SNR produces zero events at
  the default 6 dB squelch, and the reported reason names the squelch limit
  rather than claiming the angle was quiet.
- **Audio eligibility (spec §9.4.1), one test per row of the §9.4.2 table.**
  - 5 channel-valid events of which 1 is audio-eligible, `min_valid_events = 3`
    → channel score present, `capture_score_audio_db == std::nullopt`,
    `audio_insufficient == true`, status `ok`. The status must **not** become
    `insufficient_data`: that word describes the channel path.
  - channel floor reliable, **audio floor unreliable** → same shape, and
    `status_detail` names the audio floor. This is the row revision 3 omitted,
    and without it the same capture could be argued into two statuses.
  - channel floor unreliable → *both* scores absent and status
    `noise_floor_unreliable`, regardless of the audio floor.
  - `valid_audio_event_count <= valid_event_count` asserted as an invariant over
    a fuzz corpus of event lists.
  - the three causes of audio ineligibility (§9.4.1) each produce it in
    isolation: short guarded window, `S_a <= N_a`, unreliable audio floor.
- **Yield fields (spec §9.5).** For a capture with 7 valid events in 60 s,
  `events_per_minute == 7.0` and `events_total == 7`; with
  `max_retained_events = 4`, `events_retained == 4` while `events_total`,
  `events_per_minute`, the capture score, and every percentile are **unchanged**
  — asserted by comparing field by field against the uncapped run. This is the
  test that stops the retention cap from becoming a measurement change.
  `detected_fraction` equals the fraction of frames inside a valid event,
  checked against a hand-counted fixture.

**Implementation notes.**

```cpp
// (P - N) / N in the linear power domain, then convert to dB.
// P / N would be SNR + 1 - the total-channel-power quantity that is
// explicitly not SNR. Events at or below the floor are discarded rather
// than reported as a negative or infinite number.
if (!(p_event > nf.power)) { ev.valid = false; continue; }
const double snr_lin = (p_event - nf.power) / nf.power;
const double snr_db  = 10.0 * std::log10(snr_lin);
```

- Two-stage floor, in this order: probe at `min(5, noise_percentile)`, compute
  `active_probe_fraction` **against the probe floor**, apply R1, then R2, then
  compute the reported floor at `noise_percentile`. Never use the reported floor
  to judge its own validity.
- `detect_events` merges before discarding. Write it in that order and let the
  test for the 200/100/200 pattern hold you to it.
- `estimate_snr` returns without touching the metric fields when
  `nf.status != Reliable`. There is no partial result.

**Acceptance criteria.** Tests A–H pass at the stated tolerances with the
stated seeds. No tolerance is widened to achieve a pass. The carrier-prominence
constant in `Config`'s default matches the value the Monte Carlo justified.

**Stop condition.** If Test B cannot reach 1.5 dB, the fault is in the chain or
the framing, not in the tolerance. Report it and fix the cause.

**Commit.** `feat: add two-stage noise-floor estimation, event detection, and linear-domain SNR`

---

## WP6 — Canonical persistence, attempt state, resume, and hardening

**Purpose.** `session.json` as the single source of truth, with a single atomic
visit-commit, receiver segments, the session lifecycle, durable retry intent,
derived CSV, migration, and the filesystem hardening. Spec §6.6, §11, §13.
**Depends on.** WP2 (records, config).
**Role / effort.** Application / reliability engineer — 4 days. (Revision 4
added the four-outcome commit contract, its reconciliation path, and the
directory-fd write protocol.)

**Files.** `src/persist/path_safety.*`, `atomic_write.*`, `csv.*`,
`session_store.h`, `json_session_store.*`, `session_loader.*`,
`tests/test_persist.cpp`, `tests/test_resume.cpp`,
`tests/support/fault_store.h` (the fault-injecting `ISessionStore` decorator).

**Interfaces produced.** `ISessionStore` exactly as spec §6.6 — one mutator,
`commit_visit(const VisitCommit&)`, plus `begin_receiver_segment`, `pause`,
`finalize`, and `flush_exports`. Plus:

```cpp
namespace rtlangle {
// Spec 13.3. Guarantees the output contains no '/', no substring "..", and is
// neither "." nor ".."; an input that cannot satisfy that yields an empty
// string, which the caller drops. The dot-run collapse in step 2 is what makes
// ".." unrepresentable - revision 3 stripped only LEADING dots and let "a..b"
// through while claiming it could not.
std::string slugify_label(std::string_view);            // [A-Za-z0-9._-]{0,32}

// Spec 13.2. Takes a directory descriptor and a SINGLE path component; asserts
// the name contains no '/'. O_NOFOLLOW guards the final component only, which
// is exactly what this signature can express - the old
// open_no_symlink(const path&, ...) took a multi-component path and so implied
// a protection it could not provide.
int open_at_no_symlink(int dirfd, std::string_view name, int flags, mode_t);

// Opens the session directory once, after mkdir(0700) succeeded, with
// O_DIRECTORY|O_NOFOLLOW|O_RDONLY. Every later write goes through this fd.
class SessionDir {
public:
  static std::optional<SessionDir> create(const std::filesystem::path&, std::string& err);
  static std::optional<SessionDir> open_existing(const std::filesystem::path&, std::string& err);
  int fd() const;
};

// tmp -> fsync(file) -> renameat(dirfd) -> fsync(dirfd), returning WHICH of the
// four spec 11.1.1 outcomes occurred rather than a bool.
CommitResult write_atomic_at(int dirfd, std::string_view name,
                             std::string_view bytes);

struct LoadResult {
  SessionRecord record;
  std::vector<std::string> repairs;    // "removed stale tmp", "regenerated csv",
                                       // "session was not shut down cleanly"
  std::optional<std::string> error;    // set when the session must not be reopened
};
LoadResult load_session(const std::filesystem::path& dir);
std::vector<VisitKey> remaining_visits(const SessionRecord&);
std::vector<ValidationError> resume_conflicts(const Config& stored, const Config& cli);
}
```

**Failing tests first.**

- **Atomicity.** After every `commit_visit`, `session.json` parses and no
  `session.json.tmp.*` remains. Assert by committing 50 attempts in a loop and
  parsing after each.
- **The retry is one commit, and the fault matrix proves it.** This is the
  defect this package exists to close. Revision 2 required a retry to supersede
  the attempt *and* set `pending_retry` in one commit while exposing
  `append_attempt`, `set_disposition`, and `record_retry_intent` as three
  separate calls, which cannot satisfy that requirement. An attempt is now
  written once, with its final disposition: a retry commits the just-finished
  attempt as `superseded` alongside `pending_retry`, so there is no
  already-committed record to go back and change. Build that `VisitCommit`, then
  inject a fault at each of the six boundaries below. After **every** injection
  assert the two invariants:

  ```
  I1: no visit has more than one attempt whose disposition is
      `accepted` or `abandoned`
  I2: pending_retry is non-null  <=>  the named visit's latest attempt has
      disposition `superseded`
  ```

  Both invariants must hold for the pre-commit record and the post-commit
  record, and no intermediate state may be observable. `load_session` asserts
  I1 and I2 on open and reports a violation as `LoadResult::error`, not as a
  repair.
- **Fault injection asserts the exact `CommitOutcome`, not just "recoverable".**
  Spec §11.1.1 fixes which boundary yields which value; a test that only checked
  recoverability would pass on an implementation that reported a durable commit
  as a failure, and the caller would then retry and duplicate the attempt.

  | Injection point | Required `CommitOutcome` | Additional assertion |
  |---|---|---|
  | before temp write | `NotCommitted` | `session.json` byte-identical to before |
  | after temp write, before `fsync(file)` | `NotCommitted` | byte-identical; stray tmp removed on reopen and reported in `repairs` |
  | at `fsync(file)` | `NotCommitted` | as above |
  | at `renameat`, classifiable error | `NotCommitted` | as above |
  | at `fsync(dirfd)`, rename having succeeded | **`CommittedNotDurable`** | the attempt **is** readable afterwards; a `durability_warnings` entry is recorded; the caller does **not** retry |
  | during CSV regeneration | **`Committed`** | attempt present and durable; CSV rebuilt on next open; `repairs` names it |
  | `renameat` returns an unclassifiable error | **`Indeterminate`** | caller runs §11.1.2 before anything else |

  Every case additionally asserts invariants I1 and I2. The
  `CommittedNotDurable` row has its own regression note: revision 3's "a throw
  means nothing was committed" would have classified it as a failure.
- **`EINTR` is retried inside the wrappers, and only `EINTR`.** Inject a signal
  during `openat`, `write`, and `fsync` and assert the commit still returns
  `Committed`: a signal must never become a `CommitOutcome`. `renameat` is not
  interruptible on Linux, so the commit point is never reached in a retry loop
  and the retry cannot create commit ambiguity; assert no wrapper loops on it.
  Assert that `EIO` from `fsync` is **not** retried and classifies by the matrix
  above — `NotCommitted` at step 2, `CommittedNotDurable` at step 4.
- **`Indeterminate` reconciliation (spec §11.1.2), three cases.** With the
  attempt present on disk, `reload_from_disk` succeeds and the caller continues
  as `Committed`; with it absent, as `NotCommitted`; with `reload_from_disk`
  itself failing or the reloaded record violating I1 or I2, the app layer writes
  `state: aborted` with `abort_reason` and `finished_utc`, and a later resume of
  that directory is refused.
- **Both derived artifacts are recoverable (spec §11.1.3).** Truncate
  `measurements.csv`, reopen, and assert it is regenerated from the JSON and
  matches attempt-for-attempt; delete it entirely and assert the same; corrupt
  it with invalid UTF-8 and assert the same. Delete or truncate `report.txt` and
  assert `rtlangle report <dir>` reproduces it from `session.json` alone and
  that nothing ever reads it back. The CSV header contains
  `host_dropped_samples`, `valid_audio_events`, `audio_insufficient`,
  `events_per_minute`, `detected_fraction`, `segment_id`, `angle_index`, and the
  three `applied_*` columns, and does **not** contain `sample_overruns`.
- **Receiver segments.** `begin_receiver_segment` appends a segment with the
  applied baseline and returns its id; every attempt committed afterwards
  carries that id; a second call (as a resume would make) appends a second
  segment and later attempts carry the new id. A record whose attempt names a
  segment that does not exist is a load error.
- **Session lifecycle, four states.** `pause(partial)` sets `state: paused`,
  `summary_partial: true`, leaves `finished_utc` JSON `null`, and writes a
  `report.txt` whose first line marks it `PARTIAL`. `finalize(summary)` sets
  `completed`, `summary_partial: false`, and stamps `finished_utc`.
  `abort(partial, reason)` sets `aborted`, `summary_partial: true`,
  `abort_reason`, stamps `finished_utc`, and marks the report
  `ABORTED — PARTIAL`. Loading a `completed` **or `aborted`** session returns
  `LoadResult::error` naming `finished_utc` (and `abort_reason` where present);
  loading a `paused` one succeeds, clears the summary, and returns `state` to
  `running`; loading a `running` one succeeds and adds "session was not shut
  down cleanly" to `repairs`.
- **Retry durability.** Commit a retry, simulate a crash by discarding the
  in-memory store, reload, and assert `remaining_visits` starts at that visit
  and that the next attempt number is 2.
- **Completion rules.** A visit with an `accepted` non-`ok` attempt is complete;
  a visit with only `superseded` attempts is not; an `abandoned` (skipped) visit
  is complete.
- **Superseded attempts are inert.** Construct a record with a `superseded`
  30 dB attempt and an `accepted` 10 dB attempt at one angle and assert every
  consumer sees 10 — the aggregator input, the CSV rank columns, and
  `remaining_visits`.
- **Event retention.** An attempt whose `events` vector is capped at
  `max_retained_events` round-trips with `events_total > events_retained`, and
  the loader never reconstructs the missing events or treats `events_retained`
  as the true count.
- **Schema.** `schema_version: 2` is rejected with both versions named; a
  synthetic version-0 fixture migrates through the migration function and is
  then loadable.
- **Label sanitisation (spec §13.3).** Table-driven over every worked example in
  the spec table, plus a property test: **for no input does `slugify_label`
  return a string containing `..`**, over a fuzz corpus that includes `a..b`,
  `...`, `a...b`, `.` × 40, and 10 000 random byte strings. `a..b -> a.b` is the
  direct regression guard — revision 3's rule stripped only leading dots while
  `.` was inside its allowed set, so `a..b` passed through unchanged under a
  claim that `..` "can never appear". Also assert the assembled directory name
  is rejected when it contains `/` or equals `.` or `..`.
- **Filesystem hardening (spec §13.2).**
  - `SessionDir::create` on an existing path fails with `EEXIST`, including when
    that path is a symlink, and nothing is written through it.
  - With the `dirfd` held, replacing an **intermediate** directory component
    mid-session does not redirect a write: swap a parent directory and assert
    the bytes still land in the original inode. This is the property revision 3
    claimed for `O_NOFOLLOW` alone and could not deliver.
  - A symlink at the final component of the config path, a `file:` source, or a
    `.cu8.json` sidecar is refused with the path named.
  - A `session.json` over 64 MiB is refused. Directory mode 0700, file mode
    0600 — assert with `stat`.
- **CSV escaping.** A note containing `a,b`, `he said "hi"`, and an embedded
  newline round-trips through the CSV per RFC 4180 and is byte-identical in the
  JSON.
- **Resume conflicts.** Every `ExperimentDefining` field produces a conflict
  when overridden; every `ReportOrOperational` field does not; every
  `PathDetermined` field is silently taken from the resume path. Table-driven
  over the full field list from WP2. **Every §7.4 field is asserted by name not
  to produce a conflict**, as a set equality against §7.4's list: they are
  reporting thresholds applied at render time, so overriding one on a resume is
  legitimate and changes only which warnings appear. Revision 3 had an
  exception here, `primary_metric`, which decision Q8 removed along with the
  test it guarded. A stored `angle_provider.automated` that differs from the
  resumed invocation's provider is refused.
- **Attempt ceiling with reserved capacity (spec §7.7).** The store refuses a
  commit past `attempt_ceiling` with a message naming it, and the refusal does
  not corrupt the record — the committed attempts still load. Separately, the
  *retry offer* is withheld earlier, once
  `attempts_committed + remaining_visits == attempt_ceiling`, so every remaining
  visit can still take its one attempt and the session always reaches its last
  visit. A test drives a session to that boundary and asserts it completes.
  Revision 3 said the tool would "offer to finalise" here, which the controller
  cannot do (spec §6.6) and which would have discarded takeable visits.

**Implementation notes.**

- Write order is fixed and testable: temp (`O_CREAT|O_EXCL|O_WRONLY|O_NOFOLLOW`,
  0600) → `fsync(file)` → `rename` → `fsync(dir)`. The directory `fsync` is what
  makes the rename itself durable; omitting it is a real bug on power loss.
- `commit_visit` applies the whole `VisitCommit` to an in-memory copy of the
  record, serialises **that copy**, and only swaps it into place after the
  directory `fsync` returns. This is what makes the transaction all-or-nothing:
  a throw anywhere in the write leaves the live record untouched.
- Temp names are `session.json.tmp.<pid>`, not a fixed predictable name.
- The fault-injecting decorator lives under `tests/support/` — it is a test
  double, not a generator, so it belongs there.
- Notes are stored verbatim. Control-character neutralisation is WP8's
  `safe_text` at render time only.
- The whole document is rewritten on every commit. Spec §11.1 gives the
  arithmetic and §7.7 the ceilings that keep it cheap; do not add a journal
  here, and do not raise the ceiling without redoing that arithmetic.

**Acceptance criteria.** Every fault boundary has a test, and invariants I1 and
I2 are asserted after every one of them. `load_session` never returns a record
that lost a committed attempt and never returns one that violates I1 or I2. No
production file under `src/` includes anything from `tests/`.

**Stop condition.** If a fault-injection case loses data or leaves a half-applied
`VisitCommit`, stop; do not proceed to the controller with a lossy or
non-atomic store.

**Commit.** `feat: add canonical session store with atomic visit commits, receiver segments, and fault recovery`

---


## WP7 — RTL-SDR source, retunable scan source, and hardware evidence

**Purpose.** The real device, behind the contracts frozen in WP4, plus the
separate hardware-test executable with machine-readable outcomes. Spec §6.1–6.2,
§7.8, §14.3.
**Depends on.** WP4.
**Role / effort.** Hardware integration engineer — 2 days.

**Files.** `src/source/rtlsdr_source.*`, `src/app/device_check.*`,
`tests/hardware/hardware_test.cpp`, `tests/test_rtlsdr_offline.cpp`.

**Interfaces produced.** `RtlSdrSource final : public ITunableSampleSource`,
`DeviceList enumerate_devices()`, `GainTable gain_table(int index)`.

**Failing tests first.**

Hardware-free tests (in `rtlangle_tests`):

- Named error mapping: `usb_claim_interface error -6` maps to
  `"RTL-SDR device N is busy - close the SDR application that is using it and
  try again."`; `-ENODEV` maps to a not-found message listing the devices that
  were enumerated. Test the mapping function directly with the error codes; no
  device required.
- Gain snapping: given the Blog V4 gain table as a fixture, `max` selects the
  highest entry and an arbitrary request snaps to the nearest entry; the
  snapped value is what is reported as `requested`→`applied`.
- `SourceInfo` requested/applied divergence is reported: a fake librtlsdr shim
  returning a different applied rate produces a warning string naming both.
- With `RTLANGLE_WITH_RTLSDR=OFF`, `make_source("rtlsdr", ...)` fails with a
  message naming the CMake option, and the translation unit is not compiled.

Hardware test (`rtlangle_hardware_test`, separate executable):

- Prints exactly one of `RTLANGLE_HARDWARE=passed|skipped|failed`.
- Returns 77 when no device is present or the device is busy; returns 0 on a
  successful receive; returns 1 when a device opened but an assertion failed.
- Asserts: one second of samples read, sample distribution not constant,
  `clipped_fraction` below the limit, applied gain equals the snapped request,
  `rtlsdr_set_tuner_gain_mode(dev,1)` and `rtlsdr_set_agc_mode(dev,0)` both
  returned success and `info().agc_enabled` is false, and
  `rtlsdr_set_bias_tee` was never called with a non-zero argument.

**Implementation notes.**

- `rtlsdr_read_async` on a worker thread feeding the bounded ring buffer built
  in WP4. On worker exit for any reason, set the error state and notify — the
  WP4 wakeup test covers this path.
- Check **every** librtlsdr return code and name the failing call in the error.
- Read back sample rate, centre frequency, and tuner gain after setting them;
  populate both `requested_*` and `applied_*`. Snapping is expected and produces
  an informational message, not a warning — spec §10.4's W7 compares applied
  values against the *segment baseline*, not against the request, so a device
  with a coarse gain table does not raise a spurious warning on every capture.
- AGC is disabled unconditionally. The offline tests drive `RtlSdrSource`
  through a librtlsdr shim that records every call and its arguments, and assert
  that opening a device always calls `rtlsdr_set_tuner_gain_mode(dev, 1)` and
  `rtlsdr_set_agc_mode(dev, 0)`, that neither is ever called with the opposite
  argument, and that both return codes are checked. There is no configuration
  that changes this, because `Config` has no AGC field (spec Q7).
- `retune()` calls `rtlsdr_set_center_freq` then discards the ring buffer, so no
  sample from the previous frequency can leak into the next dwell.
- Bias tee: only when `Config::bias_tee`; interactive confirmation is WP8/WP11.

**Acceptance criteria.** `ctest -LE hardware` still passes with no device
attached and does not execute this binary. `ctest -L hardware` reports
**Skipped** with no device, and the printed line says `skipped`. A skipped
result is never described as passed anywhere.

**Stop condition.** If a device is present but busy, that is `skipped`, not
`failed`, and hardware validation remains pending. Do not work around a busy
device by weakening the test.

**Commit.** `feat: add RTL-SDR source, retuning, and machine-readable hardware test`

---

## WP8 — Terminal interface and angle providers

**Purpose.** The interactive coloured menu, signal-safe raw mode, control
character neutralisation, and the two angle providers. Spec §6.4–6.5, §12.3–12.4.
**Depends on.** WP2.
**Role / effort.** UI engineer — 2 days.

**Files.** `src/ui/terminal_ui.h`, `safe_text.*`, `ansi_terminal_ui.*`,
`scripted_terminal_ui.*`, `src/angle/*`, `tests/test_ui.cpp`,
`tests/test_angle_provider.cpp`, `tests/test_terminal_pty.cpp`.

**Interfaces produced.** `ITerminalUi` and `IAngleProvider` exactly as spec
§6.4–6.5, plus `ScriptedTerminalUi` with `push_line`, `push_menu_choice`,
`push_confirm`, `push_cancel`, and `emitted()` returning every rendered line.

**Failing tests first.**

- `safe_text("\x1b[31mred")` yields a visible `\x1B` form and no live escape;
  UTF-8 above U+007F passes through unchanged; DEL and C1 are neutralised.
- Menu navigation through `ScriptedTerminalUi`: arrow down twice then Enter
  returns index 2; `q` returns −1; an out-of-range initial index clamps.
- `prompt_line` returns the default on blank input and the typed value
  otherwise.
- **Non-TTY safety**: with stdin a pipe, `interactive()` is false, `menu()`
  reads a numbered line, and `tcsetattr` is never called — assert with a shim
  counting `tcsetattr` invocations.
- **PTY signal test** (`tests/test_terminal_pty.cpp`): open a pseudo-terminal,
  run a menu, deliver `SIGINT`, and assert the terminal attributes after the
  handler match the attributes before raw mode was entered. Assert the saved
  state lives at namespace scope by taking its address in the test — a handler
  must never reference a destroyed stack object.
- **Angle provider**: blank input accepts the planned angle; non-numeric
  re-prompts; a value outside `[0,360]` or non-finite re-prompts; a deviation
  beyond `max_angle_deviation_deg` requires confirmation and a declined
  confirmation re-prompts rather than accepting; `Retry`, `Skip`, `Quit`, and
  `Add a note` each produce the right `AngleOutcome`; the prompt on attempt 2
  says "Retry this angle" and never "previous".
- `FixedAngleProvider` returns `Proceed` with `actual_deg == planned_deg` and
  never touches the UI — assert `emitted()` is empty.

**Implementation notes.**

- Saved `termios` lives in a namespace-scope struct with an
  `std::atomic<bool> valid` flag. `RawMode` sets it on construction, clears on
  destruction. Handlers call only `tcsetattr`, `write`, and re-raise with the
  default disposition — all async-signal-safe.
- Colour is enabled only when `isatty(STDOUT_FILENO)`, `NO_COLOR` is unset, and
  `--no-color` was not given. Write the colour predicate as a rule and assert
  the rule, not a fixed expectation:
  `CHECK(color_enabled(false) == (::isatty(STDOUT_FILENO) != 0));` — CTest does
  not guarantee whether stdout is a pipe.
- Settling is **not** in the provider; the controller applies it (WP9).

**Acceptance criteria.** The PTY test passes in the sanitizer build. No UI code
is reachable from `metrics/` or `dsp/`.

**Commit.** `feat: add signal-safe terminal interface and angle providers`

---

## WP9 — Experiment controller and accepted-attempt state machine

**Purpose.** The visit → position → settle → capture → measure → commit loop,
with retry, skip, quit, pause, and crash-resume semantics that match WP6's model
exactly. Spec §11.3–11.5, §12.4.
**Depends on.** WP4, WP5, WP6, WP8.
**Role / effort.** Application engineer — 2.5 days.

**Files.** `src/experiment/visit_plan.*`, `src/experiment/controller.*`,
`tests/test_visit_plan.cpp`, `tests/test_controller.cpp`.

**Interfaces produced.**

```cpp
namespace rtlangle {
struct Visit {
  std::string visit_id;      // "r1-i003" - round, then the ANGLE INDEX, 3 digits
  int         round = 0;
  std::size_t angle_index = 0;
  double      planned_deg = 0.0;   // full precision, never re-derived from the id
};
std::vector<Visit> build_visit_plan(const Config&);

class ExperimentController {
public: // `segment_id` is created by the app layer via
        // ISessionStore::begin_receiver_segment and passed in. The controller
        // stamps it on every attempt and never creates one, because opening the
        // device is the app layer's job on both the new and the resume path.
        ExperimentController(ISampleSource&, IAngleProvider&, ITerminalUi&,
                             ISessionStore&, dsp::Chain&, const Config&,
                             std::string segment_id);
        // The controller calls commit_visit() and, only in response to
        // CommitOutcome::Indeterminate, reload_from_disk(). It calls none of
        // pause(), finalize(), or abort(): all three take a SessionSummary, and
        // computing one is WP10's job, above this layer (spec 6.6).
        // It therefore ALWAYS leaves the session state at Running.
        enum class Result { Completed, QuitRequested, Failed };
        Result run();
};
}
```

**Why the identifier changed.** Revision 2 used `"r1-a45.00"`, formatting the
angle to two decimals while spec §4.3 de-duplicates only angles differing by
more than `1e-6`. `--angles "45.001,45.002"` therefore produced two planned
angles sharing one `visit_id`, and on resume the second visit would have been
treated as the first — a silent loss of one measurement per collision. The index
form cannot collide, and `planned_deg` is persisted separately at full precision.

**Failing tests first.**

All use `SyntheticSource`, `FixedAngleProvider` or a scripted
`ManualAngleProvider`, `ScriptedTerminalUi`, and a real `JsonSessionStore` in a
temporary directory.

- `build_visit_plan` for 7 angles × 2 rounds alternating yields 14 visits, round
  1 ascending, round 2 descending, and unique `visit_id` values of the form
  `r<round>-i<3-digit index>`.
- **Identifier collision regression.** `--angles "45.001,45.002"` yields two
  visits per round with distinct `visit_id` values and distinct `planned_deg`
  values that differ by 0.001; a resume after committing the first completes the
  second rather than skipping it.
- A clean run produces 14 attempts, all `accepted`, all `ok`, returns
  `Result::Completed`, and **leaves `state: running`**. Assert the state
  directly: writing `completed` is WP11's job, and a controller that did it here
  would be finalising a session whose summary it may not compute. Assert with a
  counting decorator that `finalize`, `pause`, and `abort` were each called zero
  times, for every one of the three `Result` values.
- **Settling is the controller's job**: assert `settle_s` elapses between the
  provider returning `Proceed` and the first read, and that `flush()` is called
  exactly once per capture, after settling and before the first read.
- **The controller stamps the segment id it was given** on every attempt it
  commits, and never calls `begin_receiver_segment` itself. Assert with a
  counting store decorator that the count of `begin_receiver_segment` calls made
  during `run()` is zero.
- **Retry**: a scripted `Retry` after an `insufficient_data` attempt produces
  two attempts for that visit, the first `superseded`, the second `accepted`.
  Assert with a counting store decorator that the retry cost **exactly one**
  `commit_visit` call, not three — and that the single call's argument carried
  `disposition == superseded` *and* `pending_retry == {visit_id, 2}` together.
  Assert also that the first attempt is never observed with any other
  disposition: it is written `superseded` the first and only time it is written.
- **Accept as-is**: declining the retry leaves one `accepted` attempt with a
  non-`ok` status, and the visit is complete.
- **Skip**: produces one `abandoned` attempt with status `skipped`; the visit is
  complete for resume.
- **Quit ends the loop and decides nothing**: a scripted `Quit` commits the
  `abandoned`/`cancelled` attempt — **one** `commit_visit` — and returns
  `Result::QuitRequested` with the state still `running`. The `pause()` write is
  a *second, separate* commit made later by the app layer, and WP11 asserts it;
  spec §11.3.1 is explicit that these are two commits and not atomic with each
  other. Revision 2 finalised inside the controller and then had WP12 resume the
  finalised session, which spec §11.5 now refuses.
- **`Indeterminate` handling.** A store decorator returning
  `CommitOutcome::Indeterminate` makes the controller call `reload_from_disk`
  before anything else. With the attempt **present** it advances. With the
  attempt **absent** it enters the `NotCommitted` ladder (spec §11.1.1) instead
  of re-committing, and `run()` returns `Result::Failed`. With `reload_from_disk`
  itself failing it also returns `Result::Failed` without writing any state. Two
  of the three cases therefore end the run; WP11 aborts in both.
- **`CommittedNotDurable` is not a failure.** A decorator returning it makes the
  controller record the warning and **advance**; assert the attempt appears
  exactly once, because a retry here would duplicate it.
- **A final `NotCommitted` ends the run without advancing (spec §11.1.1).** A
  decorator returning `CommitOutcome::NotCommitted` from `commit_visit` makes
  `run()` return `Result::Failed` at that visit. Assert three observables: the
  angle provider's `request()` count stops at the failing visit, so no later
  visit was started; `session.json` holds **zero** attempts for that `visit_id`,
  so nothing was duplicated and nothing left half-written; and `commit_visit`
  was called exactly once for that visit, because the controller does not
  re-issue a commit that reported nothing written. The state on disk is still
  `running` — writing `aborted` is WP11's job.
- **Resume after a pause** reopens the same directory, returns `state` to
  `running`, clears the partial summary, and — with the app layer supplying a
  **second** segment id — completes the remaining visits. The final record
  contains every visit exactly once and two receiver segments, and each
  attempt's `segment_id` matches the segment that was live when it was taken.
- **Retry capacity is reserved (spec §7.7).** Drive a session to
  `attempts_committed + remaining_visits == attempt_ceiling` and assert the
  retry option is not offered, the message names the remaining visit count, and
  every remaining visit still takes its one attempt so the run reaches
  `Result::Completed`.
- **Crash and resume**: destroy the controller and store mid-visit, reload with
  `load_session`, build a new controller, and assert it resumes at the right
  visit and attempt and that the final record has no duplicate or lost visit.
- **Capture failure**: a source configured to time out produces an attempt with
  status `timeout`, offers a retry, and never produces metrics.

**Implementation notes.**

- The capture-status to attempt-status mapping is spec §11.3's table; implement
  it as one total function and test every row. Metrics are written only on the
  `ok` row; every other row leaves them unset, never zero.
- Order per visit: provider → settle → `flush()` → capture → metrics → ask the
  operator → **one** `commit_visit(...)`. The operator's choice is known before
  the commit, so the attempt is written with its final disposition the first and
  only time it is written: `accepted`, `abandoned`, or — when the operator
  retries — `superseded` alongside the `pending_retry` that names its
  replacement. There is no `begin_visit`, no separate `set_disposition`, and no
  separate `record_retry_intent`; spec §6.6 explains why three calls could not
  satisfy §11.4's one-commit requirement.
- The controller commits the attempt **before** advancing to the next visit.
- The controller owns no statistics. Ranking is WP10; the controller must not
  compute an angle score, a yield, or a warning.

**Acceptance criteria.** Every row of the transition table in spec §11.3.1 is
produced by at least one controller test, and each asserts the persisted
disposition, the resulting session `state`, the number of `commit_visit` calls,
and the resume outcome. Write the test as a table driven by that same spec
table, so a row added to the spec without a test fails the suite.

**Commit.** `feat: add experiment controller with atomic visit commits and pause semantics`

---


## WP10 — Aggregation, quality warnings, and the report

**Purpose.** Capture-level aggregation, detection yield, per-metric eligibility,
the two descriptive rankings, the eight labelled data-quality warnings, and the
rendered report with its single required headline. Spec §9.5, §10.
**Depends on.** WP5, WP9.
**Role / effort.** Application engineer with a measurement reviewer — 2 days.
(Revision 4 removed the decision engine this package used to carry; what remains
is aggregation and reporting.)

**Files.** `src/experiment/aggregator.*`, `src/experiment/quality_checks.*`,
`src/ui/report_renderer.*`, `tests/test_aggregation.cpp`,
`tests/test_quality_checks.cpp`, `tests/test_report.cpp`.

**Read spec §10.0 and §10.4 before writing a line of this package.** Two things
about it are easy to get wrong in opposite directions. The estimand is
conditional on detection, so a score without a yield beside it is not
interpretable — that is why W6 exists. And Phase 1 takes **no decision**, so
nothing here may combine warnings into a verdict, rank-order their severity, or
stop evaluating at the first one that fires. A warning list that short-circuits
is a gate ladder wearing a different name.

**Interfaces produced.**

```cpp
namespace rtlangle {
enum class Metric { Channel, Audio };

struct AngleSummary {
  double planned_deg = 0.0;
  double actual_mean_deg = 0.0, actual_spread_deg = 0.0;   // circular, 360 deg
  int    n_captures = 0;          // captures with a CHANNEL score
  int    n_captures_audio = 0;    // captures with an AUDIO score; <= n_captures
  int    n_excluded = 0, n_valid_events = 0;
  std::optional<double> score_channel_db, score_audio_db;  // either may be absent
  DescriptiveSpread spread_channel, spread_audio;          // NOT a confidence interval
  std::optional<double> yield_events_per_min;              // spec 9.5
  std::optional<double> detected_fraction;
  double noise_floor_dbfs = 0.0, noise_floor_spread_db = 0.0;
  std::vector<std::string> flags;
  std::vector<std::string> warning_ids;   // which of W1..W8 name this angle
  std::string status;
};

// One entry per warning that FIRED. Warnings that did not fire are absent;
// there is no pass/fail pair, because nothing passes.
struct Warning {
  std::string id;         // "W1".."W8"
  std::string message;    // already formatted, with the numbers in it
  std::vector<double> angles_deg;   // which angles it names, may be empty
};

struct SessionSummary {
  std::vector<AngleSummary> angles;
  std::vector<double> ranking_channel, ranking_audio;   // planned angles, best first
  std::vector<double> ranking_yield;                    // spec 9.5, feeds W6
  Metric report_metric = Metric::Channel;               // which table is listed first
  std::string report_metric_source = "default";         // default | flag | interactive
  std::vector<Warning> warnings;                        // W1..W8, all that fired
  bool partial = false;                                 // rendered from pause() or abort()
};
// NOTE the absences. There is no resolved_best_angle, no resolution_method, no
// resolution_strength, no resolution_reason, and no exploratory flag: the
// headline is unconditional (spec 10.6), so a boolean selecting between two
// headlines would have nothing to select.

SessionSummary aggregate(const SessionRecord&);          // levels 1 and 2, plus yield
void evaluate_warnings(SessionSummary&, const Config&);  // appends W1..W8; decides nothing
namespace ui {
  // Reorders which table is listed first. Asserts internally that it mutates
  // only report_metric and report_metric_source.
  void choose_report_metric(SessionSummary&, ITerminalUi&);
}
std::string render_report(const SessionRecord&, const SessionSummary&, bool color);
}
```

**Failing tests first.**

- **The unit is the capture.** A record with one angle having a single capture
  of 50 events at 20 dB and another angle having four captures at 10 dB each
  gives scores of 20 and 10 with `n_captures` 1 and 4. Assert that changing the
  *number of events* inside a capture does not change that capture's vote, and
  that W1 names the one-capture angle.
- **Superseded attempts are invisible.** As in WP6, assert the score ignores
  them.
- **Per-metric eligibility flows to the angle level (spec §9.4.1).** An angle
  whose captures all have `audio_insufficient` has `n_captures_audio == 0`,
  `score_audio_db == nullopt`, is absent from `ranking_audio`, is present in
  `ranking_channel`, and renders as `-` in the audio table rather than `0`.
  `n_captures_audio <= n_captures` is asserted as an invariant over fuzzed
  records. A §10.5 note fires whenever the two differ at any angle.
- **Yield aggregates like the score.** An angle's `yield_events_per_min` is the
  median of its accepted `ok` captures' `events_per_minute`, and a capture with
  more events raises the angle's yield without changing its score.
- **Test J — the estimand, and warning W6.** Construct the selection effect
  directly. Angle `A` sees a population of transmissions in which only the
  strong half clears the squelch. Angle `B` sees the same population 3 dB
  stronger, so the weak half clears it too. Then:

  ```
  assert  angle_score(B)  <  angle_score(A)      // the BETTER angle scores LOWER
  assert  angle_yield(B)  >  angle_yield(A)
  assert  W6 fired, and its message carries both yields and the ratio
  assert  both rankings are still printed in full
  ```

  A second fixture with concordant score and yield leaves W6 silent. This test
  is written before `evaluate_warnings` exists.
- **Test L — every warning, every time, no short-circuit.** Enumerate the ids
  explicitly — `W1`, `W2`, `W3`, `W4`, `W5`, `W6`, `W7`, `W8` — with a fixture
  that fires each and a fixture that does not, and assert the set of ids the
  suite exercises equals the set spec §10.4 defines. A test written against the
  range "W1–W8" would pass while a warning was never implemented, and **W3 (top
  two within `min_effect_db`) and W4 (the two rankings disagree) are the ones at
  risk**: they were §10.5 prose notes in revision 3 and are the only two with no
  revision-3 gate behind them to inherit an implementation from.

  A record engineered to trip W1, W5, and W8 simultaneously reports **all
  three** — assert the count and the id set. Revision 3 evaluated gates in order
  and stopped at the first failure; doing that here would silently hide two of
  the three problems. A clean session produces an empty `warnings` vector.
- **W7 compares to the baseline, not the request.** An attempt whose applied
  gain differs from the *request* but equals its segment baseline does **not**
  fire W7; one that differs from its segment baseline does, naming the segment
  and both values. Revision 2's requested-equals-applied test would have fired
  on every real device, since librtlsdr snaps to a gain table.
- **W2 is an observation, not a test.** With three rounds run and angles 30 and
  45 sharing only two, W2 fires and its message names both angles and both
  counts. It does not compute a difference, an interval, or a p-value —
  asserted structurally in Test K below.
- **Test K — no decision exists.** Structural and greppable:
  - `to_json(SessionSummary)` has no key containing `resolved`, `resolution`, or
    `winner`, over a fully-populated instance;
  - no string reachable from `render_report` contains `resolved`,
    `statistically`, `significant`, `Highest measured`, `best angle`, `optimal`,
    or `confidence interval`;
  - the spec §10.6 headline appears in **every** rendered report — sparse,
    rich, partial, and aborted alike — because there is only one headline;
  - `evaluate_warnings` is a pure function of `(summary, config)` and does not
    write any field of `AngleSummary` other than `warning_ids`;
  - the whole of `src/experiment/` links without any symbol from WP1's removed
    inferential helpers, because they do not exist.
- **Report-metric choice moves nothing.** Rendering the same record twice with
  the two `report_metric` values produces reports whose *table order* differs
  and whose every number and every warning is byte-identical. Assert by
  serialising the summary before and after `choose_report_metric` and diffing.
  A non-interactive UI leaves the configured value.
- **The report leads with the rankings.** Assert the rendered text contains both
  ranking tables and the warning list *before* the §10.6 headline, and that
  every ranking row showing a score also shows a yield.
- **Report wording.** The report contains exactly the §10.6 string, once, and
  contains no alternative headline. Assert the exact bytes.
- **Spreads are labelled as descriptive.** The rendered spread column is headed
  in a way that cannot be read as a confidence interval, and the report states
  which of min–max or interquartile it used for each angle.
- Circular statistics are used for the actual-angle mean and spread — a session
  with actual angles `{350, 10}` reports a mean near 0, not 180 — and angles are
  never folded modulo 180.

**Implementation notes.**

- Level 1 scores = medians of that capture's eligible event SNRs, per metric.
  Level 2 scores = medians of the capture scores, per metric, over the captures
  eligible for that metric. Yield follows the identical two-level shape. State
  in the rendered table that with `n = 2` the median equals the mean.
- `evaluate_warnings` appends to `summary.warnings` and stamps `warning_ids` on
  the angles each warning names. It evaluates all eight unconditionally; there
  is no early return anywhere in it, and a review that finds one has found a
  bug.
- `render_report` is a pure function of `(record, summary, color)`. It never
  computes a warning and never re-aggregates, which is what makes the
  report-metric test a byte comparison rather than a judgement call.
- Nothing in this package includes a statistics header. If a reviewer sees a
  `sqrt` of a variance, ask what it is for.

**Acceptance criteria.** `SessionSummary` carries no resolution field and no
`exploratory` flag; `render_report` emits the one §10.6 headline in every case;
`evaluate_warnings` reports every warning that applies rather than the first.
All three are asserted by tests that fail on a grep, not by review.

**Commit.** `feat: add capture-level aggregation, detection yield, and data-quality warnings`

---


## WP11 — CLI, main orchestration, and the airband scan

**Purpose.** Wire everything into commands, and implement scan with exposure
accounting on the retuning contract. Spec §12.1–12.2, §12.5.
**Depends on.** WP7, WP9, WP10.
**Role / effort.** Application engineer — 2.5 days.

**Files.** `src/app/cli_parser.*`, `run_command.*`, `scan_command.*`,
`device_check.*`, `menus.*`, `main.cpp`, `tests/test_cli.cpp`,
`tests/test_scan.cpp`, `tests/test_commands.cpp`.

**Interfaces produced.**

```cpp
namespace rtlangle::app {
struct ParseResult { Config config; std::string command; std::vector<ValidationError> errors; bool help=false, version=false; };
ParseResult parse_cli(int argc, char** argv);

struct ScanChannel { std::uint32_t center_hz; int exposures, hits; std::optional<double> activity; };
std::vector<ScanChannel> run_scan(ITunableSampleSource&, const Config&, ITerminalUi&);

int run_command(const Config&, ITerminalUi&);      // returns the process exit code
int scan_command(const Config&, ITerminalUi&);
int device_check_command(const Config&, ITerminalUi&);
int report_command(const std::filesystem::path&, ITerminalUi&);
}
```

**Failing tests first.**

- Every flag in spec §7 parses. Spec §7 is the authoritative field list and the
  plan deliberately does not restate it; instead the test is generated from
  `Config` itself — enumerate the keys of `to_json(Config)` and assert each has a
  parser entry, a bound, a `resume_class`, and a `field_scope` for all four
  commands. A field added to `Config` without a flag therefore fails the test
  rather than silently becoming unreachable. Spot-check by hand at least
  `--resume`, `--report-metric`, `--min-captures`, `--min-effect-db`,
  `--yield-concordance-ratio`, `--max-retained-events`, `--angle-reference`,
  `--setup-note`, `--probe-percentile`, `--read-timeout-factor`,
  `--read-timeout-slack`, `--scan-usable-fraction`, and every other `--scan-*`.
- **Flags that no longer exist produce the standard unknown-flag error (exit 2)**
  and are named individually so that reintroducing any of them requires deleting
  an assertion: `--agc` and `--allow-agc` (spec decision Q7), and
  `--primary-metric` and `--family-alpha` (decisions Q8 and Q1).
- Precedence: defaults < config file < CLI. A field set in all three ends up
  with the CLI value; a field set only in the file ends up with the file value.
- **Command scope (spec §7.9).** Table-driven over `Config` × the four commands:
  every `Refused` pair produces exit code 2 with a message naming both the flag
  and the command, and every `Used` pair is accepted. Spot-checks:
  `report --gain 400` is an error; `run --scan-dwell 100` is an error;
  `scan --rounds 4` is an error; `run` without `--freq` is an error naming
  `--freq`; `run --resume <dir>` without `--freq` is accepted; a field left at
  its default never triggers a scope error even for a command that refuses it.
  **`report` accepts the whole of §7.4** — `--min-captures`, `--min-effect-db`,
  `--yield-concordance-ratio`, `--noise-drift-warn-db` — because those change
  only which warnings the re-render raises; assert that re-rendering a stored
  session with a stricter `--min-captures` adds W1 and leaves every measured
  number byte-identical.
- `--non-interactive` with `--source rtlsdr` is a configuration error naming the
  reason; with `synthetic` it is accepted.
- Exit codes and the state each writes (spec §12.1): usage error → 2, no session
  created; device busy → 3; a completed synthetic run → 0 with
  `state: completed`; a run ended by `Quit` → 0 with `state: paused`; a run the
  app layer ends on an unrecoverable fault → 1 with `state: aborted` and
  `abort_reason` set.
- **Scan dwell placement and exposure accounting**, all on a synthetic tunable
  band. Spec §12.5.1 gives the rule and its proof; these tests assert it:
  - with the defaults, `U == 409600`, `D == 30000`, and the generated step is
    `S == U - D == 379600`, satisfying `2D <= S <= U - D`;
  - **every** channel in `[scan_start_hz, scan_end_hz]` finishes with
    `exposures >= 1` — assert over the whole 118–136.975 MHz band, explicitly
    including the first and the last channel;
  - **regression fixture for the revision-2 defect**: stepping by `2U` instead
    leaves every channel within `±D` of a dwell centre at `exposures == 0`, a
    60 kHz hole against 25 kHz spacing. The test drives the placement function
    with the old step, asserts the hole exists, then asserts the shipped rule
    produces none. Without this fixture the coverage test could pass for the
    wrong reason;
  - a configuration with `U < 3D` is refused by `validate` before any dwell is
    generated (spec §7.6);
  - a channel inside the DC exclusion of a given dwell gains no exposure *from
    that dwell*;
  - `activity == hits / exposures`, and a channel with `exposures == 0` reports
    `nullopt` and renders as `-`, never as `0 %`;
  - a synthetic band with transmitters at known frequencies and duty cycles
    recovers them in the correct rank order;
  - a retune failure aborts with the failing frequency named;
  - the dwell count for the default band and default receiver settings is
    asserted to be **exactly 50** (spec §12.5.1 works the arithmetic), so the
    roughly doubled sweep time is a recorded expectation rather than a later
    surprise. Naming the number is deliberate: an assertion with no expected
    value is what let the `2U` step survive a test meant to check coverage.
- **Orchestration**, one integration test per command path: new run, resume,
  synthetic, scan, devices, report. Each asserts the sequence

  ```
  parse -> validate -> validate_for_command
        -> open source
        -> create session | load_session(resume)          <- store now exists
        -> begin_receiver_segment(source.info())          <- returns segment_id
        -> build provider -> build controller(segment_id)
        -> controller.run()                               <- commits visits; leaves Running
        -> aggregate -> evaluate_warnings
        -> choose_report_metric (table order only) -> render
        -> finalize(summary) | pause(partial) | abort(partial, reason)
        -> exit code
  ```

  The order of the first four steps is not cosmetic. `begin_receiver_segment` is
  a method **on the store**, so the store must exist before it can be called,
  and it needs `source.info()`, so the source must be open before that. Assert
  the order with a recording decorator over both the store and the source.

  Assert that **the app layer, not the controller, writes every terminal or
  paused state**, mapping the controller's return value exactly:

  | `Result` | App layer calls | Final `state` |
  |---|---|---|
  | `Completed` | `finalize(summary)` | `completed` |
  | `QuitRequested` | `pause(partial)` | `paused` |
  | `Failed` | `abort(partial, reason)` | `aborted` |

  In all three cases the controller's own call count for those methods is zero
  (WP9 asserts the same invariant from its side), and the state observed
  immediately after `controller.run()` returns is `running`. The summary the
  three calls take is produced by `aggregate` and `evaluate_warnings`, which the
  controller is forbidden to run.

  **When the store cannot be written at all (spec §11.1.1).** A store decorator
  returning `CommitOutcome::NotCommitted` from `commit_visit` drives the
  controller to `Result::Failed`; the app layer then calls
  `abort(partial, reason)` and the process exits 1 with `state: aborted` and
  `abort_reason` set — §12.1's first exit-1 row. A second decorator that also
  fails `abort` is the terminus: exit code 1, `session.json` byte-identical to
  its last committed state, `state` **not** `aborted`, `finished_utc` **not**
  stamped, and every measurement committed before the fault still present and
  readable. A `NotCommitted` from `pause` or `finalize` is different and has its
  own test: no `abort` is attempted, the process exits 1, the record is left
  `running` at its last committed value, and a subsequent `--resume` of that
  directory **succeeds** and can complete the session — asserted with a
  decorator that fails each of the two in turn. Aborting there would make
  terminal a session that was fully recoverable (spec §11.1.1 step 3). That
  gives each of §12.1's two exit-1 rows a test.

  **There is no decision step in this sequence, and a test asserts that too.**
  Revision 2 listed "decide → optional metric selection" while also letting the
  selection change which metric was evaluated, so the decision was taken on one
  metric and the selection applied to another. Revision 3 fixed the ordering;
  decision Q1 removed the step. The orchestration test therefore asserts that no
  function named `apply_decision`, `resolve`, or similar is called between
  `aggregate` and `render`, and that a scripted UI choosing the other metric at
  `choose_report_metric` leaves every number in both tables and every entry in
  `warnings` byte-identical.

**Implementation notes.**

- Dwell placement follows spec §12.5.1 exactly: `U = usable_fraction *
  sample_rate / 2`, `D = dc_exclusion`, `S = U - D`, `c_0 = scan_start + U`,
  `c_i = c_0 + i*S`, continuing until `c_i >= scan_end + D`, with at least two
  dwells. Do not step by the full usable span; the proof in §12.5.1 shows why
  that leaves a permanent `2D`-wide hole at every dwell centre.
- The band noise floor per dwell is the median of the in-span channel powers,
  and a hit is `open_db` above it — the same constant the experiment uses, which
  is why `scan` accepts `--open-db` (spec §7.9).
- Scan writes nothing under `sessions/`. It prints a copyable
  `rtlangle run --freq <hz>` line for the top channel.
- `main` is a thin dispatcher: parse, validate for the command, print errors,
  dispatch, return the code.

**Acceptance criteria.** Every command path has an integration test that runs
without hardware. No `Config` field is unreachable from the CLI, and no field
lacks a `field_scope` entry for every command.

**Commit.** `feat: add CLI, command orchestration, and airband scan with proved band coverage`

---


## WP12 — End-to-end verification, documentation, and release evidence

**Purpose.** Prove the whole thing works on synthetic data, produce the
documentation and the example session, and record hardware evidence honestly.
Spec §14.4, §16.
**Depends on.** WP11.
**Role / effort.** Whole team — 2 days.

**Files.** `tests/test_end_to_end.cpp`, `README.md`, `docs/architecture.md`,
`examples/session-synthetic/`, `.gitignore` update.

**Failing tests first.**

**How the synthetic session varies SNR by angle — and why it needs a test
double.** `SyntheticSource` has one configured SNR, and nothing in the
production wiring tells it which angle is being visited; a source that knew
would be a production type coupled to the controller, which spec §5 forbids.
Revision 2 asked the source to emit "a different SNR per angle" with no
mechanism at all. Two **test doubles** supply the mechanism, both under
`tests/support/`, and neither touches `src/`:

```cpp
// tests/support/scripted_capture_source.h
// Owns a production SyntheticSource and forwards every ISampleSource call to
// it. advance_to(planned_deg) sets the generator's SNR from the script.
class ScriptedCaptureSource final : public ISampleSource {
public:
  ScriptedCaptureSource(SyntheticParams base, std::map<double,double> snr_by_angle);
  void advance_to(double planned_deg);      // called by ScriptedAngleProvider
  /* ISampleSource: delegates to the inner SyntheticSource */
};

// tests/support/scripted_angle_provider.h
// A real IAngleProvider whose request() is already told the planned angle, so
// it is the natural place to tell the source which visit is starting.
class ScriptedAngleProvider final : public IAngleProvider {
public:
  ScriptedAngleProvider(ScriptedCaptureSource&, std::deque<AngleOutcome> script);
  AngleOutcome request(double planned_deg, int attempt) override {
    source_.advance_to(planned_deg);        // <- the whole coupling, test-side only
    return next_scripted_outcome();
  }
};
```

The controller, the store, the aggregator, and the renderer are used unmodified;
the only thing the test injects is which SNR the generator produces next. The
SNR change takes effect at the next `flush()`, which the controller calls exactly
once per capture after settling (WP9), so a capture is never generated at two
SNRs.

- **Full synthetic session.** Assert: the session completes with
  `state: completed`; `session.json`, `measurements.csv`, and `report.txt` exist
  and reconcile attempt-for-attempt; the seeded strongest angle is first in the
  channel ranking; the report carries the single §10.6 headline, a yield column
  beside every score, and both ranking tables. Run it at 2 rounds and at 4: the
  numbers differ, W1 fires at 2 rounds and not at 4, and **the headline is
  byte-identical in both**, because there is only one.
- **Full synthetic session, selection-effect variant.** The script gives the
  *better* angle a population whose weak half only it can detect, reproducing
  spec §10.0 end to end. Assert the run completes, both rankings are printed,
  W6 fires with both yields in its message, and no output anywhere claims a best
  angle. This is Test J at whole-program level.
- **Full synthetic session, audio-sparse variant.** A script in which most
  events fail audio eligibility (spec §9.4.1). Assert the channel ranking is
  complete, the audio table shows `-` for the affected angles rather than `0`,
  `n_captures_audio < n_captures` appears in the §10.5 notes, and no capture
  was mis-statused as `insufficient_data`.
- **Quit, pause, and resume end-to-end.** A scripted `Quit` at visit 5 leaves
  `state: paused`, `finished_utc: null`, and a `report.txt` marked `PARTIAL`
  built from the completed subset. Assert that immediately after the controller
  returns the state was still `running`, and that `pause()` came from the app
  layer — two commits, not one (spec §11.3.1). Resuming the same directory
  returns it to `running`, opens a second receiver segment, completes the
  remaining visits, and ends `completed` with every visit present exactly once.
  A further resume of the now-`completed` directory is **refused** naming
  `finished_utc`.
- **Abort end-to-end.** A store decorator that returns
  `CommitOutcome::Indeterminate` and then fails `reload_from_disk` drives the
  run to `state: aborted` with `abort_reason` set, `finished_utc` stamped, exit
  code 1, and a `report.txt` marked `ABORTED — PARTIAL` containing every
  measurement committed before the fault. Resuming that directory is refused.
- **Example session is generated, not written.** The committed
  `examples/session-synthetic/` is produced by running the built binary with a
  fixed seed, and a test regenerates it and asserts byte equality against the
  committed copy. A hand-written example would drift from the real output shape.

**Documentation.**

`README.md` must cover: dependencies and the exact Ubuntu packages; build and
test commands; the physical protocol of spec §4 including how to **mark one
dipole arm** and choose and record the 0° reference, and why θ and θ+180 are two
measurements rather than one; every flag from spec §7 and which command accepts
it (§7.9); the CNR-versus-audio-SNR distinction; the minimum detectable SNR and
what it means; **spec §10.0's estimand in plain language**, including the worked
explanation that a lower median SNR can mean more weak signals were heard, and
why the yield column is printed beside every score; **what the tool does not do
— it ranks and it warns, it never names a best angle (spec §2 Q1)**; what W1–W8
mean in plain language and that they are warnings rather than a verdict; why
more rounds help (they make the printed spread informative, not because any
count would resolve anything); every limitation from spec §15.1; the forbidden
claims from §15.2; and the busy-device instruction — close the SDR application
normally, or identify the holder with `fuser -v /dev/bus/usb/*/*` and stop that
specific process.

The README must not contain a broad `pkill -f` pattern, must not describe
`host_dropped_samples` as a device overrun, and must not contain the words
"statistically resolved", "best angle", or "confidence interval" — the WP12
grep sweep checks all three.

`.gitignore` must ignore `sessions/` without ignoring
`examples/session-synthetic/` — verify with `git check-ignore -v` on both paths.

**The first hardware experiment.** The README ends with this block, and the
completion report repeats it:

```bash
# 1. Close any other SDR application that is using the device (SDR++, gqrx,
#    dump1090). To find what is holding it:
#        fuser -v /dev/bus/usb/*/*
#    then close that application normally.

# 2. Confirm the device is visible and free.
./build/rtlangle devices

# 3. Find an airband channel that actually carries traffic near you (~1 minute).
./build/rtlangle scan

# 4. Run the experiment on the frequency the scan recommended.
#    --rounds 4 gives each angle four captures, which is what makes the spread
#    printed beside each score informative; the default 2 gives you a single
#    interval between two points and raises warning W1. No number of rounds
#    makes this tool name a best angle - it ranks and it warns (spec 2, Q1).
#    Budget roughly 4 rounds x 7 angles x (60 s + positioning) ~ 45 minutes.
./build/rtlangle run \
    --freq <FREQUENCY_FROM_SCAN> \
    --start-deg 0 --end-deg 90 --step-deg 15 \
    --rounds 4 --order alternating \
    --duration 60 --settle 5 \
    --gain max \
    --angle-reference "marked arm along the balcony rail, pointing at the street" \
    --label first-experiment
```

Read the output as a ranking plus a list of caveats, not as an answer. If W6
fires, the top-scoring angle heard *fewer* transmissions than another one, and
spec §10.0 explains why that is the signature of the measurement rather than of
the antenna.

**Stale-term check against the code.** WP0 could only review prohibitions in
prose; here the terms must be genuinely absent from the shipped artifacts:

```bash
grep -rnE 'pkill|M_PI|save[-_]iq|completed_keys|rank[-_]metric' src/ README.md docs/architecture.md
# expect zero matches
grep -rnE 'agc_mode\(dev, ?1\)|gain_mode\(dev, ?0\)|--agc|allow_agc' src/ README.md
# expect zero matches - AGC has no enable path (spec Q7)
grep -rnE 'selected_metric|sample_overruns|append_attempt|set_disposition|record_retry_intent|begin_visit' src/ README.md
# expect zero matches - each names a construct revision 3 replaced
grep -rnE 'resolved_best_angle|resolution_(method|strength|reason)|primary_metric|family_alpha|t_quantile|paired_(ci95|lower_bound)|signflip|exploratory' src/ README.md
# expect zero matches - decision Q1 removed every one of them
grep -rniE 'statistically resolved|Highest measured|best angle|is optimal|confidence interval' src/ README.md docs/architecture.md
# expect zero matches - spec 15.2
grep -rn 'fftw_plan\|lfftw3[^f]' CMakeLists.txt cmake/ src/     # expect zero matches
grep -rn 'tests/' src/ --include=*.h --include=*.cpp             # expect zero matches
```

Positive checks, each expected to match:

```bash
grep -rn 'events_per_minute' src/ README.md      # yield is never omitted
grep -rn 'host_dropped_samples' src/ README.md   # honest overrun naming
grep -rn 'commit_visit' src/                     # single atomic mutator
grep -rn 'CommitOutcome' src/                    # four-outcome commit contract
grep -rn 'valid_audio_event_count' src/          # per-metric eligibility
grep -rn 'Exploratory ranking only' src/ README.md   # the one headline
```

**Release verification.** Run every command in spec §14.4 from a clean
worktree, retain the full output, and record each gate as **passed**, **failed**,
or **pending**. Parse `RTLANGLE_HARDWARE=` from the hardware test's output; a
`skipped` result is reported as *hardware validation pending*, never as passed.

**Acceptance criteria.** All six Phase 1 completion conditions from
`docs/acceptance-gates.md` are recorded with their evidence. Nothing is
described as verified that was not run.

**Stop condition.** If hardware is unavailable, WP12 still completes — with the
hardware gate marked pending. Do not delay the other gates waiting for a device,
and do not mark the hardware gate anything other than pending.

**Commit.** `docs: add README, architecture notes, generated example session, and release evidence`

---

## Traceability

### Spec sections to work packages

| Spec section | Work package |
|---|---|
| §1 purpose, non-goals, safety, legal | WP0, WP12 (README) |
| §2 product decisions | WP0 |
| §3 platform, environment, FFTW precision | WP1 |
| §4 physical protocol, angle generation, circular math, visit order, actual angle | WP2 (math), WP8 (entry), WP12 (README protocol) |
| §5 architecture, targets, build gates | WP1 |
| §6.1 `ISampleSource` | WP4 |
| §6.2 `ITunableSampleSource` | WP4 (contract), WP7 (device), WP11 (scan use) |
| §6.3 `CaptureRunner` | WP4 |
| §6.4 `IAngleProvider` | WP8 |
| §6.5 `ITerminalUi` | WP8 |
| §6.6 `ISessionStore` | WP6 |
| §7 configuration, bounds, cross-field rules, resource limits, gain | WP2 (schema), WP11 (CLI), WP7 (gain) |
| §7.5 `.cu8` metadata, sidecar, lifecycle | WP4 |
| §7.7 resource ceilings and preflight arithmetic | WP2 (estimate), WP6 (enforcement) |
| §7.9 per-command field scope | WP2 (`field_scope`), WP11 (enforcement) |
| §8.1 offset tuning, mixer sign | WP3 |
| §8.2 filter conventions | WP3 |
| §8.3 framing | WP3 |
| §8.4 latency and audio alignment | WP3 (latency), WP5 (alignment) |
| §8.5 clipping | WP4 |
| §9.1 two-stage noise floor | WP5 |
| §9.2 event detection | WP5 |
| §9.3 channel SNR | WP5 |
| §9.4, §9.4.1, §9.4.2 audio SNR and per-metric eligibility | WP5 (per capture), WP10 (angle level) |
| §9.5 detection yield | WP5 (per capture), WP10 (aggregation, G8) |
| §10.0 the estimand | WP10 (Test J), WP12 (README) |
| §10.1 capture as experimental unit | WP10 |
| §10.2 descriptive rankings | WP10 |
| §10.3 `report_metric`, display only | WP2 (classification), WP10 (Test K), WP11 |
| §10.4 warnings W1–W8, not a decision ladder | WP10 (Test L) |
| §10.5 comparability warnings | WP10 |
| §10.6 result wording | WP10 |
| §11.1.1 commit point and the four `CommitOutcome` values | WP6 (fault matrix), WP9 (caller behaviour) |
| §11.1.2 `Indeterminate` reconciliation | WP6, WP9, WP11 (abort) |
| §11.1.3 derived, recoverable CSV and report | WP6 |
| §11.1.4 persistence cost arithmetic | WP2 (estimate), WP6 |
| §11.2 schema, session state, receiver segments, visit ids | WP2 (records), WP6, WP9 |
| §11.3.1 transition table | WP6 (persistence outcome), WP9 (controller path) |
| §11.4 retry durability as one commit | WP6, WP9 |
| §11.5 resume and lifecycle | WP6, WP9 |
| §11.6 derived CSV | WP6 |
| §12.1–12.2 commands, exit codes, menu | WP11 |
| §12.3 terminal behaviour | WP8 |
| §12.4 per-visit interaction | WP8, WP9 |
| §12.5, §12.5.1 scan and dwell-coverage proof | WP11 |
| §13.1 threat model | WP6, WP12 (README) |
| §13.2 `O_NOFOLLOW` scoping and the `dirfd` protocol | WP6 |
| §13.3 label sanitisation, dot-run collapse | WP6 |
| §13.4 modes, temp names, size limits | WP6, WP8 |
| §14.1–14.2 hardware-free tests | WP1–WP11 (each package's own tests) |
| §14.3 hardware evidence states | WP7 |
| §14.4 release verification | WP12 |
| §15.1 limitations | WP10 (report), WP12 (README) |
| §15.2 forbidden claims | WP10 (Test K greps), WP12 (stale-term sweep) |
| §16 deliverables | WP12 |

### Validation-report gaps to work packages

| Gap | Summary | Work package |
|---|---|---|
| G1 | Circular noise-floor reliability check | WP5 |
| G2 | Events treated as independent replicates | WP10 |
| G3 | `fftwf_*` linked against `fftw3` | WP1 |
| G4 | Undefined `resume_dir`, scan options, retuning, `main` wiring | WP2, WP4, WP11 |
| G5 | Hardware test green without hardware | WP1 (registration), WP7 (body) |
| G6 | Retried captures contaminate ranking | WP6, WP9, WP10 |
| G7 | Audio window not aligned after filtering | WP3, WP5 |
| G8 | Angle endpoint, alternating claim, circular geometry | WP0, WP2 |
| G9 | Metric selection can manufacture a winner | Closed by decision Q1 — nothing is tested |
| G10 | JSON and CSV can disagree after a crash | WP6 |
| G11 | No read timeout, cancellation, or per-capture drop deltas | WP4 |
| G12 | `--save-iq` has no implementation path | WP0 (removed by decision Q2) |
| G13 | Production code depending on `tests/support` | WP4 |
| G14 | Path traversal, permissions, terminal injection | WP6, WP8 |
| G15 | Signal handler holding stack state; non-TTY raw mode | WP8 |
| G16 | Unbounded duration and rounds; no storage preflight | WP2 |
| G17 | Scan retuning, exposure denominators, band edges | WP11 |
| G18 | Filter edges, NCO rounding metadata, clipping, transients | WP3, WP4 |
| G19 | Hardware and native-terminal behaviour unexecuted | WP7, WP8, WP12 |

### Mandatory corrections to work packages

| # | Correction | Work package |
|---|---|---|
| 1 | Two-stage probe floor and persistent-carrier guard | WP5 |
| 2 | Captures, not events, as the experimental unit | WP10 |
| 2b | The unit rule now governs both metrics separately (spec §9.4.1) | WP5, WP10 |
| 3 | Separate the metric field, the rankings, and the conclusion. Revision 3 split `primary_metric` from `report_metric`; revision 4 removed the conclusion entirely, leaving `report_metric` as a display choice with nothing downstream of it | WP2, WP10, WP11 |
| 4 | `fftw3f` linkage; `std::numbers::pi_v<double>` | WP1 |
| 5 | Separate hardware executable with passed/skipped/failed | WP1, WP7 |
| 6 | Every CLI/config field defined before use; retuning contract | WP2, WP4, WP11 |
| 7 | Visit IDs, attempt numbers, retry intent, dispositions | WP6, WP9 |
| 8 | Filter latency, transient trim, audio window compensation, absolute audio test | WP3, WP4, WP5 |
| 9 | Canonical `session.json`, atomic CSV regeneration, schema handling, fault injection | WP6 |
| 10 | Timeouts, cancellation, worker wakeup, capture-start timestamp, per-capture host-drop deltas | WP4 |
| 11 | Production code independent of `tests/support` | WP4 |
| 12 | Angle endpoints, circular distance and means, actual-angle validation, physical 0° | WP2, WP8, WP12 |
| 13 | Clipping, requested-versus-applied settings, transients, NCO offset metadata | WP3, WP4, WP7 |
| 14 | Label and path sanitisation, symlink refusal, size limits, terminal escaping, permissions, resource limits | WP2, WP6, WP8 |
| 15 | Non-TTY-safe raw mode, signal-safe handler state, no broad `pkill` | WP8, WP12 |
| 16 | Remove every stale or contradictory claim | WP0 |

### Revision-3 validation defects to work packages

The second independent validation returned seven release-blocking defects and
five secondary ones. Each is closed by a named package with a named test.

| # | Defect | Closed by | Evidence |
|---|---|---|---|
| R1 | Capture score conditioned on detected events could invert the ranking | **Closed by removal** (decision Q1): no winner is named, so no ranking is presented as a conclusion. The effect is disclosed instead — spec §10.0, §9.5, warning W6 | WP10 Test J, WP12 selection-effect end-to-end |
| R2 | Metric selection could manufacture a conclusion | **Closed by removal**: nothing is tested, so nothing can be selected into a test. `primary_metric` deleted (Q8); `report_metric` reorders tables only | WP10 Test K byte-comparison |
| R3 | Orchestration decided before selecting the metric it evaluated | **Closed by removal**: there is no decision step in the orchestration sequence | WP11 orchestration order assertion |
| R4 | "Four rounds resolve" overstated the inference | **Closed by removal**: no round count resolves anything. WP1's inferential helpers deleted; more rounds are recommended only to make the printed spread informative | WP1 totality test (no inferential helper exists), WP10 Test K |
| R5 | Retry persistence was not atomic | Spec §6.6 `commit_visit`, §11.4; WP6, WP9 | WP6 invariants I1/I2 after every fault injection; WP9 commit-count assertion |
| R6 | Quit finalised a session that was later resumed | Spec §11.2 `state`, §11.5; WP6, WP9 | WP9 pause test, WP12 quit/resume/refuse-completed |
| R7 | Scan dwell rule could not cover the band | Spec §12.5.1 proof, §7.6 feasibility rule; WP11 | WP11 coverage test plus the `2U` regression fixture |
| R8 | Dipole geometry contradicted its 360° angle math | Spec §4.2 marked arm; WP2 | WP2 no-folding test, WP10 `(θ, θ+180)` warning |
| R9 | Visit identifiers could collide | Spec §11.2 `r1-i003`; WP9 | WP9 `45.001/45.002` collision regression |
| R10 | AGC flags contradicted the AGC-off mandate | Spec Q7, §7.8; WP2, WP7, WP11 | WP2 absent-field test, WP11 unknown-flag test, WP7 shim assertions |
| R11 | G5 tested equality to the request, not stability | Spec §10.4 G5, §11.2 receiver segments; WP6, WP10 | WP10 baseline-versus-request pair of cases |
| R12 | Persistence cost understated by ~300× | Spec §11.1 arithmetic, §7.7 ceilings; WP2, WP6 | WP2 `estimate_resources` pins the table; WP6 ceiling test |
| R13 | Test B and Test D unpassable at the default squelch | WP5 `low_squelch` plus paired production-default cases | WP5 Test B and Test D |
| R14 | Synthetic end-to-end had no way to vary SNR per angle | WP12 `ScriptedCaptureSource` + `ScriptedAngleProvider` test doubles | WP12 full synthetic session |
| R15 | Fractional decimator latency truncated silently | Spec §8.4 `Latency`; WP3 | WP3 exact-rational and deliberate-fractional cases |
| R16 | `.cu8` replay metadata and lifecycle undefined | Spec §7.5; WP4 | WP4 sidecar, sequential-consumption, exhaustion tests |
| R17 | No command-specific validation | Spec §7.9; WP2 `field_scope`, WP11 | WP11 table-driven scope test |
| R18 | Channel filter could exceed Nyquist | Spec §7.1 cross-field rules; WP2 | WP2 cross-field error tests |
| R19 | Host drops called device overruns | Spec §6.1, §15.2; WP4, WP6 | WP4 naming test, WP6 CSV header test |
| R20 | WP5 missing its dependency on `CaptureOutcome` | WP5 "Depends on" now names WP4 | build order |

### Revision-4 changes to work packages

The product decision and the third validation pass. R21–R27 are Nina's seven
items, in her order.

| # | Change | Closed by | Evidence |
|---|---|---|---|
| **Q1** | Phase 1 is descriptive only; every inferential construct removed | Spec §2 Q1, §10.4, §10.6, §15.2; WP1 (helpers deleted), WP2 (config fields deleted), WP10 (rewritten) | WP10 Test K structural greps; WP1 "no inferential helper exists" |
| R21 | Commit semantics: `rename()` is the commit point; four outcomes replace "a throw means nothing committed"; the `NotCommitted` ladder is total over all five `CommitResult` methods; CSV and `report.txt` derived and recoverable | Spec §11.1.1–§11.1.3, §6.6 `CommitResult`; WP6, WP9, WP11 | WP6 per-boundary outcome matrix; WP6 `EINTR`-only retry test; WP6 reconciliation cases; WP6 derived-artifact tests; WP9 final-`NotCommitted` run; WP11 both exit-1 rows |
| R22 | Quit is one visit commit *then* a separate app-layer `pause()`, not one rename | Spec §11.3.1 preamble and the app-layer table; WP9, WP11, WP12 | WP9 state-after-return assertion; WP12 quit/resume end-to-end |
| R23 | Controller completion leaves the store `running`; WP11 finalizes | Spec §6.6 caller table, §11.3.1; WP9, WP11 | WP9 zero-call assertion for all three `Result` values; WP11 `Result` → state mapping |
| R24 | Attempt ceiling: reserved retry capacity, plus an `aborted` terminal state | Spec §7.7, §6.6 `abort()`, §11.5; WP2, WP6, WP9, WP11 | WP2 boundary walk; WP6 ceiling test; WP9 reserved-capacity run; WP12 abort end-to-end |
| R25 | Metric-specific audio eligibility via `valid_audio_event_count`, with missing-score aggregation | Spec §9.4.1, §9.4.2, §10.1; WP2 (records), WP5, WP10 | WP5 one test per §9.4.2 row; WP10 angle-level absence test; WP12 audio-sparse end-to-end |
| R26 | Remaining numeric helpers are total; no permitted configuration reaches undefined behaviour | WP1 | WP1 totality table under ASan/UBSan |
| R27 | Internal `..` cannot survive slug sanitisation; `O_NOFOLLOW` claim scoped accurately | Spec §13.1–§13.3; WP6 | WP6 `a..b` regression and no-`..` property test; WP6 intermediate-component swap test |

---

## Final verification

Run from a clean worktree after WP12, retaining full output:

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure -LE hardware
ctest --test-dir build --output-on-failure -L  hardware      # Skipped is a valid outcome

cmake -S . -B build-nohw -G Ninja -DRTLANGLE_WITH_RTLSDR=OFF
cmake --build build-nohw
ctest --test-dir build-nohw --output-on-failure

cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure -LE hardware

./build/rtlangle run --source synthetic --non-interactive --freq 118350000 \
    --rounds 4 --duration 5 --settle 0 --label verify
# expect exit 0, state "completed", both ranking tables with a yield column
# beside every score, the W1-W8 warnings that apply, and exactly one headline.

./build/rtlangle run --source synthetic --non-interactive --freq 118350000 \
    --rounds 4 --duration 5 --settle 0 --agc --label should-fail
# expect exit 2, unknown flag --agc (spec decision Q7)

./build/rtlangle run --source synthetic --non-interactive --freq 118350000 \
    --rounds 4 --duration 5 --settle 0 --primary-metric channel --label should-fail
# expect exit 2, unknown flag --primary-metric (spec decisions Q1 and Q8)

grep -c 'Exploratory ranking only' sessions/*verify*/report.txt   # expect 1
git diff --check
git status --short
git check-ignore -v sessions/ ; git check-ignore -v examples/session-synthetic/ || true
```

Record every gate as **passed**, **failed**, or **pending**. Parse
`RTLANGLE_HARDWARE=` from the hardware test output rather than inferring
success from an aggregate green run. A `skipped` hardware result means
**hardware validation pending** and must be reported that way.
