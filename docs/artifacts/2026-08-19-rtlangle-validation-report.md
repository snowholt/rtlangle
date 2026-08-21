# Artifact 2 — rtlangle Phase 1 Validation Report

Date: 2026-08-19  
Repository head reviewed: `5cbc412dad4166f26114d9adfdac481f4360ff7d`  
Recommendation: **Reworked before implementation**  
Assessment confidence: **93/100**

## Executive assessment

The plan has a sound product boundary, a generally sensible C++ architecture, the correct linear-domain CNR formula, unusually explicit reproducibility metadata, and a strong test-first intent. Those qualities make it worth preserving.

It is not implementation-ready. The proposed noise-floor reliability algorithm contradicts its own tests and can mistake a high-occupancy capture for quiet noise; event-level pooling overstates statistical confidence; the FFTW API and linked library use different precisions; the hardware test can pass without testing hardware; several CLI fields and the scan retuning contract do not exist; retries can contaminate final rankings; audio windows are not actually aligned after filtering; and `--save-iq` has no implementation path.

The correct decision is therefore **Reworked before implementation**, not “approved with revisions.” The required changes affect measurement validity, buildability, state semantics, and release evidence rather than wording alone.

## Evidence and uncertainty

Verified directly:

- The clean `main` branch contains only the 745-line design and 4,051-line implementation plan; there is no source, build system, or executable to test.
- The three commits are `36c864e`, `8a8ae93`, and `5cbc412`; the current worktree was clean before this report added artifacts.
- The machine reports Ubuntu 24.04.4, GCC 13.3.0, CMake 3.28.3, Ninja 1.11.1, FFTW 3.3.10, and `librtlsdr` pkg-config version `0.6.0-128-g240b` from `/usr/local`.
- `pkg-config fftw3` links `-lfftw3`, which exports `fftw_plan_dft_1d`; the plan calls `fftwf_plan_dft_1d`, which is exported by `-lfftw3f`. The current CMake fragment would therefore link the wrong library.
- No RTL-SDR USB device and no SDR++ process were visible to the inspection commands in this environment. This does not prove absence from the physical machine; hardware status remains unverified.

Not verified:

- Any proposed compilation, unit test, DSP tolerance, terminal behavior, resume path, scan behavior, real reception path, or generated example output.
- The claimed current SDR++ PID. It was not present during this revalidation and must not be copied into a handoff as current fact.
- Real-world measurement repeatability or the ability of carrier-offset diagnostics to distinguish transmitters.

## Strongest reasons the plan is sound

1. **The product claim is bounded.** It repeatedly limits the result to measured reception quality and rejects transmitter-direction inference. That is the right scientific and product boundary.
2. **The primary channel metric is defined correctly.** `(P_event - N) / N` is computed before conversion to dB, and the 0 dB negative control is designed to catch the common `P/N` error.
3. **Offset tuning and fixed gain address real SDR confounders.** Avoiding the center DC spur, reading back applied gain, defaulting AGC off, recording PPM and seed, and warning about overruns all improve comparability.
4. **The separation of hardware, UI, angle input, and experiment logic is strategically useful.** Synthetic and scripted implementations make most behavior testable without a dongle and leave a credible seam for future servo control.
5. **Persistence is treated as experimental evidence, not incidental output.** Fully resolved configuration, source metadata, per-event records, atomic JSON intent, and resume checks are the correct categories of information.
6. **The test plan includes meaningful negative controls.** Pure noise, the forbidden `P/N` metric, filter rejection, quit/resume, malformed input, and hardware-pending wording are better than happy-path-only coverage.
7. **The plan acknowledges major limitations.** Squelch detection limits, intermittent traffic, actual-angle uncertainty, temporal confounding, gain changes, noise drift, and confidence overlap are explicitly surfaced.

## Section scores

TF = technical feasibility; CO = completeness; SR = security and reliability; SM = scalability and maintainability; BV = business value; IR = implementation readiness.

| Major section | TF | CO | SR | SM | BV | IR |
|---|---:|---:|---:|---:|---:|---:|
| 1. Purpose, scope, and safety | 9 | 7 | 8 | 8 | 8 | 7 |
| 2. Architecture, build, and dependencies | 6 | 6 | 6 | 8 | 7 | 5 |
| 3. Angle protocol and experiment design | 8 | 5 | 6 | 7 | 6 | 5 |
| 4. DSP chain | 7 | 6 | 5 | 8 | 7 | 5 |
| 5. Metrics, statistics, and ranking | 5 | 4 | 3 | 6 | 6 | 3 |
| 6. Sources, capture, and hardware | 7 | 5 | 4 | 7 | 8 | 4 |
| 7. Persistence and resume | 8 | 6 | 5 | 6 | 8 | 5 |
| 8. Terminal UI, CLI, and scan | 7 | 5 | 5 | 7 | 8 | 4 |
| 9. Testing and verification | 7 | 6 | 5 | 7 | 8 | 4 |
| 10. Delivery and execution strategy | 8 | 6 | 6 | 7 | 8 | 5 |

