# rtlangle — Phase 1 Antenna-Angle Experiment (Design Specification)

Date: 2026-08-19
Revision: 4 (descriptive-only rework)
Status: Approved for implementation — independently revalidated 2026-08-19
Scope: Phase 1 only — receive-only CLI, manual antenna positioning, Linux.

Revision 2 superseded the text approved at commit `36c864e` after an
independent validation found nine release-blocking defects. Revision 3
superseded revision 2 after a second validation found seven more. Revision 4
follows a product decision and a third validation pass.

**The product decision.** Phase 1 no longer names a winner. Opportunistic
airband traffic cannot support one: the score is conditioned on a detection
threshold that moves with the antenna (§10.0), captures at two angles never
observe the same transmission, and the traffic itself varies minute to minute.
Revision 3 tried to hold that line with a ladder of eight gates. Revision 4
removes the ladder instead. `rtlangle` measures, ranks, and reports what is
wrong with the measurement; it does not decide. Everything that existed only to
justify a decision — `resolved_best_angle`, the paired confidence bounds, the
intersection–union argument, the sign-flip confirmation, and the wording that
announced a result — is gone, not softened.

What survives is what was always the useful part: two descriptive rankings,
detection yield beside every score, and a labelled list of the things that make
a comparison untrustworthy.

Nothing in this document may be described as verified until an implementation
exists and its tests have run.


---

## 1. Purpose, non-goals, and the safety boundary

### 1.1 Purpose

`rtlangle` measures how well an RTL-SDR receives AM airband transmissions as a
function of a dipole antenna's orientation. The operator rotates the antenna by
hand through a planned sequence of angles. At each angle the tool captures IQ
samples for a fixed duration under a frozen receiver configuration, detects
real transmissions inside the captured stream, computes reception-quality
metrics, and stores the result before moving on. At the end it ranks the
angles descriptively, and reports every reason the ranking might be misleading.
It does not decide which angle is best (§2, decision Q1).

### 1.2 Non-goals for Phase 1

Out of scope, and not to be added opportunistically: any GUI; servo, stepper or
ESP32 control; serial protocols; machine-learning or AI denoising; transmitter
direction finding; transmitter or aircraft identification; raw-IQ recording
(see §2, decision Q2); and placeholder implementations presented as complete.

A future `SerialServoAngleProvider` must be addable without modifying DSP,
metrics, persistence, or experiment logic. The `IAngleProvider` seam in §6.4 is
the whole of that provision; nothing else is built for it now.

### 1.3 Safety boundary

The RTL2832U + R828D hardware has **no transmit path**. There is no RF
emission capability to guard, and no runtime check can meaningfully assert the
absence of hardware that does not exist. The single outbound electrical path is
the bias-tee DC feed on the coax centre conductor, intended for powering an
external LNA.

Therefore:

- `rtlangle` never calls `rtlsdr_set_bias_tee` with a non-zero argument unless
  `Config::bias_tee` is true, which requires the explicit `--bias-tee` flag.
- In an interactive session, enabling the bias tee additionally requires a
  confirmation prompt that names the risk: DC on the feedline can damage a
  passive antenna or a receiver not expecting it.
- The README states the absence of a transmit path as a fact about the
  hardware, not as a property enforced by this program.

Receiving is passive and does not radiate. Nothing in this tool causes the
device to emit RF.

### 1.4 Legal and operational note

Airband reception legality varies by jurisdiction. The README states that the
operator is responsible for confirming that receiving and recording aeronautical
voice traffic is lawful where they are, and that `rtlangle` stores derived
metrics rather than audio (see §2, Q2). This is a documentation obligation, not
a runtime check.

---

## 2. Product decisions

Q1–Q6 were decided on 2026-08-19 before revision 2; Q7–Q9 after the second
validation; Q1 was then revisited and Q8 retired after the third. All are
normative for Phase 1. Where a decision constrains what the tool may claim, the
constraint is carried into §10 and §15.

| ID | Decision | Chosen | Consequence carried into this spec |
|---|---|---|---|
| Q1 | Strength of conclusion | **Descriptive only. Phase 1 never names a winner.** | There is no `resolved_best_angle`, no `resolution_method`, no `resolution_strength`, and no `resolution_reason`. §10.4 is a list of labelled warnings, not a decision ladder. §10.6 has one headline and it says the ranking is exploratory. |
| Q2 | Raw-IQ recording | **Removed from Phase 1** | `--save-iq` does not exist. No `raw/` directory. `.cu8` *replay* remains an input source (§7.5). Deferred to Phase 2 behind a byte-preserving source contract. |
| Q3 | Default guided experiment | **Two alternating rounds** | `--rounds` default 2, `--order` default `alternating`. Two captures per angle is below `min_captures_advisory`, so the default experiment raises warning W1. More rounds are recommended (§10.4) because they make the round-to-round spread visible, not because any number of them would resolve anything. |
| Q4 | Airband scan | **In Phase 1** | `ITunableSampleSource` (§6.2) is part of the release. Scan carries exposure accounting, DC exclusion, and a proved band-edge coverage rule (§12.5.1). |
| Q5 | Platform | **Linux only** | termios, POSIX `renameat`/`fsync`, `O_NOFOLLOW`, and `/usr/local` librtlsdr are assumed. No portability abstraction is written. Building elsewhere is unsupported and untested. |
| Q6 | Document handling | **Revised in place** | This file and the plan are the sole authorities. Git history and the validation artifacts are the provenance. |
| Q7 | AGC | **Removed from Phase 1** | `--agc` and `--allow-agc` do not exist. AGC is unconditionally disabled (§7.8). |
| Q8 | Pre-registering the tested metric | **Retired — the problem it solved no longer exists** | Revision 3 froze a `primary_metric` before capture so that looking at both rankings could not decide which one got tested. With Q1 descriptive-only, nothing is tested, so there is nothing to pre-register and no selection effect to guard against. The field is gone; `report_metric` (§7.4) remains as a display choice. |
| Q9 | Estimand for opportunistic airband | **Stated explicitly, with detection yield printed beside every score** | §10.0 defines what is being estimated and what it is conditional on; §10.2 always prints detection yield next to the SNR ranking; §10.4 W6 raises a warning when the score ranking and the yield ranking disagree. |

**Q1 and the earlier metric decision.** The project owner originally chose
"store both metrics, pick at report time." That decision survives intact and is
now the whole of the metric story: both metrics are always computed, always
persisted, and always printed, and `report_metric` chooses which one the report
lists first. Nothing downstream depends on the choice.

**Q9 and why it still matters.** Revision 2 scored a capture as the median SNR
of the events it *detected*. Detection requires the signal to clear the squelch,
about 4.74 dB at defaults (§9.3). An angle that receives better therefore
detects transmissions a worse angle never hears at all — and those extra, weak
events pull its median *down*. The ranking can invert. Removing the winner does
not remove that effect; it removes the pretence that the tool had accounted for
it. §10.0 names the estimand, §10.2 prints detection yield beside the score, and
§10.4 W6 says so out loud when the two disagree.


## 3. Platform and verified environment

Verified by direct inspection on the development machine on 2026-08-19:

| Component | Version | How verified |
|---|---|---|
| OS | Ubuntu 24.04.4 LTS | `lsb_release` |
| Compiler | g++ 13.3.0 | `g++ --version` |
| CMake / Ninja | 3.28.3 / 1.11.1 | `cmake --version`, `ninja --version` |
| librtlsdr | 0.6.0-128-g240b, resolved from `/usr/local` | `pkg-config --modversion librtlsdr` |
| librtlsdr fork | rtl-sdr-blog | `RTL-SDR Blog V4 Detected` string present in the shared object |
| FFTW double precision | 3.3.10 | `pkg-config --modversion fftw3` |
| FFTW **single** precision | 3.3.10, `-lfftw3f` | `pkg-config --modversion fftw3f`; `fftwf_plan_dft_1d` confirmed defined in `libfftw3f.so.3` and **absent** from `libfftw3.so.3` |
| libusb | 1.0.27 | `pkg-config --modversion libusb-1.0` |

**Not verified, and not to be claimed as verified:** the presence, freedom, or
correct operation of any RTL-SDR device. No device and no conflicting SDR
process were visible to the validation inspection. Hardware status is
`pending` until a hardware test run produces the machine-readable outcome
defined in §14.3.

**Why the fork matters.** Tuner gain is the controlled variable of the entire
experiment. The stock Ubuntu `librtlsdr` carries the wrong gain table for an
RTL-SDR Blog V4, so a requested gain would be silently snapped to a different
applied gain. `rtlangle` records the driver version string, the device name,
and the *read-back applied* gain in every session, so a wrong driver is visible
in the record rather than hidden in the results.

**FFTW precision.** The implementation uses the single-precision API
(`fftwf_*`) and therefore must link `fftw3f`, via
`pkg_check_modules(FFTW3F REQUIRED IMPORTED_TARGET fftw3f)` and
`PkgConfig::FFTW3F`. Mixing `fftwf_*` calls with `-lfftw3` is a link error and
was the state of the superseded text. The two precisions are never mixed.

---

## 4. The physical experiment

### 4.1 What the operator physically does

The antenna is a half-wave dipole on a mount that allows rotation in the
horizontal plane about a vertical axis through the feedpoint. For each planned
angle the operator rotates the antenna to that angle, confirms, waits out a
settling delay, and stands clear while the capture runs.

### 4.2 Definition of 0° and of the rotation

Absolute bearing is deliberately *not* required, because the tool never claims
a direction. Angles are defined relative to a physical reference the operator
establishes once:

- **Rotation plane and axis.** Azimuth: rotation in the horizontal plane about
  a vertical axis passing through the dipole feedpoint.
- **Marked arm.** Before the first capture the operator marks **one** of the two
  dipole elements — a band of tape is enough. The angle is the bearing of that
  marked arm. Marking one arm is what makes the rotation a full 360° circle with
  360 distinct orientations, which is what the circular arithmetic of §4.5
  assumes.
- **0° mark.** 0° is a fixed physical mark the operator chooses before the
  first capture and does not move for the duration of the session — for
  example, "marked arm pointing along the balcony rail, towards the street."
  It is entered once as `--angle-reference` and stored verbatim in session
  metadata.
- **Positive direction.** Increasing angle is clockwise when viewed from above.
- **Held constant for the whole session,** and recorded as free text in
  `--setup-note`: antenna height above ground, feedline routing and length,
  mount position, and where the operator stands during a capture. A human body
  within roughly a wavelength (about 2.5 m at 118 MHz) perturbs the pattern; if
  the operator stands in a different place at different angles, that difference
  is inside the measurement.

**Why 0° and 180° are still two measurements.** An ideal dipole in free space
has a pattern symmetric under a 180° rotation of its axis, so theory predicts
that θ and θ+180 receive identically. A dipole on a balcony does not: the
feedline, the mount, the wall behind it, and the operator's own position break
that symmetry. `rtlangle` therefore treats θ and θ+180 as **distinct
orientations** and never folds them together. A large measured difference
between the two is not a measurement fault — it is evidence that the
environment, not the antenna, dominates the pattern, and the report says so
(§10.5). This is why §4.5 uses 360° circular statistics and not axial
(modulo-180°) statistics.

Because 0° is arbitrary and relative, results are comparable **within** one
session and are not comparable across sessions unless the reference and setup
notes match.


### 4.3 Angle generation

Default sequence: `0, 15, 30, 45, 60, 75, 90` from the defaults
`--start-deg 0 --end-deg 90 --step-deg 15`.

The generation rule is exactly:

```
angles = { start + i*step  :  i = 0,1,2,...  while  start + i*step <= end + 1e-9 }
```

The endpoint is included only when it lies on the grid. `--start-deg 0
--end-deg 90 --step-deg 20` therefore produces `0, 20, 40, 60, 80` and **not**
`0, 20, 40, 60, 80, 90`. A non-grid `--end-deg` produces an informational
message naming the last generated angle, so the operator is never surprised by
a missing endpoint. The superseded text contained two contradictory rules here;
this is the single rule.

`--angles "0,30,60,90"` overrides the three range flags and is used verbatim
after validation, deduplication (within 1e-6), and ascending sort.

### 4.4 Angle validation

Each rule produces its own diagnostic message naming the offending value:

- `step_deg` finite and `> 0`.
- `start_deg`, `end_deg` finite and within `[0, 360]`.
- `end_deg >= start_deg`.
- Generated angle count within `[2, 180]`.
- Explicit `--angles` entries finite and within `[0, 360]`.
- If the generated set contains a pair `(θ, θ+180)`, emit an informational
  message (not a warning, and not an error) that the two are predicted to be
  equivalent for an ideal dipole, so any measured difference between them is
  environmental (§4.2). Sampling both is a legitimate and useful choice; the
  message exists so the operator knows what a difference would mean.

### 4.5 Circular angle arithmetic

Angles are directions on a circle, so linear arithmetic is wrong near the
wrap point. Two helpers are normative and used everywhere an angle difference
or an angle average is computed:

```
circular_distance_deg(a, b) = min( |a-b| mod 360 , 360 - (|a-b| mod 360) )
circular_mean_deg(xs)       = normalize_deg( atan2( mean sin(xs), mean cos(xs) ) )
```

`normalize_deg` maps into `[0, 360)`. `circular_distance_deg(350, 10)` is 20,
not 340. The spread of entered actual angles is reported as the circular
standard deviation `sqrt(-2 * ln(R))` in degrees, where `R` is the resultant
vector length; when `R` is 0 the spread is reported as undefined rather than
infinite.

### 4.6 Visit order

`--order forward | reverse | alternating | random`.

- `forward` — ascending in every round.
- `reverse` — descending in every round.
- `alternating` — round 1 ascending, round 2 descending, alternating onward.
- `random` — shuffled independently per round with `std::mt19937` seeded from
  `--seed`. When `--seed` is absent, one value is drawn from
  `std::random_device`, used, and **recorded in session metadata**, so the
  realised order is reproducible after the fact.

Default: `alternating` (with the default `--rounds 2`).

**What alternating does and does not do.** Airband traffic varies minute to
minute, so a single forward pass confounds angle with time. Alternating the
direction each round *reduces* that confound by balancing the linear component
of any time trend across angles. It does not eliminate it: with 7 angles at 60 s
plus positioning, one round spans roughly 8–10 minutes, so the first and last
angles of a round are still sampled further apart in time than the middle ones,
and non-linear traffic variation is not balanced at all. Any claim that
alternating "breaks the correlation" is false and must not appear in the code,
the report, or the documentation.

### 4.7 Actual-angle entry

The operator's estimate of the angle actually achieved is prompted at every
visit; blank input accepts the planned angle. Validation: the input must parse
as a finite double within `[0, 360]`; anything else re-prompts with the reason.
If `circular_distance_deg(actual, planned) > max_angle_deviation_deg` (default
30) the tool requires an explicit confirmation before accepting, which catches
typing `9` for `90`.

Both values are persisted. **Ranking uses `planned_deg`,** because that is the
experimental factor; `actual_deg` is evidence about how well the factor was
realised. The report flags any accepted attempt whose circular deviation
exceeds `angle_deviation_warn_deg` (default 5), and §10.4 warning W8 names any
ranked angle carrying such a flag.

---

## 5. Architecture and dependency direction

Dependencies point downward only. No lower layer includes a header from a
higher one. `experiment/` names only abstract interfaces, which is what makes
the end-to-end test hardware-free and the future servo provider a drop-in.

```
app/          main, CliParser, RunCommand, ScanCommand, DeviceCheckCommand, Menus
   |
ui/           ITerminalUi <- AnsiTerminalUi | ScriptedTerminalUi
              ReportRenderer, TerminalSafeText
   |
experiment/   ExperimentController, VisitPlan, CaptureRunner, Aggregator, QualityChecks
   |
   +-- angle/    IAngleProvider   <- ManualAngleProvider | FixedAngleProvider
   +-- source/   ISampleSource    <- SyntheticSource | IqFileSource | RtlSdrSource
   |             ITunableSampleSource <- RtlSdrSource   (scan only)
   +-- persist/  ISessionStore    <- JsonSessionStore
   |             AtomicWrite, CsvExport, SessionLoader, PathSafety
   +-- metrics/  NoiseFloorEstimator, EventDetector, SnrEstimator
         |
       dsp/      FirDesign, OffsetMixer, FirDecimator, ChannelFilter,
                 AmDemodulator, Framer, Spectrum, Chain
         |
       core/     Config, AngleMath, Records, Statistics, Decibels, Version
```

**Five interfaces** form the whole abstraction boundary:
`ISampleSource`, `ITunableSampleSource`, `IAngleProvider`, `ITerminalUi`,
`ISessionStore`. The superseded text claimed four while using a concrete
`SessionStore`; storage is now an interface so persistence faults can be
injected in tests.

**Dependency direction rule, normative:** production code under `src/` must
never include anything from `tests/`. The synthetic AM-voice generator is
production code (`src/source/synthetic_source.*`) because a production source
type depends on it; tests may call it, and `tests/support/` holds only
assertions and fixtures.

### 5.1 Build targets

| Target | Contents |
|---|---|
| `rtlangle_core` | static library: `core/ dsp/ metrics/ source/ angle/ persist/ ui/ experiment/` |
| `rtlangle` | the CLI executable: `app/` |
| `rtlangle_tests` | all hardware-free unit and integration tests (doctest) |
| `rtlangle_hardware_test` | separate executable, CTest label `hardware`, pass/skip/fail exit codes per §14.3 |

The hardware test is a separate *executable* because CTest labels apply to
registered tests, not to doctest cases inside one binary. Keeping it separate is
what makes `ctest -LE hardware` a true hardware-free run.

### 5.2 Dependencies and build gates

- C++20, CMake 3.24+, Ninja. `std::numbers::pi_v<double>`; `M_PI` is not used.
- `PkgConfig::FFTW3F` (`fftw3f`), `librtlsdr` via pkg-config, `Threads::Threads`.
- nlohmann/json 3.11.3 and doctest 2.4.11: `find_package` first, `FetchContent`
  fallback pinned by URL hash. Resolved versions echoed at configure time.
