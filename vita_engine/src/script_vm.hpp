#pragma once

#include "audio.hpp"
#include "config.hpp"
#include "renderer.hpp"
#include "text.hpp"

#include <cstdint>
#include <json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace fsn {

class ExtrasManager;

class ScriptVM {
 public:
  bool init(const std::string& data_root, Renderer* renderer, AudioEngine* audio,
            TextRenderer* text, AppConfig* config, const std::string& global_flags_path,
            ExtrasManager* extras = nullptr);

  TitleBgmInfo get_title_bgm_info() const;
  const nlohmann::json& manifest() const { return manifest_; }
  std::string resolve_bgm_for_ost(const std::string& raw_name, OstMode ost,
                                  int64_t* loop_start = nullptr,
                                  int64_t* loop_len = nullptr) const;

  bool start_preset(int preset_index, const std::string& choice_backup_path = "");
  bool start_flowchart(const std::string& fcf_stem, int start_node = -1);
  bool load_script(const std::string& rel_script_path);
  bool load_script_by_stem(const std::string& stem, const std::string& target_label = "",
                           bool preserve_scene = false);

  bool load_quicksave(const std::string& save_path);
  bool write_quicksave(const std::string& save_path) const;

  void set_choice_backup_path(const std::string& path) { choice_backup_path_ = path; }
  void refresh_after_config_change();

  void update(float dt_sec, bool btn_up, bool btn_down, bool btn_advance,
              bool btn_skip_held, bool btn_toggle_text,
              bool touch_tap = false, float touch_x = 0.0f, float touch_y = 0.0f);
  void draw();

  bool is_finished() const { return finished_; }
  bool is_backlog_open() const { return backlog_open_; }
  void set_backlog_open(bool open) { backlog_open_ = open; }
  void scroll_backlog(int delta);
  bool is_choice_active() const { return choice_active_; }
  std::string status_summary() const;

 private:
  struct Layer {
    bool visible = false;
    std::string image_name;
    std::string image_path;
    float x = 0.0f;
    float y = 0.0f;
    float alpha = 1.0f;
    float scale = 1.0f;
    bool fliplr = false;
    bool flipud = false;
    int z_index = 0;
    BlendMode blend = BlendMode::Alpha;

    // Tween state
    bool moving = false;
    float move_elapsed = 0.0f;
    float move_duration = 0.0f;
    float start_x = 0.0f, start_y = 0.0f, start_a = 1.0f;
    float end_x = 0.0f, end_y = 0.0f, end_a = 1.0f;
    float accel = 0.0f;
  };

  struct BgTransition {
    bool active = false;
    float elapsed = 0.0f;
    float duration = 0.0f;
    float vague = 0.15f;
    std::string next_name;
    std::string next_path;
    std::string rule_path;
    bool next_fliplr = false;
    bool next_flipud = false;
    bool to_solid = false;
    float solid_r = 0.0f, solid_g = 0.0f, solid_b = 0.0f;
  };

  struct CallFrame {
    std::string script_stem;
    size_t ip = 0;
    nlohmann::json script;
    float coord_scale = 0.5f;
    float coord_ox = 0.0f;
    float coord_oy = 0.0f;
  };

  struct ChoiceItem {
    int target = -1;
    std::string text;
  };

  std::string data_root_;
  std::string global_flags_path_;
  std::string choice_backup_path_;
  Renderer* renderer_ = nullptr;
  AudioEngine* audio_ = nullptr;
  TextRenderer* text_ = nullptr;
  AppConfig* config_ = nullptr;
  ExtrasManager* extras_ = nullptr;

  nlohmann::json manifest_;
  nlohmann::json script_;
  nlohmann::json flowchart_;
  std::unordered_map<std::string, float> correct_x_;

  // Persistent global ("g*") and route-local flags
  std::unordered_map<std::string, int> global_flags_;
  std::unordered_map<std::string, int> route_flags_;

  // Flowchart tracking
  bool in_flowchart_ = false;
  std::string fcf_stem_;
  int fcf_node_id_ = 0;
  std::string cur_script_stem_;
  std::vector<CallFrame> call_stack_;