Unweighted mean: **6.2/10**. Measurement-validity and implementation-readiness dimensions are more important than the unweighted mean suggests; the release decision is driven by the P0 gates below.

## Score justifications

### 1. Purpose, scope, and safety

- **TF 9:** A receive-only, manual-angle CLI is achievable with the verified compiler and SDR libraries.
- **CO 7:** Non-goals, source types, reports, and warnings are defined, but success criteria, physical angle reference, resource limits, and legal/jurisdictional guidance are absent.
- **SR 8:** Bias tee is opt-in and AGC is guarded; however, interactive bias-tee confirmation and clipping gates are missing.
- **SM 8:** Phase 1 is intentionally bounded and has future seams; `--save-iq` and scan scope weaken that boundary.
- **BV 8:** It can answer a useful personal engineering question without GUI/servo cost, provided the result stays descriptive.
- **IR 7:** The goal is clear enough to start refinement, but the document’s “Approved for implementation” status is premature.

### 2. Architecture, build, and dependencies

- **TF 6:** The layered C++ design is viable, but `fftwf_*` plus `-lfftw3` is a verified link mismatch.
- **CO 6:** File boundaries are detailed, yet scan retuning, main orchestration, no-hardware compilation details, and an actual storage interface are incomplete.
- **SR 6:** Hash-pinned fallbacks and warnings help; sanitizer/CI gates, dependency refresh policy, and offline-build behavior are not planned.
- **SM 8:** Small value-oriented modules and test doubles should age well.
- **BV 7:** The architecture supports hardware-free iteration and later servo integration, though it is somewhat elaborate for a personal Phase 1 CLI.
- **IR 5:** Production `SyntheticSource` is said to consume `tests/support`, the executable task number is inconsistent, and the named external execution skill is unavailable here.

### 3. Angle protocol and experiment design

- **TF 8:** Manual positioning and deterministic visit plans are straightforward.
- **CO 5:** The physical 0° reference, rotation axis, feedline/operator placement, circular statistics, and per-round acceptance rules are missing.
- **SR 6:** Deviation confirmation prevents typos, but actual-angle input lacks explicit finite/range validation and the default single round permits a known confound.
- **SM 7:** `IAngleProvider` supports future automation, although settling inside the provider and a misleading retry label blur responsibilities.
- **BV 6:** The protocol produces exploratory comparisons; intermittent unrelated aircraft traffic limits decision quality.
- **IR 5:** The spec’s endpoint-inclusion text conflicts with the plan’s `0,20,40,60,80` test, and “breaks correlation” conflicts with the later “reduces confounding” wording.

### 4. DSP chain

- **TF 7:** Offset mixing, staged decimation, channel filtering, envelope detection, and Welch spectra are conventional and feasible.
- **CO 6:** Filter construction, state, and tests are described, but latency, transient trimming, exact passband/stopband semantics, and clipping are not.
- **SR 5:** Audio and channel vectors having equal length does not make them time-aligned after FIR filtering; short-event audio SNR can be biased.
- **SM 8:** Stateless builders and chunk-invariant stateful components are a maintainable structure.
- **BV 7:** The chain directly supports the intended metric and scan diagnostics.
- **IR 5:** `M_PI` contradicts the portable C++20 constraint, fractional NCO rounding is not represented in records, and the FFTW linkage is wrong.

### 5. Metrics, statistics, and ranking

- **TF 5:** The core CNR estimator is feasible, but the reliability and inference approach needs redesign.
- **CO 4:** No independent-capture analysis, minimum effect size, multiple-attempt rule, or defensible resolved-winner policy is specified.
- **SR 3:** The 20th-percentile occupancy check is circular and fails its own 90%-active example; unreliable retries may be pooled; event-level confidence intervals assume independence that the experiment does not provide.
- **SM 6:** Explicit records and helper functions are good, but statistics are coupled to an invalid experimental unit.
- **BV 6:** Descriptive rankings are useful; overstated confidence or an operator-selected “winner” would reduce trust.
- **IR 3:** The all-active test cannot pass with the proposed algorithm, and the promised absolute ±2 dB audio test was reduced to monotonicity.

### 6. Sources, capture, and hardware

- **TF 7:** librtlsdr, the Blog V4 API, and single-precision FFTW are installed locally; async capture is achievable.
- **CO 5:** Timeouts, cancellation, worker error propagation, scan retuning, readback of all applied settings, and raw-IQ flow are absent.
- **SR 4:** A blocked `read()` can hang, overrun semantics are not defined per capture, and the controller pseudocode appears to pass `now` after capture as the capture start.
- **SM 7:** Source interfaces and synthetic/file doubles are a strong base, but scan needs a separate tunable contract.
- **BV 8:** Named busy/device errors, gain tables, and scan support directly help the operator complete an experiment.
- **IR 4:** The hardware test returns early and stays green on no device or open failure; no device was verified in this review.