- Warnings: `-Wall -Wextra -Wpedantic -Wconversion -Wshadow` on project targets
  only (not on fetched dependencies).
- `-DRTLANGLE_WITH_RTLSDR=OFF` builds the synthetic-only tool and the complete
  hardware-free test suite. Both `ON` and `OFF` configurations must build clean.
- A Debug configuration with `-fsanitize=address,undefined` must run the
  hardware-free suite clean.

---

## 6. Interfaces

All five are normative. Signatures are written in the `rtlangle` namespace with
sub-namespaces `dsp`, `metrics`, `ui`, `app` as appropriate.

### 6.1 `ISampleSource`

A source is a blocking, cancellable, timeout-bounded producer of normalised
complex samples at a fixed rate. The superseded `bool read(...)` could block
forever and could not distinguish "no data yet" from "device gone".

```cpp
enum class ReadStatus { Ok, Timeout, EndOfStream, Error, Cancelled };

struct ReadResult {
  std::size_t  samples  = 0;    // samples actually written into `out`
  ReadStatus   status   = ReadStatus::Ok;
  std::string  error;           // non-empty only when status == Error
  // Cumulative count of samples the HOST dropped because the ring buffer
  // between the device worker and the reader was full. See the note below on
  // why this is not a device-level overrun count.
  std::uint64_t host_dropped_samples = 0;
};

struct SourceInfo {
  std::string   driver;              // "librtlsdr 0.6.0-128-g240b" | "synthetic" | "iq-file"
  std::string   device_name;
  std::string   serial;
  std::uint32_t requested_sample_rate_hz = 0;
  std::uint32_t applied_sample_rate_hz   = 0;
  std::uint32_t requested_center_hz      = 0;   // fc + applied_offset_hz
  std::uint32_t applied_center_hz        = 0;
  int           requested_gain_tenth_db  = 0;
  int           applied_gain_tenth_db    = 0;
  bool          agc_enabled              = false;
  int           ppm                      = 0;
  std::int64_t  applied_offset_hz        = 0;   // integer NCO offset actually used
};

class ISampleSource {
public:
  virtual ~ISampleSource() = default;
  virtual SourceInfo info() const = 0;
  // Fills at most out.size() samples. Returns early on deadline, cancel,
  // end of stream, or error. Never blocks past `deadline`.
  virtual ReadResult read(std::span<std::complex<float>> out,
                          std::chrono::steady_clock::time_point deadline) = 0;
  virtual void flush()  = 0;   // discard buffered samples (used after settling)
  virtual void cancel() = 0;   // wake any blocked read; subsequent reads return Cancelled
  // Fraction of raw input samples at the ADC rails since the last flush().
  virtual double clipped_fraction() const = 0;
};
```

Every implementation must honour the deadline, must return `Error` with a
populated `error` string when its worker thread fails, and must make a worker
failure wake a blocked reader rather than let it time out silently.

`requested_*` versus `applied_*` exists because librtlsdr snaps sample rate,
frequency, and gain to what the hardware supports. Snapping is legitimate and
expected; what must not happen is the applied values *changing* part-way through
a session. The applied values are therefore persisted per attempt and §10.4
§10.4 warning W7 compares them against a per-segment baseline, not against the
request.

**Dropped samples are a host-side count, and the name says so.** `librtlsdr`'s
asynchronous API reports no device-level or USB-level overflow counter; a true
device overflow reaches this program only as an unobservable discontinuity in
the sample stream. The only quantity `rtlangle` can honestly measure is how many
samples *it* discarded because its own bounded ring buffer was full while the
reader was busy in the DSP chain. That is what `host_dropped_samples` counts,
and it is what the persisted `host_dropped_samples` field and the §10.5 warning
refer to. No document, report string, or column header may call it a device
overrun.

### 6.2 `ITunableSampleSource`

Scan needs to retune; the experiment must not. Rather than down-casting an
experiment source, retuning is a separate interface that only the scan path
requires.

```cpp
class ITunableSampleSource : public ISampleSource {
public:
  // Retune and discard all buffered samples captured at the previous
  // frequency. Returns false and sets `error` on failure.
  virtual bool retune(std::uint32_t center_hz, std::string& error) = 0;
};
```

`RtlSdrSource` implements both. `SyntheticSource` implements
`ITunableSampleSource` too, so scan is testable without hardware: its synthetic
band contains transmitters at known frequencies and known duty cycles.

### 6.3 `CaptureRunner`

Not an interface — a concrete collaborator — but its contract is normative
because several defects lived here.

```cpp
struct CaptureOutcome {
  enum class Status { Ok, Timeout, SourceError, Clipped, InsufficientSamples, Cancelled };
  Status                    status = Status::Ok;
  std::string               detail;
  std::chrono::system_clock::time_point started_utc;   // taken BEFORE the first read
  double                    wall_duration_s = 0.0;
  std::uint64_t             host_dropped_samples = 0;  // this capture only, not cumulative
  double                    clipped_fraction = 0.0;
  SourceInfo                applied;                   // snapshot at capture time (§10.4 W7)
  std::vector<std::complex<float>> channel;            // channel-rate, transient-trimmed
  std::vector<float>               audio;              // channel-rate audio band
  std::size_t               audio_latency_samples = 0; // see §8.4
};
```

Rules:

1. `started_utc` is sampled **before** the first `read`, never after the capture
   loop. The superseded controller pseudocode timestamped after reading.
2. `host_dropped_samples` is `dropped_at_end - dropped_at_start` for this
   capture. A cumulative counter would make every later capture look worse than
   the first.
3. The read deadline is `duration_s * read_timeout_factor + read_timeout_slack_s`
   (defaults 1.5 and 5.0). Exceeding it yields `Timeout`, not a hang.
4. Startup transients are trimmed before the channel buffer is filled: the
   first `chain_latency_samples + transient_guard_samples` channel samples are
   discarded, where `chain_latency_samples` is the **ceiling** of the exact
   chain latency (§8.4) and `transient_guard_samples` is twice the total FIR
   length of the chain expressed at the channel rate. Using the ceiling
   guarantees the trim never leaves part of the transient behind.
5. After trimming, exactly `round(duration_s * channel_rate_hz)` channel samples
   are collected. Fewer means `InsufficientSamples`.
6. `clipped_fraction > max_clipped_fraction` yields `Clipped`.
7. `applied` is a copy of `source.info()` taken at capture start. It is what the
   attempt persists and what §10.4 warning W7 compares against the
   receiver-segment baseline (§11.2). Without it, W7 would have nothing to
   examine.
8. Any status other than `Ok` produces a failed attempt (§11.3); it never
   produces metrics.


### 6.4 `IAngleProvider`

The forward-compatibility seam. Unchanged in shape from the approved design,
extended so that settling is the controller's responsibility rather than the
provider's, and so the provider can report an automated angle readback.

```cpp
struct AngleOutcome {
  enum class Cmd { Proceed, Retry, Skip, Quit };
  Cmd                        cmd        = Cmd::Proceed;
  double                     actual_deg = 0.0;   // operator estimate, or servo feedback
  std::optional<std::string> note;
};

class IAngleProvider {
public:
  virtual ~IAngleProvider() = default;
  virtual std::string_view name() const = 0;     // "manual" | future "serial-servo"
  virtual bool is_automated() const = 0;         // recorded in session metadata
  // Ask for the antenna to be placed at `planned_deg` and report the outcome.
  // `attempt` is 1 for a first visit and increments on retry, so the provider
  // can word its prompt correctly.
  virtual AngleOutcome request(double planned_deg, int attempt) = 0;
};
```

Settling (`--settle`) is applied by `ExperimentController` between a `Proceed`
outcome and the capture, and is followed by `source.flush()`. Putting it in the
controller keeps the timing identical for every provider, so a future servo
provider cannot accidentally change the settling behaviour and thereby the
measurement.

`ManualAngleProvider` drives `ITerminalUi`. `FixedAngleProvider` returns
`Proceed` with `actual_deg == planned_deg` and never prompts; it serves
`--non-interactive` replay and is the second implementation that proves the
interface is not coupled to a terminal.

### 6.5 `ITerminalUi`

```cpp
class ITerminalUi {
public:
  virtual ~ITerminalUi() = default;
  virtual bool interactive() const = 0;   // false for pipes, --non-interactive
  virtual int  menu(std::string_view title, std::span<const MenuItem> items,
                    int initial_index) = 0;           // index, or -1 for cancel
  virtual std::optional<std::string> prompt_line(std::string_view label,
                                                 std::string_view default_value) = 0;
  virtual bool confirm(std::string_view question, bool default_yes) = 0;
  virtual void heading(std::string_view) = 0;
  virtual void info(std::string_view)  = 0;
  virtual void warn(std::string_view)  = 0;
  virtual void error(std::string_view) = 0;
  virtual void progress(std::string_view label, double fraction) = 0;
  virtual void table(const Table&) = 0;
};
```

Behavioural requirements are in §12.

### 6.6 `ISessionStore`

Storage is an interface so that persistence faults can be injected without a
filesystem, and so the controller depends on a contract rather than on JSON.

The mutating surface is deliberately **one** call. Revision 2 exposed
`append_attempt`, `set_disposition`, and `record_retry_intent` separately while
§11.4 required a retry to supersede the attempt and record the retry intent *in
one commit*. Three calls cannot satisfy a one-commit requirement.

```cpp
enum class SessionState { Running, Paused, Completed, Aborted };

struct PendingRetry { std::string visit_id; int next_attempt; };

// Everything one visit outcome changes, committed atomically or not at all.
struct VisitCommit {
  AttemptRecord               attempt;      // the attempt just produced
  Disposition                 disposition;  // accepted | superseded | abandoned
  std::optional<PendingRetry> pending_retry;  // set on retry; nullopt clears any existing
};

// What the caller must do about the result. See 11.1 for the write protocol
// and which boundary produces which value.
enum class CommitOutcome {
  Committed,            // rename() succeeded and the directory fsync succeeded.
                        // The record on disk contains the commit and will
                        // survive power loss.
  CommittedNotDurable,  // rename() succeeded; the directory fsync did not. The
                        // commit IS in the current filesystem state and every
                        // later read sees it, but it may not survive power loss.
                        // The caller warns, records it in the session, and
                        // continues. It is NOT a failed commit.
  NotCommitted,         // failed at or before rename(). The canonical record is
                        // byte-for-byte what it was. The caller may retry the
                        // commit or abort.
  Indeterminate,        // rename() returned an error the platform does not let
                        // us classify. The caller MUST run the reconciliation
                        // of 11.1 before doing anything else.
};
struct CommitResult { CommitOutcome outcome = CommitOutcome::Committed; std::string detail; };

class ISessionStore {
public:
  virtual ~ISessionStore() = default;
  virtual const SessionRecord& record() const = 0;

  // Opens a receiver segment: a span of attempts taken through one continuous
  // device configuration. Called once at session start and once per resume.
  // The returned id is stamped on every attempt taken in the segment.
  virtual std::string begin_receiver_segment(const SourceInfo& applied) = 0;

  // The single mutator. Disposition, retry intent, and the attempt itself land
  // in the same rename() or none of them do. Returns rather than throws: a
  // failed commit is an expected operational condition with four distinct
  // meanings, and an exception can only express one of them.
  [[nodiscard]] virtual CommitResult commit_visit(const VisitCommit&) = 0;

  // Re-reads session.json from disk and replaces the in-memory record with it.
  // The recovery step for CommitOutcome::Indeterminate (11.1).
  [[nodiscard]] virtual CommitResult reload_from_disk() = 0;

  // Quit: writes the summary and sets state to Paused. finished_utc stays null.
  [[nodiscard]] virtual CommitResult pause(const SessionSummary& partial) = 0;
  // Normal end: writes the summary, sets Completed, stamps finished_utc.
  [[nodiscard]] virtual CommitResult finalize(const SessionSummary& summary) = 0;
  // Unrecoverable end: writes whatever summary can be built, sets Aborted,
  // stamps finished_utc, and records the reason. Terminal, like Completed.
  [[nodiscard]] virtual CommitResult abort(const SessionSummary& partial,
                                           std::string_view reason) = 0;

  virtual void flush_exports() = 0;    // regenerate measurements.csv from canonical JSON
};
```

**There is nothing to supersede.** An attempt is persisted once, at the moment
the operator's choice for it is known, and it is persisted with its *final*
disposition. A retry commits the just-finished attempt as `superseded` together
with the `pending_retry` that names its replacement; it never revisits an
already-committed record. That is why one call is sufficient. Revision 2's
design committed the attempt as `accepted` first and then went back to change
it, which is what made three calls seem necessary.

There is no `begin_visit`. Revision 2 had one, but it wrote nothing durable that
`commit_visit` does not write, and an in-memory "visit started" marker that
survives no crash is a false comfort: §11.3.1 already defines a crash before the
attempt commits as "re-enter this visit at the same attempt number".

**Who calls what, so the seam has one owner.**

| Method | Called by | When |
|---|---|---|
| `begin_receiver_segment` | the **app layer** (`RunCommand`) | after the source is opened and the session record exists, before the controller is constructed. The returned id is passed *into* the controller, which stamps it on every attempt and never creates one. |
| `commit_visit`, `reload_from_disk` | the **controller** | `commit_visit` exactly once per visit outcome; `reload_from_disk` only in response to `Indeterminate`. |
| `pause` / `finalize` / `abort` | the **app layer** | after the controller returns, and after `aggregate` has produced the summary the call requires. |

`ExperimentController` cannot call `pause`, `finalize`, or `abort`, because all
three take a `SessionSummary` and the controller is forbidden to compute one —
aggregation lives above it (§10). **The controller always leaves the session
`running`**; the app layer decides which terminal or paused state to write. It
signals which by its return value.

Between the controller returning and the app layer writing a state, the record
still says `running`. That window is not a defect: a crash inside it is exactly
the unclean-shutdown case §11.5 already handles, and the alternative — letting
the controller write a state it cannot summarise — is what produced the
quit-finalises-then-resume contradiction in revision 2.

`JsonSessionStore` is the production implementation (§11). Tests use a
fault-injecting decorator that fails at each defined boundary and asserts the
`CommitOutcome` §11.1 requires for that boundary.


## 7. Configuration

Precedence, lowest to highest: built-in defaults, `--config <file.json>`,
command-line flags, interactive edits in the menu. The fully resolved `Config`
is serialised verbatim into `session.json`, so a session is reproducible from
its own record.

**This section is the authoritative field list.** Every field below exists in
`Config`, in the CLI parser, in the JSON schema, in the resume classification of
§11.5, in the per-command applicability table of §7.9, and in the README table.
No field may be consumed anywhere before it appears here, and **every field has
a CLI flag** — a field with no flag could not be overridden, which would make
its §11.5 classification meaningless. The plan does not restate this list; its
tests derive from `Config`'s own serialisation, so a field added here without a
flag, a bound, a command scope, or a classification fails the suite rather than
drifting between two hand-maintained lists.

Neither this document nor the plan states how many fields there are. A count in
prose is a number that goes stale silently; the count is obtained by running

