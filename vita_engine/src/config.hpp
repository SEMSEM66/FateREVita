#pragma once

#include <cstdint>
#include <json.hpp>
#include <psp2/io/stat.h>

#include <fstream>
#include <string>

namespace fsn {

enum class OstMode {
  New = 0,
  Old = 1,
};

enum class ContentMode {
  AllAges = 0,
  HMosaic = 1,
  HDemosaic = 2,
};

struct TitleBgmInfo {
  std::string new_path;
  std::string old_path;
  int64_t old_loop_start = 0;
  int64_t old_loop_len = 0;
};

/**
 * Persistent user configuration stored in ux0:data/FateReVita/config.json.
 */
struct AppConfig {
  OstMode ost_mode = OstMode::New;
  ContentMode content_mode = ContentMode::AllAges;
  bool unlock_all_routes = false;
  bool unlock_all_extras = false;
  float bgm_vol = 0.8f;
  float se_vol = 0.9f;
  float voice_vol = 1.0f;
  bool setup_completed = false;

  const char* ost_label() const {
    return (ost_mode == OstMode::Old) ? "Old (2004 / PS2)" : "New (Remastered)";
  }

  const char* content_label() const {
    switch (content_mode) {
      case ContentMode::HMosaic:
        return "18+ (Original Mosaic)";
      case ContentMode::HDemosaic:
        return "18+ (Uncensored)";
      default:
        return "All-Ages (Remastered)";
    }
  }

  bool load(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return false;
    nlohmann::json j = nlohmann::json::parse(ifs, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;

    std::string ost = j.value("ost_mode", "new");
    ost_mode = (ost == "old") ? OstMode::Old : OstMode::New;

    std::string cm = j.value("content_mode", "all_ages");
    if (cm == "h_mosaic") {
      content_mode = ContentMode::HMosaic;
    } else if (cm == "h_demosaic") {
      content_mode = ContentMode::HDemosaic;
    } else {
      content_mode = ContentMode::AllAges;
    }

    // Keep authentic Remastered route progression (Prologue -> Fate -> UBW -> HF).
    unlock_all_routes = false;
    unlock_all_extras = j.value("unlock_all_extras", false);
    bgm_vol = j.value("bgm_vol", 0.8f);
    se_vol = j.value("se_vol", 0.9f);
    voice_vol = j.value("voice_vol", 1.0f);
    setup_completed = j.value("setup_completed", false);
    return true;
  }

  bool save(const std::string& path) const {
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir("ux0:data/FateReVita", 0777);
    nlohmann::json j;
    j["version"] = 2;
    j["ost_mode"] = (ost_mode == OstMode::Old) ? "old" : "new";
    if (content_mode == ContentMode::HMosaic) {
      j["content_mode"] = "h_mosaic";
    } else if (content_mode == ContentMode::HDemosaic) {
      j["content_mode"] = "h_demosaic";
    } else {
      j["content_mode"] = "all_ages";
    }
    j["unlock_all_routes"] = unlock_all_routes;
    j["unlock_all_extras"] = unlock_all_extras;
    j["bgm_vol"] = bgm_vol;
    j["se_vol"] = se_vol;
    j["voice_vol"] = voice_vol;
    j["setup_completed"] = setup_completed;

    std::ofstream ofs(path, std::ios::trunc);
    if (!ofs.is_open()) return false;
    ofs << j.dump(2) << '\n';
    return ofs.good();
  }
};

}  // namespace fsn