### 7. Persistence and resume

- **TF 8:** Atomic replace plus file/directory `fsync` is implementable on the target Linux platform.
- **CO 6:** JSON and CSV shapes are extensive, but attempt disposition, schema migration, collision handling, and recovery reconciliation are missing.
- **SR 5:** JSON may commit while CSV does not; predictable `.tmp`, mode 0644, unsanitized labels, and symlink/control-character handling are weak.
- **SM 6:** Rewriting the whole JSON is acceptable for normal sessions but becomes O(n²), and rounds/duration are unbounded.
- **BV 8:** Crash-resume and auditable configuration materially increase the usefulness of long experiments.
- **IR 5:** `finished_utc` is a string although the schema shows `null`, and failed/retried visits do not have a coherent persisted state.

### 8. Terminal UI, CLI, and scan

- **TF 7:** POSIX terminal handling and a wideband activity scan are feasible.
- **CO 5:** `resume_dir`, scan fields, scan exposure accounting, retune API, and end-to-end `main` wiring are missing.
- **SR 5:** The proposed signal handler can retain a pointer to expired stack state; non-TTY raw mode and terminal-control injection are not handled.
- **SM 7:** `ITerminalUi` plus scripted UI is a clean test seam.
- **BV 8:** Guided menus, named errors, scan, and copyable commands reduce operator friction.
- **IR 4:** The documented piped menu check conflicts with raw-mode assumptions, and the README proposes the overly broad `pkill -f sdrpp` command.

### 9. Testing and verification

- **TF 7:** Most planned tests are executable without hardware and use deterministic seeds.
- **CO 6:** Coverage is broad but lacks failure injection, latency checks, clipping, timeout/cancellation, pseudo-terminal signals, scan-edge accounting, and command-level orchestration.
- **SR 5:** Negative controls are good; the green-but-pending hardware design and impossible occupancy test make the suite misleading.
- **SM 7:** Per-module tests and end-to-end fixtures support refactoring, though one monolithic doctest executable makes CTest labels unsafe.
- **BV 8:** Reliable automation would substantially lower the risk of plausible-looking RF mistakes.
- **IR 4:** No test has run because no implementation exists, and the current CTest layout does not truly exclude hardware cases.

### 10. Delivery and execution strategy

- **TF 8:** Incremental TDD commits are practical.
- **CO 6:** Eighteen tasks cover most features but omit a pre-code reconciliation gate, CI, operational failure handling, and explicit acceptance ownership.
- **SR 6:** Every task calls for green tests, but there are no stop conditions for invalid statistical assumptions beyond estimator tolerances.
- **SM 7:** Small commits and layer order are sensible; a fresh subagent per task can create consistency risk without a stable contract and integration owner.
- **BV 8:** Delivering synthetic-first and hardware-last minimizes wasted hardware debugging.
- **IR 5:** Executing the current 4,051-line plan would encode known contradictions; the revised plan should replace it before task dispatch.

## Gap and action register

Effort: S = up to 1 day, M = 2–4 days, L = 1–2 weeks. Impact reflects the consequence if left unresolved.

