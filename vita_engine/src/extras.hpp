#pragma once

#include "audio.hpp"
#include "config.hpp"
#include "renderer.hpp"
#include "text.hpp"

#include <cstdint>
#include <json.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fsn {

class ScriptVM;

struct BgmTrackInfo {
  std::string id;
  int order = 0;
  std::string title;
  bool has_new = false;
  bool has_old = false;
  std::vector<std::string> sub_unlocks;
};

/**
 * Manages persistent Music Room & CG Gallery unlock state in
 * ux0:data/FateReVita/extras.json alongside index metadata from the pipeline.
 */
class ExtrasManager {
 public:
  bool init(const std::string& data_root, const std::string& extras_path,
            const nlohmann::json& manifest);

  bool load();
  bool save() const;

  bool unlock_bgm(const std::string& raw_stem);
  bool is_bgm_unlocked(const std::string& id, bool unlock_all) const;
  int unlocked_bgm_count(bool unlock_all) const;
  const std::vector<BgmTrackInfo>& bgm_tracks() const { return bgm_tracks_; }

 private:
  std::string data_root_;
  std::string extras_path_;
  std::vector<BgmTrackInfo> bgm_tracks_;
  std::unordered_map<std::string, std::vector<std::string>> bgm_sub_unlocks_;
  std::unordered_set<std::string> bgm_known_ids_;

  std::unordered_set<std::string> unlocked_bgm_;
  std::unordered_set<std::string> seen_cgs_;

  void load_bgm_index(const nlohmann::json& manifest);
};

class MusicRoomScreen {
 public:
  void init(const std::string& data_root, Renderer* renderer, AudioEngine* audio,
            TextRenderer* text, AppConfig* config, const std::string& config_path,
            ExtrasManager* extras, ScriptVM* vm, bool circle_confirm);
  void enter();

  // Returns true while Music Room stays open; false when exiting back to Title.
  bool update(float dt_sec, bool up, bool down, bool left, bool right,
              bool confirm, bool back, bool btn_triangle, bool btn_square,
              bool btn_l, bool btn_r, bool btn_start, bool btn_select,
              bool touch_tap, float touch_x, float touch_y);
  void draw();

 private:
  std::string data_root_;
  std::string config_path_;
  Renderer* renderer_ = nullptr;
  AudioEngine* audio_ = nullptr;
  TextRenderer* text_ = nullptr;
  AppConfig* config_ = nullptr;
  ExtrasManager* extras_ = nullptr;
  ScriptVM* vm_ = nullptr;
  bool circle_confirm_ = false;

  int selected_ = 0;
  int scroll_top_ = 0;
  int playing_idx_ = -1;
  bool playing_old_ost_ = false;
  bool use_old_ost_ = false;
  bool loop_enabled_ = true;
  float anim_time_ = 0.0f;

  void ensure_scroll_visible();
  void play_track(int index);
  void step_unlocked(int dir);
  void toggle_play_pause();
  void toggle_ost();
};

}  // namespace fsn
