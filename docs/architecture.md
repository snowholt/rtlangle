# rtlangle architecture

This document is a map for someone reading the code. The normative statements
live in `docs/superpowers/specs/2026-08-19-rtl-sdr-antenna-angle-design.md`;
what follows is the shape the implementation took and, where it matters, why.

## Layering

Dependencies point downward only. No lower layer includes a header from a higher
one. `experiment/` names abstract interfaces rather than concrete types, which
is what makes the whole orchestration path testable with no hardware and what a
future servo angle provider would drop into.

```
app/          main, CliParser, RunCommand, ScanCommand, DeviceCheck, Menus
   |
ui/           ITerminalUi <- AnsiTerminalUi | ScriptedTerminalUi
              ReportRenderer, safe_text
   |
experiment/   ExperimentController, VisitPlan, CaptureRunner, Aggregator, QualityChecks
   |
   +-- angle/    IAngleProvider   <- ManualAngleProvider | FixedAngleProvider
   +-- source/   ISampleSource    <- SyntheticSource | IqFileSource | RtlSdrSource
   |             ITunableSampleSource <- RtlSdrSource | SyntheticSource  (scan only)
   +-- persist/  ISessionStore    <- JsonSessionStore
   |             AtomicWrite, Csv, SessionLoader, PathSafety
   +-- metrics/  NoiseFloor, EventDetector, SnrEstimator
         |
       dsp/      FirDesign, OffsetMixer, FirDecimator, ChannelFilter,
                 AmDemodulator, Framer, Spectrum, Chain
         |
       core/     Config, AngleMath, Records, Statistics, Db, Utc, Version
```

**Five interfaces** are the whole abstraction boundary: `ISampleSource`,
`ITunableSampleSource`, `IAngleProvider`, `ITerminalUi`, `ISessionStore`.
Storage is an interface so persistence faults can be injected without a
filesystem and so the controller depends on a contract rather than on JSON.

**Production code never includes anything from the test-support directory.** The
synthetic AM-voice generator is production code under `src/source/` because
`--source synthetic` is a production source type; tests may call it, and
`tests/support/` holds only assertions, fixtures, and doubles.

`SourceInfo` is an exception worth naming: it describes a source but lives in
`core/records.h`, because it is *persisted* — it is a receiver segment's
baseline, and warning W7 compares every attempt's applied settings against it.
Putting the struct in `source/` would have made `core/` depend on a layer above
it.

## Targets

| Target | Contents |
|---|---|
| `rtlangle_core` | static library: `core/ dsp/ metrics/ source/ angle/ persist/ ui/ experiment/ app/` |
| `rtlangle` | the CLI executable |
| `rtlangle_tests` | every hardware-free unit and integration test (doctest) |
| `rtlangle_hardware_test` | a separate executable, CTest label `hardware`, pass/skip/fail exit codes |

The hardware test is a separate *executable* because CTest labels apply to
registered tests, not to cases inside one binary. Keeping it separate is what
makes `ctest -LE hardware` a genuinely hardware-free run.

Project targets carry `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`;
fetched dependencies are attached as SYSTEM includes so their headers never
produce a diagnostic. `-Werror` makes "builds with no warnings" a structural
property rather than a check somebody has to remember to run.

## The signal chain

```
tune to (center_hz + offset)                      default offset 250 kHz
  -> u8 IQ pairs -> complex<float>,  (x - 127.4f) / 127.5f
  -> clipping census on the RAW BYTES
  -> OffsetMixer   x exp(+j 2 pi f_off t)          at 1.024 MHz
  -> FirDecimator  /8   -> 128 kHz
  -> FirDecimator  /4   ->  32 kHz  (the channel rate)
  -> ChannelFilter low-pass, passband 4 kHz, stopband 5 kHz
  -> transient trim, then framing: 1024 samples, hop 512  (32 ms / 16 ms)
       |
       +-> AmDemodulator -> audio band 300-3400 Hz, at the same channel rate
```

Four things in this chain are less obvious than they look.

