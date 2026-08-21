# Artifact 1 — rtlangle Phase 1 Revalidated Plan

Date: 2026-08-19  
Status: Reworked draft; implementation may start only after Gate 0 is accepted  
Scope: Receive-only C++20 CLI for comparative antenna-orientation experiments with manual positioning

## 1. Outcome and acceptance boundary

`rtlangle` will capture AM airband IQ at a controlled tuner configuration, calculate channel and post-demodulation quality metrics, and produce a descriptive ranking of planned antenna orientations.

The product may say **“highest measured reception quality under this experiment.”** It must not infer transmitter direction, identify an aircraft from carrier offset, or call an angle a statistically resolved winner unless the resolution gates in section 7 pass.

Phase 1 is complete only when:

1. A clean hardware-free build and test run passes with RTL-SDR support both enabled and disabled.
2. Dependency linkage, including single-precision FFTW, is verified from a clean build.
3. Synthetic tests cover the full DSP chain, high-occupancy captures, constant carriers, pure noise, short events, audio alignment, clipping, and retry/resume behavior.
4. A hardware test produces a machine-readable result of `passed`, `skipped`, or `failed`; `skipped` cannot be reported as passed.
5. At least one real receive session completes without overruns or clipping and its JSON, CSV, and report reconcile.
6. Every documented command has been executed, and unexecuted hardware or operator validation is explicitly marked pending.

## 2. Explicit Phase 1 scope

Included:

- Manual planned-angle experiments over one selected frequency.
- Synthetic and `.cu8` replay sources.
- RTL-SDR Blog V4 receive support with fixed manual gain by default.
- A dedicated retunable scan path for finding active 25 kHz channels.
- Channel CNR-style SNR, post-demodulation audio SNR, per-capture diagnostics, descriptive rankings, uncertainty warnings, resume, and professional text reports.

Excluded:

- GUI, servo/ESP32 control, transmitter-direction estimation, transmitter or aircraft identification, denoising, and automatic conclusions from unresolved data.
- Raw-IQ recording. Remove `--save-iq` from Phase 1 rather than ship an unimplemented or lossy path. Reintroduce it only with a byte-preserving source contract, storage preflight, and retention controls.

## 3. Gate 0 — reconcile requirements before code

Owner: Product owner with DSP lead  
Effort: 0.5–1 day  
Exit criterion: the design specification and this plan contain no contradictory normative rules.

Required decisions and edits:

- Define generated angles as `start + i*step` while the result is `<= end + 1e-9`; do not append a non-grid endpoint. Thus `0:20:90` produces `0,20,40,60,80`.
- Use circular angular distance `min(|a-b|, 360-|a-b|)` and circular means. Define 0° physically: viewing direction, rotation axis, which dipole element is referenced, antenna height, feedline routing, and operator position.
- Set the guided experiment default to two alternating rounds. Describe this as balancing a linear time trend and reducing—not eliminating—time and traffic confounding.
- Separate `selected_metric` from `resolved_best_angle`. An operator may choose which ranking to inspect at report time, but that choice cannot override data-quality or uncertainty gates.
- Make `session.json` the canonical record. `measurements.csv` is a derived export that the loader can regenerate.
- Remove claims that carrier offset identifies an aircraft. It is only a comparability diagnostic.
- Replace the claimed “four interfaces” with the actual boundary: `ISampleSource`, `ITunableSampleSource`, `IAngleProvider`, `ITerminalUi`, and `ISessionStore`.

## 4. Architecture and dependency baseline

Owner: C++ platform engineer  
Depends on: Gate 0

### 4.1 Targets

- `rtlangle_core`: core values, DSP, metrics, sources, persistence, UI abstractions, and experiment orchestration.
- `rtlangle`: CLI and interactive application wiring.
- `rtlangle_tests`: hardware-free unit and integration tests.
- `rtlangle_hardware_test`: separate binary registered as CTest label `hardware`.

The production synthetic generator belongs under `src/source/`; tests may call it, but production code must never depend on `tests/support/`.

### 4.2 Dependencies

- C++20 and CMake 3.24+.
- Link FFTW single precision consistently: `pkg_check_modules(FFTW3F REQUIRED IMPORTED_TARGET fftw3f)`, `PkgConfig::FFTW3F`, and `fftwf_*` APIs. Alternatively convert the implementation wholly to `fftw_*`; never mix them.
- Use `std::numbers::pi_v<double>` instead of non-standard `M_PI`.
- Keep nlohmann/json and doctest pinned by checksum. Record resolved versions in configure output.
- When RTL-SDR support is enabled, record the pkg-config version and actual source metadata. The current machine resolves `librtlsdr` from `/usr/local`; do not assume a runtime version API exists.

### 4.3 Build quality gates

- Warnings: `-Wall -Wextra -Wpedantic -Wconversion -Wshadow` for project code.
- CI matrix: GCC and Clang; RTL-SDR on/off; Debug plus ASan/UBSan; release build.
- Network-free path: document required system packages or provide a controlled dependency cache.
- No dependency on an unavailable “superpowers” skill; task execution must work inline or through whatever reviewed workflow is actually available.

