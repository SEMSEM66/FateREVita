#include "title_screen.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fsn {

namespace {

int wrap_idx(int cur, int delta, int count) {
  return ((cur + delta) % count + count) % count;
}

float step_vol(float cur, int delta) {
  int pct = static_cast<int>(std::round(cur * 10.0f)) + delta;
  pct = std::max(0, std::min(10, pct));
  return pct / 10.0f;
}

}  // namespace

void TitleScreen::init(const std::string& data_root, Renderer* renderer, AudioEngine* audio,
                       TextRenderer* text, AppConfig* config, const std::string& config_path,
                       const TitleBgmInfo& bgm_info, bool has_save) {
  data_root_ = data_root;
  renderer_ = renderer;
  audio_ = audio;
  text_ = text;
  config_ = config;
  config_path_ = config_path;
  bgm_info_ = bgm_info;
  has_save_ = has_save;
  apply_volumes();
}

void TitleScreen::apply_volumes() {
  if (audio_ && config_) {
    audio_->set_master_volumes(config_->bgm_vol, config_->se_vol, config_->voice_vol);
  }
}

void TitleScreen::play_title_bgm() {
  if (!audio_ || !config_) return;
  if (config_->ost_mode == OstMode::Old && !bgm_info_.old_path.empty()) {
    audio_->play_bgm(bgm_info_.old_path, 450, true,
                     bgm_info_.old_loop_start, bgm_info_.old_loop_len);
  } else if (!bgm_info_.new_path.empty()) {
    audio_->play_bgm(bgm_info_.new_path, 450, true);
  }
}

void TitleScreen::enter(bool has_save) {
  has_save_ = has_save;
  sub_mode_ = SubMode::Main;
  selected_ = has_save_ ? 1 : 0;
  sub_row_ = 0;
  apply_volumes();
  play_title_bgm();
}

void TitleScreen::adjust_option(int row, int dir) {
  if (!config_) return;
  if (row == 0) {
    config_->ost_mode = (config_->ost_mode == OstMode::New) ? OstMode::Old : OstMode::New;
    play_title_bgm();
  } else if (row == 1) {
    int cm = (static_cast<int>(config_->content_mode) + dir + 3) % 3;
    config_->content_mode = static_cast<ContentMode>(cm);
  } else if (row == 2) {
    config_->unlock_all_extras = !config_->unlock_all_extras;
  } else if (row == 3) {
    config_->bgm_vol = step_vol(config_->bgm_vol, dir);
    apply_volumes();
  } else if (row == 4) {
    config_->se_vol = step_vol(config_->se_vol, dir);
    apply_volumes();
  } else if (row == 5) {
    config_->voice_vol = step_vol(config_->voice_vol, dir);
    apply_volumes();
  }
  audio_->play_se("ui_move", data_root_ + "/audio/se/se_title_move.ogg");
}