```bash
awk '/^### 7\.1/,/^### 7\.8/' <this file> | grep -cE '^\| `[a-z_]+` / `--'
```

### 7.1 Receiver

| Field / flag | Type | Default | Bounds and validation |
|---|---|---|---|
| `device_index` / `--device` | int | 0 | `[0, 63]` |
| `center_hz` / `--freq` | uint32 | *(unset)* | `[24e6, 1.766e9]`; accepts `118.1M`, `121500k`; **required by `run`** (§7.9) |
| `sample_rate_hz` / `--sample-rate` | uint32 | 1024000 | must lie in `[225001,300000]` or `[900001,3200000]`; see the decimation rule below |
| `gain_tenth_db` / `--gain` | int or `max` | `max` | `[-10, 500]` or `max`; snapped to the device table, applied value read back |
| `ppm` / `--ppm` | int | 0 | `[-200, 200]` |
| `offset_tune_hz` / `--offset-tune-hz` | int64 | 250000 | 0 disables with a warning; see the offset rule below |
| `channel_bw_hz` / `--channel-bw` | uint32 | 8000 | `[8000, 25000]`; see the channel-bandwidth rules below |
| `channel_rate_hz` / `--channel-rate` | uint32 | 32000 | `[16000, 96000]` |
| `bias_tee` / `--bias-tee` | bool | false | interactive confirmation required (§1.3) |

There is no `--agc` and no `--allow-agc` (decision Q7). AGC is unconditionally
disabled; see §7.8.

**Cross-field rules, each with its own validation message naming both fields.**
Revision 2 stated only `channel_bw_hz < channel_rate_hz`, which permits
configurations the DSP chain cannot build: at the old lower bound of
`channel_bw_hz = 2000` the channel filter passes ±1 kHz while the audio path
expects 3.4 kHz of content, and at `channel_bw_hz = 25000` with
`channel_rate_hz = 16000` the channel filter's stopband edge of 13.5 kHz lies
above the 8 kHz Nyquist frequency. Both are now refused.

| Rule | Why |
|---|---|
| `channel_bw_hz + 2000 < channel_rate_hz` | The channel filter's stopband edge is `channel_bw_hz/2 + 1000` (§8.2) and must stay below the channel-rate Nyquist frequency. |
| `channel_bw_hz >= 8000` | The audio low-pass stopband is 4000 Hz (§8.2). A channel narrower than ±4 kHz would remove the audio band the demodulator is specified to recover. |
| `sample_rate_hz % channel_rate_hz == 0` | The decimation cascade is integer-factor only. |
| `2 <= sample_rate_hz / channel_rate_hz <= 256` | Bounds the cascade. |
| every prime factor of `sample_rate_hz / channel_rate_hz` is `<= 16` | The factorisation helper builds stages from factors in `[2,16]`. A ratio of, say, 17 has no such factorisation; the error names the ratio and suggests the nearest compatible `sample_rate_hz`. |
| `\|offset_tune_hz\| + channel_bw_hz/2 < 0.45 * sample_rate_hz` | Keeps the wanted channel inside the usable part of the band after offset tuning (§8.1). |

### 7.2 Experiment

| Field / flag | Type | Default | Bounds and validation |
|---|---|---|---|
| `duration_s` / `--duration` | double | 60.0 | `(0, 3600]` |
| `settle_s` / `--settle` | double | 3.0 | `[0, 600]` |
| `start_deg` / `--start-deg` | double | 0 | §4.4 |
| `end_deg` / `--end-deg` | double | 90 | §4.4 |
| `step_deg` / `--step-deg` | double | 15 | §4.4 |
| `angles_deg` / `--angles` | list<double> | *(unset)* | §4.4; overrides the range |
| `rounds` / `--rounds` | int | **2** | `[1, 100]`; also bounded by §7.7 |
| `order` / `--order` | enum | `alternating` | `forward\|reverse\|alternating\|random` |
| `seed` / `--seed` | uint64 | drawn and recorded | any |
| `angle_reference` / `--angle-reference` | string | `""` | ≤ 200 chars; prompted interactively when empty (§4.2) |
| `setup_note` / `--setup-note` | string | `""` | ≤ 500 chars |
| `max_angle_deviation_deg` / `--max-angle-deviation-deg` | double | 30.0 | `(0, 180]` |
| `angle_deviation_warn_deg` / `--angle-deviation-warn-deg` | double | 5.0 | `(0, 180]` |

### 7.3 Measurement

| Field / flag | Type | Default | Bounds and validation |
|---|---|---|---|
| `noise_percentile` / `--noise-percentile` | double | 20.0 | `(0, 50]` |
| `probe_percentile` / `--probe-percentile` | double | `min(5, noise_percentile)` | `(0, noise_percentile]` |
| `open_db` / `--open-db` | double | 6.0 | `(0, 60]`, `> close_db` |
| `close_db` / `--close-db` | double | 3.0 | `[0, 60]` |
| `min_event_ms` / `--min-event-ms` | double | 300.0 | `[10, 60000]` |
| `merge_gap_ms` / `--merge-gap-ms` | double | 200.0 | `[0, 60000]` |
| `min_valid_events` / `--min-valid-events` | int | 3 | `[1, 1000]` |
| `max_active_fraction` / `--max-active-fraction` | double | 0.70 | `(0, 1)` |
| `carrier_prominence_db` / `--carrier-prominence-db` | double | 10.0 | `(0, 60]`; calibrated by §14.2 Monte Carlo |
| `carrier_persistence` / `--carrier-persistence` | double | 0.90 | `(0, 1]` |
| `audio_guard_ms` / `--audio-guard-ms` | double | 50.0 | `[0, 5000]` |
| `min_audio_window_ms` / `--min-audio-window-ms` | double | 100.0 | `[10, 60000]` |
| `max_clipped_fraction` / `--max-clipped-fraction` | double | 1e-4 | `[0, 1)` |
| `max_retained_events` / `--max-retained-events` | int | 32 | `[4, 500]`; how many per-event records an attempt keeps in `session.json`. Metrics are computed over **every** valid event; only the persisted detail is capped, and the attempt records `events_total` beside `events_retained` so the truncation is visible (§11.3). See §7.7 for why a cap exists. |
| `read_timeout_factor` / `--read-timeout-factor` | double | 1.5 | `[1.0, 10.0]` |
| `read_timeout_slack_s` / `--read-timeout-slack` | double | 5.0 | `[0.1, 600]` |

### 7.4 Reporting thresholds

Phase 1 takes no decision (Q1), so this table holds no test parameters. Every
field below sets the threshold at which the report *says something*, and nothing
below changes what is measured or what is ranked.

| Field / flag | Type | Default | Meaning |
|---|---|---|---|
| `report_metric` / `--report-metric` | enum | `channel` | Which ranking the report lists first: `channel` or `audio`. Display only — both rankings are always computed and always printed. `ReportOrOperational`, and an interactive session may change it at report time (§10.3). |
| `min_captures_advisory` / `--min-captures` | int | 4 | Below this many accepted `ok` captures at a ranked angle, warning **W1** fires for that angle; `[2, 100]`. It is advice about how much data stands behind a median, not a threshold anything passes. |
| `min_effect_db` / `--min-effect-db` | double | 1.0 | When the top two angles' scores differ by less than this, warning **W3** fires; `[0, 30]`. A display threshold for "this gap is too small to act on", not an effect-size test. |
| `yield_concordance_ratio` / `--yield-concordance-ratio` | double | 0.90 | Warning **W6**: fires when the top-scoring angle's detection yield is below this fraction of the best yield among ranked angles; `(0, 1]`. A point comparison — see §9.5. |
| `noise_drift_warn_db` / `--noise-drift-warn-db` | double | 3.0 | Warning **W5**: cross-angle noise-floor spread above this; `[0.1, 30]` |

There is no `primary_metric` (decision Q8) and no `family_alpha`: the first
existed to pre-register a test and the second to size one, and Phase 1 runs no
test.


### 7.5 Session, source, and output

| Field / flag | Type | Default | Bounds and validation |
|---|---|---|---|
| `session_root` / `--session-dir` | path | `sessions/` | must not be a symlink; created 0700 |
| `label` / `--label` | string | `""` | slug `[A-Za-z0-9._-]{0,32}` after sanitisation (§13) |
| `resume_dir` / `--resume` | path | *(unset)* | existing session directory; §11.5 |
| `source_spec` / `--source` | string | `rtlsdr` | `rtlsdr` \| `synthetic` \| `file:<path.cu8>`; the file must exist, be a regular file, and not be a symlink |
| `synthetic_snr_db` / `--synthetic-snr-db` | double | 12.0 | `[-20, 60]` |
| `synthetic_duty` / `--synthetic-duty` | double | 0.25 | `(0, 1)` |
| `no_color` / `--no-color` | bool | false | also honours `NO_COLOR` and non-TTY stdout |
| `non_interactive` / `--non-interactive` | bool | false | requires `--source synthetic` or `file:`, since `ManualAngleProvider` cannot function without prompts |

**`.cu8` replay: metadata, matching, and lifecycle.** A raw `.cu8` file carries
no header, so nothing in the bytes says what sample rate, centre frequency, or
gain produced them. Revision 2 left this undefined. The rules are:

- The file is interpreted at the `sample_rate_hz`, `center_hz`, `gain_tenth_db`,
  and `ppm` of the current `Config`. `IqFileSource` reports those as **both**
  `requested_*` and `applied_*`, because a file has no hardware to snap
  anything, and sets `driver` to `"iq-file"` and `device_name` to the file's
  basename.
- If a sidecar `<path>.cu8.json` exists, it is read and must contain
  `sample_rate_hz` and `center_hz`. A sidecar that disagrees with the resolved
  `Config` is a **configuration error** naming both values — never a silent
  override, and never a silent mismatch. A sidecar is optional; its absence is
  not an error, and the tool never writes one in Phase 1 (decision Q2).
- The sidecar is opened under the same `O_NOFOLLOW` and 1 MiB size rules as a
  config file (§13).
- The stream is consumed **sequentially across the whole session**: visit 2
  continues where visit 1 stopped. A file is a recording of one span of time,
  and rewinding it for every visit would present the same seconds as if they
  were different captures at different angles.
- End of file yields `EndOfStream`; the capture that hits it ends with
  `InsufficientSamples`, and every remaining visit does the same. The tool does
  not loop the file — looping would fabricate replicates that do not exist.
  Preflight computes whether the file is long enough for the planned session
  from its size and warns, naming the shortfall in seconds, before the first
  capture.
- A file whose length is not a whole number of `u8` I/Q pairs is truncated to
  the last whole pair, and the dropped odd byte is reported once.

### 7.6 Scan

| Field / flag | Type | Default | Bounds |
|---|---|---|---|
| `scan_start_hz` / `--scan-start` | uint32 | 118000000 | `[24e6, 1.766e9]` |
| `scan_end_hz` / `--scan-end` | uint32 | 136975000 | `> scan_start_hz` |
| `scan_channel_hz` / `--scan-channel` | uint32 | 25000 | `[6250, 200000]` |
| `scan_dwell_ms` / `--scan-dwell` | double | 250.0 | `[20, 10000]` |
| `scan_passes` / `--scan-passes` | int | 4 | `[1, 100]` |
| `scan_dc_exclusion_hz` / `--scan-dc-exclusion` | uint32 | 30000 | `[0, sample_rate_hz/4]` |
| `scan_usable_fraction` / `--scan-usable-fraction` | double | 0.80 | `(0, 1]`; fraction of `sample_rate_hz` treated as usable per dwell |
| `scan_top_n` / `--scan-top` | int | 20 | `[1, 200]` |

**Cross-field rule.** Write `U = scan_usable_fraction * sample_rate_hz / 2` for
the usable half-span and `D = scan_dc_exclusion_hz`. §12.5 proves that the band
is fully covered only when a dwell step `S` exists with `2D <= S <= U - D`, and
such an `S` exists exactly when `U >= 3D`. The configuration rule is therefore

```
scan_usable_fraction * sample_rate_hz  >=  6 * scan_dc_exclusion_hz
```

with its own validation message naming both values and the implied minimum
`scan_usable_fraction`. The per-field bound `scan_dc_exclusion_hz <=
sample_rate_hz/4` alone admits infeasible configurations — at `D = 256000` with
`U = 409600` no step satisfies the coverage condition — which is why the cross-
field rule is separate and mandatory.

### 7.7 Resource limits and preflight

Before a run starts, the tool computes and displays:

```
angles x rounds                          = V planned visits
V * (duration_s + settle_s)              = minimum capture time
attempt_bytes = 1.2 KiB + max_retained_events * 0.2 KiB
projected final session.json  = A_max * attempt_bytes
projected cumulative rewrite  = A_max * (A_max + 1) / 2 * attempt_bytes
```

where `A_max` is the attempt ceiling below. The cumulative-rewrite line is
displayed because §11.1 rewrites the whole canonical document on every commit,
which is quadratic in the attempt count; see §11.1 for the corrected arithmetic
and for why the ceiling is where it is.

Rules:

- `rounds * angle_count` may not exceed **200** planned visits. At the default
  60 s duration that is already 3.3 hours of pure capture time.
- Total attempts, including superseded ones, may not exceed **250**
  (`attempt_ceiling`).
- **Capacity for one attempt per remaining visit is reserved and can never be
  spent on a retry.** A retry is offered only when

  ```
  attempts_committed + remaining_planned_visits < attempt_ceiling
  ```

  so the session can always reach its last visit. When that inequality fails,
  the retry option is not offered; the operator's choices are `Accept as-is` or
  `Skip`, and the message says the retry budget is exhausted and how many visits
  remain. Because planned visits are capped at 200 and the ceiling is 250, at
  least 50 retries are available in the worst case and 236 in the default
  14-visit experiment.

  Revision 3 said the tool would "offer to finalise" here. It cannot: the
  controller does not finalise anything (§6.6), and stopping a session early
  because a retry was refused would throw away visits that were still takeable.
  Reserving capacity means the situation resolves itself.
- If a session nevertheless cannot continue — including a `commit_visit` that
  returns `CommitOutcome::NotCommitted` (§11.1.1), a `CommitOutcome::Indeterminate`
  whose reconciliation also fails (§11.1), and a resumed record that already
  violates the ceiling — the app layer writes the terminal state
  **`aborted`** with whatever summary can be built and a stated reason. It is
  terminal like `completed`: resume refuses it (§11.5) and the rendered report
  is marked `ABORTED — PARTIAL`.
- Projected final `session.json` above **16 MiB** is a configuration error
  naming `max_retained_events` and the projection. This keeps the document well
  under the 64 MiB read limit of §13 even after every retry.
- Projected cumulative rewrite above **2 GiB** is a configuration error naming
  the same two values.
- Total planned capture time above `3600 s` requires an explicit confirmation
  naming the estimate. In `--non-interactive` mode it is an error instead.
- `session.json` is refused above 64 MiB on read (§13).
- A configuration file above 1 MiB is refused.
- Free space on the session filesystem is checked before start; below 64 MiB is
  an error naming the path. (With Q2 there is no raw-IQ storage, so this is a
  small check, not a large one.)

### 7.8 Gain and AGC handling

Gain is the controlled variable. On open, `rtlangle` calls
`rtlsdr_get_tuner_gains()`, snaps the requested value to the nearest supported
entry, calls `rtlsdr_set_tuner_gain_mode(dev, 1)` for manual tuner gain, and
calls `rtlsdr_set_agc_mode(dev, 0)` to disable the RTL2832U's digital AGC. Every
return code is checked and a failure aborts with the failing call named.

**AGC is unconditionally off and there is no way to turn it on** (decision Q7).
Both calls are made on every device open and both are verified. A gain that
changes during a session would make every comparison between angles meaningless,
so Phase 1 does not offer the option at all. `SourceInfo::agc_enabled` remains in
the record as *evidence* that it was off, not as a setting that could be true.

The **applied** gain, read back with `rtlsdr_get_tuner_gain()`, is what is
persisted. A difference from the requested value at session start is normal —
the device has a fixed gain table — and produces an informational message naming
both values. What is *not* normal is the applied gain changing between attempts
in one receiver segment; that is what §10.4 warning W7 detects.

**Gain never changes during a session.** If clipping is detected (§8.5), the
capture fails and the tool instructs the operator to restart the experiment at a
lower fixed gain. It does not silently reduce gain and continue, because the
captures before and after would not be comparable.

### 7.9 Which fields each command uses

A flag that a command does not use is a **usage error** naming the flag and the
command, exit code 2. Silently ignoring it would let an operator believe a
setting took effect when it did not — for example `rtlangle report --gain 400`,
which cannot change anything about an already-recorded session.

| Command | Required | Used | Refused |
|---|---|---|---|
| `run` | `center_hz` (or `resume_dir`, which supplies it) | §7.1–§7.5 | all `scan_*` |
| `scan` | — | §7.1 except `center_hz`; `open_db`; §7.6; `source_spec`, `synthetic_snr_db`, `synthetic_duty`, `no_color`, `non_interactive` | §7.2, the rest of §7.3, §7.4, `session_root`, `label`, `resume_dir` |
| `devices` | — | `device_index`, `no_color` | everything else |
| `report` | the session directory as a positional argument | `report_metric`, `min_captures_advisory`, `min_effect_db`, `yield_concordance_ratio`, `noise_drift_warn_db`, `no_color`, `non_interactive` | everything else |

`scan` uses `open_db` because a dwell hit is defined relative to the dwell's own
band noise floor by that same threshold (§12.5); it is one constant, not two.

`report` accepts the whole of §7.4 because those fields change only which
warnings the rendered text raises, never what was measured. Re-rendering an old
session with a stricter `--min-captures` is a legitimate way to ask "what would
this look like if I demanded more data?", and it cannot alter a stored number.

`scan` accepts `--source synthetic` and the two `synthetic_*` fields because
`SyntheticSource` implements `ITunableSampleSource` (§6.2); that is what makes
the §14.1 band-coverage tests runnable with no hardware.

`run --resume <dir>` does not require `--freq`, because §11.5 rejects any
attempt to change it and the stored value is authoritative.

---

## 8. Signal chain

```
tune hardware to (center_hz + applied_offset_hz)        default offset 250 kHz
  -> u8 IQ pairs -> complex<float>,  (x - 127.4f) / 127.5f
  -> clipping census on the raw bytes                   (§8.5)
  -> OffsetMixer   x exp(+j 2 pi f_off t)   at 1.024 MHz
  -> FirDecimator  /8   -> 128 kHz
  -> FirDecimator  /4   ->  32 kHz  (channel rate)
  -> ChannelFilter low-pass, passband 4 kHz, stopband 5 kHz
  -> transient trim, then framing: 1024 samples, hop 512  (32 ms / 16 ms)
       |
       +-> AmDemodulator -> audio band 300-3400 Hz, same channel rate
