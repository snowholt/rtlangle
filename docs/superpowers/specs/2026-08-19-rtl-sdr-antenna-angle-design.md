# rtlangle — Phase 1 Antenna-Angle Experiment (Design)

Date: 2026-08-19
Status: Approved for implementation
Scope: Phase 1 only — receive-only CLI, manual antenna positioning.

## 1. Purpose

`rtlangle` measures how well an RTL-SDR receives AM airband transmissions as a
function of the operator's dipole antenna orientation. The operator rotates the
antenna by hand through a planned sequence of angles; at each angle the tool
captures IQ samples, detects real transmissions, computes reception-quality
metrics, and stores the result. At the end it ranks the angles.

The tool reports the **highest measured reception quality under this
experiment**. It does not and cannot report the physical direction of a
transmitter.

### 1.1 Non-goals for Phase 1

Explicitly out of scope: any GUI, servo or ESP32 control, serial protocols,
machine-learning or AI denoising, and placeholder implementations. A future
`SerialServoAngleProvider` must be addable without changing DSP, metrics, or
experiment logic; nothing beyond that interface seam is built now.

### 1.2 Transmit safety

The RTL-SDR (RTL2832U + R828D) has no transmit path in hardware. The only
outbound energy the device can produce is the bias-tee DC feed on the coax
centre conductor, intended for powering an external LNA. `rtlangle` leaves the
bias tee **off** by default and enables it only when `--bias-tee` is passed
explicitly, printing a warning when it does. This is documented in the README
rather than enforced by a runtime check, because there is no transmit path to
guard.

## 2. Target environment

Verified on the development machine on 2026-08-19:

| Component | Version | Notes |
|---|---|---|
| OS | Ubuntu 24.04.4 LTS | |
| Compiler | g++ 13.3.0 | C++20 |
| CMake | 3.28.3 | Ninja generator available (1.11.1) |
| librtlsdr | 0.6.0-128-g240b | rtl-sdr-blog fork; verified `RTL-SDR Blog V4 Detected` present in the shared object |
| libusb | 1.0.27 | |
| FFTW3 | 3.3.10 | double precision |
| Device | RTL-SDR Blog V4 (`0bda:2838`, R828D tuner) | |

The rtl-sdr-blog fork matters: the Blog V4 needs its tuner gain table, and gain
is the controlled variable of the whole experiment. On startup `rtlangle`
prints the driver version string and the device name so a wrong driver is
visible in the session record.

## 3. Architecture

Dependencies point downward only. `experiment/` knows four interfaces and no
concrete class; that is what makes the end-to-end test hardware-free and the
future servo provider a drop-in.

```
app/         main.cpp, CliParser, ScanCommand, DeviceCheckCommand
  |
ui/          ITerminalUi <- AnsiTerminalUi | ScriptedTerminalUi
             Menu, ReportRenderer
  |
experiment/  ExperimentController   (the angle -> capture -> measure -> persist loop)
  |
  +-- angle/    IAngleProvider  <- ManualAngleProvider | FixedAngleProvider
  +-- source/   ISampleSource   <- RtlSdrSource | SyntheticSource | IqFileSource
  +-- persist/  SessionStore, CsvAppender, SessionLoader
  +-- metrics/  NoiseFloorEstimator, EventDetector, SnrEstimator
        |
      dsp/     OffsetMixer, FirDecimator, ChannelFilter, AmDemodulator, WelchPsd
        |
      core/    Config, AngleSequence, Measurement, EventRecord, Decibels
```

### 3.1 `core/`

Plain value types and free functions. No I/O, no threads, no logging.

- `Config` — the full effective configuration (see §7), a single aggregate that
  is serialised verbatim into session metadata.
- `AngleSequence` — generation and validation (see §4).
- `EventRecord`, `Measurement`, `SessionMetadata` — the persisted record shapes
  (see §8).
- `db.h` — `to_db(linear)`, `from_db(db)`, guarded against non-positive input.

### 3.2 `source/ISampleSource`

