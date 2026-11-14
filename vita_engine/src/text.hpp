#pragma once

#include "renderer.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace fsn {

struct TextRun {
  std::string utf8;
  bool italic = false;
  bool newline_after = false;
};

class TextRenderer {
 public:
  bool init(Renderer* renderer, const std::string& font_path, int px_size = 22);
  void shutdown();

  void clear_page();
  void append_raw_line(const std::string& raw, bool is_interlude = false);
  void add_newline();
  void set_wait_prompt(bool waiting_page);

  void update(float dt_sec);
  void reveal_all();
  bool is_revealing() const;

  void draw(bool visible, bool interlude_mode, const Texture* page_break_tex,
            const Texture* line_break_tex, float anim_time);

  void draw_simple(const std::string& utf8, float x, float y, float scale = 1.0f,
                   float r = 1.0f, float g = 1.0f, float b = 1.0f, float a = 1.0f);

  const std::vector<std::string>& history() const { return history_; }

 private:
  struct Glyph {
    int w = 0;
    int h = 0;
    int bearing_x = 0;
    int bearing_y = 0;
    int advance = 0;
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
  };

  struct LayoutChar {
    uint32_t cp = 0;
    float x = 0.0f;
    float y = 0.0f;
    bool italic = false;
  };

  Renderer* renderer_ = nullptr;
  void* ft_lib_ = nullptr;
  void* ft_face_ = nullptr;
  int font_px_ = 22;

  GLuint atlas_tex_ = 0;
  static constexpr int kAtlasSize = 512;
  int atlas_pen_x_ = 1;
  int atlas_pen_y_ = 1;
  int atlas_row_h_ = 0;
  std::unordered_map<uint32_t, Glyph> glyphs_;

  std::vector<TextRun> current_runs_;
  std::vector<LayoutChar> layout_chars_;
  std::vector<std::string> history_;

  float cursor_x_ = 50.0f;
  float cursor_y_ = 41.0f;
  float revealed_chars_ = 0.0f;
  float chars_per_sec_ = 55.0f;
  bool wait_page_break_ = false;
  bool interlude_ = false;

  const Glyph* get_glyph(uint32_t cp);
  void layout_text_segment(const std::string& utf8, bool italic, bool is_interlude);
  static uint32_t next_utf8(const std::string& s, size_t& idx);
};

}  // namespace fsn
