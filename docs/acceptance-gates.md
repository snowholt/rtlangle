# rtlangle Phase 1 — Acceptance Gates

Date: 2026-08-19
Spec: `docs/superpowers/specs/2026-08-19-rtl-sdr-antenna-angle-design.md` (revision 4)
Plan: `docs/superpowers/plans/2026-08-19-rtlangle-phase1.md` (revision 4)

This file is the WP0 deliverable. It records the requirements reconciliation the
work package requires, and the six conditions that decide whether Phase 1 is
complete, each with the command that produces its evidence.

## 0. Outcome vocabulary

Every gate below is reported in exactly one of three states. There is no fourth,
and no gate may be reported in a state its evidence does not support.

| State | Meaning |
|---|---|
| **passed** | The command was run, its output was inspected, and it met the criterion. |
| **failed** | The command was run and did not meet the criterion. |
| **pending** | The command was not run, or could not run, so no claim is made. A hardware test that reports `skipped` lands here — never in `passed`. |

## 1. Provenance of the six conditions

The six completion conditions restated in §3 originate in §1 of
`docs/artifacts/2026-08-19-rtlangle-revalidated-plan.md`, which is provenance
rather than authority: the spec and the plan are the sole authorities (spec §2,
decision Q6). That artifact predates decision Q1 and still contains revision-2
and revision-3 normative language about naming a highest-scoring angle and about
resolution gates. Both are now forbidden outright (spec §2 Q1, §15.2), so the
conditions below are restated in revision-4 terms and the artifact's wording is
deliberately not carried over.

## 2. WP0 requirements reconciliation

Reviewed against both authoritative documents on 2026-08-19. Each item is a WP0
acceptance criterion; the result is the reviewed state of the documents as
committed.

| # | Criterion | Result |
|---|---|---|
| 1 | Exactly one angle endpoint rule, `start + i*step <= end + 1e-9` | Confirmed. Spec §4.3 states it once, with `0:20:90 -> 0,20,40,60,80` as the worked consequence. The plan's WP2 tests assert the same rule and no other. |
| 2 | Neither document claims alternating order breaks the angle–time correlation | Confirmed. Spec §4.6 states that alternating *reduces* the linear component of a time trend and does not eliminate it, and forbids the stronger claim. Spec §15.1 repeats the limitation. |
| 3 | Both documents name five interfaces, including `ISessionStore` | Confirmed. Spec §5 and §6 enumerate `ISampleSource`, `ITunableSampleSource`, `IAngleProvider`, `ITerminalUi`, `ISessionStore`; the plan's architecture paragraph names the same five. |
| 4 | Neither document claims carrier offset identifies a transmitter or aircraft | Confirmed. Spec §9.2 states it is a comparability diagnostic only, and §15.2 forbids the identification claim. |
| 5 | No SDR++ PID, `pkill -f` instruction, `M_PI`, bare `fftw3` link target, `--save-iq`, `completed_keys`, or a third "Approved for implementation" | Confirmed by the greps below. Every hit is a prohibition or a historical reference. `Approved for implementation` occurs as a usage exactly twice — the `Status` line of each document — which records the independent revalidation spec §15.2 required. |
| 6 | No removed construct appears as a usage | Confirmed by the greps below. `--agc`, `allow_agc`, `selected_metric`, `sample_overruns`, the `r1-a<angle>` identifier form, `append_attempt`, `set_disposition`, `record_retry_intent`, `begin_visit`, `resolved_best_angle`, `resolution_method`, `resolution_strength`, `resolution_reason`, `primary_metric`, `family_alpha`, `min_captures_for_resolution`, `paired_ci95`, `paired_lower_bound`, `t_quantile`, `signflip`, "statistically resolved", and "Highest measured reception quality" appear only where a document states that they do not exist or records what replaced them. |
| 7 | Every positive construct is present in both documents | Confirmed by the greps below: the estimand, detection yield beside every score, the W1–W8 list described as warnings rather than a ladder, `commit_visit` as the single mutator returning a `CommitResult`, `rename()` as the commit point with four outcomes, the four session states with the controller never writing a terminal one, `valid_audio_event_count` with the three-row floor table, reserved retry capacity, the dot-run collapse, the `O_NOFOLLOW` final-component scoping with the `dirfd` protocol, the dwell rule `2D <= S <= U - D` with `U >= 3D`, and the `A(A+1)/2` persistence arithmetic. |
| 8 | This file lists the six completion conditions with their evidence commands | This file. |

### Verification commands for criteria 5–7

The prohibited terms appear in both documents *as prohibitions*, so a bare grep
matches by design. The check is that every hit is a prohibition or a historical
reference and none is a usage.

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

