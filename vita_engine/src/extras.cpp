#include "extras.hpp"
#include "log.hpp"
#include "script_vm.hpp"

#include <psp2/io/stat.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace fsn {

namespace {

constexpr int kVisibleTrackRows = 10;

std::string lower_ascii(std::string s) {
  for (char& c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

std::string clean_bgm_stem(const std::string& raw) {
  std::string s = lower_ascii(raw);
  auto slash = s.rfind('/');
  if (slash != std::string::npos) s = s.substr(slash + 1);
  auto dot = s.rfind('.');
  if (dot != std::string::npos) s = s.substr(0, dot);
  return s;
}

bool read_json_doc(const std::string& path, nlohmann::json& out) {
  std::ifstream ifs(path);
  if (!ifs.is_open()) return false;
  std::stringstream ss;
  ss << ifs.rdbuf();
  out = nlohmann::json::parse(ss.str(), nullptr, false);
  return !out.is_discarded() && out.is_object();
}

}  // namespace

bool ExtrasManager::init(const std::string& data_root, const std::string& extras_path,
                         const nlohmann::json& manifest) {
  data_root_ = data_root;
  extras_path_ = extras_path;
  load_bgm_index(manifest);
  load();
  log::info("[Extras] Initialized (%u tracks, %u unlocked BGM)",
            static_cast<unsigned>(bgm_tracks_.size()),
            static_cast<unsigned>(unlocked_bgm_.size()));
  return true;
}

void ExtrasManager::load_bgm_index(const nlohmann::json& manifest) {
  bgm_tracks_.clear();
  bgm_sub_unlocks_.clear();
  bgm_known_ids_.clear();

  nlohmann::json idx;
  if (read_json_doc(data_root_ + "/audio/bgm_index.json", idx) &&
      idx.contains("tracks") && idx["tracks"].is_array()) {
    for (const auto& t : idx["tracks"]) {
      if (!t.is_object()) continue;
      BgmTrackInfo info;
      info.id = lower_ascii(t.value("id", ""));
      if (info.id.empty()) continue;
      info.order = t.value("order", 999);
      info.title = t.value("title", info.id);
      info.has_new = !t.value("new_file", "").empty();
      info.has_old = !t.value("old_file", "").empty();
      if (t.contains("sub_unlocks") && t["sub_unlocks"].is_array()) {
        for (const auto& sub : t["sub_unlocks"]) {
          if (sub.is_string()) {
            info.sub_unlocks.push_back(lower_ascii(sub.get<std::string>()));
          }
        }
      }
      bgm_known_ids_.insert(info.id);
      if (!info.sub_unlocks.empty()) {
        bgm_sub_unlocks_[info.id] = info.sub_unlocks;
      }
      bgm_tracks_.push_back(std::move(info));
    }
    return;
  }

  // Fallback if bgm_index.json is not present: construct list from manifest
  std::unordered_set<std::string> stems;
  if (manifest.contains("bgm") && manifest["bgm"].is_object()) {
    for (auto it = manifest["bgm"].begin(); it != manifest["bgm"].end(); ++it) {
      std::string k = lower_ascii(it.key());
      if (k.size() > 1 && k.back() == 'g' &&
          manifest["bgm"].contains(k.substr(0, k.size() - 1))) {
        continue;
      }
      stems.insert(k);
    }
  }
  if (manifest.contains("bgm_old") && manifest["bgm_old"].is_object()) {
    for (auto it = manifest["bgm_old"].begin(); it != manifest["bgm_old"].end(); ++it) {
      stems.insert(lower_ascii(it.key()));
    }
  }
  std::vector<std::string> sorted_stems(stems.begin(), stems.end());
  std::sort(sorted_stems.begin(), sorted_stems.end());
  for (size_t i = 0; i < sorted_stems.size(); ++i) {
    const auto& s = sorted_stems[i];
    BgmTrackInfo info;
    info.id = s;
    info.order = static_cast<int>(i + 1);
    info.title = s;
    info.has_new = manifest.contains("bgm") && manifest["bgm"].contains(s);
    info.has_old = manifest.contains("bgm_old") && manifest["bgm_old"].contains(s);
    bgm_known_ids_.insert(s);
    bgm_tracks_.push_back(std::move(info));
  }
}

bool ExtrasManager::load() {
  unlocked_bgm_.clear();
  seen_cgs_.clear();
  nlohmann::json j;
  if (!read_json_doc(extras_path_, j)) return false;

  if (j.contains("unlocked_bgm") && j["unlocked_bgm"].is_array()) {
    for (const auto& v : j["unlocked_bgm"]) {
      if (v.is_string()) unlocked_bgm_.insert(lower_ascii(v.get<std::string>()));
    }
  }
  if (j.contains("seen_cgs") && j["seen_cgs"].is_array()) {
    for (const auto& v : j["seen_cgs"]) {
      if (v.is_string()) seen_cgs_.insert(v.get<std::string>());
    }
  }
  return true;
}

bool ExtrasManager::save() const {
  sceIoMkdir("ux0:data", 0777);
  sceIoMkdir("ux0:data/FateReVita", 0777);

  nlohmann::json j;
  j["version"] = 1;

  std::vector<std::string> bgms(unlocked_bgm_.begin(), unlocked_bgm_.end());
  std::sort(bgms.begin(), bgms.end());
  j["unlocked_bgm"] = bgms;

  std::vector<std::string> cgs(seen_cgs_.begin(), seen_cgs_.end());
  std::sort(cgs.begin(), cgs.end());
  j["seen_cgs"] = cgs;

  std::ofstream ofs(extras_path_, std::ios::trunc);
  if (!ofs.is_open()) return false;
  ofs << j.dump(2) << '\n';
  return ofs.good();
}

bool ExtrasManager::unlock_bgm(const std::string& raw_stem) {
  std::string s = clean_bgm_stem(raw_stem);
  if (s.empty()) return false;

  if (s.size() > 1 && s.back() == 'g' && bgm_known_ids_.count(s.substr(0, s.size() - 1))) {
    s.pop_back();
  }

  bool changed = false;
  if (!unlocked_bgm_.count(s)) {
    unlocked_bgm_.insert(s);
    changed = true;
  }
  auto sub_it = bgm_sub_unlocks_.find(s);
  if (sub_it != bgm_sub_unlocks_.end()) {
    for (const auto& sub : sub_it->second) {
      if (!unlocked_bgm_.count(sub)) {
        unlocked_bgm_.insert(sub);
        changed = true;
      }
    }
  }

  if (changed) {
    save();
  }
  return changed;
}

bool ExtrasManager::is_bgm_unlocked(const std::string& id, bool unlock_all) const {
  if (unlock_all) return true;
  return unlocked_bgm_.count(id) > 0;
}

int ExtrasManager::unlocked_bgm_count(bool unlock_all) const {
  if (unlock_all) return static_cast<int>(bgm_tracks_.size());
  int count = 0;
  for (const auto& t : bgm_tracks_) {
    if (unlocked_bgm_.count(t.id)) ++count;
  }
  return count;
}

void MusicRoomScreen::init(const std::string& data_root, Renderer* renderer, AudioEngine* audio,
                           TextRenderer* text, AppConfig* config, const std::string& config_path,
                           ExtrasManager* extras, ScriptVM* vm, bool circle_confirm) {
  data_root_ = data_root;
  renderer_ = renderer;
  audio_ = audio;
  text_ = text;
  config_ = config;
  config_path_ = config_path;
  extras_ = extras;
  vm_ = vm;
  circle_confirm_ = circle_confirm;
}

void MusicRoomScreen::enter() {
  selected_ = 0;
  scroll_top_ = 0;
  playing_idx_ = -1;
  loop_enabled_ = true;
  use_old_ost_ = config_ && (config_->ost_mode == OstMode::Old);
  playing_old_ost_ = use_old_ost_;
  anim_time_ = 0.0f;
  if (audio_) {
    audio_->stop_bgm(0);
  }
}

void MusicRoomScreen::ensure_scroll_visible() {
  const auto& tracks = extras_->bgm_tracks();
  int count = static_cast<int>(tracks.size());
  if (count <= 0) {
    selected_ = 0;
    scroll_top_ = 0;
    return;
  }
  selected_ = std::max(0, std::min(selected_, count - 1));
  if (selected_ < scroll_top_) {
    scroll_top_ = selected_;
  } else if (selected_ >= scroll_top_ + kVisibleTrackRows) {
    scroll_top_ = selected_ - kVisibleTrackRows + 1;
  }
  scroll_top_ = std::max(0, std::min(scroll_top_, std::max(0, count - kVisibleTrackRows)));
}

void MusicRoomScreen::play_track(int index) {
  if (!extras_ || !vm_ || !audio_ || !config_) return;
  const auto& tracks = extras_->bgm_tracks();
  if (index < 0 || index >= static_cast<int>(tracks.size())) return;
  const auto& t = tracks[static_cast<size_t>(index)];
  if (!extras_->is_bgm_unlocked(t.id, config_->unlock_all_extras)) return;

  OstMode desired_ost = use_old_ost_ ? OstMode::Old : OstMode::New;
  bool actual_old = use_old_ost_;
  if (use_old_ost_ && !t.has_old && t.has_new) {
    desired_ost = OstMode::New;
    actual_old = false;
  } else if (!use_old_ost_ && !t.has_new && t.has_old) {
    desired_ost = OstMode::Old;
    actual_old = true;
  }

  int64_t lstart = 0;
  int64_t llen = 0;
  std::string path = vm_->resolve_bgm_for_ost(t.id, desired_ost, &lstart, &llen);
  if (path.empty()) return;

  audio_->play_bgm(path, 0, loop_enabled_, lstart, llen);
  playing_idx_ = index;
  playing_old_ost_ = actual_old;
}

void MusicRoomScreen::step_unlocked(int dir) {
  if (!extras_ || !config_) return;
  const auto& tracks = extras_->bgm_tracks();
  int count = static_cast<int>(tracks.size());
  if (count <= 0) return;

  int start = (playing_idx_ >= 0) ? playing_idx_ : selected_;
  for (int step = 1; step <= count; ++step) {
    int cand = ((start + dir * step) % count + count) % count;
    if (extras_->is_bgm_unlocked(tracks[static_cast<size_t>(cand)].id,
                                 config_->unlock_all_extras)) {
      selected_ = cand;
      ensure_scroll_visible();
      play_track(cand);
      return;
    }
  }
}

void MusicRoomScreen::toggle_play_pause() {
  if (!audio_ || !extras_ || !config_) return;
  if (audio_->is_bgm_playing()) {
    audio_->set_bgm_paused(!audio_->is_bgm_paused());
    return;
  }
  const auto& tracks = extras_->bgm_tracks();
  if (selected_ >= 0 && selected_ < static_cast<int>(tracks.size()) &&
      extras_->is_bgm_unlocked(tracks[static_cast<size_t>(selected_)].id,
                               config_->unlock_all_extras)) {
    play_track(selected_);
  }
}

void MusicRoomScreen::toggle_ost() {
  use_old_ost_ = !use_old_ost_;
  if (audio_) {
    audio_->play_se("ui_move", data_root_ + "/audio/se/se_title_move.ogg");
  }
  if (playing_idx_ >= 0 && audio_ && audio_->is_bgm_playing()) {
    bool was_paused = audio_->is_bgm_paused();
    play_track(playing_idx_);
    if (was_paused) {
      audio_->set_bgm_paused(true);
    }
  }
}

bool MusicRoomScreen::update(float dt_sec, bool up, bool down, bool left, bool right,
                             bool confirm, bool back, bool btn_triangle, bool btn_square,
                             bool btn_l, bool btn_r, bool btn_start, bool btn_select,
                             bool touch_tap, float touch_x, float touch_y) {
  anim_time_ += dt_sec;

  if (playing_idx_ >= 0 && audio_ && !audio_->is_bgm_playing()) {
    playing_idx_ = -1;
  }

  if (back) {
    if (audio_) {
      audio_->stop_bgm(0);
      audio_->play_se("ui_move", data_root_ + "/audio/se/se_title_move.ogg");
    }
    return false;
  }

  const auto& tracks = extras_->bgm_tracks();
  int count = static_cast<int>(tracks.size());

  if (up && count > 0) {
    selected_ = (selected_ + count - 1) % count;
    ensure_scroll_visible();
    audio_->play_se("ui_move", data_root_ + "/audio/se/se_title_move.ogg");
  }
  if (down && count > 0) {
    selected_ = (selected_ + 1) % count;
    ensure_scroll_visible();
    audio_->play_se("ui_move", data_root_ + "/audio/se/se_title_move.ogg");
  }
  if (left && count > 0) {
    selected_ = std::max(0, selected_ - kVisibleTrackRows);
    ensure_scroll_visible();
    audio_->play_se("ui_move", data_root_ + "/audio/se/se_title_move.ogg");
  }
  if (right && count > 0) {
    selected_ = std::min(count - 1, selected_ + kVisibleTrackRows);
    ensure_scroll_visible();
    audio_->play_se("ui_move", data_root_ + "/audio/se/se_title_move.ogg");
  }

  if (touch_tap) {
    // Track list rows (x: 36..924, y: 74..394)
    for (int r = 0; r < kVisibleTrackRows; ++r) {
      int idx = scroll_top_ + r;
      if (idx >= count) break;
      float ry = 74.0f + r * 32.0f;
      if (touch_x >= 36.0f && touch_x <= 924.0f && touch_y >= ry && touch_y <= ry + 30.0f) {
        selected_ = idx;
        ensure_scroll_visible();
        if (playing_idx_ == idx && audio_ && audio_->is_bgm_playing()) {
          toggle_play_pause();
        } else {
          play_track(idx);
        }
        return true;
      }
    }
    // Control bar buttons at y: 448..482
    if (touch_y >= 448.0f && touch_y <= 484.0f) {
      if (touch_x >= 36.0f && touch_x < 168.0f) btn_triangle = true;
      else if (touch_x >= 174.0f && touch_x < 268.0f) btn_l = true;
      else if (touch_x >= 274.0f && touch_x < 368.0f) btn_r = true;
      else if (touch_x >= 374.0f && touch_x < 546.0f) btn_square = true;
      else if (touch_x >= 552.0f && touch_x < 678.0f) btn_start = true;
      else if (touch_x >= 684.0f && touch_x < 830.0f) btn_select = true;
      else if (touch_x >= 836.0f && touch_x <= 924.0f) {
        if (audio_) {
          audio_->stop_bgm(0);
          audio_->play_se("ui_move", data_root_ + "/audio/se/se_title_move.ogg");
        }
        return false;
      }
    }
  }

  if (confirm && count > 0) {
    if (playing_idx_ == selected_ && audio_ && audio_->is_bgm_playing()) {
      toggle_play_pause();
    } else {
      play_track(selected_);
    }
  }
  if (btn_triangle) toggle_play_pause();
  if (btn_l) step_unlocked(-1);
  if (btn_r) step_unlocked(1);
  if (btn_square) toggle_ost();
  if (btn_start) {
    loop_enabled_ = !loop_enabled_;
    if (audio_) {
      audio_->set_bgm_loop(loop_enabled_);
      audio_->play_se("ui_move", data_root_ + "/audio/se/se_title_move.ogg");
    }
  }
  if (btn_select && config_) {
    config_->unlock_all_extras = !config_->unlock_all_extras;
    config_->save(config_path_);
    if (audio_) {
      audio_->play_se("ui_decide", data_root_ + "/audio/se/se_title_decide.ogg");
    }
  }

  return true;
}

void MusicRoomScreen::draw() {
  const Texture* bg = renderer_->get_texture(data_root_ + "/images/ui/title_bg_01.png");
  if (bg) {
    renderer_->draw_texture(bg, 0.0f, 2.0f, 960.0f, 540.0f, 1.0f);
  } else {
    renderer_->draw_rect(0.0f, 0.0f, 960.0f, 544.0f, 0.05f, 0.07f, 0.14f, 1.0f);
  }

  renderer_->draw_rect(22.0f, 16.0f, 916.0f, 512.0f, 0.04f, 0.06f, 0.12f, 0.92f);
  renderer_->draw_rect(22.0f, 16.0f, 916.0f, 3.0f, 0.35f, 0.62f, 0.95f, 0.95f);

  const auto& tracks = extras_->bgm_tracks();
  bool unlock_all = config_ && config_->unlock_all_extras;
  int total = static_cast<int>(tracks.size());
  int unlocked_n = extras_->unlocked_bgm_count(unlock_all);

  text_->draw_simple("MUSIC ROOM", 40.0f, 26.0f, 1.0f, 0.88f, 0.94f, 1.0f, 1.0f);

  char hdr_buf[128];
  std::snprintf(hdr_buf, sizeof(hdr_buf), "OST: %s   |   Tracks: %d / %d%s",
                use_old_ost_ ? "Old (2004 / PS2)" : "New (Remastered)",
                unlocked_n, total, unlock_all ? " [ALL UNLOCKED]" : "");
  text_->draw_simple(hdr_buf, 380.0f, 30.0f, 0.78f, 0.78f, 0.90f, 1.0f, 0.95f);

  renderer_->draw_rect(36.0f, 64.0f, 888.0f, 1.0f, 0.30f, 0.45f, 0.70f, 0.7f);

  for (int r = 0; r < kVisibleTrackRows; ++r) {
    int idx = scroll_top_ + r;
    if (idx >= total) break;
    const auto& t = tracks[static_cast<size_t>(idx)];
    bool sel = (idx == selected_);
    bool unlocked = extras_->is_bgm_unlocked(t.id, unlock_all);
    bool is_playing = (idx == playing_idx_ && audio_ && audio_->is_bgm_playing());
    bool is_paused = (is_playing && audio_->is_bgm_paused());

    float ry = 74.0f + r * 32.0f;
    if (sel) {
      float glow = 0.36f + 0.10f * std::sin(anim_time_ * 5.0f);
      renderer_->draw_rect(36.0f, ry, 876.0f, 29.0f, 0.22f, 0.48f, 0.88f, glow);
    } else if (is_playing) {
      renderer_->draw_rect(36.0f, ry, 876.0f, 29.0f, 0.14f, 0.34f, 0.54f, 0.45f);
    } else {
      renderer_->draw_rect(36.0f, ry, 876.0f, 29.0f, 0.08f, 0.11f, 0.20f, 0.40f);
    }

    char row_buf[160];
    if (unlocked) {
      std::snprintf(row_buf, sizeof(row_buf), "%02d. %s", idx + 1, t.title.c_str());
    } else {
      std::snprintf(row_buf, sizeof(row_buf), "%02d. [Locked] Track %02d", idx + 1, idx + 1);
    }
    float tr = unlocked ? (sel ? 1.0f : 0.90f) : 0.50f;
    float tg = unlocked ? (sel ? 0.98f : 0.92f) : 0.54f;
    float tb = unlocked ? (sel ? 0.72f : 0.98f) : 0.62f;
    text_->draw_simple(row_buf, 48.0f, ry + 5.0f, 0.78f, tr, tg, tb, 1.0f);

    const char* src_tag = (t.has_new && t.has_old)
                              ? "[NEW / OLD]"
                              : (t.has_new ? "[NEW ONLY]" : "[OLD ONLY]");
    text_->draw_simple(src_tag, 680.0f, ry + 6.0f, 0.68f, 0.68f, 0.78f, 0.92f, unlocked ? 0.9f : 0.45f);

    if (is_playing) {
      const char* st = is_paused ? "PAUSED" : "PLAYING";
      text_->draw_simple(st, 815.0f, ry + 6.0f, 0.70f, 0.55f, 1.0f, 0.65f, 1.0f);
    }
  }

  // Scrollbar track & thumb
  if (total > kVisibleTrackRows) {
    renderer_->draw_rect(918.0f, 74.0f, 6.0f, 318.0f, 0.15f, 0.20f, 0.32f, 0.6f);
    float ratio = static_cast<float>(kVisibleTrackRows) / static_cast<float>(total);
    float thumb_h = std::max(24.0f, 318.0f * ratio);
    float max_top = static_cast<float>(std::max(1, total - kVisibleTrackRows));
    float thumb_y = 74.0f + (318.0f - thumb_h) * (static_cast<float>(scroll_top_) / max_top);
    renderer_->draw_rect(918.0f, thumb_y, 6.0f, thumb_h, 0.42f, 0.72f, 1.0f, 0.9f);
  }

  // Now Playing status box
  renderer_->draw_rect(36.0f, 404.0f, 888.0f, 38.0f, 0.08f, 0.12f, 0.22f, 0.85f);
  std::string now_str = "Stopped - Select an unlocked track and press Confirm";
  if (playing_idx_ >= 0 && playing_idx_ < total && audio_ && audio_->is_bgm_playing()) {
    int sec = static_cast<int>(audio_->get_bgm_time_sec());
    int mm = sec / 60;
    int ss = sec % 60;
    const char* st = audio_->is_bgm_paused() ? "PAUSED" : "PLAYING";
    char nb[200];
    std::snprintf(nb, sizeof(nb), "Now: %s [%s]   |   %02d:%02d   |   %s   |   Loop: %s",
                  tracks[static_cast<size_t>(playing_idx_)].title.c_str(),
                  playing_old_ost_ ? "Old" : "New",
                  mm, ss, st, loop_enabled_ ? "ON" : "OFF");
    now_str = nb;
  } else {
    char nb[128];
    std::snprintf(nb, sizeof(nb), "Stopped   |   OST: %s   |   Loop: %s",
                  use_old_ost_ ? "Old (2004)" : "New (Remastered)",
                  loop_enabled_ ? "ON" : "OFF");
    now_str = nb;
  }
  text_->draw_simple(now_str, 48.0f, 413.0f, 0.76f, 0.90f, 0.95f, 1.0f, 1.0f);

  // Action buttons
  struct BtnDef {
    float x;
    float w;
    const char* lbl;
  };
  const BtnDef btns[7] = {
      {36.0f, 130.0f, "Play/Pause"},
      {174.0f, 92.0f, "< Prev"},
      {274.0f, 92.0f, "Next >"},
      {374.0f, 170.0f, use_old_ost_ ? "OST: Old" : "OST: New"},
      {552.0f, 124.0f, loop_enabled_ ? "Loop: ON" : "Loop: OFF"},
      {684.0f, 144.0f, unlock_all ? "Unlock: ALL" : "Unlock: Norm"},
      {836.0f, 88.0f, "Back"},
  };
  for (const auto& b : btns) {
    renderer_->draw_rect(b.x, 448.0f, b.w, 34.0f, 0.14f, 0.22f, 0.38f, 0.85f);
    text_->draw_simple(b.lbl, b.x + 10.0f, 456.0f, 0.70f, 0.90f, 0.95f, 1.0f, 1.0f);
  }

  text_->draw_simple(
      circle_confirm_
          ? "D-Pad: Browse | Circle/Tri: Play/Pause | L/R: Prev/Next | Sq: OST | Start: Loop | Sel: Unlock All | Cross: Back"
          : "D-Pad: Browse | Cross/Tri: Play/Pause | L/R: Prev/Next | Sq: OST | Start: Loop | Sel: Unlock All | Circle: Back",
      32.0f, 496.0f, 0.62f, 0.75f, 0.84f, 0.95f, 0.85f);
}

}  // namespace fsn