```

### 8.1 Offset tuning and the mixer sign

The RTL-SDR's DC offset spur sits exactly at the tuned centre frequency, which
is exactly where the AM carrier would land if the receiver were tuned directly
to the channel. Tuning `offset_tune_hz` away and mixing back digitally keeps the
spur out of both the signal measurement and the noise-floor estimate. Offset
tuning is on by default; `--offset-tune-hz 0` disables it and warns that
measurements will be contaminated by the DC spur.

The mixer multiplies by `exp(+j 2 pi f_off t)`. The sign is **positive**
because the hardware is tuned *above* the wanted signal (`center_hz + offset`),
so the signal appears at baseband frequency `-offset` and must be shifted up to
DC.

Phase is held in an exact integer accumulator modulo `sample_rate_hz`, feeding a
65536-entry sine/cosine lookup table. An integer accumulator cannot drift over a
60-second capture, whereas a floating-point recurrence can. If the requested
offset is not representable as an integer number of accumulator steps, the
applied integer offset is what is used and it is recorded as
`SourceInfo::applied_offset_hz`; the difference is reported when it exceeds 1 Hz.

`|offset| + channel_bw/2 < 0.45 * sample_rate` is a configuration constraint,
checked before the device is opened.

### 8.2 Filter conventions

Every FIR is designed by the Kaiser window method from three explicit
parameters — **passband edge, stopband edge, stopband attenuation** — never
from a single ambiguous "cutoff", which the superseded text used and which is
read as −3 dB, −6 dB, or the band edge depending on the reader.

| Filter | Passband edge | Stopband edge | Stopband attenuation |
|---|---|---|---|
| Decimation stage (rate `R`, factor `M`) | `0.40 * (R/M)` | `0.50 * (R/M)` | 60 dB |
| Channel filter | `channel_bw_hz / 2` (4000 Hz) | `channel_bw_hz / 2 + 1000` (5000 Hz) | 60 dB |
| Audio low-pass | 3400 Hz | 4000 Hz | 60 dB |

Every stopband edge in this table must lie strictly below the Nyquist frequency
of the rate the filter runs at; the §7.1 cross-field rules are what guarantee it
for the two configurable filters. All FIRs are designed with an **odd** tap
count, so each has an integer group delay `(T-1)/2` at its own input rate — the
property §8.4 relies on.

The audio high-pass at 300 Hz and the carrier DC-block at 25 Hz are single-pole
IIR sections; their coefficients are stated as `a = exp(-2*pi*fc/fs)` and their
group delay is not compensated (see §8.4).

Every stateful filter exposes `reset()` and produces bit-identical output for
the same input whether that input is delivered in one block or in arbitrary
smaller chunks. This is a normative property, tested directly, because the
capture loop delivers whatever the device buffer yields.

### 8.3 Framing

At the channel rate, with `M = 1024` and hop `H = 512`:

```
P[k] = (1/M) * sum_{n = kH}^{kH+M-1} |x[n]|^2
```

`P[k]` is in-channel power in normalised full-scale units squared, measured over
the ±4 kHz channel only — never over the full 1.024 MHz — so the number reflects
the antenna rather than the receiver's filter width. A frame is 32 ms and the
hop is 16 ms; the 50 % overlap means adjacent frames are correlated, which
matters for §10 and is why frames are not treated as independent samples.

### 8.4 Latency and event-to-audio alignment

A linear-phase FIR of `T` taps (odd, §8.2) delays its input by exactly `(T-1)/2`
samples **at its own input rate**. Referring every stage's delay to the channel
rate gives the chain latency:

```
stages s = 1..S with decimation factors M_1..M_S, channel filter M = 1
d_s              = (T_s - 1) / 2                      # integer, at stage s input rate
chain_latency_ch = sum over s of  d_s / prod_{j >= s} M_j     # generally FRACTIONAL
```

**The sum is rational, not integral, and the implementation must not pretend
otherwise.** Revision 2's `FirDecimator::group_delay_output_samples()` returned
`(taps-1)/(2*factor)` in integer arithmetic, which silently truncates: with
`T = 129` taps and `M = 8` the true delay is 8 channel samples exactly, but with
`T = 131` it is 8.125 and the integer expression reports 8. Accumulated over two
decimation stages plus the channel filter the error is up to one channel sample,
which is harmless for the audio guard but is exactly the kind of quiet rounding
that makes an impulse-response test disagree with a formula and get "fixed" by
widening a tolerance.

`Chain` therefore keeps the latency as an exact rational and exposes three
things:

```cpp
struct Latency {
  std::int64_t  num, den;        // exact chain latency in channel samples = num/den
  std::size_t   ceil_samples;    // ceil(num/den) - used for the transient trim
  double        residual;        // ceil_samples - num/den, in [0,1)
};
Latency Chain::latency() const;
std::size_t Chain::latency_samples() const { return latency().ceil_samples; }
```

- The **transient trim** (§6.3 rule 4) uses `ceil_samples`, so it can only ever
  over-trim, never leave transient behind.
- The **audio alignment** below uses `ceil_samples` as well; the residual is at
  most one channel sample (31 µs at 32 kHz) against a guard interval of 50 ms,
  so it is absorbed by the guard by three orders of magnitude.
- The impulse-response test asserts the measured peak index equals
  `floor(num/den)` or `ceil(num/den)`, not a single value it cannot always hit.

The **audio path adds its own delay** after the channel stream: the audio
low-pass contributes `(T_audio - 1) / 2` channel samples, which is an exact
integer because the audio filter does not decimate. `CaptureOutcome` carries this
as `audio_latency_samples`. Events are detected on the channel frame powers;
mapping an event window to the audio buffer therefore requires shifting by
`audio_latency_samples`. The superseded text assumed equal-length arrays were
time-aligned, which they are not.

The single-pole IIR sections are not group-delay compensated. Instead, a guard
interval absorbs both the residual IIR delay and the FIR transition:

```
audio_start = event_start + audio_latency_samples + guard_samples
audio_end   = event_end   + audio_latency_samples - guard_samples
```

with `guard_samples = audio_guard_ms * channel_rate / 1000` (default 50 ms).
If the guarded window is shorter than `min_audio_window_ms` (default 100 ms),
the event yields no audio SNR and is marked `audio_snr_valid = false`; it
remains valid for the channel metric. A test asserts that the residual
misalignment from the uncompensated IIR sections is smaller than the guard.


### 8.5 Clipping and front-end health

Clipping is measured on the **raw bytes**, before normalisation: an I or Q byte
equal to 0 or 255 is at an ADC rail. `clipped_fraction` is the count of such
bytes divided by the total byte count since the last `flush()`.

`clipped_fraction > max_clipped_fraction` (default 1e-4) fails the capture with
status `clipped` and a message instructing the operator to restart the
experiment at a lower fixed gain (see §7.8 — gain is never changed mid-session).
The measured fraction is persisted on every attempt, including passing ones, so
a marginal front end is visible in the record.

---

## 9. Measurement

### 9.1 Noise-floor reliability — the two-stage probe

The superseded rule was circular: it used the 20th-percentile floor itself to
decide whether enough quiet frames existed to make that percentile meaningful.
When 90 % of a capture is occupied, the 20th percentile lands *inside* a
transmission, the floor looks normal, and the computed active fraction comes out
near zero — so a capture that was almost entirely signal would be accepted as
quiet noise and would produce confident-looking SNR. The plan's own "all-active"
test could never have passed.

The replacement estimates the floor twice, at two different percentiles, and
uses the lower one only to judge occupancy:

```
K frames, powers P[0..K-1]
probe_percentile = min(5, noise_percentile)                       # default 5
N_probe          = percentile(P, probe_percentile)
active_probe     = count( P[k] > N_probe * from_db(open_db) ) / K
dynamic_range_db = to_db( percentile(P, 95) / N_probe )
```

Decision rules, applied in order:

- **R1.** `active_probe > max_active_fraction` (default 0.70) →
  `noise_floor_unreliable`. No noise floor, no SNR, no events reported as
  valid. The operator is offered a retry.
- **R2.** Otherwise, if `dynamic_range_db < open_db` **and** a persistent
  carrier is present (below) → `noise_floor_unidentifiable`. This is the case
  where the channel is occupied by something that never keys down, so there is
  no quiet reference anywhere in the capture and a flat power series cannot be
  distinguished from noise by percentiles alone.
- **R3.** Otherwise the capture has a usable floor:
  `N = percentile(P, noise_percentile)`.

**Persistent-carrier detection.** For each analysis window (a Welch-averaged
periodogram over ≥ 16 segments of the channel stream), compute the ratio of the
strongest bin within ±`channel_bw/2` to the median bin power of the same
window. A carrier is *present* in that window when the ratio exceeds
`carrier_prominence_db` (default 10.0). It is *persistent* when it is present in
at least `carrier_persistence` (default 0.90) of the windows.

The 10 dB default is a starting value, not a verified one. §14.2 requires a
Monte Carlo calibration that must demonstrate a false-positive rate below 1 % on
pure noise and a detection rate above 99 % on a constant carrier at 6 dB SNR; if
the default fails, the implementer adjusts the constant and records the measured
rates in the test file. Welch averaging is required precisely because the
peak-to-median ratio of a single unaveraged periodogram over 1024 bins is
already about 10 dB for pure noise.

**Known limit, to be documented rather than hidden.** When occupancy exceeds
roughly `100 - probe_percentile` percent, the probe floor is itself
contaminated and R1 can miss. R2 is the second line of defence for the common
form of that case (a constant carrier). A capture that is fully occupied by
*varying* traffic with no quiet frame at all remains a failure mode of this
estimator; it is listed in §15.

### 9.2 Event detection

Hysteresis squelch on `P[k]`, thresholds relative to `N`:

- open when `P[k] > N * from_db(open_db)` — default `open_db = 6.0`
- close when `P[k] < N * from_db(close_db)` — default `close_db = 3.0`
- candidates separated by less than `merge_gap_ms` (default 200) are **merged
  first**
- candidates shorter than `min_event_ms` (default 300) are **discarded after**
  merging

Order matters: merging first prevents a single transmission with a brief pause
from being discarded as two short fragments.

**Boundary handling.** An event that is already open at the first frame, or
still open at the last frame, is `truncated`. Its true extent is unknown and its
median is taken over a partial transmission, so it is recorded in the audit
trail with `valid = false` and excluded from all metrics.

Each surviving event records start and end frame index, start and duration in
seconds, wall-clock UTC, and the measured carrier frequency offset (the
parabolically interpolated FFT peak within ±2 kHz of DC).

**What the carrier offset is for.** It is a *comparability diagnostic* only: a
large change in carrier offset between two angles indicates that different
transmitters were heard, which is the main threat to the result. It does
**not** identify a transmitter and does not establish that two events came from
the same aircraft; several aircraft share a channel and offsets drift with
temperature and Doppler. Any claim of identification is forbidden (§15).

### 9.3 Channel SNR — the exact calculation

For each valid, non-truncated event:

```
P_event  = median over the event's frames of P[k]
SNR_lin  = (P_event - N) / N
SNR_dB   = 10 * log10(SNR_lin)
```

The subtraction happens **before** the division, in the linear power domain, and
only then is the result converted to decibels.

This is the single most important line in the tool. `P_event / N` is `SNR + 1`:
at 0 dB true SNR it reports 3.01 dB, at −3 dB it reports 1.76 dB, and it is
exactly the "total channel power relative to noise" quantity that must never be
called SNR. Events with `P_event <= N` are discarded as invalid rather than
producing a NaN or a negative-infinity decibel value. Test D in §14.2 exists
solely to fail if this subtraction is ever removed.

Because the channel filter passes the AM carrier together with its sidebands,
this quantity is a carrier-plus-sideband-to-noise ratio in the channel
bandwidth — a CNR. It is named `channel_snr_db` everywhere, and the README
states plainly that it is a CNR and not a post-demodulation audio SNR.

**Minimum detectable SNR.** An event is only detected once it exceeds the
squelch, so the weakest SNR this tool can report is
`10*log10(from_db(open_db) - 1)` — about **4.74 dB** at the default 6 dB
threshold. This value is computed by `minimum_detectable_snr_db(open_db)`,
printed in the session header, and stated in the README. It is a property of the
measurement, not a bug, but it means an angle can look "insufficient" simply
because everything it heard was weak.

**The squelch censors the sample, and that is the origin of §10.0.** Only
transmissions above this floor enter the event list at all, so `channel_snr_db`
is measured over a *selected* subset whose selection threshold moves with the
antenna. §10.0 states the resulting estimand; §9.5 defines the companion
measurement that makes the selection visible.

### 9.4 Audio SNR and audio eligibility

The envelope-demodulated, DC-blocked, 300–3400 Hz band-passed stream is framed
identically to the channel stream. With `A[k]` the per-frame audio power, over
the latency-shifted, guarded window of §8.4:

```
N_a      = percentile(A[all frames], noise_percentile)
S_a      = median over the guarded event window of A[k]
SNRa_lin = (S_a - N_a) / N_a
SNRa_dB  = 10 * log10(SNRa_lin)
```

Same subtraction rule as §9.3, and the same discard rule: `S_a <= N_a` yields no
audio SNR.

#### 9.4.1 Two event counts, because the two metrics do not qualify together

An event can be perfectly good for the channel metric and unusable for the audio
one. Three independent things cause that, and revision 3 left the consequence
undefined:

- the guarded audio window is shorter than `min_audio_window_ms` (§8.4);
- `S_a <= N_a`, so the audio SNR would be negative or infinite;
- the *audio* noise reference fails the §9.1 reliability check while the
  *channel* floor passes.

Every event therefore carries `audio_snr_valid`, and every capture carries two
counts:

```
valid_event_count       = valid, non-truncated events                (channel-eligible)
valid_audio_event_count = those of them with audio_snr_valid == true (audio-eligible)
```

**Both scores use the same `min_valid_events` constant against their own
count.** There is one threshold, applied twice:

| Score | Computed when | Otherwise |
|---|---|---|
| `capture_score_channel_db` | `valid_event_count >= min_valid_events` | absent (JSON `null`) |
| `capture_score_audio_db` | `valid_audio_event_count >= min_valid_events` | absent (JSON `null`), and the capture is flagged `audio_insufficient` |

A capture with a channel score and no audio score is **still `ok`**. Attempt
status describes the channel path and the channel floor (§11.3); audio
eligibility is a per-metric property recorded in a flag, not a status. Such a
capture contributes to the channel ranking and is simply absent from the audio
one.

#### 9.4.2 The audio noise floor's own reliability

`N_a` obeys the same two-stage check as the channel floor (§9.1), run over the
audio frame powers. There are three outcomes, and each has exactly one effect:

| Channel floor | Audio floor | Attempt status | Channel score | Audio score |
|---|---|---|---|---|
| unreliable / unidentifiable | any | `noise_floor_unreliable` / `noise_floor_unidentifiable` | absent | absent |
| reliable | reliable | `ok` (subject to event counts) | per the table above | per the table above |
| reliable | unreliable / unidentifiable | **`ok`** | per the table above | **absent**, flag `audio_insufficient`, `status_detail` names the audio floor |

The third row is the one revision 3 omitted. Without it the same capture could
be argued into two different statuses: `ok` because the channel measurement is
sound, or `noise_floor_unreliable` because *a* floor failed. It is `ok`, and the
missing audio score is recorded where a missing audio score belongs.


### 9.5 Detection yield — the co-primary measurement

An SNR conditioned on detection cannot, on its own, say whether an angle is
better. The complementary quantity is **how much** the angle heard, which the
squelch does not censor in the same direction: a better angle crosses the
detection threshold on transmissions a worse angle never registers, so it should
show a *higher* yield even when its median SNR is lower.

Two yield figures are computed for every `ok` capture, from the same event list
and frame series the SNR uses:

```
valid_events        = number of valid, non-truncated events
events_per_minute   = valid_events / (duration_s / 60)
detected_fraction   = (frames inside a valid event) / (total frames)
```

`events_per_minute` is the headline yield. `detected_fraction` is reported
beside it because the two answer different questions — many short transmissions
versus few long ones — and an angle that moves only one of them is telling you
something specific.

Yield is aggregated exactly like the SNR score and by the same rule (§10.1): the
capture is the unit, and an angle's yield is the median of its accepted `ok`
captures' `events_per_minute`.

**What yield is and is not.** Yield is proportional to how much traffic occurred
during that capture, which is outside the operator's control and varies minute
to minute. It is therefore *not* a standalone ranking metric — a quiet minute at
a good angle yields less than a busy minute at a bad one. It is a **concordance
check**: across enough rounds, if angle A really receives better than angle B, A
should not be hearing systematically fewer transmissions than B. §10.4 warning
W6 uses it in exactly that one-directional way and in no other.

**W6 is a point comparison, not a test.** It compares two median yields against
a fixed ratio with no allowance for their uncertainty. Airband traffic is bursty
and a median over four captures is noisy, so W6 will sometimes fire because the
traffic happened to be uneven rather than because the selection effect is
present. Since it raises a warning rather than blocking anything (decision Q1), a
false positive costs the reader a sentence of context, not a conclusion — which
is why a point comparison is proportionate here and would not have been as a
gate.

## 10. Aggregation and the decision policy

### 10.0 The estimand — what is actually being estimated

Every number this tool reports is an estimate of something, and revision 2 never
wrote down what. Doing so is what makes the rest of §10 checkable.

**The estimand for `channel_snr_db` at angle θ is:**

> the median, over the population of transmissions that occurred during this
> session's captures at angle θ **and were strong enough at angle θ to cross
> the squelch**, of the in-channel carrier-plus-sideband power relative to that
> capture's noise floor.

Three features of that sentence are load-bearing:

1. **It is conditional on detection.** The conditioning set depends on θ,
   because the squelch is a fixed threshold relative to the noise floor and a
   better angle lifts more transmissions across it. This is a *selection effect
   on the sample*, not a bias in the estimator, and no amount of averaging
   removes it.
2. **The population is session-local.** It is the traffic that happened during
   those particular minutes, not "airband traffic" in general. Captures at two
   angles never observe the same transmission, so the two populations are
   different and the comparison is unmatched by construction.
3. **It is a CNR, not an audio SNR** (§9.3), and it is relative to this
   receiver's own noise floor at this gain (§15.1).

**The consequence, stated plainly.** A higher `angle_score` does **not** by
itself mean an angle receives better. It can equally mean the angle heard only
the loud transmissions. The two explanations are distinguished by detection
yield (§9.5): "receives better" predicts a higher score **and** at least
comparable yield; "heard only the loud ones" predicts a higher score **and
lower** yield. That is the entire content of §10.4 warning W6, and it is why
§10.2 never prints a score without its yield beside it.

**What would remove the problem, and why Phase 1 does not do it.** A repeatable
reference signal — a continuously transmitting station such as an ATIS, a
VOLMET, a VOR, or a broadcast FM carrier — has no on/off behaviour, so there is
nothing to detect and nothing to condition on: the measurement becomes a
straight carrier-to-noise ratio against an out-of-band noise reference, matched
across angles. That is the scientifically clean version of this experiment. It
is a different measurement path from §9.1's two-stage floor and §9.2's event
detector, so it is Phase 2 scope (§15.3), and Phase 1 says so rather than
approximating it.


### 10.1 The experimental unit is the capture, not the event

Events inside one capture share a receiver configuration, a minute of
propagation, an antenna position, and often a single transmitter. Two frames
inside one event are 50 % overlapped by construction. Treating events as
independent replicates — as revision 1 did — inflates `n` and makes a spread
look tighter than the experiment earned.

Aggregation is two-level:

**Level 1 — within a capture.** The event distribution is a *diagnostic*: count,
median, p10/p25/p75/p90 are all reported. The capture's scores are

```
capture_score_channel = median over that capture's valid events of channel SNR_dB
capture_score_audio   = median over that capture's audio-eligible events of audio SNR_dB
capture_yield         = valid_event_count / (duration_s / 60)               # 9.5
```

each subject to its own eligibility rule in §9.4.1. Either score may be absent
without the other being absent.

**Level 2 — across rounds.** For each planned angle, the accepted `ok` captures
at that angle each contribute exactly one vote **to each ranking they are
eligible for**:

```
angle_score_channel = median over accepted ok captures having a channel score
angle_score_audio   = median over accepted ok captures having an audio score
angle_yield         = median over accepted ok captures of capture_yield

