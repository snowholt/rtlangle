#pragma once

#include "core/angle_math.h"

#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rtlangle {

// Which of the two descriptive rankings the report lists first. Display only:
// both are always computed and always printed (spec section 10.3), and nothing
// downstream of the report consumes the choice.
enum class Metric { Channel, Audio };
std::optional<Metric> parse_metric(std::string_view);
std::string_view to_string(Metric);

// ---------------------------------------------------------------------------
// Config — spec section 7 is the authoritative field list. Every field here has
// a CLI flag, a bound, a resume classification, and a per-command scope, and a
// single test enumerates the serialised keys once and drives all three tables
// from that enumeration, so a field added without one of them fails the suite.
// ---------------------------------------------------------------------------
struct Config {
  // 7.1 Receiver
  int                          device_index = 0;
  std::optional<std::uint32_t> center_hz;                 // required by `run`
  std::uint32_t                sample_rate_hz = 1024000;
  std::optional<int>           gain_tenth_db;             // nullopt means "max"
  int                          ppm = 0;
  std::int64_t                 offset_tune_hz = 250000;
  std::uint32_t                channel_bw_hz = 8000;
  std::uint32_t                channel_rate_hz = 32000;
  bool                         bias_tee = false;

  // 7.2 Experiment
  double              duration_s = 60.0;
  double              settle_s = 3.0;
  double              start_deg = 0.0;
  double              end_deg = 90.0;
  double              step_deg = 15.0;
  std::vector<double> angles_deg;                          // overrides the range
  int                 rounds = 2;
  VisitOrder          order = VisitOrder::Alternating;
  std::uint64_t       seed = 0;                            // drawn and recorded when absent
  std::string         angle_reference;
  std::string         setup_note;
  double              max_angle_deviation_deg = 30.0;
  double              angle_deviation_warn_deg = 5.0;

  // 7.3 Measurement
  double noise_percentile = 20.0;
  double probe_percentile = 5.0;          // default min(5, noise_percentile)
  double open_db = 6.0;
  double close_db = 3.0;
  double min_event_ms = 300.0;
  double merge_gap_ms = 200.0;
  int    min_valid_events = 3;
  double max_active_fraction = 0.70;
  double carrier_prominence_db = 10.0;
  double carrier_persistence = 0.90;
  double audio_guard_ms = 50.0;
  double min_audio_window_ms = 100.0;
  double max_clipped_fraction = 1e-4;
  int    max_retained_events = 32;
  double read_timeout_factor = 1.5;
  double read_timeout_slack_s = 5.0;

  // 7.4 Reporting thresholds. Every field here sets the threshold at which the
  // report SAYS something. None of them changes what is measured or ranked.
  Metric report_metric = Metric::Channel;
  int    min_captures_advisory = 4;
  double min_effect_db = 1.0;
  double yield_concordance_ratio = 0.90;
  double noise_drift_warn_db = 3.0;

  // 7.5 Session, source, and output
  std::string session_root = "sessions";
  std::string label;
  std::string resume_dir;
  std::string source_spec = "rtlsdr";     // rtlsdr | synthetic | file:<path.cu8>
  double      synthetic_snr_db = 12.0;
  double      synthetic_duty = 0.25;
  bool        no_color = false;
  bool        non_interactive = false;

  // 7.6 Scan
  std::uint32_t scan_start_hz = 118000000;
  std::uint32_t scan_end_hz = 136975000;
  std::uint32_t scan_channel_hz = 25000;
  double        scan_dwell_ms = 250.0;
  int           scan_passes = 4;
  std::uint32_t scan_dc_exclusion_hz = 30000;
  double        scan_usable_fraction = 0.80;
  int           scan_top_n = 20;
};

struct ValidationError {
  std::string field;
  std::string message;
};

// ---------------------------------------------------------------------------
// The field registry. One entry per serialised Config key, and the single
// source of the resume classification and the per-command scope.
// ---------------------------------------------------------------------------

// Spec section 11.5 step 6.
enum class FieldClass { ExperimentDefining, ReportOrOperational, PathDetermined };

// Spec section 12.1.
enum class Command { Run, Scan, Devices, Report };
std::optional<Command> parse_command(std::string_view);
std::string_view to_string(Command);