## 5. Configuration and resource safety

Owner: Application engineer

Create a complete `Config` before any feature consumes it. It includes all fields used by the CLI, including `resume_dir` and either a nested `ScanConfig` or `scan_dwell_ms`/`scan_passes`.

Validation must cover finiteness and bounds for every numeric field, actual angles in `[0,360]`, source syntax, file existence/type, channel/filter constraints, and session cost. Recommended limits:

- `duration_s`: `(0, 3600]`
- `rounds`: `[1,100]`
- angles: `[2,180]`
- total planned capture time and estimated storage shown before start; require explicit confirmation above one hour.

Configuration precedence remains defaults < JSON file < CLI < interactive edits. A resume uses stored experiment-defining fields and rejects conflicting overrides with a reason. Cosmetic output options may change on resume.

## 6. DSP and measurement pipeline

Owner: DSP engineer  
Depends on: sections 4–5

### 6.1 Signal chain

1. Tune to `center_hz + applied_offset_hz`.
2. Convert unsigned IQ using one documented convention, tested at 0, midpoint, and 255.
3. Mix the wanted signal to DC with an integer phase accumulator.
4. Decimate in validated stages whose anti-alias passband and stopband are defined explicitly.
5. Apply a channel filter with separate passband edge and stopband edge; the documented ±4 kHz band must not use a −6 dB “cutoff” ambiguously.
6. Demodulate AM and filter audio to 300–3400 Hz.

Every stateful filter exposes reset behavior and output latency. Discard startup transients, compensate the FIR group delay when mapping event windows to audio, and apply a documented guard interval to short events. Store the applied integer NCO offset if a requested fractional offset is rounded.

### 6.2 Framing and event detection

- Channel frames: 1024 samples, hop 512 at the effective channel rate.
- Event detection remains hysteretic and merges gaps before duration filtering.
- Validate event boundaries at capture start/end and after audio-delay compensation.

### 6.3 Noise-floor reliability

Do not use the configured 20th-percentile floor to decide whether enough quiet frames existed; that test is circular when quiet occupancy is below 20%.

Use a two-stage estimator:

1. Compute a probe floor at `min(5, noise_percentile)` percent and derive `probe_active_fraction` from it.
2. If `probe_active_fraction > max_active_fraction`, mark the capture `noise_floor_unreliable` and do not emit SNR.
3. Otherwise compute the reported noise floor at the configured percentile.
4. If no events are detected but a persistent spectral carrier is prominent above the broadband median by more than `open_db`, mark the floor unidentifiable rather than treating the carrier as noise.

Monte Carlo tests must demonstrate that pure noise is not falsely flagged, 90% activity is flagged, a constant strong carrier cannot yield a fabricated SNR, and configured SNR sweeps remain within tolerance.

### 6.4 SNR definitions

For each valid event:

```text
P_event = median in-channel frame power over the guarded event window
SNR_lin = (P_event - N) / N
SNR_dB  = 10 log10(SNR_lin)
```

The same subtraction rule applies to audio power. Values at or below the noise estimate are invalid, not floored into plausible numbers. Retain the 0 dB negative-control test that distinguishes this from `P_event/N`.

Add an absolute audio-SNR reference test with separated signal-only and noise-only streams; monotonicity alone is insufficient.

## 7. Experimental unit, aggregation, and decision policy

Owner: DSP/statistics reviewer  
Depends on: section 6

An event is an observation inside a capture, not an independent experimental replicate. The primary per-angle score is therefore built in two levels:

1. Compute an event distribution and median within each accepted capture.
2. Aggregate the per-capture medians across rounds for each planned angle, giving every capture one vote.

Failed, unreliable, or superseded retry attempts remain in the audit record but are excluded from ranking. Do not pool their events into a later successful attempt.

Reports always show descriptive channel and audio rankings. A `resolved_best_angle` may be set only when:

- every ranked angle meets the minimum accepted-capture count;
- the top two angles are compared on the selected metric using a paired/blocked interval when the visit design permits it;
- the 95% interval for the top-minus-second difference excludes zero;
- the estimated difference is at least `min_effect_db` (default 1 dB);
- no relevant clipping, overrun, unreliable-floor, gain-change, or severe angle-deviation gate failed.

With fewer than four accepted captures per angle, label the ranking exploratory and do not claim statistical resolution. Store `selected_metric`, `ranking_channel`, `ranking_audio`, `resolved_best_angle`, `resolution_method`, and `resolution_reason` separately.

## 8. Sources, capture, and hardware

Owner: Hardware integration engineer

### 8.1 Source contracts

`ISampleSource::read` returns a structured result: sample count, timeout/end-of-file/error status, source error text, and current overrun count. Reads have a finite timeout and support cancellation. A worker-thread failure must wake blocked readers.

`ITunableSampleSource` adds `retune(center_hz)` and buffer reset for the scan command; scanning must not depend on down-casting an experiment source.

