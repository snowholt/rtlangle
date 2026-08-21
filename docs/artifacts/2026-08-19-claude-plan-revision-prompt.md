# Claude Prompt — Revise the rtlangle Phase 1 Specification and Plan

You are revising an existing C++/RTL-SDR project plan. Treat all earlier completion and approval claims as hypotheses. Revalidate the repository state and use the committed documents plus the independent validation artifacts as evidence.

## Repository and evidence

Work in:

```text
/home/snowholt/coding/cpp/rtlSDR
```

Read these files completely before proposing edits:

```text
docs/superpowers/specs/2026-08-19-rtl-sdr-antenna-angle-design.md
docs/superpowers/plans/2026-08-19-rtlangle-phase1.md
docs/artifacts/2026-08-19-rtlangle-revalidated-plan.md
docs/artifacts/2026-08-19-rtlangle-validation-report.md
```

Also inspect:

```bash
git status --short --branch
git log --oneline --decorate -8
```

Current review baseline: commit `5cbc412dad4166f26114d9adfdac481f4360ff7d`. Verify that rather than assuming it remains current.

There is currently no implementation. This task revises planning documents only. Do not create source code, build files, tests, generated sessions, commits, branches, or pull requests.

## Phase 1 — ask Nina for material product decisions

Before editing anything, ask Nina for one compact batch of decisions. Use a table with columns `ID`, `Decision`, `Options`, `Recommendation`, and `Tradeoff`. Make each option selectable by a short code. Wait for her response before revising the files.

Ask exactly these questions unless the repository already contains a newer explicit answer:

### Q1 — What strength of conclusion should Phase 1 support?

- `Q1-A` — Descriptive rankings only. Always show measured rankings, but never declare a statistically resolved winner. Lowest complexity and most scientifically conservative.
- `Q1-B` — Descriptive rankings plus a resolved winner only when capture-level confidence, minimum-effect, and data-quality gates pass. **Recommended.** More useful without presenting preference as evidence.
- `Q1-C` — Always name the top angle for the operator-selected metric. Fastest to understand but scientifically weak; do not recommend.

### Q2 — What should happen to raw-IQ recording?

- `Q2-A` — Remove `--save-iq` from Phase 1 and defer exact raw recording to Phase 2. **Recommended.** Keeps the source contract and storage scope controlled.
- `Q2-B` — Implement byte-preserving `.cu8` capture in Phase 1, with disk preflight, permissions, retention guidance, and tests. Higher effort and storage use.
- `Q2-C` — Save normalized complex-float samples under a differently named flag and format. Easier, but not byte-identical to hardware input.

### Q3 — What guided experiment should be the default?

- `Q3-A` — Two alternating rounds. **Recommended for Phase 1.** About 16 minutes of capture for seven 60-second angles, plus positioning; balances a linear time trend but does not eliminate traffic variability.
- `Q3-B` — Four balanced/randomized rounds. Stronger uncertainty evidence, roughly twice the experiment time.
- `Q3-C` — One forward round. Fast exploratory mode only; must never produce a resolved winner.

### Q4 — Is the airband scan part of the Phase 1 release gate?

- `Q4-A` — Keep scan in Phase 1 and add a real retunable-source contract, exposure accounting, DC exclusion, and edge-coverage tests. **Recommended** if finding a usable channel is essential to the first-run experience.
- `Q4-B` — Defer scan and require the operator to supply a known frequency. Smaller, safer Phase 1.

### Q5 — What platform scope should the design promise?

- `Q5-A` — Linux/Ubuntu only for Phase 1. **Recommended.** Matches termios, POSIX atomic-write behavior, librtlsdr verification, and the current machine.
- `Q5-B` — Portable C++ core with Linux-only hardware/UI adapters, while other platforms remain unverified. More abstraction work.
- `Q5-C` — Linux, macOS, and Windows support in Phase 1. Substantially larger test and packaging scope; do not recommend.

### Q6 — How should the existing documents be preserved?

