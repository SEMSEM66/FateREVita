#pragma once

#include "audio.hpp"
#include "config.hpp"
#include "renderer.hpp"
#include "text.hpp"
#include <cstdint>
#include <string>

namespace fsn {

enum class TitleAction {
  None,
  NewGame,
  Continue,
  Exit,
};

class TitleScreen {
 public:
  void init(const std::string& data_root, Renderer* renderer, AudioEngine* audio,
            TextRenderer* text, AppConfig* config, const std::string& config_path,
            const TitleBgmInfo& bgm_info, bool has_save);
  void enter(bool has_save);
  TitleAction update(float dt_sec, bool up, bool down, bool left, bool right,
                     bool confirm, bool back, bool touch_tap, float touch_x, float touch_y);
  void draw();

  int start_preset() const { return start_preset_; }
  bool is_main() const { return sub_mode_ == SubMode::Main; }
  void apply_volumes();

 private:
  enum class SubMode {
    Main,
    NewGameSetup,
    ConfigMenu,
  };

  std::string data_root_;
  std::string config_path_;
  Renderer* renderer_ = nullptr;
  AudioEngine* audio_ = nullptr;
  TextRenderer* text_ = nullptr;
  AppConfig* config_ = nullptr;
  TitleBgmInfo bgm_info_;

  bool has_save_ = false;
  SubMode sub_mode_ = SubMode::Main;
  int selected_ = 0;
  int sub_row_ = 0;
  int start_preset_ = 0;
  float anim_time_ = 0.0f;

  void play_title_bgm();
  void adjust_option(bool is_setup, int row, int dir);
  void draw_main();
  void draw_options_panel(bool is_setup);
};

}  // namespace fsn