```cpp
struct SourceInfo {
  std::string driver;          // "librtlsdr 0.6.0-128-g240b" | "synthetic" | "iq-file"
  std::string device_name;     // "Generic RTL2832U OEM" etc.
  std::string serial;
  uint32_t    sample_rate_hz;
  uint32_t    tuned_center_hz; // fc + offset, what the hardware was actually told
  int         applied_gain_tenth_db;
  bool        agc_enabled;
  int         ppm;
};

class ISampleSource {
public:
  virtual ~ISampleSource() = default;
  virtual SourceInfo info() const = 0;
  // Blocking. Fills `out` with exactly `count` interleaved complex samples,
  // already converted to normalised complex<float>. Returns false on
  // unrecoverable error and sets `error`.
  virtual bool read(std::span<std::complex<float>> out, std::string& error) = 0;
  virtual void flush() = 0;    // discard buffered samples (used after settling)
};
```

`RtlSdrSource` uses `rtlsdr_read_async` on a worker thread feeding a bounded
ring buffer; `read()` drains it. Overruns increment a counter recorded in the
measurement. Byte-to-float conversion is `(x - 127.4f) / 127.5f`.

Named error handling: `usb_claim_interface error -6` maps to
`"RTL-SDR device N is busy - close SDR++, gqrx, dump1090 or other SDR software
and try again."` `rtlsdr_open` returning `-ENODEV` maps to a device-not-found
message listing the devices that were enumerated.

`SyntheticSource` generates AM-voice-like signal plus complex AWGN at a
configurable SNR, with configurable talk/silence duty cycle. `IqFileSource`
replays a `.cu8` file.

### 3.3 `dsp/`

- `OffsetMixer` — multiplies by `exp(+j 2 pi f_off t)` at the full sample rate.
  The sign is positive because the hardware is tuned *above* the signal
  (`fc + offset`), so the signal of interest appears at baseband frequency
  `-offset` and must be shifted up to DC. Phase is held in an exact integer
  modulo-`fs` accumulator feeding a 65536-entry lookup table, so it never
  drifts over a 60-second capture.
- `FirDecimator` — polyphase decimating FIR. Two stages: /8 then /4.
- `ChannelFilter` — FIR low-pass, cutoff `channel_bw_hz / 2` (default 4 kHz),
  Kaiser window, ~60 dB stopband, run at the channel rate.
- `AmDemodulator` — envelope `|x|`, DC-block (single-pole high-pass, removes the
  carrier term), then a 300–3400 Hz band-pass for the audio band.
- `WelchPsd` — FFTW-backed periodogram averaging, used by the scan command and
  by per-event carrier-offset estimation.

### 3.4 `metrics/`

`NoiseFloorEstimator`, `EventDetector`, `SnrEstimator` — see §5 and §6. All
three operate on already-framed data and have no knowledge of hardware.

### 3.5 `angle/IAngleProvider`

```cpp
struct AngleOutcome {
  enum class Cmd { Proceed, Retry, Skip, Quit };
  Cmd                        cmd;
  double                     actual_deg;   // operator estimate, or servo feedback
  std::optional<std::string> note;
};

class IAngleProvider {
public:
  virtual ~IAngleProvider() = default;
  virtual std::string_view name() const = 0;      // "manual" | future "serial-servo"
  virtual bool is_automated() const = 0;          // recorded in session metadata
  virtual AngleOutcome request(double planned_deg) = 0;
  virtual void settle(std::chrono::milliseconds) = 0;
};
```

`ManualAngleProvider` holds a reference to `ITerminalUi`. It prints the planned
angle, waits for confirmation, prompts for the actual angle (blank accepts the
planned value), and exposes the retry / skip / note / quit commands.

`FixedAngleProvider` is the non-interactive implementation: it returns
`Proceed` with `actual_deg == planned_deg` and never prompts. It exists for
`--non-interactive` replay and synthetic runs, and it is the second
implementation that proves the interface is not accidentally coupled to the
terminal. A future
servo provider fills `actual_deg` from encoder feedback, returns `Proceed`, and
touches no terminal. Neither the controller nor any DSP code changes.

### 3.6 `ui/ITerminalUi`

