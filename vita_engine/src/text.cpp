#include "text.hpp"
#include "log.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace fsn {

namespace {
constexpr float kLeftMargin = 50.0f;
constexpr float kTopMargin = 41.0f;
constexpr float kLineWidth = 840.0f;
constexpr float kLineHeightNormal = 37.0f;
constexpr float kLineHeightInterlude = 32.0f;

bool is_all_digits(const std::string& s, size_t start) {
  if (start >= s.size()) return false;
  for (size_t i = start; i < s.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
  }
  return true;
}

bool is_kag_inline_tag(const std::string& name) {
  if (name.rfind("line", 0) == 0 && is_all_digits(name, 4)) return true;
  static const char* const kTags[] = {
      "lr", "r", "nolr", "br", "l", "p", "font", "resetfont",
      "wdash", "wacky", "wsay", "indent", "endindent", "ruby",
      "ch", "graph", "align", "style", "er", "wait", "cm", "ct",
      "delay", "resetdelay", "szlig", "auml", "ouml", "uuml",
      "Auml", "Ouml", "Uuml", "hellip", "mdash", "locate", "emb",
  };
  for (const char* t : kTags) {
    if (name == t) return true;
  }
  return false;
}
}  // namespace

uint32_t TextRenderer::next_utf8(const std::string& s, size_t& i) {
  if (i >= s.size()) return 0;
  uint8_t c = static_cast<uint8_t>(s[i]);
  if (c < 0x80) {
    ++i;
    return c;
  }
  if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
    uint32_t cp = ((c & 0x1F) << 6) | (static_cast<uint8_t>(s[i + 1]) & 0x3F);
    i += 2;
    return cp;
  }
  if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
    uint32_t cp = ((c & 0x0F) << 12) |
                  ((static_cast<uint8_t>(s[i + 1]) & 0x3F) << 6) |
                  (static_cast<uint8_t>(s[i + 2]) & 0x3F);
    i += 3;
    return cp;
  }
  if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
    uint32_t cp = ((c & 0x07) << 18) |
                  ((static_cast<uint8_t>(s[i + 1]) & 0x3F) << 12) |
                  ((static_cast<uint8_t>(s[i + 2]) & 0x3F) << 6) |
                  (static_cast<uint8_t>(s[i + 3]) & 0x3F);
    i += 4;
    return cp;
  }
  ++i;
  return '?';
}

bool TextRenderer::init(Renderer* renderer, const std::string& font_path, int px_size) {
  renderer_ = renderer;
  font_px_ = px_size;

  FT_Library lib = nullptr;
  if (FT_Init_FreeType(&lib) != 0) {
    log::info("[Text] FT_Init_FreeType failed");
    return false;
  }
  FT_Face face = nullptr;
  if (FT_New_Face(lib, font_path.c_str(), 0, &face) != 0) {
    log::info("[Text] FT_New_Face failed: %s", font_path.c_str());
    FT_Done_FreeType(lib);
    return false;
  }
  FT_Set_Pixel_Sizes(face, 0, px_size);
  ft_lib_ = lib;
  ft_face_ = face;

  atlas_tex_ = renderer_->create_alpha_texture(kAtlasSize, kAtlasSize, nullptr);
  atlas_pen_x_ = 1;
  atlas_pen_y_ = 1;
  atlas_row_h_ = 0;

  // Pre-warm printable ASCII
  for (uint32_t cp = 32; cp < 127; ++cp) {
    get_glyph(cp);
  }
  get_glyph(0x2014);  // em-dash for [line2]
  get_glyph(0x2026);  // ellipsis

  clear_page();
  log::info("[Text] FreeType initialized (%s, %dpx)", font_path.c_str(), px_size);
  return true;
}

void TextRenderer::shutdown() {
  if (atlas_tex_) {
    glDeleteTextures(1, &atlas_tex_);
    atlas_tex_ = 0;
  }
  if (ft_face_) {
    FT_Done_Face(static_cast<FT_Face>(ft_face_));
    ft_face_ = nullptr;
  }
  if (ft_lib_) {
    FT_Done_FreeType(static_cast<FT_Library>(ft_lib_));
    ft_lib_ = nullptr;
  }
  glyphs_.clear();
}

