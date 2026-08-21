#include "source/source_factory.h"

#include "source/iq_file_source.h"
#include "source/synthetic_source.h"

#if RTLANGLE_WITH_RTLSDR
#include "source/rtlsdr_source.h"
#endif

namespace rtlangle {

std::unique_ptr<ISampleSource> make_source(const Config& cfg, std::string& error) {
  error.clear();

  if (cfg.source_spec == "synthetic") {
    SyntheticParams p;
    p.sample_rate_hz = cfg.sample_rate_hz;
    p.center_hz = cfg.center_hz.value_or(118350000);
    p.offset_hz = cfg.offset_tune_hz;
    p.gain_tenth_db = cfg.gain_tenth_db.value_or(496);
    p.ppm = cfg.ppm;
    p.snr_db = cfg.synthetic_snr_db;
    p.duty = cfg.synthetic_duty;
    p.seed = cfg.seed;
    return std::make_unique<SyntheticSource>(p);
  }

  if (cfg.source_spec.rfind("file:", 0) == 0) {
    const std::string path = cfg.source_spec.substr(5);
    auto src = std::make_unique<IqFileSource>(path, cfg, error);
    if (!src->ok()) return nullptr;
    return src;
  }

  if (cfg.source_spec == "rtlsdr") {
#if RTLANGLE_WITH_RTLSDR
    return make_rtlsdr_source(cfg, error);
#else
    error =
        "this build has no RTL-SDR support: it was configured with "
        "-DRTLANGLE_WITH_RTLSDR=OFF. Use --source synthetic or "
        "--source file:<path.cu8>, or reconfigure with -DRTLANGLE_WITH_RTLSDR=ON.";
    return nullptr;
#endif
  }

  error = "unknown source \"" + cfg.source_spec +
          "\": expected rtlsdr, synthetic, or file:<path.cu8>.";
  return nullptr;
}

std::vector<std::string> take_source_notices(const ISampleSource& source) {
  if (const auto* file = dynamic_cast<const IqFileSource*>(&source)) {
    return file->notices();
  }
  return {};
}

}  // namespace rtlangle
