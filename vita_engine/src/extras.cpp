#include "extras.hpp"
#include "log.hpp"
#include "script_vm.hpp"
#include "title_screen.hpp"
#include "video_player.hpp"

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
constexpr int kGridCols = 4;
constexpr int kGridRows = 3;
constexpr int kGridPerPage = kGridCols * kGridRows;

std::string lower_ascii(std::string s) {
  for (char& c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

std::string clean_stem(const std::string& raw) {
  std::string s = raw;
  auto slash = s.rfind('/');
  if (slash != std::string::npos) s = s.substr(slash + 1);
  auto dot = s.rfind('.');
  if (dot != std::string::npos) {
    std::string ext = lower_ascii(s.substr(dot));
    if (ext == ".ogg" || ext == ".wav" || ext == ".fcd" || ext == ".png" ||
        ext == ".jpg" || ext == ".webp" || ext == ".bmp" || ext == ".mpg") {
      s = s.substr(0, dot);
    }
  }
  return s;
}

std::string clean_bgm_stem(const std::string& raw) {
  return lower_ascii(clean_stem(raw));
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
  load_cg_index();
  load();
  log::info("[Extras] Initialized (bgm=%u unlocked=%u | cg_groups=%u seen=%u)",
            static_cast<unsigned>(bgm_tracks_.size()),
            static_cast<unsigned>(unlocked_bgm_.size()),
            static_cast<unsigned>(cg_groups_.size()),
            static_cast<unsigned>(seen_cgs_.size()));
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

void ExtrasManager::load_cg_index() {
  cg_groups_.clear();
  cg_categories_ = {"All", "Movies"};
  cg_stem_map_.clear();
  cg_sub_unlocks_.clear();

  nlohmann::json idx;
  if (read_json_doc(data_root_ + "/gallery/cg_index.json", idx)) {
    if (idx.contains("categories") && idx["categories"].is_array() &&
        !idx["categories"].empty()) {
      cg_categories_.clear();
      for (const auto& c : idx["categories"]) {
        if (c.is_string()) cg_categories_.push_back(c.get<std::string>());
      }
      if (std::find(cg_categories_.begin(), cg_categories_.end(), "Movies") ==
          cg_categories_.end()) {
        auto pos = cg_categories_.empty() ? cg_categories_.begin() : cg_categories_.begin() + 1;
        cg_categories_.insert(pos, "Movies");
      }
    }

    if (idx.contains("sub_unlocks") && idx["sub_unlocks"].is_object()) {
      for (auto it = idx["sub_unlocks"].begin(); it != idx["sub_unlocks"].end(); ++it) {
        if (!it.value().is_array()) continue;
        std::vector<std::string> kids;
        for (const auto& k : it.value()) {
          if (k.is_string()) kids.push_back(k.get<std::string>());
        }
        cg_sub_unlocks_[lower_ascii(it.key())] = kids;
      }
    }

    if (idx.contains("cgs") && idx["cgs"].is_array()) {
      cg_groups_.reserve(idx["cgs"].size() + 3);
      for (const auto& cg : idx["cgs"]) {
        if (!cg.is_object()) continue;
        CgGroupInfo g;
        g.id = cg.value("id", "");
        g.category = cg.value("category", "Extra");
        g.is_adult = cg.value("is_adult", false);
        g.thumb_all_ages = cg.value("thumb_all_ages", "");
        g.thumb_mosaic = cg.value("thumb_mosaic", "");
        g.thumb_demosaic = cg.value("thumb_demosaic", "");
        if (cg.contains("variants") && cg["variants"].is_array()) {
          for (const auto& v : cg["variants"]) {
            if (!v.is_object()) continue;
            CgVariantInfo vi;
            vi.stem = v.value("stem", "");
            vi.all_ages = v.value("all_ages", "");
            vi.mosaic = v.value("mosaic", "");
            vi.demosaic = v.value("demosaic", "");
            if (!vi.stem.empty()) {
              cg_stem_map_[vi.stem] = vi.stem;
              cg_stem_map_[lower_ascii(vi.stem)] = vi.stem;
              g.variants.push_back(std::move(vi));
            }
          }
        }
        if (!g.id.empty() && !g.variants.empty()) {
          cg_groups_.push_back(std::move(g));
        }
      }
    }
  }

  auto find_thumb = [&](const std::string& cat) -> std::string {
    for (const auto& g : cg_groups_) {
      if (!g.is_adult && g.category == cat && !g.thumb_all_ages.empty()) {
        return g.thumb_all_ages;
      }
    }
    return "";
  };

  const struct {
    const char* id;
    const char* movie_id;
    const char* title;
    const char* cat_thumb;
  } kMovies[3] = {
      {"MOVIE_01", "op01", "OP 1: Fate", "Prologue"},
      {"MOVIE_02", "op02", "OP 2: UBW", "UBW"},
      {"MOVIE_03", "op03", "OP 3: Heaven's Feel", "Heaven's Feel"},
  };

  std::vector<CgGroupInfo> movie_entries;
  for (const auto& m : kMovies) {
    CgGroupInfo mg;
    mg.id = m.id;
    mg.category = "Movies";
    mg.is_adult = false;
    mg.is_movie = true;
    mg.movie_id = m.movie_id;
    mg.movie_title = m.title;
    mg.thumb_all_ages = find_thumb(m.cat_thumb);
    movie_entries.push_back(std::move(mg));
  }
  cg_groups_.insert(cg_groups_.begin(), movie_entries.begin(), movie_entries.end());
}

bool ExtrasManager::load() {
  unlocked_bgm_.clear();
  seen_cgs_.clear();
  unlocked_movies_.clear();
  nlohmann::json j;
  if (!read_json_doc(extras_path_, j)) return false;

  if (j.contains("unlocked_bgm") && j["unlocked_bgm"].is_array()) {
    for (const auto& v : j["unlocked_bgm"]) {
      if (v.is_string()) unlocked_bgm_.insert(lower_ascii(v.get<std::string>()));
    }
  }
  if (j.contains("seen_cgs") && j["seen_cgs"].is_array()) {
    for (const auto& v : j["seen_cgs"]) {
      if (v.is_string()) {
        std::string s = v.get<std::string>();
        auto it = cg_stem_map_.find(s);
        seen_cgs_.insert(it != cg_stem_map_.end() ? it->second : s);
      }
    }
  }
  if (j.contains("unlocked_movies") && j["unlocked_movies"].is_array()) {
    for (const auto& v : j["unlocked_movies"]) {
      if (v.is_string()) unlocked_movies_.insert(clean_bgm_stem(v.get<std::string>()));
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

  std::vector<std::string> movies(unlocked_movies_.begin(), unlocked_movies_.end());
  std::sort(movies.begin(), movies.end());
  j["unlocked_movies"] = movies;

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

  if (changed) save();
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

bool ExtrasManager::mark_cg_seen(const std::string& raw_stem) {
  if (cg_stem_map_.empty()) return false;
  std::string s = clean_stem(raw_stem);
  if (s.empty()) return false;

  auto it = cg_stem_map_.find(s);
  if (it == cg_stem_map_.end()) {
    it = cg_stem_map_.find(lower_ascii(s));
  }
  if (it == cg_stem_map_.end()) return false;

  const std::string& canonical = it->second;
  bool changed = false;
  if (!seen_cgs_.count(canonical)) {
    seen_cgs_.insert(canonical);
    changed = true;
  }

  auto sub_it = cg_sub_unlocks_.find(lower_ascii(canonical));
  if (sub_it != cg_sub_unlocks_.end()) {
    for (const auto& child : sub_it->second) {
      auto cit = cg_stem_map_.find(lower_ascii(child));
      const std::string& ck = (cit != cg_stem_map_.end()) ? cit->second : child;
      if (!seen_cgs_.count(ck)) {
        seen_cgs_.insert(ck);
        changed = true;
      }
    }
  }

  if (changed) save();
  return changed;
}

bool ExtrasManager::unlock_movie(const std::string& raw_stem) {
  std::string s = clean_bgm_stem(raw_stem);
  if (s.empty()) return false;
  if (unlocked_movies_.count(s)) return false;
  unlocked_movies_.insert(s);
  save();
  return true;
}

bool ExtrasManager::is_movie_unlocked(const std::string& id, bool unlock_all) const {
  if (unlock_all) return true;
  return unlocked_movies_.count(clean_bgm_stem(id)) > 0;
}

bool ExtrasManager::is_cg_variant_seen(const std::string& stem, bool unlock_all) const {
  if (unlock_all) return true;
  return seen_cgs_.count(stem) > 0;
}

bool ExtrasManager::is_cg_group_unlocked(const CgGroupInfo& group, bool unlock_all) const {
  if (unlock_all) return true;
  if (group.is_movie) return is_movie_unlocked(group.movie_id, unlock_all);
  for (const auto& v : group.variants) {
    if (seen_cgs_.count(v.stem)) return true;
  }
  return false;
}

int ExtrasManager::unlocked_cg_group_count(ContentMode mode, bool unlock_all) const {
  int count = 0;
  for (const auto& g : cg_groups_) {
    if (mode == ContentMode::AllAges && g.is_adult) continue;
    if (is_cg_group_unlocked(g, unlock_all)) ++count;
  }
  return count;
}

int ExtrasManager::total_cg_group_count(ContentMode mode) const {
  int count = 0;
  for (const auto& g : cg_groups_) {
    if (mode == ContentMode::AllAges && g.is_adult) continue;
    ++count;
  }
  return count;
}

// --- MusicRoomScreen ---

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
  if (audio_) audio_->stop_bgm(0);
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
  if (audio_) audio_->play_se("ui_move", data_root_ + "/audio/se/se_title_move.ogg");
  if (playing_idx_ >= 0 && audio_ && audio_->is_bgm_playing()) {
    bool was_paused = audio_->is_bgm_paused();
    play_track(playing_idx_);
    if (was_paused) audio_->set_bgm_paused(true);
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
    if (audio_) audio_->play_se("ui_decide", data_root_ + "/audio/se/se_title_decide.ogg");
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

  if (total > kVisibleTrackRows) {
    renderer_->draw_rect(918.0f, 74.0f, 6.0f, 318.0f, 0.15f, 0.20f, 0.32f, 0.6f);
    float ratio = static_cast<float>(kVisibleTrackRows) / static_cast<float>(total);
    float thumb_h = std::max(24.0f, 318.0f * ratio);
    float max_top = static_cast<float>(std::max(1, total - kVisibleTrackRows));
    float thumb_y = 74.0f + (318.0f - thumb_h) * (static_cast<float>(scroll_top_) / max_top);
    renderer_->draw_rect(918.0f, thumb_y, 6.0f, thumb_h, 0.42f, 0.72f, 1.0f, 0.9f);
  }

  renderer_->draw_rect(36.0f, 404.0f, 888.0f, 38.0f, 0.08f, 0.12f, 0.22f, 0.85f);
  std::string now_str;
  if (playing_idx_ >= 0 && playing_idx_ < total && audio_ && audio_->is_bgm_playing()) {
    int sec = static_cast<int>(audio_->get_bgm_time_sec());
    const char* st = audio_->is_bgm_paused() ? "PAUSED" : "PLAYING";
    char nb[200];
    std::snprintf(nb, sizeof(nb), "Now: %s [%s]   |   %02d:%02d   |   %s   |   Loop: %s",
                  tracks[static_cast<size_t>(playing_idx_)].title.c_str(),
                  playing_old_ost_ ? "Old" : "New",
                  sec / 60, sec % 60, st, loop_enabled_ ? "ON" : "OFF");
    now_str = nb;
  } else {
    char nb[128];
    std::snprintf(nb, sizeof(nb), "Stopped   |   OST: %s   |   Loop: %s",
                  use_old_ost_ ? "Old (2004)" : "New (Remastered)",
                  loop_enabled_ ? "ON" : "OFF");
    now_str = nb;
  }
  text_->draw_simple(now_str, 48.0f, 413.0f, 0.76f, 0.90f, 0.95f, 1.0f, 1.0f);

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

// --- CgGalleryScreen ---

void CgGalleryScreen::init(const std::string& data_root, Renderer* renderer, AudioEngine* audio,
                           TextRenderer* text, AppConfig* config, const std::string& config_path,
                           ExtrasManager* extras, ScriptVM* vm, TitleScreen* title,
                           bool circle_confirm) {
  data_root_ = data_root;
  renderer_ = renderer;
  audio_ = audio;
  text_ = text;
  config_ = config;
  config_path_ = config_path;
  extras_ = extras;
  vm_ = vm;
  title_ = title;
  circle_confirm_ = circle_confirm;
}

void CgGalleryScreen::enter() {
  cat_idx_ = 0;
  selected_ = 0;
  viewer_open_ = false;
  hide_overlay_ = false;
  viewer_group_ = nullptr;
  unlocked_variants_.clear();
  active_full_path_.clear();
  anim_time_ = 0.0f;
  clear_thumb_cache();
  rebuild_filtered();
}

void CgGalleryScreen::leave() {
  close_viewer();
  clear_thumb_cache();
}

void CgGalleryScreen::rebuild_filtered() {
  filtered_.clear();
  if (!extras_ || !config_) return;

  const auto& cats = extras_->cg_categories();
  if (cat_idx_ < 0 || cat_idx_ >= static_cast<int>(cats.size())) cat_idx_ = 0;
  const std::string active_cat = cats.empty() ? "All" : cats[static_cast<size_t>(cat_idx_)];
  const bool hide_adult = (config_->content_mode == ContentMode::AllAges);

  for (const auto& g : extras_->cg_groups()) {
    if (hide_adult && g.is_adult) continue;
    if (active_cat != "All" && g.category != active_cat) continue;
    filtered_.push_back(&g);
  }

  if (filtered_.empty()) {
    selected_ = 0;
  } else {
    selected_ = std::max(0, std::min(selected_, static_cast<int>(filtered_.size()) - 1));
  }
}

void CgGalleryScreen::clear_thumb_cache() {
  if (!renderer_) {
    thumb_lru_.clear();
    return;
  }
  for (const auto& p : thumb_lru_) {
    renderer_->release_texture(p);
  }
  thumb_lru_.clear();
}

const Texture* CgGalleryScreen::get_thumb_texture(const std::string& abs_path) {
  if (!renderer_ || abs_path.empty()) return nullptr;
  auto it = std::find(thumb_lru_.begin(), thumb_lru_.end(), abs_path);
  if (it != thumb_lru_.end()) {
    thumb_lru_.erase(it);
  } else if (thumb_lru_.size() >= kMaxThumbCache) {
    renderer_->release_texture(thumb_lru_.front());
    thumb_lru_.pop_front();
  }
  thumb_lru_.push_back(abs_path);
  return renderer_->get_texture(abs_path);
}

std::string CgGalleryScreen::resolve_group_thumb(const CgGroupInfo& g) const {
  if (!config_) return g.thumb_all_ages;
  if (config_->content_mode == ContentMode::HDemosaic && !g.thumb_demosaic.empty()) {
    return g.thumb_demosaic;
  }
  if (config_->content_mode == ContentMode::HMosaic && !g.thumb_mosaic.empty()) {
    return g.thumb_mosaic;
  }
  return !g.thumb_all_ages.empty() ? g.thumb_all_ages : g.thumb_mosaic;
}

std::string CgGalleryScreen::resolve_variant_path(const CgVariantInfo& v) const {
  ContentMode cm = config_ ? config_->content_mode : ContentMode::AllAges;
  std::string rel;
  if (cm == ContentMode::HDemosaic) {
    rel = !v.demosaic.empty() ? v.demosaic : (!v.mosaic.empty() ? v.mosaic : v.all_ages);
  } else if (cm == ContentMode::HMosaic) {
    rel = !v.mosaic.empty() ? v.mosaic : v.all_ages;
  } else {
    rel = !v.all_ages.empty() ? v.all_ages : v.mosaic;
  }
  return rel.empty() ? "" : (data_root_ + "/" + rel);
}

void CgGalleryScreen::close_viewer() {
  if (renderer_ && !active_full_path_.empty()) {
    renderer_->release_texture(active_full_path_);
  }
  active_full_path_.clear();
  viewer_open_ = false;
  hide_overlay_ = false;
  viewer_group_ = nullptr;
  unlocked_variants_.clear();
  viewer_var_pos_ = 0;
}

void CgGalleryScreen::load_viewer_variant(int pos) {
  if (!viewer_group_ || unlocked_variants_.empty()) return;
  int n = static_cast<int>(unlocked_variants_.size());
  viewer_var_pos_ = ((pos % n) + n) % n;
  int v_idx = unlocked_variants_[static_cast<size_t>(viewer_var_pos_)];
  std::string next_path = resolve_variant_path(viewer_group_->variants[static_cast<size_t>(v_idx)]);
  if (next_path != active_full_path_) {
    if (renderer_ && !active_full_path_.empty()) {
      renderer_->release_texture(active_full_path_);
    }
    active_full_path_ = next_path;
  }
}

void CgGalleryScreen::open_viewer(const CgGroupInfo* group) {
  if (!group || !extras_ || !config_) return;
  unlocked_variants_.clear();
  for (size_t i = 0; i < group->variants.size(); ++i) {
    if (extras_->is_cg_variant_seen(group->variants[i].stem, config_->unlock_all_extras)) {
      unlocked_variants_.push_back(static_cast<int>(i));
    }
  }
  if (unlocked_variants_.empty()) return;

  viewer_group_ = group;
  viewer_open_ = true;
  hide_overlay_ = false;
  load_viewer_variant(0);
}

bool CgGalleryScreen::update(float dt_sec, bool up, bool down, bool left, bool right,
                             bool confirm, bool back, bool btn_triangle, bool btn_square,
                             bool btn_l, bool btn_r, bool btn_select,
                             bool touch_tap, float touch_x, float touch_y) {
  anim_time_ += dt_sec;

  if (viewer_open_) {
    if (back) {
      close_viewer();
      if (audio_) audio_->play_se("ui_move", data_root_ + "/audio/se/se_title_move.ogg");
      return true;
    }
    if (btn_square) {
      hide_overlay_ = !hide_overlay_;
      return true;
    }
    int step = 0;
    if (left || btn_l) step = -1;
    if (right || btn_r) step = 1;
    if (touch_tap) {
      step = (touch_x < 480.0f) ? -1 : 1;
    }
    if (step != 0 && unlocked_variants_.size() > 1) {
      load_viewer_variant(viewer_var_pos_ + step);
      if (audio_) audio_->play_se("ui_move", data_root_ + "/audio/se/se_title_move.ogg");
    }
    return true;
  }

  if (back) {
    leave();
    if (audio_) audio_->play_se("ui_move", data_root_ + "/audio/se/se_title_move.ogg");
    return false;
  }

  int count = static_cast<int>(filtered_.size());
  int total_pages = std::max(1, (count + kGridPerPage - 1) / kGridPerPage);
  int cur_page = (count > 0) ? (selected_ / kGridPerPage) : 0;
  int page_start = cur_page * kGridPerPage;

  if (count > 0) {
    if (left) {
      selected_ = (selected_ + count - 1) % count;
      audio_->play_se("ui_move", data_root_ + "/audio/se/se_title_move.ogg");
    }
    if (right) {
      selected_ = (selected_ + 1) % count;
      audio_->play_se("ui_move", data_root_ + "/audio/se/se_title_move.ogg");
    }
    if (up) {
      if (selected_ >= kGridCols) selected_ -= kGridCols;
      else selected_ = std::max(0, count - 1);
      audio_->play_se("ui_move", data_root_ + "/audio/se/se_title_move.ogg");
    }
    if (down) {
      if (selected_ + kGridCols < count) selected_ += kGridCols;
      else selected_ = selected_ % kGridCols;
      if (selected_ >= count) selected_ = count - 1;
      audio_->play_se("ui_move", data_root_ + "/audio/se/se_title_move.ogg");
    }
  }

  if (btn_l || btn_r) {
    int ncats = static_cast<int>(extras_->cg_categories().size());
    if (ncats > 0) {
      cat_idx_ = (cat_idx_ + (btn_l ? ncats - 1 : 1)) % ncats;
      selected_ = 0;
      rebuild_filtered();
      audio_->play_se("ui_move", data_root_ + "/audio/se/se_title_move.ogg");
    }
  }

  if (touch_tap) {
    // Check 4x3 grid thumbnails
    for (int slot = 0; slot < kGridPerPage; ++slot) {
      int idx = page_start + slot;
      if (idx >= count) break;
      int col = slot % kGridCols;
      int row = slot / kGridCols;
      float cx = 46.0f + col * 220.0f;
      float cy = 68.0f + row * 122.0f;
      if (touch_x >= cx && touch_x <= cx + 204.0f && touch_y >= cy && touch_y <= cy + 114.0f) {
        selected_ = idx;
        confirm = true;
        break;
      }
    }
    // Check footer buttons at y = 446..482
    if (touch_y >= 446.0f && touch_y <= 482.0f) {
      if (touch_x >= 36.0f && touch_x < 134.0f && count > 0) {
        int np = (cur_page + total_pages - 1) % total_pages;
        selected_ = np * kGridPerPage;
        audio_->play_se("ui_move", data_root_ + "/audio/se/se_title_move.ogg");
        return true;
      }
      if (touch_x >= 142.0f && touch_x < 240.0f && count > 0) {
        int np = (cur_page + 1) % total_pages;
        selected_ = np * kGridPerPage;
        audio_->play_se("ui_move", data_root_ + "/audio/se/se_title_move.ogg");
        return true;
      }
      if (touch_x >= 248.0f && touch_x < 408.0f) {
        int ncats = static_cast<int>(extras_->cg_categories().size());
        if (ncats > 0) {
          cat_idx_ = (cat_idx_ + 1) % ncats;
          selected_ = 0;
          rebuild_filtered();
          audio_->play_se("ui_move", data_root_ + "/audio/se/se_title_move.ogg");
        }
        return true;
      }
      if (touch_x >= 416.0f && touch_x < 652.0f) btn_triangle = true;
      else if (touch_x >= 660.0f && touch_x < 824.0f) btn_select = true;
      else if (touch_x >= 832.0f && touch_x <= 924.0f) {
        leave();
        if (audio_) audio_->play_se("ui_move", data_root_ + "/audio/se/se_title_move.ogg");
        return false;
      }
    }
  }

  if (btn_triangle && config_) {
    int cm = (static_cast<int>(config_->content_mode) + 1) % 3;
    config_->content_mode = static_cast<ContentMode>(cm);
    config_->save(config_path_);
    clear_thumb_cache();
    rebuild_filtered();
    if (audio_) audio_->play_se("ui_decide", data_root_ + "/audio/se/se_title_decide.ogg");
  }

  if (btn_select && config_) {
    config_->unlock_all_extras = !config_->unlock_all_extras;
    config_->save(config_path_);
    if (audio_) audio_->play_se("ui_decide", data_root_ + "/audio/se/se_title_decide.ogg");
  }

  if (confirm && selected_ >= 0 && selected_ < count) {
    const CgGroupInfo* g = filtered_[static_cast<size_t>(selected_)];
    if (extras_->is_cg_group_unlocked(*g, config_ && config_->unlock_all_extras)) {
      if (audio_) audio_->play_se("ui_decide", data_root_ + "/audio/se/se_title_decide.ogg");
      if (g->is_movie) {
        clear_thumb_cache();
        if (audio_) audio_->stop_bgm(0);
        if (vm_) {
          VideoPlayer::play(g->movie_id, true, data_root_, vm_->manifest(), renderer_, audio_);
        }
        if (title_) title_->play_bgm();
      } else {
        open_viewer(g);
      }
    } else if (audio_) {
      audio_->play_se("ui_move", data_root_ + "/audio/se/se_title_move.ogg");
    }
  }

  return true;
}

void CgGalleryScreen::draw() {
  if (viewer_open_) {
    renderer_->draw_rect(0.0f, 0.0f, 960.0f, 544.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    if (!active_full_path_.empty()) {
      const Texture* tex = renderer_->get_texture(active_full_path_);
      if (tex && tex->h > 0) {
        float scale = std::min(960.0f / static_cast<float>(tex->w),
                               544.0f / static_cast<float>(tex->h));
        float dw = static_cast<float>(tex->w) * scale;
        float dh = static_cast<float>(tex->h) * scale;
        float dx = (960.0f - dw) * 0.5f;
        float dy = (544.0f - dh) * 0.5f;
        renderer_->draw_texture(tex, dx, dy, dw, dh, 1.0f);
      }
    }
    if (!hide_overlay_ && viewer_group_) {
      renderer_->draw_rect(0.0f, 0.0f, 960.0f, 36.0f, 0.03f, 0.05f, 0.10f, 0.72f);
      renderer_->draw_rect(0.0f, 508.0f, 960.0f, 36.0f, 0.03f, 0.05f, 0.10f, 0.72f);
      char top_buf[128];
      std::snprintf(top_buf, sizeof(top_buf), "%s  (%s)  -  Variant %d / %u",
                    viewer_group_->id.c_str(), viewer_group_->category.c_str(),
                    viewer_var_pos_ + 1,
                    static_cast<unsigned>(unlocked_variants_.size()));
      text_->draw_simple(top_buf, 20.0f, 7.0f, 0.80f, 0.90f, 0.96f, 1.0f, 1.0f);
      text_->draw_simple(
          circle_confirm_
              ? "L/R or Left/Right: Variant  |  Square: Hide HUD  |  Cross: Back to Grid"
              : "L/R or Left/Right: Variant  |  Square: Hide HUD  |  Circle: Back to Grid",
          20.0f, 515.0f, 0.74f, 0.82f, 0.88f, 0.98f, 0.95f);
    }
    return;
  }

  const Texture* bg = renderer_->get_texture(data_root_ + "/images/ui/title_bg_01.png");
  if (bg) {
    renderer_->draw_texture(bg, 0.0f, 2.0f, 960.0f, 540.0f, 1.0f);
  } else {
    renderer_->draw_rect(0.0f, 0.0f, 960.0f, 544.0f, 0.05f, 0.07f, 0.14f, 1.0f);
  }

  renderer_->draw_rect(22.0f, 16.0f, 916.0f, 512.0f, 0.04f, 0.06f, 0.12f, 0.92f);
  renderer_->draw_rect(22.0f, 16.0f, 916.0f, 3.0f, 0.35f, 0.62f, 0.95f, 0.95f);

  bool unlock_all = config_ && config_->unlock_all_extras;
  ContentMode cm = config_ ? config_->content_mode : ContentMode::AllAges;
  const auto& cats = extras_->cg_categories();
  const std::string cat_name = cats.empty() ? "All" : cats[static_cast<size_t>(cat_idx_)];

  text_->draw_simple("CG GALLERY", 40.0f, 24.0f, 0.98f, 0.88f, 0.94f, 1.0f, 1.0f);

  char hdr_buf[160];
  std::snprintf(hdr_buf, sizeof(hdr_buf), "Category: %s   |   Mode: %s   |   CGs: %d / %d",
                cat_name.c_str(),
                config_ ? config_->content_label() : "All-Ages",
                extras_->unlocked_cg_group_count(cm, unlock_all),
                extras_->total_cg_group_count(cm));
  text_->draw_simple(hdr_buf, 220.0f, 28.0f, 0.72f, 0.78f, 0.90f, 1.0f, 0.95f);

  renderer_->draw_rect(36.0f, 58.0f, 888.0f, 1.0f, 0.30f, 0.45f, 0.70f, 0.7f);

  int count = static_cast<int>(filtered_.size());
  int total_pages = std::max(1, (count + kGridPerPage - 1) / kGridPerPage);
  int cur_page = (count > 0) ? (selected_ / kGridPerPage) : 0;
  int page_start = cur_page * kGridPerPage;

  for (int slot = 0; slot < kGridPerPage; ++slot) {
    int idx = page_start + slot;
    if (idx >= count) break;
    const CgGroupInfo* g = filtered_[static_cast<size_t>(idx)];
    int col = slot % kGridCols;
    int row = slot / kGridCols;
    float cx = 46.0f + col * 220.0f;
    float cy = 68.0f + row * 122.0f;
    bool sel = (idx == selected_);
    bool unlocked = extras_->is_cg_group_unlocked(*g, unlock_all);

    if (sel) {
      float glow = 0.45f + 0.14f * std::sin(anim_time_ * 5.0f);
      renderer_->draw_rect(cx - 2.0f, cy - 2.0f, 208.0f, 118.0f, 0.24f, 0.56f, 0.96f, glow);
    }
    renderer_->draw_rect(cx, cy, 204.0f, 114.0f, 0.08f, 0.11f, 0.20f, 0.88f);

    if (unlocked) {
      std::string rel_t = resolve_group_thumb(*g);
      const Texture* ttex = rel_t.empty() ? nullptr : get_thumb_texture(data_root_ + "/" + rel_t);
      if (ttex) {
        renderer_->draw_texture(ttex, cx + 22.0f, cy + 4.0f, 160.0f, 90.0f, 1.0f);
      } else {
        renderer_->draw_rect(cx + 22.0f, cy + 4.0f, 160.0f, 90.0f, 0.14f, 0.22f, 0.36f, 1.0f);
      }
      if (g->is_movie) {
        renderer_->draw_rect(cx + 22.0f, cy + 4.0f, 160.0f, 22.0f, 0.04f, 0.08f, 0.16f, 0.82f);
        text_->draw_simple("MOVIE [PLAY]", cx + 48.0f, cy + 7.0f, 0.62f, 0.65f, 0.95f, 1.0f, 1.0f);
        text_->draw_simple(g->movie_title, cx + 18.0f, cy + 96.0f, 0.62f, 0.92f, 0.96f, 1.0f, 1.0f);
      } else {
        int seen_vars = 0;
        for (const auto& v : g->variants) {
          if (extras_->is_cg_variant_seen(v.stem, unlock_all)) ++seen_vars;
        }
        char lbl[64];
        std::snprintf(lbl, sizeof(lbl), "%s   (%d/%u)", g->id.c_str(), seen_vars,
                      static_cast<unsigned>(g->variants.size()));
        text_->draw_simple(lbl, cx + 18.0f, cy + 96.0f, 0.62f, 0.92f, 0.96f, 1.0f, 1.0f);
      }
    } else {
      renderer_->draw_rect(cx + 22.0f, cy + 4.0f, 160.0f, 90.0f, 0.05f, 0.07f, 0.12f, 1.0f);
      text_->draw_simple(g->is_movie ? "[MOVIE] LOCKED" : "LOCKED",
                         cx + (g->is_movie ? 44.0f : 72.0f), cy + 40.0f,
                         0.72f, 0.50f, 0.54f, 0.62f, 0.9f);
      text_->draw_simple(g->is_movie ? g->movie_title : g->id,
                         cx + 18.0f, cy + 96.0f, 0.62f, 0.52f, 0.56f, 0.65f, 0.85f);
    }
  }

  // Footer controls
  char page_lbl[48];
  std::snprintf(page_lbl, sizeof(page_lbl), "Pg %d/%d", cur_page + 1, total_pages);
  struct BtnDef {
    float x;
    float w;
    std::string lbl;
  };
  const BtnDef btns[6] = {
      {36.0f, 98.0f, "< Prev"},
      {142.0f, 98.0f, "Next >"},
      {248.0f, 160.0f, "Cat: " + cat_name},
      {416.0f, 236.0f, std::string("Mode: ") + (config_ ? config_->content_label() : "")},
      {660.0f, 164.0f, unlock_all ? "Unlock: ALL" : "Unlock: Norm"},
      {832.0f, 92.0f, "Back"},
  };
  for (const auto& b : btns) {
    renderer_->draw_rect(b.x, 446.0f, b.w, 34.0f, 0.14f, 0.22f, 0.38f, 0.85f);
    text_->draw_simple(b.lbl, b.x + 8.0f, 455.0f, 0.64f, 0.90f, 0.95f, 1.0f, 1.0f);
  }
  text_->draw_simple(page_lbl, 848.0f, 60.0f, 0.68f, 0.78f, 0.88f, 1.0f, 0.95f);

  text_->draw_simple(
      circle_confirm_
          ? "D-Pad: Browse | Circle/Tap: View | L/R: Category | Tri: Content Mode | Sel: Unlock All | Cross: Back"
          : "D-Pad: Browse | Cross/Tap: View | L/R: Category | Tri: Content Mode | Sel: Unlock All | Circle: Back",
      32.0f, 496.0f, 0.62f, 0.75f, 0.84f, 0.95f, 0.85f);
}

}  // namespace fsn