const TextRenderer::Glyph* TextRenderer::get_glyph(uint32_t cp) {
  auto it = glyphs_.find(cp);
  if (it != glyphs_.end()) return &it->second;
  if (!ft_face_) return nullptr;

  FT_Face face = static_cast<FT_Face>(ft_face_);
  if (FT_Load_Char(face, cp, FT_LOAD_RENDER) != 0) {
    if (cp != '?') return get_glyph('?');
    return nullptr;
  }

  FT_GlyphSlot g = face->glyph;
  int gw = g->bitmap.width;
  int gh = g->bitmap.rows;

  if (atlas_pen_x_ + gw + 2 >= kAtlasSize) {
    atlas_pen_x_ = 1;
    atlas_pen_y_ += atlas_row_h_ + 2;
    atlas_row_h_ = 0;
  }
  if (atlas_pen_y_ + gh + 2 >= kAtlasSize) {
    // Atlas full; fallback to '?'
    if (cp != '?') return get_glyph('?');
    return nullptr;
  }

  if (gw > 0 && gh > 0) {
    renderer_->update_alpha_texture(atlas_tex_, atlas_pen_x_, atlas_pen_y_, gw, gh,
                                    g->bitmap.buffer);
  }

  Glyph out;
  out.w = gw;
  out.h = gh;
  out.bearing_x = g->bitmap_left;
  out.bearing_y = g->bitmap_top;
  out.advance = static_cast<int>(g->advance.x >> 6);
  out.u0 = static_cast<float>(atlas_pen_x_) / kAtlasSize;
  out.v0 = static_cast<float>(atlas_pen_y_) / kAtlasSize;
  out.u1 = static_cast<float>(atlas_pen_x_ + gw) / kAtlasSize;
  out.v1 = static_cast<float>(atlas_pen_y_ + gh) / kAtlasSize;

  atlas_pen_x_ += gw + 2;
  atlas_row_h_ = std::max(atlas_row_h_, gh);

  auto [ins, ok] = glyphs_.emplace(cp, out);
  return &ins->second;
}

void TextRenderer::clear_page() {
  layout_chars_.clear();
  current_runs_.clear();
  cursor_x_ = kLeftMargin;
  cursor_y_ = kTopMargin;
  revealed_chars_ = 0.0f;
  wait_page_break_ = false;

  if (atlas_tex_ && atlas_pen_y_ >= (kAtlasSize * 3) / 4) {
    std::vector<uint8_t> zeros(kAtlasSize * kAtlasSize, 0);
    renderer_->update_alpha_texture(atlas_tex_, 0, 0, kAtlasSize, kAtlasSize, zeros.data());
    glyphs_.clear();
    atlas_pen_x_ = 1;
    atlas_pen_y_ = 1;
    atlas_row_h_ = 0;
    for (uint32_t cp = 32; cp < 127; ++cp) get_glyph(cp);
    get_glyph(0x2014);
    get_glyph(0x2026);
  }
}

void TextRenderer::add_newline() {
  float lh = interlude_ ? kLineHeightInterlude : kLineHeightNormal;
  cursor_x_ = kLeftMargin;
  cursor_y_ += lh;
}

void TextRenderer::set_wait_prompt(bool waiting_page) {
  wait_page_break_ = waiting_page;
}