- `Q6-A` — Revise the existing spec and plan in place, relying on Git history and the validation artifacts for provenance. **Recommended.** Leaves one authoritative spec and one authoritative plan.
- `Q6-B` — Mark the existing documents superseded and create new dated v2 files. Clear audit trail but creates more documents to navigate.

Tell Nina she may reply compactly, for example:

```text
Q1-B, Q2-A, Q3-A, Q4-A, Q5-A, Q6-A
```

Do not ask Nina to decide implementation details such as FFTW precision, noise-estimator correctness, event independence, audio latency, persistence atomicity, timeout semantics, or test layout. Resolve those using engineering evidence and the mandatory corrections below.

## Phase 2 — revise after Nina answers

After receiving her selections, briefly restate them and edit the specification and plan. If she selects an option that weakens scientific or safety guarantees, preserve her product choice but label its consequences explicitly and do not fabricate validation.

### Mandatory technical corrections

The revised documents must resolve every gap in the validation report, including the following release-blocking items:

1. Replace the circular 20th-percentile occupancy check. Specify and test a feasible two-stage probe-floor approach plus a persistent-carrier/unidentifiable-floor guard, or a demonstrably stronger method. A 90%-active capture must not be accepted as quiet noise, and pure noise must not be falsely flagged.
2. Treat captures/rounds—not individual events—as the independent experimental units. Keep event distributions as within-capture diagnostics. Define how capture medians are aggregated and how paired/blocked uncertainty is calculated.
3. Separate `selected_metric`, descriptive rankings, and `resolved_best_angle`. Operator choice must not override uncertainty, clipping, overrun, gain, angle, or noise-floor gates.
4. Link FFTW precision consistently. If code uses `fftwf_*`, specify `fftw3f`/`PkgConfig::FFTW3F`. Use `std::numbers::pi_v<double>` rather than `M_PI` under the portable C++20 rule.
5. Use a separate hardware-test executable with machine-readable `passed`, CTest `skipped`, and `failed` outcomes. A missing or busy device must not be reported as passed and must not leak into the hardware-free CTest target.
6. Define every CLI/config field before use, including resume and scan fields. If scan remains, add an `ITunableSampleSource` or equivalent retuning contract and per-channel exposure accounting.
7. Add visit IDs, attempt numbers, retry intent, and accepted/superseded/abandoned dispositions. Failed, unreliable, or superseded attempts remain auditable but never contribute to ranking.
8. Define filter latency, transient trimming, and event-to-audio window compensation. Add an absolute audio-SNR reference test; monotonicity alone is insufficient.
9. Make `session.json` canonical. Define atomic JSON writes, atomic derived CSV regeneration/reconciliation, explicit schema-version handling, `finished_utc: null` while active, crash boundaries, and fault-injection tests.
10. Add finite timeouts, cancellation, worker-error wakeup, capture-start timestamps taken before reading, and per-capture overrun deltas.
11. Keep production code independent of `tests/support`; reusable synthetic generation belongs in production source code.
12. Resolve angle endpoint behavior, circular angular distance/means, actual-angle finite/range validation, and the physical definition of 0° and rotation geometry.
13. Add clipping/front-end health checks, requested-versus-applied hardware settings, startup-transient handling, and applied NCO-offset metadata.
14. Add label/path sanitization, symlink protection, bounded config/session-file sizes, terminal-control escaping, private session permissions, resource estimates, and duration/round limits.
15. Make terminal raw mode safe for non-TTY input and signals; no handler may retain a pointer to destroyed stack state. Replace broad `pkill -f` documentation with a safe manual-close instruction or exact-process alternative.
16. Remove every stale or contradictory claim, including:
    - endpoint inclusion versus `0,20,40,60,80`;
    - “alternating breaks the correlation” versus “reduces the confound”;
    - “four interfaces” while using a concrete `SessionStore`;
    - the wrong executable task number;
    - promised ±2 dB audio accuracy versus a monotonic-only test;
    - mean signal power versus median implementation;
    - any claim that carrier offset identifies the same aircraft;
    - any current SDR++ PID or hardware state that was not freshly verified.