```cpp
class ITerminalUi {
public:
  virtual ~ITerminalUi() = default;
  virtual int  menu(std::string_view title, std::span<const MenuItem> items,
                    int initial_index) = 0;       // returns index, or -1 for cancel
  virtual std::optional<std::string> prompt_line(std::string_view label,
                                                 std::string_view default_value) = 0;
  virtual bool confirm(std::string_view question, bool default_yes) = 0;
  virtual void heading(std::string_view) = 0;
  virtual void info(std::string_view) = 0;
  virtual void warn(std::string_view) = 0;
  virtual void error(std::string_view) = 0;
  virtual void progress(std::string_view label, double fraction) = 0;
  virtual void table(const Table&) = 0;
};
```

`AnsiTerminalUi` puts the terminal into raw mode only for the duration of a
menu, restores it through an RAII guard, and installs `SIGINT`/`SIGTERM`
handlers that restore termios and re-show the cursor before exiting — a Ctrl-C
must never leave the operator with a broken shell. Colour is emitted only when
`isatty(STDOUT_FILENO)` is true and `NO_COLOR` is unset; `--no-color` forces it
off. Arrow keys, `j`/`k`, Enter, `q`, and Escape are recognised.

`ScriptedTerminalUi` consumes a queue of pre-programmed responses and records
every emitted line, so tests assert on UI output without a terminal.

### 3.7 `experiment/ExperimentController`

Owns the state machine. Constructed with `ISampleSource&`, `IAngleProvider&`,
`ITerminalUi&`, `SessionStore&`, and a `Config`. For each `(round, angle)` in
the visit plan:

1. `ui.heading()` the planned angle, round, and remaining count.
2. `provider.request(planned)` — positioning, actual angle, commands.
3. On `Proceed`: `provider.settle(settle_ms)`, then `source.flush()`.
4. Capture `duration_s` worth of samples through the DSP chain, showing progress.
5. Frame, estimate noise floor, detect events, compute per-event metrics.
6. Build a `Measurement`; render it to the terminal.
7. `store.append(measurement)` — atomic JSON rewrite plus CSV append, before
   moving on.
8. If `insufficient_data`, offer retry.

`Retry` repeats the same `(round, angle)`. `Skip` records a `skipped`
measurement with a reason and advances. `Quit` finalises the session — report
and summary are still written from whatever completed — and exits 0.

### 3.8 `persist/`

`SessionStore` writes `session.json` by serialising to `session.json.tmp` in the
same directory, `fsync`ing, then `rename()`ing over the target. `rename()`
within a directory is atomic on Linux, so a crash mid-write can never leave a
truncated session file. `measurements.csv` is opened in append mode and flushed
after each row. `SessionLoader` reads `session.json` and returns the set of
completed `(round, planned_angle)` pairs plus the stored `Config`.

## 4. Angle generation and validation

Default sequence: `0, 15, 30, 45, 60, 75, 90`.

Generated from `--start-deg` (default 0), `--end-deg` (default 90),
`--step-deg` (default 15), or given directly with `--angles "0,30,60,90"`
(which overrides the three range flags).

Validation rules, each producing a specific error message:

- `step_deg` must be finite and `> 0`.
- `start_deg` and `end_deg` must be finite and in `[0, 360]`.
- `end_deg` must be `>= start_deg`.
- The generated count must be in `[2, 180]`.
- Explicit `--angles` values must be finite, in `[0, 360]`, and are deduplicated
  (within 1e-6) and sorted ascending before use.
- Floating-point accumulation is avoided: the i-th angle is
  `start + i * step`, and the endpoint is included when
  `end - last < step - 1e-9`.
- If any angle exceeds 180 degrees, a warning is printed that a dipole pattern
  is symmetric about 180 degrees, so angles above it duplicate coverage. This is
  a warning, not an error.

### 4.1 Visit order

`--order forward | reverse | alternating | random`.

- `forward` — ascending, every round.
- `reverse` — descending, every round.
- `alternating` — round 1 ascending, round 2 descending, and so on.
- `random` — shuffled per round with `std::mt19937` seeded from `--seed`
  (default: a value drawn once from `std::random_device` and **recorded in the
  session metadata**, so the order is reproducible after the fact).

**`alternating` is the default when `--rounds > 1`; `forward` when `--rounds == 1`.**
A single sequential pass confounds angle with time, and airband traffic varies
minute to minute; alternating the direction each round breaks that correlation.

### 4.2 Actual-angle entry

