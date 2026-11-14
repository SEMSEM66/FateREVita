#pragma once

#include <vitaGL.h>
#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>

namespace fsn {

enum class BlendMode {
  Alpha,
  Additive,
  Multiply,
};

struct Texture {
  GLuint id = 0;
  int w = 0;
  int h = 0;
};

class Renderer {
 public:
  bool init();
  void shutdown();

  void begin_frame();
  void end_frame();

  const Texture* get_texture(const std::string& path);
  void release_texture(const std::string& path);
  void clear_cache();

  GLuint create_alpha_texture(int w, int h, const uint8_t* alpha_pixels);
  void update_alpha_texture(GLuint tex, int x, int y, int w, int h, const uint8_t* alpha_pixels);

  void draw_rect(float x, float y, float w, float h, float r, float g, float b, float a);

  void draw_texture(const Texture* tex, float x, float y, float w, float h,
                    float alpha = 1.0f, bool fliplr = false, bool flipud = false,
                    BlendMode blend = BlendMode::Alpha,
                    float tr = 1.0f, float tg = 1.0f, float tb = 1.0f);

  void draw_texture_uv(GLuint tex_id, float x, float y, float w, float h,
                       float u0, float v0, float u1, float v1,
                       float r, float g, float b, float a);

  void draw_rule_wipe(const Texture* next_tex, const Texture* rule_tex,
                      float x, float y, float w, float h,
                      float progress, float vague, bool fliplr, bool flipud);

  bool has_shaders() const { return has_shaders_; }

 private:
  struct CacheEntry {
    Texture tex;
    std::list<std::string>::iterator lru_it;
  };

  std::unordered_map<std::string, CacheEntry> cache_;
  std::list<std::string> lru_;
  static constexpr size_t kMaxCachedTextures = 28;

  bool has_shaders_ = false;
  GLuint rule_prog_ = 0;
  GLint u_rule_tex_ = -1;
  GLint u_rule_mask_ = -1;
  GLint u_rule_prog_ = -1;
  GLint u_rule_vague_ = -1;

  void init_shaders();
  void evict_if_needed();
};

}  // namespace fsn