n_captures        = accepted ok captures with a channel score
n_captures_audio  = accepted ok captures with an audio score      # <= n_captures
```

`n_captures_audio` is reported separately and is never assumed equal to
`n_captures`. An angle with no audio-eligible capture is absent from the audio
ranking and present in the channel one; the audio table shows `-` for it, never
`0`.

With `n = 2` the median equals the mean; the report says so where the number is
printed, so it is not over-read.

### 10.2 Descriptive rankings — the whole output

Two rankings are always computed and always printed. Each lists, per angle:

- planned angle, and the circular mean and circular spread of entered actual
  angles;
- the score for that metric, and the spread of the contributing capture scores
  (min–max for `n < 4`, interquartile range for `n >= 4`);
- **`angle_yield` in events per minute, and the median `detected_fraction`** —
  never omitted, because §10.0 makes a score without a yield uninterpretable;
- `n_captures` for that metric (`n_captures_audio` in the audio table),
  `n_captures_excluded` with reasons, and total valid events;
- pooled noise floor in dBFS and its spread across the angle's captures;
- every warning identifier from §10.4 that applies to that angle.

Angles with no eligible capture appear in the table with their status and are
excluded from ordering.

**There is no third output.** The rankings and the §10.4 warnings are what the
tool produces. It does not compute, store, or print a best angle.

### 10.3 Which ranking is listed first

`--report-metric channel|audio` (default `channel`) chooses which ranking the
report lists first. Both are always printed in full. In an interactive session
the operator may change it at report time; `summary.report_metric_source`
records `default`, `flag`, or `interactive`.

Because nothing downstream consumes the choice, it cannot influence any number
in either table. That is the entire safety property, and it is now structural
rather than argued: revision 3 needed a pre-registered `primary_metric` to stop
a report-time choice from selecting which statistical test ran, and with no test
there is nothing to select (decision Q8).

When the two rankings disagree on the top angle, warning **W4** fires and the
report says so before either table.

### 10.4 Data-quality warnings — labelled, not a decision ladder

These are **warnings about the measurement**, in the sense that a compiler
warning is about the code: each names something that makes a comparison less
trustworthy, and none of them combine into a verdict. They are printed as a
labelled list with the rankings, every one of them evaluated every time —
there is no short-circuit, because a list of problems that stops at the first
problem is not a list of problems.

| ID | Fires when | What it means for the reader |
|---|---|---|
| **W1** | any ranked angle has `n_captures < min_captures_advisory` (default 4) | That angle's median rests on very few captures. Its position in the ranking may move with one more round. |
| **W2** | the set of rounds in which two ranked angles both have an accepted `ok` capture is smaller than the number of rounds run | Those two angles were not measured under comparable traffic in every round, so part of their difference is when they were measured, not where the antenna pointed. |
| **W3** | the top two angles' scores differ by less than `min_effect_db` (default 1.0 dB) | The gap is smaller than the threshold the operator set for "worth acting on". |
| **W4** | the channel ranking and the audio ranking disagree on the top angle | The two measurement paths are not telling the same story; §9.3 explains what each one is. |
| **W5** | pooled noise floors across ranked angles span more than `noise_drift_warn_db` (default 3.0 dB) | The noise environment changed during the session, so angles measured at different times faced different conditions. |
| **W6** | the top-scoring angle's `angle_yield` is below `yield_concordance_ratio` (default 0.90) of the best yield among ranked angles | The detection-selection effect of §10.0 is visibly present: the top-scoring angle heard *fewer* transmissions than another angle, which is what "heard only the loud ones" looks like. |
| **W7** | any attempt's applied gain, sample rate, or centre frequency differs from its receiver segment's baseline, or `agc_enabled` is true anywhere | The receiver was not held constant, so the comparison is between two receivers as much as between two angles. |
| **W8** | any ranked angle carries a `clipped`, `host_dropped_samples > 0`, `noise_floor_unreliable`, `noise_floor_unidentifiable`, `audio_insufficient`, or angle-deviation flag | Named data-quality faults are present in the ranked data. The message lists the angle and the flag. |

**Where these came from.** Revision 3 had eight gates `G1`–`G8` that combined
into a resolved winner. Decision Q1 removed the winner, so the gates that only
made sense as inference are gone and the rest became warnings. The prefix
changed from `G` to `W` because "gate" meant something passed or failed.

| Revision 3 gate | Revision 4 |
|---|---|
| G1 capture count | **W1** |
| G2 pairing | **W2**, restated as a round-coverage observation |
| G3 separation, paired confidence bound | **removed — no successor.** Inferential only. |
| G4 distribution-free confirmation, sign-flip test | **removed — no successor.** Inferential only. |
| G5 receiver stability | **W7** |
| G6 data quality flags | **W8** |
| G7 environmental stability | **W5** |
| G8 yield concordance | **W6** |

`W3` and `W4` were §10.5 comparability warnings in revision 3 and are promoted
here so that the whole list is in one place.

**How many rounds to run.** More rounds are still better, and the report says
so, but the reason has changed. It is no longer "enough to resolve a winner" —
nothing resolves anything now. It is that a median over two captures shows you
almost nothing about how much that angle's measurement varies, and the
min–max or interquartile spread printed beside each score is the number that
tells you whether the ranking means anything. Four or more rounds make that
spread informative; two make it a single interval between two points. That is
the whole justification, and `min_captures_advisory` is where it is encoded.

### 10.5 Additional comparability notes

Printed with the rankings, alongside the §10.4 warnings. These are observations
rather than data-quality faults:

- any capture is `insufficient_data`, `noise_floor_unreliable`,
  `noise_floor_unidentifiable`, `clipped`, or failed, with counts;
- `n_captures_audio < n_captures` at any angle, naming the angle and both
  counts (§9.4.1);
- valid event counts across angles differ by more than a factor of 3;
- more than one receiver segment exists, because the session was resumed and the
  device was reopened between segments;
- a commit was recorded as `CommittedNotDurable` at any point (§11.1);
- `--rounds 1` was used, so angle is confounded with time;
- any accepted attempt's `circular_distance_deg(actual, planned)` exceeds
  `angle_deviation_warn_deg`;
- the ranked set contains a pair `(θ, θ+180)` whose scores differ by more than
  `noise_drift_warn_db`, which indicates that the environment rather than the
  dipole pattern dominates (§4.2);
- the distribution of event carrier offsets differs markedly between the top two
  angles, indicating different transmitters were heard;
- `events_total != events_retained` on any attempt, so the persisted event list
  was capped (§11.3).

### 10.6 Result wording

The report opens with the rankings and the warnings. Its closing headline, in
the terminal, in `report.txt`, and in the README, reads exactly this and only
this, in every session without exception:

> Exploratory ranking only. These are the reception qualities measured at each
> angle under the traffic that happened to occur. This tool does not determine
> which angle is best, and it is NOT a measurement of the transmitter's physical
> direction.

There is no second headline and no condition under which a different one is
printed. Revision 3 had two — one of which began "Highest measured reception
quality under this experiment" — selected by whether a gate ladder passed.
Decision Q1 removed the ladder, and with it the wording it existed to justify.

The string may not be reworded, abbreviated, split, or coloured away, and it may
not appear before the ranking tables.


## 11. Persistence

```
sessions/20260819-143000-airband/     mode 0700
  session.json      canonical record, atomically replaced       mode 0600
  measurements.csv  derived export, atomically regenerated      mode 0600
  report.txt        written at finalisation and on pause        mode 0600
```

There is no `raw/` directory (decision Q2).

### 11.1 The commit point, the write protocol, and what it costs

`session.json` is the single source of truth. `measurements.csv` and
`report.txt` are **derived** artifacts that can be regenerated from it at any
time. Revision 1 committed JSON and then appended to a separate CSV, so a crash
between the two left two records that disagreed and no rule for which won.

#### 11.1.1 `rename()` is the commit point

Everything before the `renameat()` is preparation and changes nothing anyone can
observe. Everything after it is bookkeeping on a commit that has already
happened. Revision 3 said "a throw means nothing was committed", which is false
in two directions: a failed directory `fsync` leaves a commit that *is* visible
to every subsequent read, and a failed CSV regeneration leaves a commit that is
both visible and durable. `commit_visit` therefore **returns** a `CommitResult`
(§6.6) rather than throwing, because there are four outcomes and an exception
can carry only one.

Write protocol, in order, for every commit:

| # | Step | If it fails |
|---|---|---|
| 1 | Serialise the whole record to `session.json.tmp.<pid>` via `openat(dirfd, ..., O_CREAT\|O_EXCL\|O_WRONLY\|O_NOFOLLOW, 0600)` | `NotCommitted` |
| 2 | `fsync` the temporary file | `NotCommitted` |
| 3 | **`renameat(dirfd, "session.json.tmp.<pid>", dirfd, "session.json")`** — the commit point, atomic within a directory on Linux | `NotCommitted` on a classifiable error; `Indeterminate` otherwise |
| 4 | `fsync(dirfd)`, which makes the rename itself durable | **`CommittedNotDurable`** |
| 5 | Regenerate `measurements.csv` by steps 1–4 from the just-committed record | **`Committed`**, with a repair note; the CSV is rebuilt on next open |

So:

- **`NotCommitted`** — the canonical record is byte-for-byte what it was.
  Nothing was written, so no measurement is lost. What the caller does next is
  fixed by the ladder below; it is not a matter of local policy.
- **`CommittedNotDurable`** — the commit is in the record and every later read
  sees it. It may not survive a power cut. The caller warns, records the event
  in the session so §10.5 can report it, and **continues**. Treating this as a
  failure and retrying would duplicate the attempt.
- **`Committed`** — normal.
- **`Indeterminate`** — `renameat` returned something that does not distinguish
  "did not happen" from "happened". The caller MUST run the reconciliation
  below before anything else.

**`EINTR` is retried inside the syscall wrappers, and only `EINTR`.** Steps 1,
2, and 4 (`openat`, `write`, `fsync`) can return `EINTR`; the wrappers restart
them internally, so a signal never becomes a `CommitOutcome`. `renameat` is not
interruptible on Linux, so the commit point itself is never reached in a retry
loop and no retry can create commit ambiguity. Every other errno classifies to
one of the four outcomes above. In particular `EIO` from `fsync` is **not**
retried: a failed `fsync` may already have discarded the error state that a
second call would then report as success.

**What a final `NotCommitted` obliges the caller to do.** Normative, and total
by enumeration over all five methods of §6.6 that return a `CommitResult` —
`commit_visit`, `pause`, `finalize`, `abort`, and `reload_from_disk` — rather
than by a sweeping clause:

1. `commit_visit` returns `NotCommitted` → `ExperimentController` returns
   `Result::Failed` at that visit. It does **not** advance to the next visit and
   does **not** re-issue the commit. Nothing was written, so there is no half
   state to repair, and re-issuing could only duplicate the attempt in the case
   where the outcome was misclassified.
2. The app layer then attempts `abort(partial, reason)`, writing the terminal
   state `aborted` with whatever summary can be built (§7.7). Exit code 1 with
   `abort_reason` set — the first exit-1 row of §12.1.
3. `pause` or `finalize` returning `NotCommitted` does **not** attempt an
   abort. Everything those two were about to summarise is already committed, and
   the record on disk still says `running`, which §11.5 reopens. Writing
   `aborted` here would make terminal a session that a later invocation could
   still pause or finalise, and §11.5 would then refuse it forever. The process
   exits 1 leaving the record at its last committed value — the second exit-1
   row of §12.1 — and the operator is told the directory can be reopened with
   `--resume`.
4. `abort` itself returning `NotCommitted` is the terminus. The process exits 1
   and leaves the canonical record in its **last committed state**: unchanged,
   `finished_utc` not stamped, no `abort_reason`, and every measurement
   committed before the fault still present. This is the second exit-1 row of
   §12.1. There is nothing further to attempt — a store that cannot write cannot
   record that it could not write.
5. `reload_from_disk` returning `NotCommitted`, or failing in any other way, is
   the case §11.1.2 step 5 already governs: the record cannot be trusted to
   continue, so the app layer writes `aborted` with the reason. It is listed
   here so that the enumeration covers every method rather than every method
   that happens to write.

An `Indeterminate` outcome whose reconciliation finds the attempt **absent**
enters this ladder at step 1; it is a `NotCommitted` in every respect that
matters to the caller.

#### 11.1.2 Reconciliation after `Indeterminate`

Normative, and the only defined recovery:

1. Call `reload_from_disk()`, which re-reads `session.json` and replaces the
   in-memory record with what is actually there.
2. Check invariants I1 and I2 (§11.4) on the reloaded record.
3. If the attempt from the failed `VisitCommit` is present, the commit
   happened: continue as if `Committed`, and note the reconciliation.
4. If it is absent, the commit did not happen: enter the `NotCommitted` ladder
   of §11.1.1 at step 1 — the controller returns `Result::Failed` without
   advancing the visit and without re-issuing the commit.
5. If step 1 fails, or the reloaded record violates I1 or I2, the session cannot
   be continued safely. The app layer writes the terminal state **`aborted`**
   (§6.6, §7.7) with the reason, and the operator is told the directory still
   holds every measurement committed before the fault.

#### 11.1.3 The derived artifacts are recoverable

Neither derived file is ever the authority:

- **`measurements.csv`** is regenerated whenever its attempt count disagrees
  with the JSON, or whenever it is missing, unreadable, or malformed. Corrupting
  it cannot lose data.
- **`report.txt`** is a pure render of `(record, summary)`, and `summary` is a
  pure function of `record` (§10). `rtlangle report <dir>` reproduces it from
  `session.json` alone. A missing or truncated `report.txt` is a repair on
  reopen, not an error, and it is never read back by anything.

#### 11.1.4 The cost of rewriting the whole document

Revision 2 claimed this was "a few tens of megabytes" at its 2000-attempt
ceiling. It is not. Rewriting an `A`-attempt document once per commit costs

```
cumulative bytes = attempt_bytes * (1 + 2 + ... + A) = attempt_bytes * A(A+1)/2
```

At 4 KiB per attempt and `A = 2000` that is **7.63 GiB**, three hundred times
the stated figure, and the final document alone is 8 MiB. The claim was wrong by
enough to matter, so the ceilings in §7.7 were chosen to make the real number
small instead:

| Quantity | Value at defaults | Formula |
|---|---|---|
| `attempt_bytes` | ≈ 7.6 KiB | `1.2 KiB + max_retained_events * 0.2 KiB`, with `max_retained_events = 32` |
| attempt ceiling `A_max` | 250 | §7.7 |
| final `session.json` | ≈ 1.9 MiB | `A_max * attempt_bytes` |
| cumulative rewrite | ≈ **232 MiB** | `attempt_bytes * A_max(A_max+1)/2` |

232 MiB written across a session whose captures total at least 3.3 hours is
about 20 KiB/s of average write traffic, which no filesystem notices. §7.7's
preflight recomputes both figures from the *configured* `max_retained_events`
and refuses the run above 16 MiB final or 2 GiB cumulative, so raising the cap
cannot silently reintroduce the problem.

**Why not a journal.** An append-only journal plus periodic compaction would
make the cost linear. It would also add a second recovery path, a second
durability contract, and a second fault-injection matrix. With the ceilings
above, the simple design is already cheap enough. A journal is named in §15.3 as
the Phase 2 change if the ceiling ever needs raising.


### 11.2 Schema

```jsonc
{
  "schema_version": 1,
  "tool_version": "rtlangle 0.1.0",
  "session_id": "20260819-143000-airband",
  "state": "running",                // running | paused | completed | aborted
  "abort_reason": null,              // set only when state == "aborted"
  "durability_warnings": [],         // one entry per CommittedNotDurable commit (§11.1)
  "started_utc": "2026-08-19T14:30:00Z",
  "finished_utc": null,              // JSON null until state is "completed" or "aborted"
  "config": { /* every field of §7, fully resolved */ },
  "angle_provider": { "name": "manual", "automated": false },

  // One entry per device open: session start, and each resume. Every attempt
  // names the segment it was taken in, and §10.4 W7 compares an attempt's
  // applied settings against its own segment's baseline.
  "receiver_segments": [
    {
      "segment_id": "seg1",
      "opened_utc": "2026-08-19T14:30:02Z",
      "baseline": {
        "driver": "librtlsdr 0.6.0-128-g240b",
        "device_name": "Generic RTL2832U OEM",
        "serial": "00000001",
        "requested_sample_rate_hz": 1024000, "applied_sample_rate_hz": 1024000,
        "requested_center_hz": 118600000,   "applied_center_hz": 118600000,
        "requested_gain_tenth_db": 500,     "applied_gain_tenth_db": 496,
        "agc_enabled": false, "ppm": 0, "applied_offset_hz": 250000
      }
    }
  ],

  "plan": {
    "angles_deg": [0,15,30,45,60,75,90],
    "rounds": 2, "order": "alternating", "seed": 1234567,
    "visits": [
      { "visit_id": "r1-i000", "round": 1, "angle_index": 0, "planned_deg": 0.0 },
      { "visit_id": "r1-i003", "round": 1, "angle_index": 3, "planned_deg": 45.0 }
      /* ... */
    ]
  },
  "pending_retry": null,             // or { "visit_id": "...", "next_attempt": 2 }
  "attempts": [ /* see §11.3 */ ],
  "summary": null,                   // written by pause(), finalize(), or abort()
  "summary_partial": false           // true when written by pause() or abort()
}
```

**Visit identifiers are index-based, not angle-formatted.** Revision 2 used
`"r1-a45.00"`, which rounds the angle to two decimals while §4.3 only
de-duplicates angles that differ by more than `1e-6`. `--angles
"45.001,45.002"` therefore produced two distinct planned angles with one
identifier, and the second visit would have overwritten the first on resume. The
identifier is now `r<round>-i<angle_index padded to 3 digits>` — `r1-i003` —
where `angle_index` is the position in the session's sorted, de-duplicated angle
list. It is unique by construction, stable across a resume, and independent of
formatting. The full-precision `planned_deg` is persisted separately in both the
plan entry and every attempt, and is what the aggregator groups by.

**`state` is the session's lifecycle.** Revision 2 had `Quit` call `finalize`,
and then had an end-to-end test resume the session it had just finalised, with
no rule for reopening a finalised record. §11.3.1 and §11.5 now define three
states and the one transition each event causes.

### 11.3 Attempts

Every capture attempt is persisted, including failures. Nothing is deleted or
overwritten; superseded attempts stay in the record for audit.

```jsonc
{
  "visit_id": "r1-i003",
  "round": 1,
  "angle_index": 3,
  "planned_deg": 45.0,
  "segment_id": "seg1",
  "attempt": 2,
  "disposition": "accepted",     // accepted | superseded | abandoned
  "status": "ok",                // see the status table below
  "status_detail": null,
  "actual_deg": 44.0,
  "angle_deviation_deg": 1.0,    // circular
  "started_utc": "2026-08-19T14:36:07Z",
  "duration_s": 60.0,
  "wall_duration_s": 60.4,

  // Applied receiver settings at this capture's start, copied from
  // CaptureOutcome::applied. W7 compares these against the segment baseline.
  "applied_sample_rate_hz": 1024000,
  "applied_center_hz": 118600000,
  "applied_gain_tenth_db": 496,
  "agc_enabled": false,

  "frames_total": 3745, "frames_active": 412, "active_probe_fraction": 0.118,
  "dynamic_range_db": 21.4,
  "noise_floor_dbfs": -62.41, "audio_noise_floor_dbfs": -58.9,
  "host_dropped_samples": 0, "clipped_fraction": 0.0,

  "valid_event_count": 7,            // channel-eligible events        (§9.4.1)
  "valid_audio_event_count": 6,      // of those, audio-eligible       (§9.4.1)
  "truncated_event_count": 1,
  "audio_insufficient": false,       // true when the audio score is absent (§9.4.1)
  "events_total": 7,             // valid events found
  "events_retained": 7,          // event objects kept below, capped by max_retained_events
  "events_per_minute": 7.0,      // §9.5 yield
  "detected_fraction": 0.11,     // §9.5

  "capture_score_channel_db": 18.2,
  "capture_score_audio_db": 14.6,    // JSON null when audio_insufficient
  "channel_snr_db": { "median": 18.2, "p10": 12.9, "p25": 15.4, "p75": 21.0, "p90": 23.8 },
  "audio_snr_db":   { "median": 14.6, "p10":  9.1, "p25": 11.8, "p75": 17.2, "p90": 19.9 },
  "events": [
    { "start_s": 3.42, "duration_s": 2.15, "utc": "2026-08-19T14:36:10Z",
      "carrier_offset_hz": -213.4, "signal_power_dbfs": -44.0,
      "channel_snr_db": 18.4, "audio_snr_db": 14.9,
      "valid": true, "truncated": false, "audio_snr_valid": true }
  ],
  "note": "cable routed along the balcony rail"
}
```

**Event retention.** Every metric — the capture score, the percentiles, the
yield — is computed over **all** valid events. `max_retained_events` (§7.3,
default 32) caps only how many event *objects* the document keeps, and the
retained subset is the first `max_retained_events` in time order. `events_total`
and `events_retained` are both persisted, so a truncated list is visible rather
than looking like a quiet capture. `events_total != events_retained` is stated in
the report's per-capture diagnostics.

**Status values**, each with exactly one meaning:

The `Ranks?` column means *ranks in the channel ranking*. Audio ranking
eligibility is decided separately by §9.4.1 and is not a function of status.

| Status | Meaning | Ranks? |
|---|---|---|
| `ok` | Channel metric computed and reliable. The audio metric may still be absent — see §9.4.1 | yes |
| `insufficient_data` | Fewer than `min_valid_events` valid events | no |
| `noise_floor_unreliable` | §9.1 R1 tripped | no |
| `noise_floor_unidentifiable` | §9.1 R2 tripped | no |
| `clipped` | `clipped_fraction` above limit | no |
| `timeout` | Read deadline exceeded | no |
| `source_error` | Device or file error; `status_detail` carries the text | no |
| `insufficient_samples` | Stream ended before the requested duration | no |
| `skipped` | Operator skipped this visit | no |
| `cancelled` | Operator quit during the capture | no |

**From capture outcome to attempt status.** `CaptureOutcome::Status` (§6.3)
describes what the capture loop did; `AttemptStatus` describes what the visit
produced. The mapping is total and is the only one:

| `CaptureOutcome::Status` | Then | `AttemptStatus` |
|---|---|---|
| `Ok` | floor is `Unreliable` | `noise_floor_unreliable` |
| `Ok` | floor is `Unidentifiable` | `noise_floor_unidentifiable` |
| `Ok` | floor reliable, valid events `< min_valid_events` | `insufficient_data` |
| `Ok` | floor reliable, enough valid events | `ok` |
| `Timeout` | — | `timeout` |
| `SourceError` | — | `source_error` |
| `Clipped` | — | `clipped` |
| `InsufficientSamples` | — | `insufficient_samples` |
| `Cancelled` | — | `cancelled` |
| *(no capture run)* | operator skipped the visit | `skipped` |

Only the fourth row can rank. Metrics are computed only on that row; every other
row leaves the metric fields unset rather than zero, so a missing measurement can
never be read as a measurement of zero.

**Disposition values**:

| Disposition | Meaning |
|---|---|
| `accepted` | This attempt is the visit's final result. It contributes to ranking **only if** `status == ok`. |
| `superseded` | A later attempt replaced it. Retained for audit; never ranked, never pooled into its successor. |
| `abandoned` | The operator skipped or quit without a result for this visit. |

A visit is **complete** when it has exactly one attempt whose disposition is
`accepted` or `abandoned`. An operator who declines to retry an
`insufficient_data` attempt turns it into `accepted` with a non-`ok` status:
the visit is done, the record is honest, and the angle simply has one fewer
contributing capture.

### 11.3.1 Transitions — one persistence outcome and one resume outcome each

Every path through a visit is listed. There is no undefined combination.

**Each row is exactly one `commit_visit()`, and therefore one `renameat()`.**
The rows that end a session are the exception and they are marked: `Quit` is
**one visit commit, and then — separately, later, from the app layer — a
`pause()` write**. Those are two commits, not one, and they are not atomic with
each other. The same is true of the last-visit row and `finalize()`.

| Capture outcome | Operator choice | The one `VisitCommit` | Session state after the commit | Visit complete? | On resume |
|---|---|---|---|---|---|
| `ok` | — (automatic) | this attempt `accepted`; `pending_retry` cleared | `running` | yes | not revisited; ranks |
| `insufficient_data` | Retry | this attempt **`superseded`**; `pending_retry = {visit_id, attempt+1}` | `running` | no | re-enter this visit at `next_attempt` |
| `insufficient_data` | Accept as-is | this attempt `accepted`; `pending_retry` cleared | `running` | yes | not revisited; does not rank |
| `insufficient_data` | Skip | this attempt `abandoned`; `pending_retry` cleared | `running` | yes | not revisited |
| `noise_floor_unreliable` / `noise_floor_unidentifiable` / `clipped` / `timeout` / `source_error` / `insufficient_samples` | Retry | as the retry row above | `running` | no | re-enter this visit at `next_attempt` |
| any of the above | Accept as-is | this attempt `accepted`; `pending_retry` cleared | `running` | yes | not revisited; does not rank |
| any of the above | Skip | this attempt `abandoned`; `pending_retry` cleared | `running` | yes | not revisited |
| any of the above | Quit | this attempt `abandoned`; `pending_retry` cleared | `running` — see the note below | yes | — |
| — | Skip before capture | a `skipped` attempt, `abandoned` | `running` | yes | not revisited |
| — | Quit before capture | a `cancelled` attempt, `abandoned` | `running` — see the note below | yes | — |
| in progress | Quit during capture | a `cancelled` attempt, `abandoned` | `running` — see the note below | yes | — |
| — | last visit committed | *(no further `VisitCommit`)* | `running` — see the note below | — | — |
| any | crash before the attempt commits | nothing written | unchanged | no | re-enter this visit at the same attempt number |
| any | crash after a retry commit | the `superseded` attempt and `pending_retry` are both present | `running` | no | re-enter this visit at `next_attempt` |

**The controller never writes a terminal or paused state.** Every row above
leaves the session `running`. The controller then returns — `QuitRequested`
after any Quit row, `Completed` after the last visit — and the **app layer**
aggregates, renders, and issues a *second, separate* commit:

| Controller returns | App layer calls | Resulting state | Resume behaviour |
|---|---|---|---|
| `QuitRequested` | `pause(partial)` | `paused` | reopens; remaining visits continue (§11.5) |
| `Completed` | `finalize(summary)` | `completed` | refused (§11.5) |
| `Failed`, or an unrecoverable §11.1.2 reconciliation | `abort(partial, reason)` | `aborted` | refused (§11.5) |

A crash in the window between the last `commit_visit` and that second commit
leaves a `running` record with every measurement intact; §11.5 reopens it and
reports the unclean shutdown. That is strictly better than the alternative
revision 2 chose, which was to let the controller finalise a session whose
summary it was not permitted to compute.

Only the `ok`/`accepted` row contributes to a ranking. Every other row leaves an
auditable record that no summary will ever read as data.

**Events from a superseded attempt are never pooled into the accepted one.**
This was the contamination path in revision 1, where a failed capture was
appended before the retry and the summariser pooled everything at that angle.

**Retry is one commit, not three.** The retry row writes the attempt with
disposition `superseded` and `pending_retry` together. Revision 2 exposed those
as three separate store calls — append the attempt, set its disposition, record
the retry intent — so a crash between them could leave `pending_retry` set on a
visit whose attempt was still `accepted`: two accepted attempts for one visit
once the retry landed, which §11.3's completion rule cannot represent.
`commit_visit()` (§6.6) makes it one `renameat()`, and because the attempt is
written with its final disposition the first time, there is nothing to go back
and change.


### 11.4 Retry durability

The retry commit writes, in one atomic replacement: the just-finished attempt
with `disposition = superseded`, and `pending_retry = { visit_id,
next_attempt }`. On resume, a non-null `pending_retry` means the controller
re-enters that visit at that attempt number rather than treating the visit as
complete. The field is cleared by the same `commit_visit()` that records the
replacement attempt's disposition.

Two invariants therefore hold after **every** possible crash point, and the
loader asserts both on open:

```
I1: no visit has more than one attempt whose disposition is `accepted`
    or `abandoned`