The operator's estimate is prompted per measurement. Blank input accepts the
planned angle. Non-numeric input re-prompts. A value deviating from the planned
angle by more than `--max-angle-deviation-deg` (default 30) requires an explicit
confirmation before being accepted, guarding against typos like `9` for `90`.
Both `planned_deg` and `actual_deg` are persisted; ranking uses `planned_deg`,
and the report flags any measurement where the two differ by more than 5 degrees.

## 5. Signal chain

```
tune hardware to (center_hz + offset_tune_hz)      default offset 250 kHz
  -> u8 IQ pairs -> complex<float>, (x - 127.4)/127.5
  -> OffsetMixer  x exp(+j 2 pi offset_tune_hz t)  at 1.024 MHz
  -> FirDecimator /8   -> 128 kHz
  -> FirDecimator /4   -> 32 kHz   (channel rate)
  -> ChannelFilter low-pass +/- 4 kHz
  -> frames of 1024 samples, hop 512 (50% overlap) = 32 ms frames
```

**Offset tuning is mandatory by default.** The RTL-SDR's DC offset spur sits
exactly at the tuned centre frequency, which is exactly where the AM carrier
would otherwise land. Tuning 250 kHz away and mixing back digitally keeps the
spur out of both the signal measurement and the noise-floor estimate.
`--offset-tune-hz 0` disables it, with a warning that measurements will be
contaminated by the DC spur.

Default `--sample-rate 1024000`. The offset must satisfy
`|offset| + channel_bw/2 < sample_rate/2 * 0.9`; violating this is a
configuration error. RTL-SDR sample rates outside the driver-supported ranges
(225001–300000 and 900001–3200000 Hz) are rejected up front with a clear
message.

## 6. Metrics

### 6.1 Framing and in-channel power

For frame `k` of the channel-rate, channel-filtered complex stream `x`:

```
P[k] = (1/M) * sum_{n in frame k} |x[n]|^2         M = 1024
```

`P[k]` is in-channel power in normalised ADC units squared. It is measured over
the ±4 kHz channel only — never over the full 1.024 MHz — so the number
reflects the antenna, not the filter width.

### 6.2 Noise floor

```
N = percentile(P[0..K-1], noise_percentile)        default noise_percentile = 20
```

taken over **all frames of that one capture**. Quiet frames dominate the low
percentiles, and using the same capture means no drift between angles leaks into
the estimate. The percentile is recorded in the session metadata.

If the fraction of frames above the open threshold exceeds
`--max-active-fraction` (default 0.70), there are too few quiet frames for the
percentile to mean anything: the measurement is flagged
`noise_floor_unreliable`, excluded from ranking, and a retry is offered.

### 6.3 Event detection

Hysteresis squelch on `P[k]`, with thresholds expressed relative to `N`:

- open when `P[k] > N * from_db(open_db)` — default `open_db = 6.0`
- close when `P[k] < N * from_db(close_db)` — default `close_db = 3.0`
- adjacent candidate events separated by less than `merge_gap_ms` (default 200)
  are merged into one
- candidates shorter than `min_event_ms` (default 300) are discarded

Each surviving event records its start and end sample index, wall-clock
timestamp, and its measured carrier frequency offset (the FFT peak bin within
±2 kHz of DC, parabolically interpolated). The carrier offset is stored so that
after the fact you can check whether 0 degrees and 90 degrees were even hearing
the same aircraft — different bearings dominate at different times, and that is
the main threat to the result.

### 6.4 Channel SNR (CNR-style) — the exact calculation

For each detected event:

```
P_event  = median over the event's frames of P[k]
SNR_lin  = (P_event - N) / N
SNR_dB   = 10 * log10(SNR_lin)
```

The subtraction happens **before** the division, in the linear power domain, and
only then is the result converted to decibels. This matters: `P_event / N` is
`SNR + 1`, which at low SNR is wrong by several decibels and is precisely the
"total channel power" quantity that must not be called SNR. If
`P_event <= N` the event is discarded as invalid rather than producing a NaN or
a negative-infinity decibel value.

Because the channel filter passes the AM carrier together with its sidebands,
this quantity is a carrier-plus-sideband-to-noise ratio in the channel
bandwidth — a CNR. It is labelled `channel_snr_db` throughout, and the README
states plainly that it is a CNR, not a post-demodulation audio SNR.