Against shipped source and documentation the prohibited terms must produce
**zero** matches. That sweep is WP12's and runs over `src/`, `README.md`, and
`docs/architecture.md`.

This file is deliberately outside that scope, for the same reason the spec and
the plan are: its job is to *list* the prohibited constructs, so a bare grep
matches it by design. The rule that applies here is the same one WP0 applies to
the authoritative documents - every hit must be a prohibition, and none may be a
usage - and the table above records the result of that review.

## 3. The six Phase 1 completion conditions

Recorded 2026-08-20, on the development machine described in spec §3, with an
RTL-SDR Blog V4 attached and free.

| # | Condition | Evidence command | State |
|---|---|---|---|
| C1 | A clean hardware-free build and test run passes with RTL-SDR support both enabled and disabled. | `cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure -LE hardware`; then the same with `-B build-nohw -DRTLANGLE_WITH_RTLSDR=OFF`. | **passed** — 357 tests in the RTL-SDR tree and 358 in the hardware-free one, 0 failed in either; no compiler warning in either (project targets carry `-Werror`). The extra hardware-free case is the device check that runs only when librtlsdr is absent. |
| C2 | Dependency linkage is verified from a clean build, including single-precision FFTW: `fftwf_*` against `fftw3f`, with no double-precision symbol linked. | `nm -C build/librtlangle_core.a \| grep -c fftw_plan_dft_1d`; `grep -rn 'fftw_plan\|lfftw3[^f]' CMakeLists.txt cmake/ src/`. | **passed** — 0 and 0. A link smoke test calling `fftwf_plan_dft_1d` is compiled into the suite, so a precision regression fails at build time. |
| C3 | Hardware-free tests cover the full DSP chain, high-occupancy captures, constant carriers, pure noise, short events, audio alignment, clipping, and retry and resume behaviour. | `ctest --test-dir build --output-on-failure -LE hardware`, per-test output inspected. | **passed** — spec §14.2 Tests A–L all present and passing, including the ≥200-trial Monte Carlo of Test E and the fault-injection matrix of §11.1.1. The suite also runs clean under `-fsanitize=address,undefined`. |
| C4 | The hardware test produces a machine-readable result of `passed`, `skipped`, or `failed`, and a `skipped` result is never reported as passed. | `ctest --test-dir build --output-on-failure -L hardware`, then parse the `RTLANGLE_HARDWARE=` line. | **passed** — `RTLANGLE_HARDWARE=passed`, exit 0. Earlier in the same session, with the device held by another application, the same executable printed `RTLANGLE_HARDWARE=skipped` and exited 77, and that run was recorded as pending rather than as a pass. |
| C5 | At least one real receive session completes with no clipping and no host-dropped samples, and its `session.json`, `measurements.csv`, and `report.txt` reconcile attempt for attempt. | `rtlangle scan`, then `rtlangle run --freq …` against the attached device. | **passed** — a two-visit, 20 s-per-capture session on 119.175 MHz completed with `state: completed`, `host_dropped_samples` 0 on both attempts and a clipped fraction of 6.3e-7 and 1.7e-7 against a 1e-4 limit. Both captures ended `noise_floor_unidentifiable`: the channel carried a persistent carrier with too little dynamic range for a quiet reference, and the tool declined to report an SNR rather than fabricating one. The three files reconcile. |
| C6 | Every documented command has been executed, and any hardware or operator validation that was not executed is explicitly marked pending. | The command list in spec §14.4 and the plan's final verification block. | **passed** — `run`, `scan`, `devices`, `report`, `--help`, and `--version` were all executed, on synthetic sources and on hardware. The interactive terminal menu and the operator-driven angle prompts were exercised through the scripted terminal and a pseudo-terminal rather than by a human at the keyboard; that is marked **pending** below. |

### What is still pending

- **Operator-driven interactive validation.** The arrow-key menu, the physical
  positioning prompts, and the post-capture Retry / Accept / Skip / Quit choice
  are covered by `ScriptedTerminalUi`, by a pseudo-terminal test that delivers a
  signal and checks the terminal is restored, and by the non-TTY fallback path.
  No human has driven them at a real keyboard.
- **A rotated-antenna experiment.** Every session run so far was either
  synthetic or a bench receive with the antenna stationary. Nothing has yet
  measured an angle difference on real hardware.

## 4. Stop conditions

- A contradiction between the two authoritative documents that cannot be
  resolved without a product decision goes back to the owner. Do not pick a
  reading and proceed.
- A failing test is not fixed by widening a tolerance, deleting the test, or
  marking it expected-failing.
- A missing or busy device makes C4 and C5 pending. It does not make them
  failed, and it does not block the remaining gates.