**Filters are designed from three explicit parameters** — passband edge,
stopband edge, stopband attenuation — never from a single "cutoff", which is
read as -3 dB, -6 dB, or the band edge depending on the reader. The Kaiser order
estimate is asymptotic and lands a fraction of a decibel short at the start of
the stopband, so `design_lowpass` grows the length until the design **meets** its
own stated attenuation, verified against its own response. A design that missed
its specification would otherwise be discovered as a failing response test and
invite loosening the assertion.

**The mixer sign is positive.** The hardware is tuned *above* the wanted signal,
so the signal appears at baseband `-offset` and must be shifted up to DC. Phase
is an exact integer accumulator modulo the sample rate feeding a 65536-entry
table: an integer accumulator cannot drift over a 60-second capture, whereas a
floating-point recurrence can.

**Decimation emits output k as the filtered input at index k\*factor**, the
standard convention. Emitting at `k*factor + (factor-1)` instead would shift the
whole output grid, and the chain latency — which sums filter delays only — would
then disagree with the measured impulse peak by nearly one channel sample per
cascade.

**The chain latency is a rational number and is kept as one.** Referring each
stage's delay to the channel rate gives `sum of d_s / prod_{j>=s} M_j`, which is
generally fractional. An integer expression truncates it silently; `Chain`
returns an exact `num/den` plus `ceil_samples` and a `residual`, the transient
trim uses the ceiling so it can only over-trim, and the impulse test asserts a
two-value set because a fractional latency cannot land on one integer.

## The measurement

**The noise floor is estimated twice.** The superseded rule was circular: it used
the 20th-percentile floor to decide whether enough quiet frames existed to make
that percentile meaningful. When 90 percent of a capture is occupied, the 20th
percentile lands *inside* a transmission and the computed activity comes out near
zero, so a capture that was almost entirely signal would be accepted as quiet
noise. The replacement probes at a lower percentile and uses that probe only to
judge occupancy; the reported floor never judges its own validity.

**The SNR subtracts before dividing.** `(P_event - N) / N`, in the linear power
domain, then converted to decibels. `P_event / N` is `SNR + 1` and is the
total-channel-power quantity that must never be called an SNR. One test exists
solely to fail if that subtraction is ever removed.

**Events merge before they are discarded.** Merging first stops a single
transmission with a brief pause from being thrown away as two short fragments.

**The experimental unit is the capture, not the event.** Events inside one
capture share a receiver configuration, a minute of propagation, an antenna
position, and often a single transmitter, and adjacent frames are 50 percent
overlapped by construction. Each accepted capture contributes exactly one vote.

**The two metrics qualify separately.** A capture can be sound for the channel
metric and unusable for the audio one; the attempt status describes the channel
path only, and audio eligibility is a per-metric flag. `valid_audio_event_count`
is tracked beside `valid_event_count` and is never assumed equal to it.

## Persistence

`session.json` is the single source of truth; `measurements.csv` and
`report.txt` are derived and recoverable.

The write protocol is fixed and testable:

| # | Step | If it fails |
|---|---|---|
| 1 | serialise to `session.json.tmp.<pid>` via `openat(O_CREAT\|O_EXCL\|O_WRONLY\|O_NOFOLLOW, 0600)` | `NotCommitted` |
| 2 | `fsync` the temporary file | `NotCommitted` |
| 3 | **`renameat(dirfd, tmp, dirfd, "session.json")`** — the commit point | `NotCommitted` on a classifiable errno, `Indeterminate` otherwise |
| 4 | `fsync(dirfd)` | **`CommittedNotDurable`** |
| 5 | regenerate `measurements.csv` | **`Committed`**, with a repair note |

`commit_visit` **returns** a `CommitResult` rather than throwing, because there
are four outcomes and an exception can carry only one. `CommittedNotDurable` is
not a failure: the commit is in the record and every later read sees it, and
retrying would duplicate the attempt.

`EINTR` is retried inside the syscall wrappers, and only `EINTR`, so a signal
never becomes a `CommitOutcome`. `EIO` from `fsync` is deliberately not retried:
a failed `fsync` may already have discarded the error state a second call would
then report as success. `renameat` is not interruptible on Linux, so the commit
point is never reached in a retry loop.