### 6.5 Audio SNR

The envelope-demodulated, DC-blocked, 300–3400 Hz band-passed audio stream is
framed identically. With `A[k]` the per-frame audio power:

```
N_a       = percentile(A[all frames], noise_percentile)
S_a       = median over the event's frames of A[k]
SNRa_lin  = (S_a - N_a) / N_a
SNRa_dB   = 10 * log10(SNRa_lin)
```

Same guard: events with `S_a <= N_a` yield no audio SNR, and the event is marked
`audio_snr_valid = false` while remaining valid for the channel metric.

### 6.6 Which metric ranks

Both metrics are computed and stored for every event, always.
`--rank-metric channel | audio | both`, **default `both`**. In `both` mode the
report produces two independent rankings side by side and declares a single
"best measured angle" only when the two rankings agree on the top angle. When
they disagree it says so explicitly and prints both. Disagreement is reported as
information, not hidden behind a default.

### 6.7 Per-angle aggregation

Events are pooled across all rounds for a given planned angle. Reported per
angle:

- planned angle, and the mean and spread of the entered actual angles
- total capture duration and number of captures (rounds completed)
- valid event count
- median channel SNR (dB) and median audio SNR (dB)
- p10 / p25 / p50 / p75 / p90 of channel SNR
- mean signal power `P_event` (dB relative to full scale)
- estimated noise floor `N` (dB relative to full scale), per capture and pooled
- 95% confidence interval on the median
- status: `ok` | `insufficient_data` | `noise_floor_unreliable` | `skipped`
- operator notes

**Confidence interval.** A distribution-free interval from binomial order
statistics: for `n` events sorted ascending, the interval is
`[x_(k), x_(n-k+1)]` with the largest `k >= 1` such that the binomial tail
probability gives at least 95% coverage. No such `k` exists for `n < 6`, so for
`n` in `[min_valid_events, 5]` the report prints the interquartile range instead
and flags the angle `low_confidence`.

An angle with fewer than `--min-valid-events` (default 3) valid events is
`insufficient_data`: it is excluded from ranking and never assigned a fabricated
SNR. During the run the operator is offered a retry when this happens.

### 6.8 Comparability warnings

The final report emits a warning for each of the following that holds:

- any angle is `insufficient_data` or `noise_floor_unreliable`
- the pooled noise floors across angles span more than
  `--noise-drift-warn-db` (default 3.0) dB — the RF environment changed during
  the session, so the angles are not cleanly comparable
- the confidence intervals of the top two angles overlap — the difference is not
  statistically resolved by this data
- valid event counts across angles differ by more than a factor of 3
- the applied tuner gain differed from the requested gain
- `--rounds 1` was used — a single pass confounds angle with time
- any `actual_deg` deviated from its `planned_deg` by more than 5 degrees
- sample overruns were recorded during any capture

### 6.9 Result wording

The headline in the terminal report, `report.txt`, and the README reads:

> Highest measured reception quality under this experiment.
> This is NOT a measurement of the transmitter's physical direction.

## 7. Configuration

Precedence, lowest to highest: built-in defaults, then `--config <file.json>`,
then command-line flags, then interactive edits in the menu. The fully resolved
`Config` is written into `session.json` so a session is reproducible.