void TextRenderer::append_raw_line(const std::string& raw, bool is_interlude) {
  interlude_ = is_interlude;
  std::string plain_history;
  bool italic = false;
  size_t pos = 0;

  auto flush_seg = [&](const std::string& seg) {
    if (!seg.empty()) {
      layout_text_segment(seg, italic, is_interlude);
      plain_history += seg;
    }
  };

  std::string accum;
  while (pos < raw.size()) {
    if (raw[pos] == '[') {
      size_t end = raw.find(']', pos);
      if (end != std::string::npos) {
        std::string tag = raw.substr(pos + 1, end - pos - 1);
        size_t sp = tag.find_first_of(" \t=");
        std::string name = (sp == std::string::npos) ? tag : tag.substr(0, sp);

        if (!is_kag_inline_tag(name)) {
          accum.push_back('[');
          accum += tag;
          accum.push_back(']');
        } else if (name.rfind("line", 0) == 0 && is_all_digits(name, 4)) {
          int n = std::atoi(name.c_str() + 4);
          int count = std::max(1, std::min(24, (n + 1) / 2));
          for (int k = 0; k < count; ++k) accum += "\xE2\x80\x94";
        } else if (name == "wdash") {
          accum += "\xE2\x80\x94\xE2\x80\x94";
        } else if (name == "mdash") {
          accum += "\xE2\x80\x94";
        } else if (name == "hellip") {
          accum += "\xE2\x80\xA6";
        } else if (name == "szlig") {
          accum += "\xC3\x9F";
        } else if (name == "auml") {
          accum += "\xC3\xA4";
        } else if (name == "ouml") {
          accum += "\xC3\xB6";
        } else if (name == "uuml") {
          accum += "\xC3\xBC";
        } else if (name == "Auml") {
          accum += "\xC3\x84";
        } else if (name == "Ouml") {
          accum += "\xC3\x96";
        } else if (name == "Uuml") {
          accum += "\xC3\x9C";
        } else if (name == "wacky") {
          int n = 1;
          size_t lp = tag.find("len=");
          if (lp != std::string::npos) n = std::atoi(tag.c_str() + lp + 4);
          n = std::max(1, std::min(16, n));
          static const char kNoise[] = "#@%*?&";
          for (int k = 0; k < n; ++k) accum.push_back(kNoise[k % 6]);
        } else if (name == "lr" || name == "r" || name == "nolr" || name == "br") {
          flush_seg(accum);
          accum.clear();
          add_newline();
        } else if (name == "font") {
          if (tag.find("italic=true") != std::string::npos) {
            flush_seg(accum);
            accum.clear();
            italic = true;
          } else if (tag.find("italic=false") != std::string::npos) {
            flush_seg(accum);
            accum.clear();
            italic = false;
          }
        } else if (name == "resetfont") {
          flush_seg(accum);
          accum.clear();
          italic = false;
        }
        pos = end + 1;
        continue;
      }
    }
    accum.push_back(raw[pos]);
    ++pos;
  }
  flush_seg(accum);

  if (!plain_history.empty()) {
    history_.push_back(plain_history);
    if (history_.size() > 200) {
      history_.erase(history_.begin());
    }
  }
}

void TextRenderer::layout_text_segment(const std::string& utf8, bool italic, bool is_interlude) {
  float lh = is_interlude ? kLineHeightInterlude : kLineHeightNormal;
  float max_x = kLeftMargin + kLineWidth;

  // Word-wrap by spaces while keeping leading punctuation intact
  size_t i = 0;
  while (i < utf8.size()) {
    // Extract next word (or single space)
    size_t word_start = i;
    bool is_space = (utf8[i] == ' ');
    if (is_space) {
      ++i;
    } else {
      while (i < utf8.size() && utf8[i] != ' ') {
        next_utf8(utf8, i);
      }
    }
    std::string token = utf8.substr(word_start, i - word_start);

    // Measure token width
    float token_w = 0.0f;
    size_t m = 0;
    while (m < token.size()) {
      uint32_t cp = next_utf8(token, m);
      const Glyph* g = get_glyph(cp);
      if (g) token_w += g->advance;
    }

    if (!is_space && cursor_x_ > kLeftMargin && cursor_x_ + token_w > max_x) {
      cursor_x_ = kLeftMargin;
      cursor_y_ += lh;
    }

    // Skip leading space on a freshly wrapped line
    if (is_space && cursor_x_ <= kLeftMargin + 0.5f) {
      continue;
    }

    size_t k = 0;
    while (k < token.size()) {
      uint32_t cp = next_utf8(token, k);
      const Glyph* g = get_glyph(cp);
      if (!g) continue;
      LayoutChar lc;
      lc.cp = cp;
      lc.x = cursor_x_;
      lc.y = cursor_y_;
      lc.italic = italic;
      layout_chars_.push_back(lc);
      cursor_x_ += g->advance;
    }
  }
}

void TextRenderer::update(float dt_sec) {
  if (revealed_chars_ < static_cast<float>(layout_chars_.size())) {
    revealed_chars_ += chars_per_sec_ * dt_sec;
    if (revealed_chars_ > static_cast<float>(layout_chars_.size())) {
      revealed_chars_ = static_cast<float>(layout_chars_.size());
    }
  }
}

