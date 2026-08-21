#include "angle/manual_angle_provider.h"

#include "core/angle_math.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace rtlangle {
namespace {

std::string degrees(double v) {
  std::ostringstream os;
  os.precision(3);
  os << std::defaultfloat << v;
  return os.str();
}

}  // namespace

std::optional<double> ManualAngleProvider::ask_actual_angle(double planned_deg) {
  for (;;) {
    const auto answer =
        terminal_.prompt_line("Actual angle reached, in degrees", degrees(planned_deg));
    if (!answer.has_value()) return std::nullopt;   // the input ended

    char* end = nullptr;
    const double value = std::strtod(answer->c_str(), &end);
    if (end == nullptr || *end != '\0' || answer->empty()) {
      terminal_.warn("\"" + *answer + "\" is not a number. Enter the angle in degrees.");
      continue;
    }
    if (!std::isfinite(value) || value < 0.0 || value > 360.0) {
      terminal_.warn("The angle must be a finite value within 0 to 360 degrees; \"" +
                     *answer + "\" is not.");
      continue;
    }

    const double deviation = circular_distance_deg(value, planned_deg);
    if (deviation > cfg_.max_angle_deviation_deg) {
      const bool confirmed = terminal_.confirm(
          "That is " + degrees(deviation) + " degrees from the planned " +
              degrees(planned_deg) + " degrees. Accept it?",
          false);
      // A declined confirmation re-prompts rather than accepting, which is what
      // catches typing 9 for 90.
      if (!confirmed) continue;
    } else if (deviation > cfg_.angle_deviation_warn_deg) {
      terminal_.warn("That is " + degrees(deviation) + " degrees from the planned angle; the "
                     "report will flag this capture.");
    }
    return value;
  }
}

AngleOutcome ManualAngleProvider::request(double planned_deg, int attempt) {
  AngleOutcome out;

  std::ostringstream title;
  title << "Position the antenna at " << degrees(planned_deg) << " degrees";
  if (attempt > 1) title << "  (attempt " << attempt << ")";
  terminal_.heading(title.str());

  if (!cfg_.angle_reference.empty()) {
    terminal_.info("Angles are measured from your 0 degree mark: " + cfg_.angle_reference);
  }
  terminal_.info(
      "Rotate the marked dipole arm to the angle above, clockwise seen from above, then "
      "stand clear of the antenna before continuing.");

  // "Retry THIS angle": there is no backtracking capability, and wording that
  // said "previous" would imply one.
  const std::array<ui::MenuItem, 5> items = {
      ui::MenuItem{"Continue", "capture at this angle"},
      ui::MenuItem{"Retry this angle", "re-position and capture again"},
      ui::MenuItem{"Skip this angle", "record no result for this visit"},
      ui::MenuItem{"Add a note", "attach a note to this capture"},
      ui::MenuItem{"Quit and save", "stop here and write a partial report"},
  };

  for (;;) {
    const int choice = terminal_.menu("What next?", items, 0);
    switch (choice) {
      case 0: {
        const auto actual = ask_actual_angle(planned_deg);
        if (!actual.has_value()) {
          out.cmd = AngleOutcome::Cmd::Quit;
          return out;
        }
        out.cmd = AngleOutcome::Cmd::Proceed;
        out.actual_deg = *actual;
        return out;
      }
      case 1:
        out.cmd = AngleOutcome::Cmd::Retry;
        out.actual_deg = planned_deg;
        return out;
      case 2:
        out.cmd = AngleOutcome::Cmd::Skip;
        out.actual_deg = planned_deg;
        return out;
      case 3: {
        const auto note = terminal_.prompt_line("Note for this capture", "");
        if (note.has_value() && !note->empty()) {
          out.note = note->substr(0, 500);   // the recorded note limit
          terminal_.info("Note recorded.");
        }
        continue;   // back to the menu; a note is not an outcome
      }
      case 4:
      default:
        // Cancel is the same decision as Quit: there is nothing else it could
        // mean at a positioning prompt.
        out.cmd = AngleOutcome::Cmd::Quit;
        out.actual_deg = planned_deg;
        return out;
    }
  }
}

}  // namespace rtlangle