TitleAction TitleScreen::update(float dt_sec, bool up, bool down, bool left, bool right,
                                bool confirm, bool back, bool touch_tap,
                                float touch_x, float touch_y) {
  anim_time_ += dt_sec;

  if (sub_mode_ == SubMode::Main) {
    const int count = 6;  // 0: New Game, 1: Continue, 2: Music Room, 3: CG Gallery, 4: Config, 5: Exit
    if (up) {
      do {
        selected_ = (selected_ + count - 1) % count;
      } while (selected_ == 1 && !has_save_);
      audio_->play_se("ui_move", data_root_ + "/audio/se/se_title_move.ogg");
    }
    if (down) {
      do {
        selected_ = (selected_ + 1) % count;
      } while (selected_ == 1 && !has_save_);
      audio_->play_se("ui_move", data_root_ + "/audio/se/se_title_move.ogg");
    }

    if (touch_tap) {
      const float bx = 80.0f;
      const float bw = 260.0f;
      for (int i = 0; i < count; ++i) {
        if (i == 1 && !has_save_) continue;
        float by = 216.0f + i * 40.0f;
        if (touch_x >= bx && touch_x <= bx + bw && touch_y >= by && touch_y <= by + 36.0f) {
          selected_ = i;
          confirm = true;
          break;
        }
      }
    }

    if (confirm) {
      audio_->play_se("ui_decide", data_root_ + "/audio/se/se_title_decide.ogg");
      if (selected_ == 0) return TitleAction::NewGame;
      if (selected_ == 1 && has_save_) return TitleAction::Continue;
      if (selected_ == 2) return TitleAction::MusicRoom;
      if (selected_ == 3) return TitleAction::CgGallery;
      if (selected_ == 4) {
        sub_mode_ = SubMode::ConfigMenu;
        sub_row_ = 0;
        return TitleAction::None;
      }
      if (selected_ == 5) return TitleAction::Exit;
    }
    return TitleAction::None;
  }

  // ConfigMenu: 6 options + Save & Back
  const int opt_count = 6;
  const int rows = 7;

  if (back) {
    config_->save(config_path_);
    audio_->play_se("ui_move", data_root_ + "/audio/se/se_title_move.ogg");
    sub_mode_ = SubMode::Main;
    return TitleAction::None;
  }

  if (up) {
    sub_row_ = wrap_idx(sub_row_, -1, rows);
    audio_->play_se("ui_move", data_root_ + "/audio/se/se_title_move.ogg");
  }
  if (down) {
    sub_row_ = wrap_idx(sub_row_, 1, rows);
    audio_->play_se("ui_move", data_root_ + "/audio/se/se_title_move.ogg");
  }

  if (left && sub_row_ < opt_count) {
    adjust_option(sub_row_, -1);
  }
  if (right && sub_row_ < opt_count) {
    adjust_option(sub_row_, 1);
  }

  if (touch_tap) {
    const float px = 110.0f;
    const float pw = 740.0f;
    const float start_y = 148.0f;
    const float row_h = 46.0f;
    for (int i = 0; i < rows; ++i) {
      float ry = start_y + i * row_h;
      if (touch_x >= px && touch_x <= px + pw && touch_y >= ry && touch_y <= ry + 38.0f) {
        sub_row_ = i;
        if (i < opt_count) {
          adjust_option(i, (touch_x < 610.0f) ? -1 : 1);
        } else {
          confirm = true;
        }
        break;
      }
    }
  }

  if (confirm) {
    if (sub_row_ < opt_count) {
      adjust_option(sub_row_, 1);
    } else {
      config_->save(config_path_);
      audio_->play_se("ui_decide", data_root_ + "/audio/se/se_title_decide.ogg");
      sub_mode_ = SubMode::Main;
    }
  }
  return TitleAction::None;
}

void TitleScreen::draw_main() {
  const Texture* logo = renderer_->get_texture(data_root_ + "/images/ui/title_logo.png");
  if (logo) {
    float lw = static_cast<float>(logo->w);
    float lh = static_cast<float>(logo->h);
    renderer_->draw_texture(logo, 520.0f, 36.0f, lw, lh, 0.96f);
  }

  const char* btn_tex[6][2] = {
      {"btn_newgame_1.png", "btn_newgame_2.png"},
      {"btn_continue_1.png", "btn_continue_2.png"},
      {"", ""},
      {"", ""},
      {"btn_config_1.png", "btn_config_2.png"},
      {"", ""},
  };
  const char* labels[6] = {"NEW GAME", "CONTINUE", "MUSIC ROOM", "CG GALLERY", "CONFIG", "EXIT"};

  for (int i = 0; i < 6; ++i) {
    float by = 216.0f + i * 40.0f;
    bool enabled = !(i == 1 && !has_save_);
    bool sel = (selected_ == i);
    float alpha = enabled ? (sel ? 1.0f : 0.72f) : 0.30f;

    if (sel) {
      float glow = 0.25f + 0.12f * std::sin(anim_time_ * 4.5f);
      renderer_->draw_rect(72.0f, by - 2.0f, 260.0f, 36.0f, 0.20f, 0.45f, 0.85f, glow);
    }

    const Texture* btex = nullptr;
    if (btn_tex[i][0][0] != '\0') {
      std::string fn = sel ? btn_tex[i][1] : btn_tex[i][0];
      btex = renderer_->get_texture(data_root_ + "/images/ui/" + fn);
    }
    if (btex) {
      renderer_->draw_texture(btex, 84.0f, by, static_cast<float>(btex->w),
                              static_cast<float>(btex->h), alpha);
    } else {
      text_->draw_simple(labels[i], 96.0f, by + 5.0f, 0.88f, 1.0f, 1.0f, 1.0f, alpha);
    }
  }

  if (config_) {
    std::string status = std::string("OST: ") + config_->ost_label() +
                         "  |  Content: " + config_->content_label();
    text_->draw_simple(status, 36.0f, 476.0f, 0.76f, 0.86f, 0.92f, 1.0f, 0.92f);
  }
  text_->draw_simple("D-Pad: Move | Confirm: Select | Start (In-Game): Pause/Menu",
                     36.0f, 506.0f, 0.72f, 0.75f, 0.82f, 0.92f, 0.80f);
}