### Revised specification requirements

The specification must be normative and internally consistent. Include:

- purpose, non-goals, safety boundary, supported platform, and chosen product decisions;
- physical experiment protocol and angle reference;
- architecture and dependency direction;
- complete source, capture, retuning, UI, provider, persistence, and store interfaces;
- complete configuration schema, validation rules, defaults, and resource limits;
- exact DSP/filter conventions, latency rules, framing, noise-floor reliability, event detection, SNR calculations, and clipping behavior;
- hierarchical aggregation and resolved/unresolved decision policy;
- canonical session schema with attempt state and migration/recovery behavior;
- terminal/CLI/scan behavior selected by Nina;
- acceptance tests and hardware evidence states;
- explicit limitations and claims the tool must never make.

Do not label the revised specification “Approved for implementation” until the final audit below passes. Use `Status: Draft — pending revalidation` while editing.

### Revised implementation-plan requirements

Replace the current 18-task sequence with dependency-ordered work packages that cannot encode a downstream feature before its contract is stable. At minimum cover:

1. Requirements reconciliation and acceptance gates.
2. CMake, correct dependencies, CI, sanitizers, and hardware-test separation.
3. Complete configuration, angle math, schemas, and resource validation.
4. DSP primitives, explicit filter edges, latency, clipping, and chunk invariance.
5. Correct noise reliability, event detection, channel/audio SNR, and Monte Carlo references.
6. Synthetic/file sources and timeout-aware capture.
7. RTL-SDR source and optional retunable scan source.
8. Canonical persistence, attempt state, resume, reconciliation, and fault injection.
9. Terminal UI and angle-provider protocol.
10. Controller state machine with accepted-attempt semantics.
11. Capture-level aggregation, ranking, uncertainty, and report policy.
12. CLI/main orchestration and scan if selected.
13. Synthetic end-to-end, quit/resume, native terminal, hardware, documentation, and release verification.

For every work package include:

- purpose and dependencies;
- files/interfaces created or changed;
- exact behavioral acceptance criteria;
- failing tests written first;
- implementation notes only where needed to remove ambiguity;
- verification commands;
- stop conditions;
- estimated effort and responsible role;
- one professional commit boundary, but do not create the commit in this task.

Do not include thousands of lines of speculative implementation code. Use exact interfaces, pseudocode, formulas, fixtures, and assertions where they materially remove ambiguity. The plan should be executable by a competent junior developer with review gates, not a generated code dump.

Do not depend on unavailable skills or require subagents. You may describe optional parallel work only where contracts are already frozen and the worktrees/files do not overlap.

## Phase 3 — revalidate the revision

After editing:

1. Read both revised documents again from top to bottom.
2. Search for every stale term and contradiction listed above.
3. Build a requirements-to-work-package traceability table.
4. Confirm every configuration field appears consistently in the schema, CLI, persistence, resume checks, tests, and documentation plan.
5. Confirm every status and attempt transition has one defined persistence and resume outcome.
6. Confirm hardware-free, hardware-skipped, and hardware-failed evidence cannot be confused.
7. Run:

```bash
git diff --check
git status --short
git diff -- docs/superpowers/specs/2026-08-19-rtl-sdr-antenna-angle-design.md \
  docs/superpowers/plans/2026-08-19-rtlangle-phase1.md
```

If Nina selected new v2 files, adjust the diff paths accordingly.

## Final response

Report:

- Nina’s selected decisions;
- files changed and their line counts;
- the most important technical corrections;
- any remaining open questions or consciously deferred scope;
- verification performed and exact results;
- one verdict: `ready for independent revalidation` or `not ready for revalidation`.

Do not claim implementation readiness yourself. The revised documents must receive an independent revalidation after this task. Do not commit or push unless Nina separately authorizes it.