| ID | Gap and evidence | Priority | Effort | Impact | Owner | Recommended next action |
|---|---|---|---|---|---|---|
| G1 | Noise reliability uses the same 20th percentile it is trying to validate; the plan’s 90%-active vector yields 0% detected activity. | P0 | M | Invalid SNR can look plausible. | DSP engineer | Implement and Monte Carlo-test the two-stage probe floor plus persistent-carrier guard in Artifact 1 §6.3. |
| G2 | Events are treated as independent replicates although they share captures, time, propagation, and possibly transmitters. | P0 | L | Confidence is overstated and winners may be false. | Statistics reviewer | Make capture medians the primary experimental unit and add paired/blocked top-angle inference. |
| G3 | `fftwf_*` calls are linked only to `fftw3`; local symbols confirm they require `fftw3f`. | P0 | S | Link failure. | Build owner | Rename the pkg-config target to FFTW3F and add a clean link smoke test. |
| G4 | `resume_dir`, scan options, retuning, and full `main` orchestration are used but not defined. | P0 | M | CLI cannot compile or scan/run correctly. | Application lead | Complete Config/ScanConfig, add `ITunableSampleSource`, and specify command-level wiring before feature code. |
| G5 | The hardware doctest returns early on absence/busy, so CTest reports success; CTest labels apply to executables, not doctest cases. | P0 | S | False release evidence. | Test owner | Create a separate hardware-test binary with explicit pass/skip/fail exit semantics. |
| G6 | A failed capture is appended before a retry; the summary can pool its events with the later successful capture. | P0 | M | Unreliable data contaminates ranking and resume. | Reliability engineer | Add visit/attempt IDs and dispositions; rank only accepted attempts and persist retry intent. |
| G7 | Equal-length channel/audio arrays ignore audio filter group delay and transients. | P0 | M | Short-event audio SNR is biased. | DSP engineer | Expose latency, shift guarded event windows, and add impulse plus short-burst alignment tests. |
| G8 | Angle endpoint rules, alternating-order claims, and mean-angle arithmetic contradict or omit circular geometry. | P0 | S | Different implementers produce different experiments. | Spec owner | Adopt Artifact 1 Gate 0 wording and circular distance/mean tests. |
| G9 | User metric selection can create `best_angle` despite metric disagreement or unresolved top intervals. | P0 | M | A preference is presented as evidence. | Product and statistics owners | Separate selected ranking from resolved winner and enforce data/effect-size gates. |
| G10 | JSON is committed before a non-fsynced CSV append; a crash can leave them inconsistent. | P1 | M | Audit exports disagree after failure. | Persistence owner | Make JSON canonical, atomically regenerate CSV, reconcile on reopen, and fault-inject each boundary. |
| G11 | Capture reads have no timeout/error wakeup; timestamps and overruns are not clearly capture-local. | P1 | M | Hangs and misleading records. | Hardware engineer | Return structured read status, add cancellation/deadlines, timestamp before first read, and record overrun deltas. |
| G12 | `--save-iq` is specified but no task carries original raw bytes into `raw/`; normalized samples cannot reproduce exact `.cu8`. | P0 | S | Promised feature is missing or lossy. | Product owner | Remove it from Phase 1, or fund a byte-preserving source/storage contract as a separate work package. |
| G13 | Production `SyntheticSource` is described as consuming helpers under `tests/support`. | P0 | S | Wrong dependency direction or duplicate code. | C++ lead | Move reusable synthesis into `src/source` and keep only assertions/fixtures under tests. |
| G14 | Session labels, file paths, USB strings, and notes can contain traversal or terminal-control characters; files are mode 0644. | P1 | M | Local overwrite, disclosure, or terminal spoofing. | Security reviewer | Slug labels, reject symlinks/control paths, use 0700/0600, cap input sizes, and escape only at rendering. |
| G15 | Signal handling stores a pointer to RawMode state and does not define non-TTY behavior. | P1 | M | Broken shell or undefined behavior after SIGINT. | UI owner | Use stable handler state, clear it on teardown, handle pipes, and test in a pseudo-terminal. |
| G16 | Duration and rounds have no upper bound; raw recording would use about 123 MB per default 60-second capture and about 1.72 GB for 14 captures. | P1 | S | Excess runtime or disk exhaustion. | Product/application owner | Add bounds, session-time/storage estimates, free-space preflight, and long-run confirmation. |
| G17 | Scan does not define retune/reset behavior, per-channel exposure denominators, or band-edge coverage. | P1 | M | Activity percentages can be wrong or scan cannot be implemented. | RF/application engineer | Build a retunable-source scan fixture and test every channel’s coverage/exclusion count. |
| G18 | Filter “cutoff” semantics, NCO rounding metadata, clipping, and startup transient removal are underspecified. | P1 | M | Systematic measurement bias. | DSP engineer | Define passband/stopband edges, record applied offset, expose latency, and add clipping/transient gates. |
| G19 | Hardware, native terminal behavior, and a real repeated-angle session remain unexecuted. | P0 release gate | L | Production readiness is unknown. | Hardware QA/operator | Run the separate hardware test and a protocol-controlled session only after all synthetic gates pass; retain exact outputs. |

## Dependencies and sequencing risks

- Metrics depend on settled DSP latency and noise reliability; implementing ranking before those contracts are fixed creates rework.
- Resume depends on accepted-attempt semantics; implementing current `completed_keys` first bakes in the wrong state model.
- Scan depends on a retunable hardware contract absent from `ISampleSource`.
- Main orchestration depends on a complete configuration schema and canonical persistence behavior.
- Hardware evidence depends on the device being free and visible. A busy or absent device is an external dependency and must remain `pending`.
- The user-guided protocol depends on a stable reference transmitter or enough balanced repeated captures. Software alone cannot remove traffic and propagation variability.

## Final recommendation

**Reworked before implementation.** Preserve the bounded product goal, interfaces, correct CNR arithmetic, reproducibility metadata, and test-first structure. Replace the invalid reliability/statistical design, close the verified build and contract gaps, and make hardware status machine-readable before assigning implementation tasks.

Overall assessment confidence is **93/100** because the findings are tied directly to the committed documents and locally verified tool/library behavior. The remaining uncertainty is substantial only where no implementation or hardware run exists.