I2: pending_retry is non-null  <=>  the named visit's latest attempt has
    disposition `superseded`
```

### 11.5 Resume and the session lifecycle

`--resume <dir>` loads `session.json` and:

1. **Refuses a `completed` or `aborted` session,** naming its `finished_utc`
   (and `abort_reason` where present) and suggesting `rtlangle report <dir>` to
   re-render it, or a new session to take more data. Both are terminal: a
   `completed` summary is a statement about a finished experiment, and an
   `aborted` one is a statement that the record could not be trusted to
   continue. Reopening either would invalidate that statement silently.
   A `paused` session reopens: `state` returns to `running`, `summary` is
   cleared, and `summary_partial` returns to `false`, because the partial report
   described a subset that is about to change. A `running` session — the record
   left by a crash — also reopens, and the loader reports that the session was
   not shut down cleanly.
2. Rejects `schema_version` greater than the supported version, naming both.
   Older supported versions are migrated by an explicit, tested migration
   function; version 1 is the first, so the migration table is currently empty
   and the mechanism is exercised by a synthetic version-0 fixture in tests.
3. Deletes any stray `session.json.tmp.*`.
4. Regenerates `measurements.csv` if its attempt count disagrees with the JSON.
5. Leaves the record with **one more receiver segment than it had** (§11.2).
   `load_session` itself opens no device and creates no segment — that is the
   app layer's job on both the new-session and the resume path (§6.6). A resumed
   session is a second device open, so its applied settings become a new
   baseline; W7 is evaluated within each segment, and §10.5 notes whenever a
   session has more than one.
6. Classifies every `Config` field into exactly one of three classes and acts
   accordingly. The classification is exhaustive by construction: a field with
   no entry is a test-visible gap, because the `resume_class` test in the plan
   enumerates `Config`'s serialised fields and requires an entry for each.

   | Class | On a conflicting CLI override | Fields |
   |---|---|---|
   | **ExperimentDefining** | Reject, naming the field and both values | `device_index`, `center_hz`, `sample_rate_hz`, `gain_tenth_db`, `ppm`, `offset_tune_hz`, `channel_bw_hz`, `channel_rate_hz`, `bias_tee`, `duration_s`, `start_deg`, `end_deg`, `step_deg`, `angles_deg`, `rounds`, `order`, `seed`, `angle_reference`, `max_angle_deviation_deg`, `angle_deviation_warn_deg`, `source_spec`, `synthetic_snr_db`, `synthetic_duty`, and all of §7.3 except the two read-timeout fields and `max_retained_events`: `noise_percentile`, `probe_percentile`, `open_db`, `close_db`, `min_event_ms`, `merge_gap_ms`, `min_valid_events`, `max_active_fraction`, `carrier_prominence_db`, `carrier_persistence`, `audio_guard_ms`, `min_audio_window_ms`, `max_clipped_fraction` |
   | **ReportOrOperational** | Accept the override | `report_metric`, `min_captures_advisory`, `min_effect_db`, `yield_concordance_ratio`, `noise_drift_warn_db`, `settle_s`, `read_timeout_factor`, `read_timeout_slack_s`, `max_retained_events`, `no_color`, `non_interactive`, `setup_note`, all `scan_*` fields |
   | **PathDetermined** | Ignore the CLI value; the resume path decides | `session_root`, `label`, `resume_dir` |

   Every §7.4 field is `ReportOrOperational`, without exception, because §7.4
   holds only reporting thresholds: they are applied when the report is
   rendered, not when a capture is taken, and raising `--min-captures` on a
   resumed session changes only which warnings appear. Revision 3 had one
   exception, `primary_metric`, which had to be frozen because it selected a
   statistical test; decision Q8 retired the field along with the test, so the
   exception is gone and the rule is uniform. `max_retained_events` is
   operational because it changes only how much detail later attempts persist,
   never what is measured. `setup_note` is operational because it is descriptive
   metadata about the physical setup, while `angle_reference` is
   experiment-defining because every recorded angle is measured from it.
7. Refuses to continue if the stored `angle_provider.automated` differs from the
   provider the resumed invocation would build, since a manual and an automated
   session are not the same experiment.
8. Computes the remaining visits as the plan minus the complete visits, in the
   stored order, and honours `pending_retry`.

Resumption never changes the receiver configuration, because a resumed session
whose gain or frequency differed would silently mix two experiments.

### 11.6 `measurements.csv`

One row per attempt, in commit order, with a header written on creation:

```
visit_id,round,angle_index,planned_deg,actual_deg,attempt,segment_id,disposition,
status,started_utc,duration_s,valid_events,valid_audio_events,audio_insufficient,
events_total,events_per_minute,
detected_fraction,capture_score_channel_db,capture_score_audio_db,
channel_snr_p25_db,channel_snr_median_db,channel_snr_p75_db,
audio_snr_median_db,noise_floor_dbfs,active_probe_fraction,dynamic_range_db,
applied_gain_tenth_db,applied_sample_rate_hz,applied_center_hz,
host_dropped_samples,clipped_fraction,note
```

The column is `host_dropped_samples`, not `sample_overruns`: §6.1 explains why
the honest name matters, and a CSV header is exactly the place a misleading name
would outlive the explanation.

RFC 4180 quoting: fields containing a comma, quote, CR, or LF are quoted and
internal quotes doubled. Notes are stored verbatim in JSON and CSV; control
characters are neutralised only at terminal render time (§13), never by mutating
the canonical record.


## 12. Terminal interface, CLI, and scan

### 12.1 Commands and exit codes

```
rtlangle run [options]         run an angle experiment (interactive by default)
rtlangle scan [options]        find active airband channels
rtlangle devices               enumerate devices, gain table, tuner type
rtlangle report <session-dir>  re-render a report from a stored session
rtlangle --help | --version
```

Invoking `rtlangle` with no arguments on a TTY opens the main menu.

| Exit code | Meaning | Session `state` on exit |
|---|---|---|
| 0 | Every planned visit is complete | `completed` |
| 0 | Session ended early by `Quit`, with a valid partial report written | `paused` |
| 1 | Runtime failure (capture, persistence, rendering) that the app layer could end cleanly | `aborted`, with `abort_reason` set |
| 1 | Runtime failure that prevented even that | unchanged; whatever was last committed |
| 2 | Usage or configuration error, including a flag a command does not accept (§7.9) | no session created |
| 3 | Device not found, busy, or refused | no session created, or unchanged on resume |

`Quit` never writes `completed`. The controller commits the visit and returns;
the app layer then writes a partial summary, sets `paused`, leaves
`finished_utc` as JSON `null`, and marks the rendered `report.txt` as
`PARTIAL` in its first line. Only running out of planned visits writes
`completed`; only an unrecoverable fault writes `aborted`, whose report is
marked `ABORTED — PARTIAL`. `completed` and `aborted` are terminal; `paused`
is not (§11.5).

### 12.2 Main menu

```
  Start new experiment
  Resume session
  Scan airband (find active channels)
  Device check / gain table
  Replay synthetic session
  Quit