  // Coordinate space conversion for current script (1920x1080 or 800x600 -> 960x544)
  float coord_scale_ = 0.5f;
  float coord_ox_ = 0.0f;
  float coord_oy_ = 0.0f;

  size_t ip_ = 0;
  bool finished_ = false;

  bool wait_click_ = false;
  bool wait_page_ = false;
  float wait_timer_ = 0.0f;
  bool wait_timer_skippable_ = true;
  bool wait_move_ = false;
  bool wait_trans_ = false;

  // Interactive choice / Tiger retry UI
  bool choice_active_ = false;
  bool tiger_retry_mode_ = false;
  std::string choice_prompt_;
  std::vector<ChoiceItem> choices_;
  int choice_selected_ = 0;

  // Staffroll / credit display accumulator
  std::vector<std::string> credits_lines_;
  float credits_scroll_ = 0.0f;
  bool credits_active_ = false;

  bool text_visible_ = true;
  bool user_hid_text_ = false;
  bool interlude_mode_ = false;
  bool backlog_open_ = false;
  int backlog_scroll_ = 0;

  // Base background + color state
  std::string bg_name_;
  std::string bg_path_;
  bool bg_fliplr_ = false;
  bool bg_flipud_ = false;
  bool bg_is_solid_ = true;
  float bg_r_ = 0.0f, bg_g_ = 0.0f, bg_b_ = 0.0f;
  float tint_r_ = 1.0f, tint_g_ = 1.0f, tint_b_ = 1.0f;
  float darken_alpha_ = 0.0f;

  BgTransition bg_trans_;

  // Active BGM stem (for live OST hot-swap and quicksave)
  std::string cur_bgm_stem_;
  bool cur_bgm_playing_ = false;

  // Character slots ("fg_*"), numbered/named layers ("L_*", "S_*", "superpose", "dash_*")
  std::unordered_map<std::string, Layer> layers_;

  // Quake camera shake
  float quake_timer_ = 0.0f;
  float quake_hmax_ = 0.0f;
  float quake_vmax_ = 0.0f;
  float anim_time_ = 0.0f;

  std::string resolve_image(const std::string& name) const;
  std::string resolve_rule(const std::string& name) const;
  std::string resolve_audio(const std::string& cat, const std::string& name,
                            int64_t* loop_start = nullptr, int64_t* loop_len = nullptr) const;
  std::string resolve_ui(const std::string& alias) const;

  void update_coord_transform();
  float to_vita_x(float x) const { return coord_ox_ + x * coord_scale_; }
  float to_vita_y(float y) const { return coord_oy_ + y * coord_scale_; }

  void load_global_flags();
  void save_global_flags() const;
  int get_flag(const std::string& name) const;
  void set_flag(const std::string& name, int val);
  bool eval_flag_cond(const nlohmann::json& cond) const;
  void apply_flag_ops(const nlohmann::json& ops);

  bool enter_fcf_node(int node_id);
  void complete_fcf_scene();
  void on_script_end();
  void clear_character_layers();
  void load_fg_slot(const std::string& pos, const std::string& file, int z_idx,
                    bool fliplr, bool flipud, int time_ms);
  void play_bgm_stem(const std::string& stem, int fade_ms);

  void step_until_blocked();
  void exec_cmd(const std::string& cmd, const nlohmann::json& args);
  void start_bg_transition(const std::string& target, const nlohmann::json& args,
                           bool solid, float sr = 0.0f, float sg = 0.0f, float sb = 0.0f);
  void draw_layer(const Layer& l, float qx, float qy);

  static float arg_float(const nlohmann::json& args, const char* key, float def = 0.0f);
  static int arg_int(const nlohmann::json& args, const char* key, int def = 0);
  static std::string arg_str(const nlohmann::json& args, const char* key, const char* def = "");
  static bool arg_bool(const nlohmann::json& args, const char* key, bool def = false);
  float pos_to_center_x(const std::string& pos) const;
};

}  // namespace fsn