void TextRenderer::reveal_all() {
  revealed_chars_ = static_cast<float>(layout_chars_.size());
}

bool TextRenderer::is_revealing() const {
  return revealed_chars_ < static_cast<float>(layout_chars_.size()) - 0.5f;
}

void TextRenderer::draw(bool visible, bool /*interlude_mode*/, const Texture* page_break_tex,
                        const Texture* line_break_tex, float anim_time) {
  if (!visible || layout_chars_.empty()) return;

  size_t limit = std::min(layout_chars_.size(),
                          static_cast<size_t>(std::floor(revealed_chars_)));

  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glBindTexture(GL_TEXTURE_2D, atlas_tex_);

  // Shadow pass + main pass for crisp legibility over bright backgrounds
  for (int pass = 0; pass < 2; ++pass) {
    float ox = (pass == 0) ? 1.5f : 0.0f;
    float oy = (pass == 0) ? 1.5f : 0.0f;
    if (pass == 0) {
      glColor4f(0.0f, 0.0f, 0.0f, 0.85f);
    } else {
      glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    }

    glBegin(GL_QUADS);
    for (size_t idx = 0; idx < limit; ++idx) {
      const LayoutChar& lc = layout_chars_[idx];
      const Glyph* g = get_glyph(lc.cp);
      if (!g || g->w == 0 || g->h == 0) continue;

      float gx = lc.x + g->bearing_x + ox;
      float gy = lc.y + font_px_ - g->bearing_y + oy;
      float shear = lc.italic ? 4.5f : 0.0f;

      glTexCoord2f(g->u0, g->v0); glVertex2f(gx + shear, gy);
      glTexCoord2f(g->u1, g->v0); glVertex2f(gx + g->w + shear, gy);
      glTexCoord2f(g->u1, g->v1); glVertex2f(gx + g->w, gy + g->h);
      glTexCoord2f(g->u0, g->v1); glVertex2f(gx, gy + g->h);
    }
    glEnd();
  }

  // Animated line/page break glyph when waiting for input
  if (!is_revealing()) {
    const Texture* ind = wait_page_break_ ? page_break_tex : line_break_tex;
    if (ind && ind->id != 0) {
      float pulse = 0.65f + 0.35f * std::sin(anim_time * 5.0f);
      // PageBreak_a.png / LineBreak_a.png are horizontal sprite strips
      int frames = wait_page_break_ ? 12 : 12;
      int frame = static_cast<int>(anim_time * 16.0f) % frames;
      float fw = 1.0f / frames;
      float u0 = frame * fw;
      float u1 = (frame + 1) * fw;
      float sz = 22.0f;
      renderer_->draw_texture_uv(ind->id, cursor_x_ + 6.0f, cursor_y_ + 4.0f, sz, sz,
                                 u0, 0.0f, u1, 1.0f, 1.0f, 1.0f, 1.0f, pulse);
    }
  }
}

void TextRenderer::draw_simple(const std::string& utf8, float x, float y, float scale,
                               float r, float g, float b, float a) {
  float cx = x;
  size_t i = 0;
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glBindTexture(GL_TEXTURE_2D, atlas_tex_);
  glColor4f(r, g, b, a);
  glBegin(GL_QUADS);
  while (i < utf8.size()) {
    uint32_t cp = next_utf8(utf8, i);
    const Glyph* gl = get_glyph(cp);
    if (!gl) continue;
    if (gl->w > 0 && gl->h > 0) {
      float gx = cx + gl->bearing_x * scale;
      float gy = y + (font_px_ - gl->bearing_y) * scale;
      float gw = gl->w * scale;
      float gh = gl->h * scale;
      glTexCoord2f(gl->u0, gl->v0); glVertex2f(gx, gy);
      glTexCoord2f(gl->u1, gl->v0); glVertex2f(gx + gw, gy);
      glTexCoord2f(gl->u1, gl->v1); glVertex2f(gx + gw, gy + gh);
      glTexCoord2f(gl->u0, gl->v1); glVertex2f(gx, gy + gh);
    }
    cx += gl->advance * scale;
  }
  glEnd();
}

}  // namespace fsn