**The store has one mutator.** A retry writes the just-finished attempt as
`superseded` together with the `pending_retry` that names its replacement, in
one rename. Three separate calls could not satisfy a one-commit requirement, and
an attempt written once with its final disposition leaves nothing to go back and
change.

Two invariants hold after every possible crash point, and the loader asserts
both:

```
I1  no visit has more than one attempt whose disposition is accepted or abandoned
I2  pending_retry is non-null  <=>  the named visit's latest attempt is superseded
```

**Two injection seams, because one cannot do both jobs.** `FileOps` injects at
the syscall boundary and is the only thing that can produce the whole outcome
matrix — a store decorator cannot make `fsync(dirfd)` fail *after* `renameat`
succeeded, nor produce an unclassifiable errno. `FaultStore` decorates
`ISessionStore` and is how the controller's and the app layer's behaviour in the
face of each outcome is tested.

## Who writes what

```
begin_receiver_segment      the APP layer, after the source is open and the
                            record exists, before the controller is built
commit_visit, reload        the CONTROLLER
pause / finalize / abort    the APP layer, after the controller returns and
                            after aggregation has produced the summary
```

**The controller always leaves the session `running`.** It cannot call pause,
finalize, or abort, because all three take a `SessionSummary` and computing one
is aggregation's job, above it. It signals which state to write by its return
value:

| `Result` | App layer calls | Final state | Exit |
|---|---|---|---|
| `Completed` | `finalize(summary)` | `completed` | 0 |
| `QuitRequested` | `pause(partial)` | `paused` | 0 |
| `Failed` | `abort(partial, reason)` | `aborted` | 1 |

A crash in the window between the last visit commit and that second commit
leaves a `running` record with every measurement intact, which the loader
reopens and reports as an unclean shutdown. That is strictly better than letting
the controller finalise a session whose summary it may not compute.

## Filesystem hardening, honestly scoped

`O_NOFOLLOW` refuses a symlink **at the final path component only**. Every
intermediate directory is still resolved normally, so it does not protect
against a symlink at `sessions/` redirecting everything underneath.

What does hold that property is a directory descriptor: `mkdir(path, 0700)`
fails with `EEXIST` if anything is already there, the directory is opened once
with `O_DIRECTORY|O_NOFOLLOW|O_RDONLY`, and every later operation is relative to
that descriptor. Once it is held, replacing any component of the path cannot
redirect a write, because the kernel resolves nothing but the final name. A test
swaps a parent directory mid-session and asserts the bytes still land in the
original inode.

This is defence in depth against accidents and against a hostile session root.
It is **not** a defence against an attacker who already has write access to the
operator's home directory: such an attacker can replace the whole directory
before the program starts, and no file-open flag helps.

## Scan coverage

A dwell centred at `c` covers `[c-U, c-D]` and `[c+D, c+U]` and **nothing** in
`(c-D, c+D)`, where `U` is the usable half-span and `D` the DC exclusion.
Stepping by the whole usable span makes consecutive dwells abut at their outer
edges and leaves the hole around every centre covered by no dwell at all — 60 kHz
against a 25 kHz channel spacing at the defaults.

The hole must instead be covered by the neighbours at `c ± S`:

```
c - S + U >= c + D   gives   S <= U - D      reach the far edge
c - S + D <= c - D   gives   S >= 2D         reach the near edge
```

Such an `S` exists exactly when `U >= 3D`, which is a configuration rule checked
before any dwell is generated. The chosen step is the largest legal one,
`S = U - D`, which at the defaults is 379600 Hz and produces 50 dwells across
the airband. A regression fixture drives the placement with the old step and
asserts the hole exists, so the coverage test cannot pass for the wrong reason.

## What is deliberately absent

There is no decision engine, no resolved angle, no test statistic, no interval
estimate, and no second report headline. Those are not omissions to be filled in
later: the tool measures, ranks, and reports what is wrong with the measurement,
and deciding is the reader's job. A structural test greps the rendered report
for the wording that would break that, and the summary's serialisation is
checked for any field name that would imply it.