// Spec section 7.9. Required and Used are accepted; anything a command does not
// use is Refused, and passing it is a usage error rather than a silent no-op.
enum class FieldScope { Required, Used, Refused };

enum class FieldType {
  Int, UInt32, UInt64, Int64, Double, Bool, String, Path,
  OptionalUInt32,   // center_hz
  GainOrMax,        // gain_tenth_db
  DoubleList,       // angles_deg
  OrderEnum,        // order
  MetricEnum,       // report_metric
};

struct FieldSpec {
  std::string_view key;    // the JSON key, and the name every message uses
  std::string_view flag;   // the CLI flag, including the leading dashes
  FieldType        type = FieldType::Double;
  FieldClass       resume = FieldClass::ExperimentDefining;
};

// Every serialised Config key, in declaration order.
std::span<const FieldSpec> field_registry();
const FieldSpec* find_field(std::string_view key);

// Exhaustive over field_registry(); a key with no entry is a programming error
// the registry test catches rather than a silent default.
FieldClass resume_class(std::string_view field);
FieldScope field_scope(std::string_view field, Command);

// A flag a command refuses is a usage error naming both the flag and the
// command (exit code 2). A field left at its default is never a scope error,
// which is why the caller passes the set of keys it actually saw.
std::vector<ValidationError> validate_for_command(const Config&,
                                                  const std::set<std::string>& explicitly_set,
                                                  Command);

// ---------------------------------------------------------------------------
// Validation and derived values
// ---------------------------------------------------------------------------

// Fields whose default is derived from another field are resolved here, once,
// before validation. Only fields the caller did not set explicitly are touched.
void apply_derived_defaults(Config&, const std::set<std::string>& explicitly_set);

// Every bound and every cross-field rule of spec section 7, each producing its
// own error naming its field and the offending value.
std::vector<ValidationError> validate(const Config&);

// Informational messages about the resolved configuration: a non-grid end
// angle, an opposed angle pair, a disabled offset tuner, a gain snapped by the
// device. Not warnings and not errors (spec sections 4.4 and 8.1).
std::vector<std::string> config_notices(const Config&);

// The sorted, de-duplicated angle list. This is the index space every visit_id
// refers to, so it is computed by one function and never re-derived.
std::vector<double> resolved_angles(const Config&);

// Spec section 7.7. The preflight arithmetic, including the cumulative rewrite
// cost that spec section 11.1.4 corrects.
struct ResourceEstimate {
  int           planned_visits = 0;
  int           attempt_ceiling = 0;       // spec section 7.7, currently 250
  double        capture_seconds = 0.0;
  std::uint64_t attempt_bytes = 0;         // 1.2 KiB + max_retained_events * 0.2 KiB
  std::uint64_t final_json_bytes = 0;      // attempt_bytes * attempt_ceiling
  std::uint64_t cumulative_bytes = 0;      // attempt_bytes * A(A+1)/2
  bool          needs_confirmation = false;  // capture_seconds > 3600
};
ResourceEstimate estimate_resources(const Config&);

// Spec section 7.7: capacity for one attempt per remaining visit is reserved
// and can never be spent on a retry, so a session can always reach its last
// visit. A retry is offered only when this returns true.
bool retry_available(int attempts_committed, int remaining_planned_visits,
                     int attempt_ceiling);

inline constexpr int kAttemptCeiling = 250;
inline constexpr int kMaxPlannedVisits = 200;
inline constexpr std::uint64_t kMaxFinalJsonBytes = 16ULL * 1024 * 1024;
inline constexpr std::uint64_t kMaxCumulativeBytes = 2ULL * 1024 * 1024 * 1024;
inline constexpr double kMaxUnconfirmedCaptureSeconds = 3600.0;

// ---------------------------------------------------------------------------
// Serialisation. The fully resolved Config is written verbatim into
// session.json, so a session is reproducible from its own record.
// ---------------------------------------------------------------------------
void to_json(nlohmann::json&, const Config&);

// Throws nlohmann::json::exception on a malformed document. Unknown keys are
// rejected with the key named rather than ignored.
void from_json(const nlohmann::json&, Config&);

// Frequency literals: "118.1M", "121500k", and bare hertz. Returns nullopt with
// the offending token echoed by the caller.
std::optional<double> parse_frequency(std::string_view);

}  // namespace rtlangle