| Flag | Default | Meaning |
|---|---|---|
| `--device` | 0 | RTL-SDR device index |
| `--freq` | *no default* | Centre frequency in Hz; accepts `118.1M`, `121500k`. Required when starting a session from the command line; the interactive `Start new experiment` menu prompts for it when it is absent |
| `--sample-rate` | 1024000 | Hz |
| `--gain` | `max` | Tenths of dB, or `max`; snapped to a supported value |
| `--agc` | off | RTL2832 automatic gain control |
| `--allow-agc` | off | Required to accept `--agc`; without it, `--agc` is a configuration error |
| `--ppm` | 0 | Frequency correction |
| `--offset-tune-hz` | 250000 | 0 disables, with a warning |
| `--channel-bw` | 8000 | Hz, total channel width |
| `--channel-rate` | 32000 | Hz, rate after decimation. `--sample-rate` must be an integer multiple of it, at least 2x |
| `--bias-tee` | off | Explicit opt-in, warns when enabled |
| `--duration` | 60 | Seconds of capture per angle |
| `--settle` | 3 | Seconds after positioning, before capture |
| `--start-deg` / `--end-deg` / `--step-deg` | 0 / 90 / 15 | Angle range |
| `--angles` | *(unset)* | Explicit list, overrides the range |
| `--rounds` | 1 | Passes over the angle set |
| `--order` | `alternating` if rounds>1 else `forward` | Visit order |
| `--seed` | random, recorded | For `--order random` |
| `--rank-metric` | `both` | `channel`, `audio`, or `both` |
| `--noise-percentile` | 20 | Percentile for the noise floor |
| `--open-db` / `--close-db` | 6.0 / 3.0 | Squelch hysteresis above `N` |
| `--min-event-ms` | 300 | Minimum event duration |
| `--merge-gap-ms` | 200 | Merge events closer than this |
| `--min-valid-events` | 3 | Below this, `insufficient_data` |
| `--max-active-fraction` | 0.70 | Above this, `noise_floor_unreliable` |
| `--noise-drift-warn-db` | 3.0 | Cross-angle noise-floor spread warning |
| `--max-angle-deviation-deg` | 30 | Confirm before accepting a distant actual angle |
| `--session-dir` | `sessions/` | Root for session directories |
| `--label` | `""` | Appended to the session directory name |
| `--resume <dir>` | — | Continue an existing session |
| `--source` | `rtlsdr` | `rtlsdr`, `synthetic`, or `file:<path.cu8>` |
| `--synthetic-snr-db` | 12 | For `--source synthetic` |
| `--save-iq` | off | Write raw captures under `raw/` |
| `--no-color` | off | Disable ANSI colour |
| `--non-interactive` | off | Never prompt; every required value must come from flags or `--config`. Because `ManualAngleProvider` cannot function without prompts, this flag is a configuration error unless `--source` is `synthetic` or `file:` (where a `FixedAngleProvider` supplies the planned angle as the actual angle). Used by the automated tests and by replay runs |

### 7.1 Gain handling

Gain is the controlled variable. `rtlangle` calls `rtlsdr_get_tuner_gains()`,
snaps the requested value to the nearest supported entry, calls
`rtlsdr_set_tuner_gain_mode(dev, 1)` for manual gain, and explicitly disables the
RTL2832 AGC. The **applied** gain — read back, not requested — is what goes into
session metadata. If it differs from the requested value, a warning is printed at
session start and the discrepancy is recorded. `--agc` is refused without
`--allow-agc`, because a changing gain would invalidate every angle comparison.

## 8. Persistence

```
sessions/20260819-143000-airband/
  session.json        rewritten atomically after every measurement
  measurements.csv    appended after every measurement
  report.txt          written at session end (and on quit)
  raw/                only with --save-iq
```

### 8.1 `session.json`

```jsonc
{
  "schema_version": 1,
  "session_id": "20260819-143000-airband",
  "started_utc": "2026-08-19T14:30:00Z",
  "finished_utc": "2026-08-19T15:12:44Z",     // null while running
  "tool_version": "rtlangle 0.1.0",
  "config": { /* the full resolved Config, every field from §7 */ },
  "source": {
    "driver": "librtlsdr 0.6.0-128-g240b",
    "device_name": "Generic RTL2832U OEM",
    "serial": "00000001",
    "sample_rate_hz": 1024000,
    "tuned_center_hz": 118350000,
    "requested_gain_tenth_db": 496,
    "applied_gain_tenth_db": 496,
    "agc_enabled": false,
    "ppm": 0
  },
  "angle_provider": { "name": "manual", "automated": false },
  "plan": { "angles_deg": [0,15,30,45,60,75,90], "rounds": 2, "order": "alternating", "seed": 1234567 },
  "measurements": [ /* see below */ ],
  "summary": { /* written at finalisation; null while running */ }
}
```

A `Measurement` entry:

```jsonc
{
  "index": 0,
  "round": 1,
  "planned_deg": 0.0,
  "actual_deg": 0.0,
  "started_utc": "2026-08-19T14:30:07Z",
  "duration_s": 60.0,
  "status": "ok",                    // ok | insufficient_data | noise_floor_unreliable | skipped
  "skip_reason": null,
  "frames_total": 3745,
  "frames_active": 412,
  "active_fraction": 0.110,
  "noise_floor_dbfs": -62.41,
  "sample_overruns": 0,
  "valid_event_count": 7,
  "channel_snr_db": { "median": 18.2, "p10": 12.9, "p25": 15.4, "p75": 21.0, "p90": 23.8,
                      "ci95_low": 15.4, "ci95_high": 21.0, "ci_method": "order-statistic" },
  "audio_snr_db":   { "median": 14.6, "p10": 9.1, "p25": 11.8, "p75": 17.2, "p90": 19.9,
                      "ci95_low": 11.8, "ci95_high": 17.2, "ci_method": "order-statistic" },
  "signal_power_dbfs": -44.2,
  "events": [
    { "start_s": 3.42, "duration_s": 2.15, "utc": "2026-08-19T14:30:10Z",
      "carrier_offset_hz": -213.4, "signal_power_dbfs": -44.0,
      "channel_snr_db": 18.4, "audio_snr_db": 14.9, "audio_snr_valid": true }
  ],
  "note": "cable routed along the balcony rail"
}
```

### 8.2 `measurements.csv`

One row per measurement, header written on creation:

```
index,round,planned_deg,actual_deg,started_utc,duration_s,status,valid_events,
channel_snr_median_db,channel_snr_p25_db,channel_snr_p75_db,
audio_snr_median_db,signal_power_dbfs,noise_floor_dbfs,active_fraction,
sample_overruns,note
```

Notes are quoted and internal quotes doubled, per RFC 4180.

### 8.3 Resume

`--resume <dir>` loads `session.json`, takes the stored `Config` (CLI flags other
than `--resume` are then rejected, so a resumed session cannot silently change
gain or frequency), computes the set of completed `(round, planned_deg)` pairs,
and continues with the remainder of the plan in the stored order. Measurements
with status `skipped` count as completed. A resumed session appends to the same
CSV and rewrites the same JSON.

## 9. Terminal interface

Main menu, arrow-key navigable:

```
  Start new experiment
  Resume session
  Scan airband (find active channels)
  Device check / gain table
  Replay synthetic session
  Quit
```

`Scan airband` sweeps 118–137 MHz in 25 kHz channels for a configurable number
of passes, computing a Welch PSD per dwell and reporting, per channel, the
fraction of dwells in which power exceeded the band noise floor by `open_db`.
It prints the busiest channels sorted by activity so the operator can pick a
frequency that actually carries traffic. It is a helper, not part of a
measurement session, and writes nothing into `sessions/`.

`Device check` opens the device, prints the driver string, device name, serial,
supported gain table, and the tuner type, then closes. This is the diagnostic to
run when `usb_claim_interface error -6` appears.

During a measurement the display shows the planned angle, round, a capture
progress bar, and on completion a compact per-angle metric block. The final
report is a colour-aligned table plus the ranking, the warnings from §6.8, and
the §6.9 wording.

## 10. Testing

Framework: doctest, fetched with CMake `FetchContent` (header-only). JSON:
nlohmann/json, likewise. Both are `find_package`-first so system copies are used
when present. Tests are built by default and can be disabled with
`-DRTLANGLE_BUILD_TESTS=OFF`. Every test in this section runs without RTL-SDR
hardware.

### 10.1 Angle generation and validation

Default sequence equals `0,15,30,45,60,75,90`; endpoint inclusion at
non-dividing steps; explicit `--angles` dedup and sort; each validation rule
from §4 produces its specific error; `alternating` order reverses on even
rounds; `random` with a fixed seed is reproducible and is a permutation of the
angle set.

### 10.2 SNR estimator — synthetic signals with known SNR

**Test A, estimator in isolation, tolerance 0.5 dB.** Feed `SnrEstimator`
channel-rate frames constructed from complex Gaussian noise of known variance,
with a known signal added over a known subset of frames. Assert the recovered
`channel_snr_db` matches `10*log10(Ps/Pn)` within 0.5 dB. The tolerance covers
the known small downward bias of the 20th-percentile noise estimate (about
0.12 dB for 1024-sample frames) plus frame-power sampling noise.