```

Arrow keys, `j`/`k`, Enter to select, `q` or Escape to cancel.

### 12.3 Terminal behaviour

- **Raw mode only when it is safe.** `AnsiTerminalUi` applies `termios` changes
  only when `isatty(STDIN_FILENO)` and `isatty(STDOUT_FILENO)` are both true.
  Otherwise `interactive()` returns false, menus fall back to reading a numbered
  choice as a line from stdin, and no `termios` call is made against a pipe.
- **Signal safety.** The saved `termios` state lives in a namespace-scope
  object, never on the stack, so a handler can never reference destroyed state.
  The RAII `RawMode` guard sets a flag on entry and clears it on exit. The
  `SIGINT`/`SIGTERM` handlers use only async-signal-safe calls: `tcsetattr` to
  restore, `write` to re-show the cursor, then re-raise the default action. A
  Ctrl-C must never leave the operator with a broken shell.
- **Colour** is emitted only when stdout is a TTY, `NO_COLOR` is unset, and
  `--no-color` was not passed.
- **Control-character neutralisation.** Every string that originates outside the
  program — session labels, operator notes, file paths, USB device descriptors,
  librtlsdr error text — passes through `ui::safe_text()` before rendering,
  which replaces C0, C1, and DEL characters with a visible `\xNN` form. This
  prevents a crafted device string from repositioning the cursor or injecting
  colour. It applies at render time only.
- **Progress** during a capture shows planned angle, round, attempt, elapsed and
  remaining time, and the live host-dropped-sample count.

### 12.4 Per-visit interaction

1. Heading: planned angle, round, visit *n* of *N*, attempt number.
2. Positioning instruction referencing the recorded `angle_reference`.
3. `Continue` / `Retry this angle` / `Skip this angle` / `Add a note` /
   `Quit and save`. The retry wording is "Retry **this** angle" — the
   superseded text said "previous", implying a backtracking capability that
   does not exist.
4. Prompt for the actual achieved angle; blank accepts the planned value;
   deviation beyond `max_angle_deviation_deg` requires confirmation.
5. Settling delay (`--settle`), then `source.flush()`.
6. Capture with progress.
7. Metrics rendered; the attempt is committed **before** the next visit begins.
8. A non-`ok` status offers `Retry` / `Accept as-is` / `Skip` / `Quit`. Each
   choice is exactly one `commit_visit()` (§11.3.1); `Quit` additionally calls
   `pause()`, which writes the partial report and sets `state: paused`.

### 12.5 Scan

Scan sweeps `scan_start_hz`..`scan_end_hz` in `scan_channel_hz` channels, for
`scan_passes` passes, using an `ITunableSampleSource`.

Per dwell:

- Retune to the dwell centre and discard buffered samples from the previous
  frequency.
- Capture `scan_dwell_ms` and compute a Welch PSD.
- The **usable span** is `scan_usable_fraction * sample_rate_hz` centred on the
  dwell, minus a **DC exclusion** of `±scan_dc_exclusion_hz` around the dwell
  centre where the RTL-SDR's DC spur sits.
- Each channel whose centre falls inside the usable span and outside the DC
  exclusion gains one **exposure**. A channel outside those bounds gains
  nothing — it is neither a hit nor a miss.
- A channel is a **hit** in that dwell when its band power exceeds the dwell's
  band noise floor (the median of all in-span channel powers) by `open_db`.

Reported activity per channel is `hits / exposures`. A channel with
`exposures == 0` is reported as `-` and never as `0 %`; dividing by a
denominator that does not exist was an error path in revision 1.

#### 12.5.1 Dwell placement, and why the previous rule could not work

Write

```
U = scan_usable_fraction * sample_rate_hz / 2      usable half-span   (409600 Hz at defaults)
D = scan_dc_exclusion_hz                           DC exclusion        (30000 Hz at defaults)
S = dwell step
```

A dwell centred at `c` covers `[c-U, c-D] ∪ [c+D, c+U]`. It covers **nothing**
in `(c-D, c+D)`.

Revision 2 stepped by the whole usable span, `S = 2U`. Consecutive dwells then
abut exactly at `c+U = (c+2U)-U`, and the hole `(c-D, c+D)` around every dwell
centre is covered by no dwell at all. At the defaults that hole is 60 kHz wide
against a 25 kHz channel spacing, so **two channels per dwell were permanently
invisible** — and §14.1's "every channel ends with `exposures >= 1`" test could
never have passed. This is the corrected rule, with its proof, before the test
was written.

**Coverage condition.** The hole around `c` must be covered by the neighbouring
dwells at `c ± S`. The dwell at `c-S` covers up to `c-S+U`, so it reaches the far
edge of the hole when

```
c - S + U  >=  c + D        i.e.   S <= U - D                    (1)
```

and its own upper arm starts at `c-S+D`, which must reach the near edge:

```
c - S + D  <=  c - D        i.e.   S >= 2D                       (2)
```

(1) also guarantees `S + D <= U`, so consecutive upper arms overlap and no gap
opens between dwells. A step satisfying both exists exactly when

```
2D <= U - D      i.e.      U >= 3D                               (3)
```

which is the §7.6 cross-field validation rule
`scan_usable_fraction * sample_rate_hz >= 6 * scan_dc_exclusion_hz`.

**The chosen step is the largest legal one,** `S = U - D`, because it minimises
the number of dwells while still satisfying (1) and (2). At the defaults
`S = 379600 Hz`.

**Centres.** With `S = U - D`:

```
c_0 = scan_start_hz + U
c_i = c_0 + i * S
K   = the smallest i with c_i >= scan_end_hz + D
```

- `scan_start_hz = c_0 - U` lies in dwell 0's lower arm, so the **first**
  channel is exposed.
- `c_K >= scan_end_hz + D` gives `scan_end_hz <= c_K - D`, and because
  `c_{K-1} < scan_end_hz + D` and `S <= U - D` we also get
  `c_K < scan_end_hz + U`, so `scan_end_hz` lies in dwell `K`'s lower arm and
  the **last** channel is exposed.
- Every interior point is covered by (1) and (2).

At least two dwells are always generated, since the hole of a single dwell would
otherwise have no neighbour to cover it; a band narrower than one dwell still
gets `K >= 1`.

**Cost.** The step is roughly half of revision 2's, so the dwell count roughly
doubles. Worked through for the default band and the default receiver settings:

```
U  = 0.80 * 1024000 / 2 = 409600
D  = 30000
S  = U - D              = 379600
c_0 = 118000000 + U     = 118409600
need smallest K with c_K >= scan_end + D = 136975000 + 30000 = 137005000
  K = 48 -> c_48 = 118409600 + 18220800 = 136630400   too low
  K = 49 -> c_49 = 118409600 + 18600400 = 137010000   ok
centres 0..49                                        = 50 dwells
50 dwells x 250 ms x 4 passes                        = 50 s of dwell time
```

Coverage of the last channel checks out: `c_49 - U = 136600400 <= 136975000 <=
136980000 = c_49 - D`. Fifty seconds of dwell time is the price of covering the
band, and the number is stated here — and asserted by name in the plan's
edge-coverage test — so that a later change to `scan_usable_fraction` or
`scan_dc_exclusion_hz` shows up as a changed expectation rather than as a
silently different sweep.

Retune failure aborts the scan with the failing frequency named. Scan writes
nothing into `sessions/`; it prints the busiest `scan_top_n` channels with a
copyable `rtlangle run --freq ...` line for the top result.


## 13. Filesystem and input hardening

### 13.1 Threat model, stated before the mitigations

`rtlangle` is a single-user tool that writes into a directory the operator owns.
The hardening below is **defence in depth against accidents and against a
hostile session root** — a `--session-dir` on a shared filesystem, a `/tmp`-like
location, or a path an unrelated program also writes to. It is **not** a defence
against an attacker who already has write access to the operator's home
directory or to the parent of the session root; such an attacker can replace the
whole directory before the program starts, and no file-open flag helps.

Saying this plainly matters because the next subsection makes a narrower claim
than revision 3 did, and a narrow true claim is worth more than a broad false
one.

### 13.2 What `O_NOFOLLOW` does and does not buy

`O_NOFOLLOW` refuses a symlink **at the final path component only**. Every
intermediate directory in the path is still resolved normally, symlinks
included. Revision 3 claimed the flag "blocks a symlink planted at a predictable
session path from redirecting a write", which overstates it: a symlink at
`sessions/` — an intermediate component — redirects everything underneath and
`O_NOFOLLOW` on `sessions/20260819-.../session.json` never sees it.

The protocol that does buy the property is a **directory file descriptor**:

1. Create the session directory with `mkdir(path, 0700)`. `mkdir` fails with
   `EEXIST` if anything is already there, symlink included, so the directory
   this program writes into is one it created.
2. Open it once with `open(path, O_DIRECTORY | O_NOFOLLOW | O_RDONLY)` and keep
   the descriptor for the lifetime of the session.
3. Do **every** later operation relative to that descriptor: `openat(dirfd, ...,
   O_NOFOLLOW)`, `renameat(dirfd, tmp, dirfd, "session.json")`,
   `fsync(dirfd)`, `unlinkat(dirfd, ...)`.

Once the descriptor is held, replacing any component of the path — including
intermediate directories — cannot redirect a write, because the kernel resolves
nothing but the final name. `open_no_symlink` therefore takes a `dirfd` and a
single name, never a multi-component path:

```cpp
// Opens `name` (a single path component, asserted to contain no '/')
// relative to `dirfd`, refusing a symlink at that name.
int open_at_no_symlink(int dirfd, std::string_view name, int flags, mode_t);
```

**Residual risk, stated:** a symlink or a replaced directory that is already in
place when step 1 runs is caught by `mkdir` returning `EEXIST`, but a hostile
*parent* of the session root that is swapped between the operator typing the
command and step 1 executing is not defended against. That is the
write-access-to-the-home-directory case §13.1 puts out of scope.

For the paths this program only *reads* — the config file, a `file:` source, a
`.cu8.json` sidecar, the resume directory — the same `O_NOFOLLOW`-on-the-final-
component rule applies, with the same limitation, and a symlink at that
component is refused with a message naming the path.

### 13.3 Label sanitisation

`--label` becomes a **single path component** and is never treated as a path.
The algorithm, in this order:

1. Replace every character outside `[A-Za-z0-9._-]` with `-`. This removes `/`
   and every separator.
2. Collapse runs of `-` to one `-`, **and runs of `.` to one `.`**.
3. Strip leading and trailing `.` and `-`.
4. Truncate to 32 characters, then strip trailing `.` and `-` again, because the
   truncation can expose one.
5. If the result is empty, `.`, or `..`, the label is dropped entirely.

Step 2's dot-run collapse is what revision 3 was missing. Its rule replaced
non-allowed characters and stripped *leading* dots, but `.` is inside the
allowed set, so `a..b` survived intact and the accompanying claim that "`..` can
therefore never appear" was false. Collapsing dot runs makes `..` unrepresentable
in the output: after step 2 no two dots are adjacent, so no substring `..`
exists anywhere in the result, not merely at the start.

Worked examples, each a test case:

| Input | Output | Why |
|---|---|---|
| `../../etc/passwd` | `etc-passwd` | separators to `-`, dot runs collapsed, leading punctuation stripped |
| `a..b` | `a.b` | the case revision 3 let through |
| `..` | *(dropped)* | empty after stripping |
| `.` | *(dropped)* | empty after stripping |
| `x/../y` | `x-.-y` | `/` to `-`, `..` collapsed to `.` |
| 100 × `a` | 32 × `a` | truncation |
| `my label!` | `my-label` | space and `!` to `-`, run collapsed, trailing `-` stripped |

Independently of the label, the **assembled directory name** is validated before
`mkdir`: it must contain no `/`, and must be neither `.` nor `..`. That check is
the invariant; sanitisation is how the label is made to satisfy it.

### 13.4 Everything else

- Session directories are created mode 0700, files mode 0600. An experiment
  record can contain location-revealing notes; it is not world-readable by
  default.
- Temporary files are `session.json.tmp.<pid>` created `O_EXCL`, not a
  predictable fixed name.
- Size limits: config file ≤ 1 MiB, `.cu8.json` sidecar ≤ 1 MiB, `session.json`
  ≤ 64 MiB on read, a note ≤ 500 characters, `angle_reference` ≤ 200
  characters, any single CLI string argument ≤ 4096 characters.
- Session directory names collide only within one second; on collision a `-2`,
  `-3` suffix is appended rather than reusing an existing directory. Because the
  directory is created with `mkdir`, a collision is detected by `EEXIST` rather
  than by a check that could race.


## 14. Acceptance tests and evidence states

Framework: doctest. Every test in §14.1–§14.2 runs without RTL-SDR hardware and
is built into `rtlangle_tests`. The hardware test is a separate executable
(§14.3).

### 14.1 Core, DSP, and I/O

- **Angle math.** Default sequence equals `0,15,30,45,60,75,90`;
  `0:20:90` produces `0,20,40,60,80`; explicit `--angles` dedup and sort; every
  validation rule produces its own message; `circular_distance_deg(350,10)==20`;
  circular mean of `{350,10}` is 0; a generated set containing `(θ, θ+180)`
  emits the §4.4 informational message and **still keeps both angles**;
  `alternating` reverses on even rounds; `random` with a fixed seed is
  reproducible and is a permutation.
- **Config.** JSON round-trip of every field; each bound produces its own error;
  every cross-field rule of §7.1 and §7.6 produces its own error naming both
  fields — specifically `channel_bw_hz = 25000` with `channel_rate_hz = 16000`
  is refused, `channel_bw_hz = 2000` is refused, a `sample_rate_hz /
  channel_rate_hz` ratio of 17 is refused naming the ratio, and
  `scan_dc_exclusion_hz = 256000` with `scan_usable_fraction = 0.8` at
  1.024 MHz is refused; unknown keys rejected; a config file over the size limit
  refused; resume override classification correct for every field in §11.5.
- **Command scope (§7.9).** Every flag a command refuses produces exit code 2
  naming the flag and the command — table-driven over the full field list, so a
  new field with no command scope fails the suite. `run` without `--freq` is an
  error; `run --resume <dir>` without `--freq` is accepted.
- **Resource preflight.** `estimate_resources` reproduces the §11.1 table:
  at `max_retained_events = 32` and 250 attempts it reports ≈1.9 MiB final and
  ≈232 MiB cumulative; at `max_retained_events = 500` the projected final size
  exceeds 16 MiB and the run is refused naming both values.
- **Filters.** Measured frequency response meets the passband and stopband edges
  of §8.2 within tolerance; every designed filter has an odd tap count;
  chunk-invariance for every stateful block; measured group delay matches
  `(T-1)/2`; `Chain::latency()` returns an exact rational whose `ceil_samples`
  and `residual` are consistent, and the impulse-response peak index equals
  `floor` or `ceil` of that rational — the test asserts a two-value set, not a
  single value it cannot always hit.
- **Mixer.** Integer accumulator phase error after 60 s at 1.024 MHz is below
  1e-6 cycles; a tone at `-offset` lands at DC within one bin.
- **Byte conversion** at 0, 127, 128, and 255. **Clipping census** counts rail
  bytes exactly.
- **Sources.** Synthetic source is deterministic for a seed; `.cu8` replay
  matches a known byte pattern; a `.cu8.json` sidecar that disagrees with the
  config is a configuration error naming both values, while a missing sidecar is
  accepted; a file consumed across two captures continues rather than rewinding;
  exhausting the file yields `insufficient_samples` for that capture and every
  later one; an odd trailing byte is dropped and reported once; a truncated file
  yields `insufficient_samples`; a read deadline yields `Timeout` and not a
  hang; `cancel()` wakes a blocked read; a worker error surfaces as `Error` with
  text.
- **Capture runner.** `started_utc` precedes the first read;
  `host_dropped_samples` is a per-capture delta and never cumulative; the
  transient trim removes exactly `ceil(chain latency) + guard` channel samples;
  clipping above the limit fails the capture; `CaptureOutcome::applied` is a
  snapshot taken at capture start.
- **Persistence.** Round-trip of every field; after every `commit_visit` the
  JSON parses and no `.tmp` remains; a stale CSV is regenerated on reopen; a
  missing, truncated, or malformed `report.txt` is regenerated and never read
  back; notes containing commas, quotes, and newlines survive;
  `schema_version 2` is rejected; a version-0 fixture migrates.
- **Commit outcomes (§11.1.1), one test per boundary**, each asserting the
  exact `CommitOutcome` and not merely "recoverable":

  | Injection point | Required outcome |
  |---|---|
  | before temp write | `NotCommitted` |
  | after temp write, before `fsync(file)` | `NotCommitted` |
  | at `fsync(file)` | `NotCommitted` |
  | at `renameat`, classifiable error | `NotCommitted` |
  | at `fsync(dirfd)`, rename having succeeded | `CommittedNotDurable` |
  | during CSV regeneration | `Committed`, with a repair note |
  | `renameat` returns an unclassifiable error | `Indeterminate` |

  Every case additionally asserts invariants I1 and I2 (§11.4), and that
  `NotCommitted` leaves `session.json` **byte-identical** to its pre-commit
  contents. `CommittedNotDurable` asserts the attempt *is* readable afterwards
  and that a `durability_warnings` entry was recorded — a retry here would
  duplicate the attempt, and a test asserts the caller does not retry.
- **`Indeterminate` reconciliation (§11.1.2).** With the attempt present on disk,
  reconciliation continues as `Committed`; with it absent, as `NotCommitted`;
  with `reload_from_disk` itself failing, the app layer writes `state: aborted`
  with `abort_reason` set and `finished_utc` stamped, and a subsequent resume of
  that directory is refused.
- **The `NotCommitted` ladder (§11.1.1) is total.** `EINTR` at `openat`,
  `write`, or `fsync` is retried inside the wrapper and never surfaces as a
  `CommitOutcome`; `EIO` from `fsync` is not retried. A `commit_visit` that
  finally returns `NotCommitted` makes the controller return `Result::Failed`
  with the visit not advanced and no attempt for that visit on disk; the app
  layer then calls `abort`, and a decorator that fails `abort` too yields exit
  code 1 with the record in its last committed state, `finished_utc` **not**
  stamped and no `abort_reason` written. `pause` or `finalize` returning
  `NotCommitted` exits 1 with the record left `running` and **no** `abort`
  attempted, and a later `--resume` of that directory succeeds. With the row
  above, each of §12.1's two exit-1 rows has a test.
- **Attempt semantics and atomicity.** A retry is **one** `commit_visit`: a
  fault injected anywhere inside it leaves either the pre-retry record or the
  full post-retry record and never a half state, asserted by checking after
  every injection point that no visit has two `accepted`-or-`abandoned`
  attempts and that `pending_retry` is set if and only if the prior attempt is
  `superseded`; a superseded attempt's events never reach the summary;
  `pending_retry` survives a simulated crash and resumes at the right visit and
  attempt; `skipped` counts as complete for resume.
- **Session lifecycle.** After the controller returns for any reason the record
  still says `running` — asserted directly, because the controller may not write
  a terminal state (§6.6). The app layer's `pause()` then writes
  `state: paused`, a partial summary, `summary_partial: true`,
  `finished_utc: null`, and a `report.txt` whose first line says `PARTIAL`;
  `finalize()` writes `completed`; `abort()` writes `aborted` with
  `abort_reason` and a report marked `ABORTED — PARTIAL`. Resuming a `paused`
  session clears the partial summary and returns to `running`; resuming a
  `completed` **or an `aborted`** session is refused naming `finished_utc`; a
  `running` record left by a crash reopens and the loader reports the unclean
  shutdown.
- **Reserved retry capacity (§7.7).** With `attempts_committed +
  remaining_visits == attempt_ceiling`, the retry option is not offered and the
  message names the remaining visit count; every remaining visit can still take
  its one attempt and the session reaches `completed`. A table-driven test walks
  the boundary at ceiling−1, ceiling, and ceiling+1.
- **Visit identifiers.** `--angles "45.001,45.002"` produces two visits with
  distinct `visit_id` values and distinct full-precision `planned_deg`; the id
  format is `r<round>-i<3-digit index>`; ids are stable across a resume.
- **Label sanitisation (§13.3).** Table-driven over every worked example in
  §13.3, plus the invariant that **no output contains the substring `..` for any
  input**, asserted over a fuzz corpus including dot-heavy strings (`a..b`,
  `...`, `a...b`, `.` × 40). The `a..b` case is the direct regression guard: it
  survived revision 3's rule, which stripped only *leading* dots while `.` was
  inside the allowed set. Also: the assembled directory name is rejected if it
  contains `/` or equals `.` or `..`.
- **Filesystem hardening (§13.2).** `mkdir` on an existing path — including an
  existing symlink — fails with `EEXIST` and the tool does not write through it.
  Once the session `dirfd` is held, replacing an **intermediate** directory
  component does not redirect a write: swap a parent directory mid-session and
  assert the bytes still land in the original inode. A symlink at the final
  component of the config path, a `file:` source, or a `.cu8.json` sidecar is
  refused with the path named. A `session.json` over 64 MiB is refused.
  Directory mode is 0700 and file mode 0600 — assert with `stat`.
- **Audio eligibility (§9.4.1).** A capture with 5 channel-valid events of which
  1 is audio-eligible, at `min_valid_events = 3`, yields a channel score, **no**
  audio score, `audio_insufficient == true`, and status `ok`. A capture whose
  audio floor is unreliable while the channel floor is reliable yields the same
  shape with `status_detail` naming the audio floor — the third row of §9.4.2.
  `valid_audio_event_count <= valid_event_count` is asserted as an invariant
  over a fuzz corpus.
- **Terminal.** Menu navigation, cancel, and prompt handling through
  `ScriptedTerminalUi`; non-TTY input never invokes `tcsetattr`; a pseudo-
  terminal test delivers `SIGINT` during a menu and asserts termios is restored;
  `safe_text()` neutralises control characters.
- **Scan.** The dwell-placement rule of §12.5.1 is asserted directly: with the
  defaults the step is `U - D = 379600 Hz`, the generated centre list satisfies
  `2D <= S <= U - D`, and **every** channel in
  `[scan_start_hz, scan_end_hz]` — including the first and the last — ends with
  `exposures >= 1`. A regression fixture pins the failure mode revision 2 had:
  stepping by `2U` leaves the 60 kHz band around each centre with
  `exposures == 0`, and the test asserts the current rule does not. Channels in
  the DC exclusion of a given dwell gain no exposure *from that dwell*; activity
  uses the true denominator; a channel never observed reports `-`; retune
  failure aborts with the frequency named; a synthetic band with transmitters at
  known frequencies and duty cycles is recovered in the right rank order.

### 14.2 Measurement correctness

These are the tests that decide whether the tool measures what it claims.

**Squelch settings inside the low-SNR tests.** Tests B and D exercise true SNRs
at and below 0 dB. At 0 dB true SNR the *total* in-channel power is only
`10*log10(2) = 3.01 dB` above the noise floor, which is **below** the production
default `open_db = 6.0`, so no event is ever detected and no median exists to
assert on. Revision 2's Test B and Test D were therefore unpassable as written.
Both now run their low-SNR sweep points with a test-only squelch of
`open_db = 1.0, close_db = 0.5` — legal under the §7.3 bounds, and roughly
6.7 standard deviations above the mean noise frame power for 1024-sample frames,
so false openings are negligible. Each of the two tests additionally asserts the
**production default** behaviour in a separate case: at `open_db = 6.0` a 3 dB
signal produces zero events and the reported reason names the
`minimum_detectable_snr_db` limit rather than claiming the channel was quiet.

- **Test A — estimator in isolation, tolerance 0.5 dB.** Feed `SnrEstimator`
  channel-rate frames built from complex Gaussian noise of known variance with a
  known signal added over a known subset of frames. The reference is derived
  **by measurement** from the separated signal-only and noise-only streams, not
  from the nominal input value: the 20th-percentile floor reads about 0.12 dB
  low for 1024-sample frames, so asserting against the nominal value would put
  the test inside the estimator's own bias budget.
- **Test B — full chain, tolerance 1.5 dB.** Generate at 1.024 MHz a complex AM
  carrier at the tuning offset, tone-modulated at 1 kHz with known depth, plus
  complex AWGN. Push it through the real
  `OffsetMixer -> FirDecimator -> FirDecimator -> ChannelFilter` chain. The
  reference is obtained by pushing the separated signal-only and noise-only
  streams through the *identical* chain and taking their mean output powers,
  which accounts for the cascade's noise-equivalent bandwidth without
  hand-derived constants. The estimator under test uses framing, hysteresis
  detection, a percentile floor, and a median, so the two share no code path.
  Swept at 0, 5, 10, and 20 dB with the test squelch above; additionally assert
  that measured differences between adjacent sweep points track true differences
  within 1.0 dB, so a constant bias cannot hide a scale error.
- **Test C — negative control.** Pure noise: zero valid events, status
  `insufficient_data`, no SNR emitted, and **not** `noise_floor_unreliable`.
- **Test D — the forbidden metric.** With the test squelch, at a true SNR of
  0 dB, assert the reported value is within tolerance of 0 dB and specifically
  **not** within 0.5 dB of 3.01 dB, which is what `10*log10(P_event/N)`
  produces. This test fails loudly if the subtraction in §9.3 is ever removed.
- **Test E — noise-floor reliability, Monte Carlo.** Over at least 200 seeded
  trials each: pure noise is flagged unreliable in under 1 % of trials; a
  90 %-occupied capture is flagged in over 99 %; a constant unmodulated carrier
  yields `noise_floor_unidentifiable` and never a fabricated SNR; the
  false-positive and detection rates for the persistent-carrier test are
  measured and recorded in the test file, and `carrier_prominence_db` is set to
  whatever value achieves them.
- **Test F — audio SNR, absolute.** A known modulation depth and noise level
  through the demodulator; assert recovery within 2.0 dB against a reference
  measured from separated streams, over a sweep of at least three input levels.
  Monotonicity alone is insufficient and does not satisfy this test.
- **Test G — audio alignment.** An impulse and a 400 ms burst confirm that
  `audio_latency_samples` compensation places the guarded window inside the
  transmission; residual misalignment is smaller than `audio_guard_ms`.
- **Test H — event detection.** Exact event count and boundaries for bursts at
  known positions; a gap shorter than `merge_gap_ms` merges; a burst shorter
  than `min_event_ms` is discarded *after* merging; hysteresis prevents
  fragmentation at the threshold; an event open at the first or last frame is
  `truncated` and excluded.
- **Test I — aggregation.** Capture medians, not events, drive the ranking:
  changing the *number* of events inside a capture does not change that
  capture's vote. A superseded attempt cannot influence a result. Circular
  statistics are used for actual-angle means. `n_captures_audio` is tracked
  separately from `n_captures`, and an angle with no audio-eligible capture is
  absent from the audio ranking, present in the channel ranking, and rendered as
  `-` rather than `0` (§10.1).
- **Test J — the estimand, and warning W6.** The selection effect of §10.0 is
  constructed directly: angle `A` receives a synthetic population of
  transmissions of which only the strong half clears the squelch, and angle `B`
  receives the same population 3 dB stronger so that the weak half also clears
  it. Assert that `angle_score(B) < angle_score(A)` — the *better* angle scores
  lower, which is the effect the estimand exists to name — and that
  `angle_yield(B) > angle_yield(A)`. Then assert that **W6 fires**, that its
  message carries both yields and the ratio, and that both rankings are still
  printed in full. A second, concordant fixture leaves W6 silent.
- **Test K — no decision exists.** Structural, and greppable rather than
  behavioural:
  - `SessionSummary` has no member whose name contains `resolved`,
    `resolution`, or `winner` — asserted over its JSON serialisation;
  - no string reachable from `render_report` contains `resolved`,
    `statistically`, `Highest measured`, `best angle`, or `optimal`;
  - the §10.6 headline appears in **every** rendered report, in the resolved-
    looking and the sparse case alike, because there is only one headline;
  - changing `report_metric` between two renders of the same record changes only
    the order in which the two tables appear — every number in both tables is
    byte-identical.
- **Test L — every warning, every time.** The test is **enumerated by id, not
  by range**: `W1`, `W2`, `W3`, `W4`, `W5`, `W6`, `W7`, `W8` each have a fixture
  that fires them and a fixture that does not, and the test asserts the set of
  ids the suite exercises equals the set §10.4 defines. Writing it as "W1–W8"
  would let a warning that was never implemented pass unnoticed — W3 and W4 are
  the likely casualties, since they were §10.5 notes in revision 3 and are the
  two with no revision-3 gate behind them.

  A record engineered to trip three warnings at once reports **all three**:
  assert the count and the id set, because §10.4 forbids short-circuiting and a
  list that stops at the first problem is a gate ladder under another name. A
  clean session reports an empty list.


### 14.3 Hardware evidence states

`rtlangle_hardware_test` is a **separate executable**, registered with CTest
under the label `hardware` and with `SKIP_RETURN_CODE 77`. Labels apply to
registered tests rather than to doctest cases inside a binary, so a separate
executable is what makes `ctest -LE hardware` a genuinely hardware-free run.

| Outcome | Exit code | CTest result | Meaning |
|---|---|---|---|
| `passed` | 0 | Passed | A device was opened and the receive path assertions held |
| `skipped` | 77 | Skipped | No device, or the device was busy. **Validation is pending.** |
| `failed` | 1 | Failed | A device was opened and an assertion failed |

The test prints exactly one machine-readable line, `RTLANGLE_HARDWARE=passed`,
`=skipped`, or `=failed`, and any completion report must parse that line rather
than infer success from a green aggregate CTest run. A green run that contains
only `skipped` is reported as **hardware validation pending**, never as passed.

The hardware test opens device 0, sets sample rate and a fixed gain, reads one
second, asserts a plausible non-constant sample distribution and a clipped
fraction below the limit, and closes. It never enables the bias tee and never
transmits.

### 14.4 Release verification

From a clean worktree, retaining full output:

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure -LE hardware
ctest --test-dir build --output-on-failure -L  hardware

cmake -S . -B build-nohw -G Ninja -DRTLANGLE_WITH_RTLSDR=OFF
cmake --build build-nohw
ctest --test-dir build-nohw --output-on-failure

cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure -LE hardware
```

