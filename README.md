# rtlangle

`rtlangle` measures how well an RTL-SDR receives AM airband transmissions as a
function of a dipole antenna's orientation. You rotate the antenna by hand
through a planned sequence of angles. At each angle the tool captures IQ samples
for a fixed duration under a frozen receiver configuration, detects real
transmissions inside the captured stream, computes reception-quality metrics,
and stores the result before moving on.

At the end it ranks the angles **descriptively** and reports every reason the
ranking might mislead.

**It does not decide which angle is the right one.** That is a product decision,
not a limitation of the implementation, and the reasons are in
[What this tool will not tell you](#what-this-tool-will-not-tell-you).

Phase 1 is Linux-only and receive-only.

---

## Contents

- [What you need](#what-you-need)
- [Build and test](#build-and-test)
- [The physical protocol](#the-physical-protocol)
- [Your first experiment](#your-first-experiment)
- [Reading the output](#reading-the-output)
- [What the two metrics are](#what-the-two-metrics-are)
- [The estimand: what is actually being measured](#the-estimand-what-is-actually-being-measured)
- [The warnings W1 to W8](#the-warnings-w1-to-w8)
- [Commands and flags](#commands-and-flags)
- [What is stored](#what-is-stored)
- [What this tool cannot do](#what-this-tool-cannot-do)
- [What this tool will not tell you](#what-this-tool-will-not-tell-you)
- [Freeing a busy device](#freeing-a-busy-device)
- [Legal note](#legal-note)

---

## What you need

An RTL-SDR (the RTL-SDR Blog V4 is what this was developed against), a half-wave
dipole on a mount that rotates in the horizontal plane, and Linux.

Ubuntu 24.04 packages:

```bash
sudo apt install build-essential cmake ninja-build pkg-config \
                 libfftw3-dev libusb-1.0-0-dev
```

`libfftw3-dev` provides both the double- and the single-precision libraries;
this program uses the **single-precision** one (`fftw3f`) throughout and never
mixes the two.

`librtlsdr` is expected to be the [rtl-sdr-blog](https://github.com/rtlsdrblog/rtl-sdr-blog)
fork, resolved from `/usr/local`. **This matters.** Tuner gain is the controlled
variable of the whole experiment, and the stock Ubuntu `librtlsdr` carries the
wrong gain table for a Blog V4: a requested gain would be silently snapped to a
different applied gain. `rtlangle` records the driver version string, the device
name, and the *read-back applied* gain in every session, so a wrong driver shows
up in the record rather than hiding in the results.

nlohmann/json and doctest are found on the system if present and downloaded and
checksum-verified if not.

## Build and test

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure -LE hardware   # no device needed
ctest --test-dir build --output-on-failure -L  hardware   # needs a free device
```

The hardware test is a separate executable so that `-LE hardware` is a genuinely
hardware-free run. It prints exactly one machine-readable line:

```
RTLANGLE_HARDWARE=passed     exit 0    a device was opened and the assertions held
RTLANGLE_HARDWARE=skipped    exit 77   no device, or the device was busy
RTLANGLE_HARDWARE=failed     exit 1    a device was opened and an assertion failed
```

A `skipped` result means hardware validation is **pending**. It is not a pass,
and nothing should report it as one.

To build without device support at all — everything except the RTL-SDR source,
including the whole hardware-free test suite:

```bash
cmake -S . -B build-nohw -G Ninja -DRTLANGLE_WITH_RTLSDR=OFF
cmake --build build-nohw && ctest --test-dir build-nohw
```

## The physical protocol

The measurement is only as good as the physical setup, and most of the setup is
about **holding everything except the angle constant**.

**Mark one dipole arm.** A band of tape is enough. The angle you record is the
bearing of that marked arm. Marking one arm is what makes the rotation a full
360-degree circle with 360 distinct orientations, which is what the tool's
circular arithmetic assumes.

**Choose a 0-degree mark and do not move it.** It is a fixed physical reference
you pick before the first capture — "marked arm pointing along the balcony rail,
towards the street" is a good one. Enter it once as `--angle-reference`; it is
stored verbatim and printed in every report. Absolute bearing is deliberately
not required, because the tool never claims a direction.

**Increasing angle is clockwise, viewed from above.**

**Hold these constant for the whole session,** and record them in
`--setup-note`: antenna height above ground, feedline routing and length, mount
position, and where you stand during a capture. A human body within roughly a
wavelength — about 2.5 m at 118 MHz — perturbs the pattern. If you stand in a
different place at different angles, that difference is inside the measurement
and the tool cannot tell it apart from the angle.

**Why 0 and 180 degrees are two measurements.** An ideal dipole in free space
has a pattern symmetric under a 180-degree rotation of its axis, so theory says
theta and theta+180 receive identically. A dipole on a balcony does not: the
feedline, the mount, the wall behind it, and your own position break that
symmetry. `rtlangle` treats them as two distinct orientations and never folds
them together. A large measured difference between the two is not a fault — it
is evidence that the environment, not the antenna, dominates the pattern, and
the report says so.

## Your first experiment

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
#    makes this tool name a single angle as the right one - it ranks and it
#    warns.
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

Read the output as a ranking plus a list of caveats, not as an answer. If
warning W6 fires, the top-scoring angle heard *fewer* transmissions than another
one, and [the estimand](#the-estimand-what-is-actually-being-measured) explains
why that is the signature of the measurement rather than of the antenna.

## Reading the output

Every session prints two ranking tables — one per metric, always both, always in
full — followed by the data-quality warnings, the comparability notes, and one
closing headline.

Each ranking row carries, for one planned angle:

| Column | What it is |
|---|---|
| `planned` | the angle you asked for; this is what the ranking groups by |
| `actual` / `spread` | the circular mean and circular standard deviation of the angles you reported reaching |
| `chan dB` / `audio dB` | the angle's score: the median over its accepted captures |
| `capture spread` | the range those captures covered — min-max below four, interquartile at four or more |
| `events/min` | the detection yield (`events_per_minute` in the record), never omitted and never separated from the score |
| `detected` | the fraction of the capture spent inside a detected transmission |
| `n` / `excl` / `events` | contributing captures, excluded captures, and total valid events |
| `floor dBFS` / `floor span` | the pooled noise floor and how much it moved |
| `warnings` | which of W1 to W8 name this angle |

The `capture spread` column says how much the contributing captures varied. It
is descriptive. It is **not** an interval estimate and carries no claim about
whether the ordering would repeat.

An angle with no eligible capture for a metric is listed under the table with
its status rather than being ranked. It renders as `-`, never as `0`: a missing
measurement is not a measurement of zero.

## What the two metrics are

**`channel_snr_db` is a carrier-to-noise ratio, not an audio SNR.** The channel
filter passes the AM carrier together with its sidebands, so the quantity is the
in-channel carrier-plus-sideband power relative to the capture's noise floor.
The README says this plainly because the two are routinely confused.

The arithmetic is, per detected transmission:

```
SNR_lin = (P_event - N) / N          in the linear power domain
SNR_dB  = 10 * log10(SNR_lin)
```

The subtraction happens **before** the division. `P_event / N` is `SNR + 1`: at
0 dB true SNR it would report 3.01 dB, and it is the "total channel power
relative to noise" quantity that must never be called an SNR.

**`audio_snr_db`** is the same shape, measured after envelope demodulation over
the 300-3400 Hz audio band, on a latency-shifted and guarded window.

A capture can be perfectly good for the channel metric and unusable for the
audio one — the guarded audio window can be too short, the audio noise reference
can be unreliable, or the audio signal can fail to clear its own floor. Such a
capture is still `ok`: it contributes to the channel ranking and is simply
absent from the audio one. The report says so in its comparability notes.

**The weakest SNR this tool can report is about 4.74 dB** at the default squelch
of 6 dB, because nothing weaker ever crosses the detection threshold. That number
is printed in every report. It is a property of the measurement, not a bug — but
it means an angle can look "insufficient" simply because everything it heard was
weak.

## The estimand: what is actually being measured

This is the most important section in this file.

The score at an angle is:

> the median, over the transmissions that occurred during this session's
> captures at that angle **and were strong enough there to cross the squelch**,
> of the in-channel carrier-plus-sideband power relative to that capture's noise
> floor.

Three things about that sentence matter.

**It is conditional on detection, and the condition moves with the antenna.** The
squelch is a fixed threshold *relative to the noise floor*. An angle that
receives better lifts more transmissions across it — including weak ones a worse
angle never registers at all. Those extra weak events pull its median **down**.

So a higher score does **not** by itself mean an angle receives better. It can
equally mean the angle heard only the loud ones. The two are told apart by the
detection yield:

| | score | yield |
|---|---|---|
| "receives better" | higher | at least comparable |
| "heard only the loud ones" | higher | **lower** |

That is why `events/min` is printed beside every score and never omitted, and it
is the whole content of warning W6.

**The population is session-local.** It is the traffic that happened during those
particular minutes, not airband traffic in general. Captures at two angles never
observe the same transmission, so the comparison is unmatched by construction.

**What would remove the problem.** A continuously transmitting reference — an
ATIS, a VOLMET, a VOR, or a broadcast FM carrier — has no on-off behaviour, so
there is nothing to detect and nothing to condition on. That is the
scientifically clean version of this experiment, it is a different measurement
path, and it is Phase 2 work. Phase 1 says so rather than approximating it.

## The warnings W1 to W8

These are warnings **about the measurement**, in the sense that a compiler
warning is about the code. Each names something that makes a comparison less
trustworthy. **None of them combine into a verdict**, and all eight are
evaluated every time — a list of problems that stopped at the first problem
would not be a list of problems.

| ID | Fires when | What it means for you |
|---|---|---|
| **W1** | a ranked angle has fewer accepted captures than `--min-captures` (default 4) | That angle's median rests on very few captures. Its position may move with one more round. |
| **W2** | two ranked angles both have an accepted capture in fewer rounds than were run | They were not measured under comparable traffic in every round, so part of their difference is *when* they were measured. |
| **W3** | the top two scores differ by less than `--min-effect-db` (default 1.0 dB) | The gap is smaller than the threshold you set for one worth acting on. |
| **W4** | the channel and audio rankings disagree on the leading angle | The two measurement paths are not telling the same story. |
| **W5** | pooled noise floors span more than `--noise-drift-warn-db` (default 3.0 dB) | The noise environment changed during the session. |
| **W6** | the top-scoring angle's yield is below `--yield-concordance-ratio` (default 0.90) of the best yield | The detection-selection effect is visibly present: the top-scoring angle heard *fewer* transmissions than another one. |
| **W7** | an attempt's applied gain, sample rate, or centre frequency differs from its receiver segment's baseline, or AGC is reported enabled anywhere | The receiver was not held constant, so the comparison is between two receivers as much as between two angles. |
| **W8** | a ranked angle carries a clipping, host-drop, unreliable-floor, unidentifiable-floor, audio-insufficient, or angle-deviation flag | Named data-quality faults are present in the ranked data. |

W6 is a point comparison against a fixed ratio with no allowance for
uncertainty. Airband traffic is bursty, so it will sometimes fire because the
traffic happened to be uneven rather than because the selection effect is
present. Since it raises a warning rather than blocking anything, a false
positive costs you a sentence of context.

**Why more rounds help.** Not because any number of them resolves anything —
nothing here resolves anything. A median over two captures tells you almost
nothing about how much that angle's measurement varies, and the spread printed
beside each score is the number that tells you whether the ranking means
anything. Four or more rounds make that spread informative; two make it a single
interval between two points.

## Commands and flags

```
rtlangle run [options]         run an angle experiment (interactive by default)
rtlangle scan [options]        find active airband channels
rtlangle devices               enumerate devices, gain table, tuner type
rtlangle report <session-dir>  re-render a report from a stored session
rtlangle --help | --version
```

Invoking `rtlangle` with no arguments on a terminal opens the main menu. Its
first entry opens a **Set up experiment** screen, so nothing has to be decided
before you start:

```
  Set up experiment

> Centre frequency   (not set)
  Angles             0 to 90 step 15  (7 angles)
  Rounds             2
  Visit order        alternating
  Capture duration   60 s
  Settle delay       3 s
  Gain               max
  Device index       0
  Source             rtlsdr
  Session label      (none)
  Angle reference    (none)
  Other option...    type any flag
  Start              blocked by --freq
  Back               return without starting

  Up/Down or j/k move | Enter select | q quit
```

Each row shows the value it currently holds, and the `Start` row shows what is
stopping the run until nothing is. Angles are typed as `0:15:90` or as a list
like `0,45,90`. `Other option...` takes any flag from the tables below verbatim
(`--noise-percentile 25`, `--min-event-ms=450`), so the screen names the options
you set often without hiding the rest.

Anything you pass on the command line is where the screen starts, so the two
ways of setting an option compose rather than compete. Every edit goes through
the same parser the command line uses, which is why a value the screen accepts
is exactly a value `rtlangle run` would have accepted.

**A flag a command does not use is a usage error naming both, with exit code 2.**
Silently ignoring it would let you believe a setting took effect when it could
not have: `rtlangle report --gain 400` cannot change anything about a session
that has already been recorded.

| Exit code | Meaning |
|---|---|
| 0 | every planned visit is complete (`completed`), or the session ended by Quit with a partial report written (`paused`) |
| 1 | a runtime failure; `aborted` with a reason where the app layer could end cleanly, otherwise the record is left at its last committed value |
| 2 | a usage or configuration error, including a flag a command does not accept |
| 3 | the device was not found, was busy, or refused |

### Receiver

| Flag | Default | Notes |
|---|---|---|
| `--device` | 0 | `[0, 63]` |
| `--freq` | *(required by `run`)* | accepts `118.1M`, `121500k`, bare hertz |
| `--sample-rate` | 1024000 | must lie in `[225001, 300000]` or `[900001, 3200000]` |
| `--gain` | `max` | tenths of a dB, or `max`; snapped to the device's table, applied value read back |
| `--ppm` | 0 | |
| `--offset-tune-hz` | 250000 | 0 disables it and warns that the DC spur will contaminate the measurement |
| `--channel-bw` | 8000 | `[8000, 25000]` |
| `--channel-rate` | 32000 | `[16000, 96000]` |
| `--bias-tee` | off | DC on the feedline; requires an explicit confirmation |

The tuner gain mode is set to manual and the RTL2832U's digital gain control is
disabled on every device open, both return codes checked. There is no flag that
turns it back on: a gain that changed during a session would make every
comparison between angles meaningless.

### Experiment

`--duration` (60 s), `--settle` (3 s), `--start-deg` (0), `--end-deg` (90),
`--step-deg` (15), `--angles` (overrides the range), `--rounds` (2),
`--order` (`alternating`), `--seed`, `--angle-reference`, `--setup-note`,
`--max-angle-deviation-deg` (30), `--angle-deviation-warn-deg` (5).

The angle generation rule is exactly `start + i*step` while the result is
`<= end + 1e-9`. The endpoint is included only when it lies on the grid, so
`--start-deg 0 --end-deg 90 --step-deg 20` produces `0, 20, 40, 60, 80` and
**not** `0, 20, 40, 60, 80, 90`. A non-grid end angle produces an informational
message naming the last generated angle, so you are never surprised by a missing
endpoint.

`--order alternating` reverses the direction each round. This **reduces** the
confound between angle and time by balancing the linear component of any time
trend across angles. It does not eliminate it: one round spans roughly 8 to 10
minutes at the defaults, so the first and last angles of a round are still
sampled further apart in time than the middle ones, and non-linear traffic
variation is not balanced at all.

### Measurement, reporting, session, and scan

`--noise-percentile`, `--probe-percentile`, `--open-db`, `--close-db`,
`--min-event-ms`, `--merge-gap-ms`, `--min-valid-events`,
`--max-active-fraction`, `--carrier-prominence-db`, `--carrier-persistence`,
`--audio-guard-ms`, `--min-audio-window-ms`, `--max-clipped-fraction`,
`--max-retained-events`, `--read-timeout-factor`, `--read-timeout-slack`;
`--report-metric`, `--min-captures`, `--min-effect-db`,
`--yield-concordance-ratio`, `--noise-drift-warn-db`;
`--session-dir`, `--label`, `--resume`, `--source`, `--synthetic-snr-db`,
`--synthetic-duty`, `--no-color`, `--non-interactive`;
`--scan-start`, `--scan-end`, `--scan-channel`, `--scan-dwell`,
`--scan-passes`, `--scan-dc-exclusion`, `--scan-usable-fraction`, `--scan-top`.

`rtlangle --help` lists every one of them. `--report-metric` chooses which
ranking is listed **first** and nothing else: both are always computed and
always printed, and nothing downstream consumes the choice.

`--source` accepts `rtlsdr`, `synthetic`, or `file:<path.cu8>`. A `.cu8` file is
replayed **sequentially across the whole session** — visit 2 continues where
visit 1 stopped — and end of file is never looped, because looping would
fabricate replicates that do not exist. An optional `<path>.cu8.json` sidecar
carrying `sample_rate_hz` and `center_hz` is read if present; one that disagrees
with the configuration is an error naming both values, never a silent override.

## What is stored

```
sessions/20260819-143000-airband/     mode 0700
  session.json      the canonical record, atomically replaced       mode 0600
  measurements.csv  a derived export, atomically regenerated        mode 0600
  report.txt        written at finalisation and on pause            mode 0600
```

`session.json` is the single source of truth. The other two are **derived** and
can be regenerated from it at any time: corrupting, truncating, or deleting
either cannot lose data. `rtlangle report <dir>` reproduces the report from
`session.json` alone.

Every capture attempt is persisted, including failures, and nothing is deleted
or overwritten. An attempt superseded by a retry stays in the record for audit
and never contributes to a ranking.

The commit point is a single `rename()`. Everything before it changes nothing
anyone can observe, and everything after it is bookkeeping on a commit that has
already happened. Session directories are created, never reused, and every write
goes through a directory descriptor held open for the session's lifetime.

`--resume <dir>` reopens a paused session, or one a crash left behind. It
refuses a completed or an aborted session — both are statements about a finished
experiment, and reopening either would invalidate that statement silently — and
it refuses any attempt to change a parameter the experiment is defined by, since
a resumed session whose gain or frequency differed would silently mix two
experiments.

The CSV column for discarded samples is `host_dropped_samples`, and the name is
deliberate: see the last entry in the next section.

## What this tool cannot do

- **It cannot find a transmitter's direction.** It compares reception quality
  between antenna orientations under the traffic that happened to occur.
- **Its SNR is conditional on detection, and the condition moves with the
  antenna.** This is the most important limitation here; the estimand section
  above states it in full. Nothing in Phase 1 removes the conditioning.
- **It cannot match transmissions across angles.** Captures at two angles happen
  at different times, so the same transmission is never observed twice.
- **It cannot identify a transmitter or an aircraft.** The carrier offset it
  records is a comparability diagnostic only: several aircraft share a channel
  and offsets drift with temperature and Doppler.
- **It cannot separate angle from time.** Alternating rounds reduce the linear
  component of a time trend; they do not remove non-linear traffic variation.
- **It cannot measure below its squelch.** Nothing weaker than about 4.74 dB at
  default settings is ever detected.
- **It cannot rescue a fully occupied capture.** If traffic never keys down and
  is not a constant carrier, the probe floor is contaminated and the capture may
  be accepted with an overstated floor.
- **It cannot correct for you.** Body position, cable routing, and height changes
  between angles enter the measurement as if they were the angle.
- **It performs no statistical test and reaches no conclusion.** The ranking
  shows which angle measured highest under the traffic that occurred. It carries
  no p-value and no claim that the ordering would repeat. Two angles whose
  spreads overlap are, as far as this tool is concerned, simply two
  measurements.
- **Its spreads are descriptive.** The min-max and interquartile ranges say how
  much the contributing captures varied. They are not interval estimates.
- **W6 can fire when nothing is wrong.** It compares median yields against a
  fixed ratio with no uncertainty allowance.
- **It is not calibrated in absolute terms.** dBFS values are relative to this
  receiver's ADC full scale at this gain, not to any physical field strength.
- **It reports host-side sample drops, not device overruns.** `librtlsdr`
  exposes no device-level overflow counter, so a true USB overflow reaches this
  program only as an unobservable discontinuity in the sample stream. What can
  honestly be counted is how many samples *this program* discarded because its
  own bounded buffer was full while the reader was busy, and that is what
  `host_dropped_samples` counts. No column, message, or document calls it
  anything else.

## What this tool will not tell you

No output of this program states or implies:

- that a result indicates the physical direction of a transmitter;
- that any angle is the right one, the correct one, or a winner — with no gate,
  threshold, or round count that would make it permissible;
- that a ranking has been resolved by any statistical procedure, or that any
  difference between angles has been tested;
- that a spread printed beside a score is an interval estimate;
- that a higher score means an angle receives better, without the
  detection-yield qualification above;
- that a carrier offset identifies a specific transmitter or aircraft;
- that alternating visit order removes the angle-time confound;
- that `host_dropped_samples` counts device or USB overruns;
- that hardware validation passed when the hardware test was skipped.

Every report closes with the same sentence, in every session without exception:

> Exploratory ranking only. These are the reception qualities measured at each
> angle under the traffic that happened to occur. This tool does not determine
> which angle is best, and it is NOT a measurement of the transmitter's physical
> direction.

There is no second headline and no condition under which a different one is
printed.

## Freeing a busy device

Only one program can hold an RTL-SDR at a time. If `rtlangle devices` reports
nothing, or a run fails with "device 0 is busy":

1. Close the SDR application normally — SDR++, gqrx, and dump1090 are the usual
   holders.
2. If you do not know what is holding it, find out:

   ```bash
   fuser -v /dev/bus/usb/*/*
   # or
   sudo lsof /dev/bus/usb/*/* 2>/dev/null
   ```

   Then stop **that specific process**. Do not use a broad pattern-matching
   kill: it can take unrelated processes with it.

## Legal note

Airband reception legality varies by jurisdiction. You are responsible for
confirming that receiving aeronautical voice traffic is lawful where you are.
`rtlangle` stores derived metrics — event times, powers, and signal-to-noise
ratios — and does not store audio or raw samples.

## Safety

The RTL2832U and R828D hardware has **no transmit path**. There is no RF
emission capability, and this statement is a fact about the hardware rather than
a property this program enforces. Receiving is passive and does not radiate.

The single outbound electrical path is the bias-tee DC feed on the coax centre
conductor, intended for powering an external low-noise amplifier. `rtlangle`
enables it only when you pass `--bias-tee`, and in an interactive session it
asks first, naming the risk: DC on the feedline can damage a passive antenna or
a receiver that is not expecting it.