**Test B, full chain, tolerance 1.5 dB.** Generate at the full 1.024 MHz rate: a
complex AM carrier at the tuning offset, tone-modulated at 1 kHz with a known
modulation depth, plus complex AWGN. Push it through the real
`OffsetMixer -> FirDecimator -> FirDecimator -> ChannelFilter` chain.

The reference SNR is obtained by measurement, not by assuming the chain is
transparent: the same signal-only stream and the same noise-only stream are each
pushed through the identical chain, and their mean output powers give
`SNR_true = P_signal_out / P_noise_out`. This is not circular — the reference
uses a direct mean over the whole separated stream, while the estimator under
test uses framing, hysteresis event detection, a percentile noise floor, and a
median. The test therefore exercises the estimator's statistics and the chain's
linearity, and it correctly accounts for the filter cascade's noise-equivalent
bandwidth without hand-derived constants.

Swept at input SNRs of 0, 5, 10 and 20 dB. Additionally assert that the measured
differences between adjacent sweep points track the true differences within
1.0 dB, so a constant bias cannot hide a scale error.

**Test C, negative control.** Pure noise, no signal: zero valid events, status
`insufficient_data`, and no SNR value emitted.

**Test D, the forbidden metric.** At a true SNR of 0 dB, assert the reported
value is within tolerance of 0 dB and specifically **not** within 0.5 dB of
3.01 dB, which is what `10*log10(P_event/N)` would produce. This test fails
loudly if anyone ever removes the subtraction.

**Test E, audio SNR.** Known modulation depth and noise level through the
demodulator; assert the audio SNR is recovered within 2.0 dB and that
`audio_snr_valid` is false when the audio band contains only noise.

### 10.3 Event detection

Synthetic frame-power sequences with bursts at known positions: exact event
count and boundaries; a gap shorter than `merge_gap_ms` merges two bursts into
one; a burst shorter than `min_event_ms` is discarded; hysteresis prevents a
burst hovering between the open and close thresholds from fragmenting; an
all-active capture trips `noise_floor_unreliable`.

### 10.4 Persistence and resume

Round-trip a session through `SessionStore` and `SessionLoader` with every field
preserved; after each append `session.json` parses and no `.tmp` file remains;
CSV row count and header match; notes containing commas, quotes and newlines
survive; resume returns exactly the uncompleted `(round, angle)` pairs, treats
`skipped` as complete, appends rather than truncating, and rejects conflicting
CLI overrides.

### 10.5 End-to-end integration

`SyntheticSource` produces AM-voice-like bursts — a multi-tone envelope with
realistic talk/silence duty cycle — at a different SNR per angle, with the
strongest angle chosen by the test. `ScriptedTerminalUi` supplies the
keystrokes; `ManualAngleProvider` and the real `ExperimentController`,
`SessionStore` and report renderer are used unmodified. Assertions: the session
completes, `session.json`, `measurements.csv` and `report.txt` all exist and are
mutually consistent, the ranking puts the seeded strongest angle first, the
report contains the §6.9 wording, and a scripted `Quit` partway through still
produces a valid report from the completed subset.

### 10.6 Hardware smoke test

`ctest -L hardware` runs a receive-only test that opens device 0, sets gain and
sample rate, reads one second of samples, asserts a plausible non-constant
sample distribution, and closes. It is excluded from the default test run and
skips with a clear message when no device is present. It never enables the bias
tee. If it cannot run, the README and the completion report state plainly that
hardware validation is pending rather than implying it passed.

## 11. Build

CMake 3.24+, C++20, Ninja or Make. `find_package(PkgConfig)` locating
`librtlsdr` and `fftw3`, `find_package(Threads)`. Targets: `rtlangle_core`
(static library holding everything except `app/`), `rtlangle` (the executable),
and `rtlangle_tests`. Warnings: `-Wall -Wextra -Wpedantic`. `-DRTLANGLE_BUILD_TESTS`
defaults to `ON`. Absence of `librtlsdr` is a configure-time error naming the
Ubuntu package, but `-DRTLANGLE_WITH_RTLSDR=OFF` builds the synthetic-only tool
and the full non-hardware test suite.

## 12. Deliverables

Source and CMake configuration; the test suite of §10; `README.md` with
dependencies, build, run and experiment instructions plus worked CLI examples;
`docs/architecture.md`; and a committed example synthetic session directory
showing real output shapes.