`CaptureRunner` records the wall-clock start before the first read, captures exactly the requested number of channel samples after transient trimming, calculates per-capture—not cumulative—overruns, and fails on timeout, source error, clipping above a configured fraction, or insufficient samples.

### 8.2 RTL-SDR opening

Keep fixed manual tuner gain and digital AGC off unless the explicit invalidating override is accepted. Check every library return code, read back sample rate, center frequency, and tuner gain, and record requested versus applied values.

Bias tee remains off unless explicitly requested. An enable warning must require confirmation in interactive mode.

### 8.3 Hardware test semantics

Build hardware testing as a separate executable. Use distinct outcomes:

- exit 0: hardware receive path passed;
- CTest skip return code: no device or device busy, validation pending;
- nonzero: device opened but the receive-path assertion failed.

The completion report must parse this outcome, not infer success from a green aggregate CTest run.

## 9. Persistence, resume, and security hardening

Owner: Application/reliability engineer

- Create session directories with mode 0700 and files with mode 0600.
- Sanitize labels to a conservative filename slug; reject traversal and control characters.
- Reject symlink targets and session-file sizes above a documented limit.
- Use an optional value for `finished_utc`; serialize it as JSON `null` until finalization.
- Treat `session.json` as canonical. Write it atomically with file and directory `fsync`.
- Regenerate `measurements.csv` atomically from canonical JSON after each accepted state transition and automatically reconcile it on reopen.
- Validate `schema_version`; reject newer schemas and migrate older supported schemas explicitly.
- Give every visit a stable `visit_id` and every attempt an `attempt` number plus `accepted`, `superseded`, or `abandoned` disposition.
- Persist the intent to retry before returning to capture so a crash resumes the correct step.
- Escape CSV per RFC 4180 and sanitize control sequences only at display time, not by mutating canonical notes.

Fault-injection tests must interrupt before/after temp write, rename, JSON commit, CSV export, retry decision, and finalization.

## 10. Terminal, CLI, scan, and application wiring

Owner: Application engineer

- The terminal UI supports TTY and non-TTY input without applying `termios` to a pipe.
- Keep stable signal-state storage; never leave a handler pointing at a destroyed stack object. Test SIGINT during a menu in a pseudo-terminal.
- Escape or visibly encode control characters from labels, notes, paths, and USB descriptors before terminal rendering.
- Rename “Retry the previous angle” to “Retry this angle” unless true backtracking is implemented.
- Report metric disagreement before asking which ranking to inspect. Preserve unresolved scientific status after the choice.

The scan implementation owns a retunable hardware source, resets the device buffer after each retune, tracks per-channel exposure counts, excludes DC-contaminated observations, and divides activity by actual exposures. Test frequency-edge coverage, retune failure, DC exclusion, and channel denominators.

`main` must have an integration-tested orchestration path for new, resume, synthetic, scan, and devices commands: validate → open source → create/reopen session → build provider → run controller → summarize → optional metric selection → render → finalize → return documented exit code.

## 11. Implementation sequence

| Order | Work package | Depends on | Exit evidence |
|---:|---|---|---|
| 0 | Reconcile spec and acceptance rules | — | Approved Gate 0 diff |
| 1 | CMake, dependencies, CI, decibel/statistics primitives | 0 | Clean on/off builds; FFTW symbol linked |
| 2 | Complete config, angle math, schemas | 1 | Round trips, boundary tests, schema rejection |
| 3 | FIR/NCO/decimation/channel/audio chain | 2 | Frequency-response, latency, chunk-invariance tests |
| 4 | Two-stage noise estimation, detection, SNR | 3 | Monte Carlo and absolute reference tests |
| 5 | Synthetic/file sources and timeout-aware capture | 4 | Deterministic capture tests |
| 6 | RTL-SDR and retunable scan source | 5 | Separate passed/skipped/failed hardware result |
| 7 | Canonical persistence, attempts, resume | 2 | Crash/fault-injection and reconciliation tests |
| 8 | UI, angle providers, protocol prompts | 2 | Scripted and pseudo-terminal tests |
| 9 | Controller and accepted-attempt state machine | 5,7,8 | Retry/quit/crash/resume integration tests |
| 10 | Hierarchical summary and report policy | 4,9 | Resolved/unresolved decision tests |
| 11 | CLI, main orchestration, scan | 6,9,10 | Command-level integration tests |
| 12 | Hardware session, documentation, example | 11 | Recorded commands and artifact reconciliation |

Each work package lands only after its relevant tests pass. A task may be split into smaller commits; no task may weaken a tolerance or convert a failed hardware check into a pass.

## 12. Final release checklist

Run from a clean worktree and retain full outputs:

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure -LE hardware
ctest --test-dir build --output-on-failure -L hardware

cmake -S . -B build-nohw -G Ninja -DRTLANGLE_WITH_RTLSDR=OFF
cmake --build build-nohw
ctest --test-dir build-nohw --output-on-failure

git diff --check
git status --short
```

Also run the sanitizer matrix, one synthetic CLI session, one quit/resume session, scan coverage tests, and—when a free device is available—one real receive session. The release note states each gate as passed, failed, or pending.