Plus:

- one synthetic end-to-end CLI session at `--rounds 4`, whose report carries a
  yield column beside every score, the full W1–W8 warning list, and the single
  §10.6 headline;
- one synthetic session constructed so the *better* angle detects more weak
  transmissions, asserting that warning W6 fires and that both rankings are
  still printed (§10.0);
- one quit → `paused` → resume → `completed` session, followed by a resume of
  the now-`completed` directory that is **refused** naming `finished_utc`;
- one session driven into `aborted` by an unrecoverable §11.1.2 reconciliation,
  whose report is marked `ABORTED — PARTIAL` and whose resume is refused;
- one scan run whose printed dwell count and coverage match §12.5.1;
- and — only when a free device exists — one real receive session whose JSON,
  CSV, and report reconcile.

Each gate is reported as passed, failed, or pending.

**Freeing a busy device.** When the device is held by other software, the
documented instruction is to close that application normally, or to identify the
holder with `fuser -v /dev/bus/usb/*/*` or `lsof` and stop that specific process.
The README must not tell the operator to run a broad `pkill -f` pattern, which
can kill unrelated processes.

---

## 15. Limitations and forbidden claims

### 15.1 What this tool cannot do

- **It cannot find a transmitter's direction.** It compares reception quality
  between antenna orientations under the traffic that happened to occur.
- **Its SNR is conditional on detection, and the condition moves with the
  antenna.** This is the most important limitation in the document. A better
  angle hears transmissions a worse angle never registers, and those extra weak
  events lower its median. A higher `angle_score` can therefore mean "received
  better" *or* "heard only the loud ones". Detection yield (§9.5) is printed
  beside every score so the two can be told apart, and warning W6 says so out
  loud when the yield disagrees — but nothing in Phase 1 *removes* the
  conditioning. §10.0 states the estimand in full; §15.3 names the Phase 2
  change that would eliminate it.
- **It cannot match transmissions across angles.** Captures at two angles happen
  at different times, so the same transmission is never observed twice. The
  comparison is unmatched by construction and no pairing at the event level is
  possible. Warning W2 reports how much round coverage two angles actually
  shared, which is the most that can honestly be said about it.
- **It cannot identify a transmitter or an aircraft.** Carrier offset is a
  comparability diagnostic only (§9.2).
- **It cannot separate angle from time.** Alternating rounds reduce the linear
  component of a time trend; they do not remove non-linear traffic variation
  (§4.6).
- **It cannot measure below its squelch.** Nothing weaker than about 4.74 dB SNR
  at default settings is ever detected, so an angle may read "insufficient"
  because everything it heard was weak (§9.3).
- **It cannot rescue a fully occupied capture.** If traffic never keys down and
  is not a constant carrier, the probe floor is contaminated and the capture may
  be accepted with an overstated floor (§9.1).
- **It cannot correct for the operator.** Body position, cable routing, and
  height changes between angles enter the measurement as if they were the angle
  (§4.2).
- **It performs no statistical test and reaches no conclusion.** Decision Q1
  makes Phase 1 descriptive. The ranking shows which angle measured highest
  under the traffic that occurred; it carries no confidence statement, no
  p-value, and no claim that the ordering would repeat. Two angles whose spreads
  overlap are, as far as this tool is concerned, simply two measurements.
  Deciding is the reader's job, and §10.4 exists to tell them what would make
  that decision unsafe.
- **Its spreads are descriptive, not inferential.** The min–max and
  interquartile ranges beside each score say how much the contributing captures
  varied. They are not confidence intervals and must not be read as ones.
- **Warning W6 can fire when nothing is wrong.** It compares median yields
  against a fixed ratio without any uncertainty allowance, so uneven traffic
  between rounds can trip it even when the selection effect is absent (§9.5).
  It costs the reader a sentence of context rather than a conclusion, which is
  why a point comparison is proportionate for a warning.
- **It is not calibrated in absolute terms.** dBFS values are relative to the
  ADC full scale of this receiver at this gain, not to any physical field
  strength.
- **It reports host-side sample drops, not device overruns.** `librtlsdr`
  exposes no device-level overflow counter, so a true USB overflow reaches this
  program only as an unobservable discontinuity (§6.1).


### 15.2 Claims the tool must never make

No code path, report string, log line, or document may state or imply:

- that a result indicates the physical direction of a transmitter;
- **that any angle is best, correct, optimal, or a winner** — unconditionally,
  with no gate, threshold, or round count that would make it permissible
  (decision Q1);
- **that a ranking is statistically resolved, significant, or confirmed**, or
  that any difference between angles has been tested;
- that a spread printed beside a score is a confidence interval;
- that a higher `angle_score` means an angle receives better, without the
  detection-yield qualification of §10.0;
- that a carrier offset identifies a specific transmitter or aircraft;
- that alternating visit order removes the angle–time confound;
- that `host_dropped_samples` counts device or USB overruns;
- that `O_NOFOLLOW` protects an intermediate path component (§13.2);
- that a label sanitiser guarantees anything §13.3 does not state;
- that hardware validation passed when the hardware test was skipped;
- that any figure in §3 was verified on hardware that was not present;
- that this document or the plan is approved for implementation before an
  independent revalidation of this revision has passed.

The second and third entries were conditional in revision 3 — an angle could be
called best "once the resolution gates have passed". Decision Q1 removed the
gates, so the prohibition is now absolute, and §14.2 Test K enforces it by
grepping the rendered output rather than by argument.

The last entry has been **discharged**: revision 4 passed an independent
revalidation on 2026-08-19, which is what the `Status` line of this document and
of the plan records. The prohibition itself stands for any later revision, which
must earn its own revalidation before either document may claim approval again.


### 15.3 Deferred to Phase 2

- **Continuous-reference measurement mode** — the change that would remove the
  detection-conditioning of §10.0 rather than merely disclosing it, and the only
  route by which a future phase could honestly name a best angle. Against a
  continuously transmitting station (ATIS, VOLMET, VOR, or a broadcast FM
  carrier) there is no squelch, no event detection, and no traffic variability:
  the measurement becomes a carrier-to-noise ratio against an out-of-band noise
  reference, matched across angles. It bypasses §9.1's two-stage floor and
  §9.2's event detector, so it is a second measurement path with its own test
  family, not a flag on the existing one.
- Raw `.cu8` recording behind a byte-preserving source contract, with the
  sidecar of §7.5 written rather than only read.
- Servo control via a `SerialServoAngleProvider`.
- Cross-session comparison with a stable reference transmitter.
- Automatic gain-range selection at session start.
- Multi-channel simultaneous measurement.
- An append-only journal with periodic compaction, if the §7.7 attempt ceiling
  ever needs raising above what full-document rewriting supports (§11.1).

---

## 16. Deliverables

Source and CMake configuration for the four targets of §5.1; the test suite of
§14; `README.md` covering dependencies, build, run, the physical protocol of §4,
the limitations of §15, and worked CLI examples; `docs/architecture.md`; and a
committed example synthetic session directory showing real output shapes,
generated by the tool rather than written by hand.