void TitleScreen::draw_config_panel() {
  renderer_->draw_rect(80.0f, 64.0f, 800.0f, 430.0f, 0.04f, 0.06f, 0.13f, 0.92f);
  renderer_->draw_rect(80.0f, 64.0f, 800.0f, 3.0f, 0.35f, 0.62f, 0.95f, 0.9f);
  renderer_->draw_rect(80.0f, 491.0f, 800.0f, 3.0f, 0.35f, 0.62f, 0.95f, 0.9f);

  text_->draw_simple("CONFIGURATION & AUDIO PREFERENCES",
                     110.0f, 84.0f, 0.96f, 0.85f, 0.92f, 1.0f, 1.0f);
  text_->draw_simple("Changes preview live and save automatically to ux0:data/FateReVita/config.json.",
                     110.0f, 114.0f, 0.72f, 0.72f, 0.80f, 0.90f, 0.90f);

  const int opt_count = 6;
  const int rows = 7;
  const float start_y = 148.0f;
  const float row_h = 46.0f;

  char vol_buf[3][16];
  if (config_) {
    std::snprintf(vol_buf[0], sizeof(vol_buf[0]), "%d%%",
                  static_cast<int>(std::round(config_->bgm_vol * 100.0f)));
    std::snprintf(vol_buf[1], sizeof(vol_buf[1]), "%d%%",
                  static_cast<int>(std::round(config_->se_vol * 100.0f)));
    std::snprintf(vol_buf[2], sizeof(vol_buf[2]), "%d%%",
                  static_cast<int>(std::round(config_->voice_vol * 100.0f)));
  }

  for (int i = 0; i < rows; ++i) {
    float ry = start_y + i * row_h;
    bool sel = (sub_row_ == i);
    if (sel) {
      float glow = 0.35f + 0.10f * std::sin(anim_time_ * 5.0f);
      renderer_->draw_rect(104.0f, ry, 752.0f, 36.0f, 0.22f, 0.48f, 0.88f, glow);
    } else {
      renderer_->draw_rect(104.0f, ry, 752.0f, 36.0f, 0.10f, 0.14f, 0.24f, 0.45f);
    }

    if (i < opt_count && config_) {
      std::string name;
      std::string val;
      if (i == 0) {
        name = "Soundtrack";
        val = config_->ost_label();
      } else if (i == 1) {
        name = "Content Edition";
        val = config_->content_label();
      } else if (i == 2) {
        name = "Extras Unlock";
        val = config_->unlock_all_extras ? "Unlock All (Music & CGs)" : "Normal (Unlock in Game)";
      } else if (i == 3) {
        name = "BGM Volume";
        val = vol_buf[0];
      } else if (i == 4) {
        name = "SE Volume";
        val = vol_buf[1];
      } else if (i == 5) {
        name = "Voice Volume";
        val = vol_buf[2];
      }
      text_->draw_simple(name, 124.0f, ry + 7.0f, 0.84f, 0.90f, 0.94f, 1.0f, 1.0f);
      std::string arrows = "<  " + val + "  >";
      float vr = sel ? 1.0f : 0.78f;
      float vg = sel ? 0.96f : 0.88f;
      text_->draw_simple(arrows, 460.0f, ry + 7.0f, 0.82f, vr, vg, 0.65f, 1.0f);
    } else {
      text_->draw_simple("[ SAVE & BACK ]", 380.0f, ry + 7.0f, 0.90f, 0.85f, 0.92f, 1.0f, 1.0f);
    }
  }

  text_->draw_simple("Up/Down: Row  |  Left/Right: Change  |  Confirm: Cycle/OK  |  Back/Triangle: Return",
                     110.0f, 508.0f, 0.72f, 0.80f, 0.86f, 0.95f, 0.85f);
}

void TitleScreen::draw() {
  const Texture* bg = renderer_->get_texture(data_root_ + "/images/ui/title_bg_01.png");
  if (bg) {
    renderer_->draw_texture(bg, 0.0f, 2.0f, 960.0f, 540.0f, 1.0f);
  } else {
    renderer_->draw_rect(0.0f, 0.0f, 960.0f, 544.0f, 0.05f, 0.07f, 0.14f, 1.0f);
  }

  if (sub_mode_ == SubMode::Main) {
    draw_main();
  } else {
    draw_config_panel();
  }
}

}  // namespace fsn
