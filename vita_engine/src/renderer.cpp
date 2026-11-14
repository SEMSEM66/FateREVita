#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "renderer.hpp"
#include "log.hpp"

#include <psp2/io/stat.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace fsn {

namespace {

bool file_exists(const char* path) {
  SceIoStat st{};
  return sceIoGetstat(path, &st) >= 0;
}

const char* kRuleVert = R"(
void main(
    float3 aPos,
    float2 aUV,
    float2 aRuleUV,
    out float4 oPos : POSITION,
    out float2 vUV : TEXCOORD0,
    out float2 vRuleUV : TEXCOORD1,
    uniform float4x4 uProj
) {
    oPos = mul(float4(aPos, 1.0), uProj);
    vUV = aUV;
    vRuleUV = aRuleUV;
}
)";

const char* kRuleFrag = R"(
float4 main(
    float2 vUV : TEXCOORD0,
    float2 vRuleUV : TEXCOORD1,
    uniform sampler2D uTex : TEXUNIT0,
    uniform sampler2D uMask : TEXUNIT1,
    uniform float uProgress,
    uniform float uVague
) : COLOR {
    float4 col = tex2D(uTex, vUV);
    float m = tex2D(uMask, vRuleUV).r;
    float edge = clamp(uVague, 0.01, 0.5);
    float p = uProgress * (1.0 + edge * 2.0) - edge;
    float alpha = 1.0 - smoothstep(p - edge, p + edge, m);
    col.a *= alpha;
    return col;
}
)";

GLuint compile_cg_shader(GLenum type, const char* src) {
  GLuint s = glCreateShader(type);
  if (!s) return 0;
  GLint len = static_cast<GLint>(std::strlen(src));
  glShaderSource(s, 1, &src, &len);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char info[512];
    glGetShaderInfoLog(s, sizeof(info), nullptr, info);
    log::info("[Renderer] Shader compile failed: %s", info);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

}  // namespace

bool Renderer::init() {
  const GLboolean res_overridden =
      vglInitExtended(0x100000, 960, 544, 16 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
  log::info("[Renderer] vglInitExtended completed (res_overridden=%d)",
            static_cast<int>(res_overridden));

  glViewport(0, 0, 960, 544);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(0.0, 960.0, 544.0, 0.0, -1.0, 1.0);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();

  glDisable(GL_DEPTH_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glEnable(GL_TEXTURE_2D);

  init_shaders();
  log::info("[Renderer] vitaGL initialized (960x544, shaders=%s)",
            has_shaders_ ? "enabled" : "fallback");
  return true;
}

void Renderer::init_shaders() {
  const bool has_shacc = file_exists("ur0:data/libshacccg.suprx") ||
                         file_exists("ux0:data/libshacccg.suprx");
  if (!has_shacc) {
    log::info("[Renderer] libshacccg.suprx not found; using fixed-function crossfade fallback");
    has_shaders_ = false;
    return;
  }

  GLuint vs = compile_cg_shader(GL_CG_VERTEX_SHADER_EXT, kRuleVert);
  GLuint fs = compile_cg_shader(GL_CG_FRAGMENT_SHADER_EXT, kRuleFrag);
  if (!vs || !fs) {
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    has_shaders_ = false;
    return;
  }

  rule_prog_ = glCreateProgram();
  glAttachShader(rule_prog_, vs);
  glAttachShader(rule_prog_, fs);
  glBindAttribLocation(rule_prog_, 0, "aPos");
  glBindAttribLocation(rule_prog_, 1, "aUV");
  glBindAttribLocation(rule_prog_, 2, "aRuleUV");
  glLinkProgram(rule_prog_);
  glDeleteShader(vs);
  glDeleteShader(fs);

  GLint linked = 0;
  glGetProgramiv(rule_prog_, GL_LINK_STATUS, &linked);
  if (!linked) {
    glDeleteProgram(rule_prog_);
    rule_prog_ = 0;
    has_shaders_ = false;
    return;
  }
  u_rule_tex_ = glGetUniformLocation(rule_prog_, "uTex");
  u_rule_mask_ = glGetUniformLocation(rule_prog_, "uMask");
  u_rule_prog_ = glGetUniformLocation(rule_prog_, "uProgress");
  u_rule_vague_ = glGetUniformLocation(rule_prog_, "uVague");
  has_shaders_ = true;
}

void Renderer::shutdown() {
  clear_cache();
  if (rule_prog_) {
    glDeleteProgram(rule_prog_);
    rule_prog_ = 0;
  }
}

void Renderer::begin_frame() {
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
}

void Renderer::end_frame() {
  vglSwapBuffers(GL_FALSE);
}

void Renderer::evict_if_needed() {
  while (cache_.size() >= kMaxCachedTextures && !lru_.empty()) {
    std::string oldest = lru_.back();
    lru_.pop_back();
    auto it = cache_.find(oldest);
    if (it != cache_.end()) {
      glDeleteTextures(1, &it->second.tex.id);
      cache_.erase(it);
    }
  }
}

const Texture* Renderer::get_texture(const std::string& path) {
  auto it = cache_.find(path);
  if (it != cache_.end()) {
    lru_.splice(lru_.begin(), lru_, it->second.lru_it);
    return &it->second.tex;
  }

  int w = 0, h = 0, ch = 0;
  stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &ch, 4);
  if (!pixels) {
    log::info("[Renderer] stbi_load failed: %s", path.c_str());
    return nullptr;
  }

  evict_if_needed();

  GLuint tid = 0;
  glGenTextures(1, &tid);
  glBindTexture(GL_TEXTURE_2D, tid);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  stbi_image_free(pixels);

  lru_.push_front(path);
  auto [ins, ok] = cache_.emplace(path, CacheEntry{{tid, w, h}, lru_.begin()});
  return &ins->second.tex;
}

void Renderer::release_texture(const std::string& path) {
  auto it = cache_.find(path);
  if (it != cache_.end()) {
    glDeleteTextures(1, &it->second.tex.id);
    lru_.erase(it->second.lru_it);
    cache_.erase(it);
  }
}

void Renderer::clear_cache() {
  for (auto& kv : cache_) {
    glDeleteTextures(1, &kv.second.tex.id);
  }
  cache_.clear();
  lru_.clear();
}

GLuint Renderer::create_alpha_texture(int w, int h, const uint8_t* alpha_pixels) {
  std::vector<uint8_t> rgba(w * h * 4, 255);
  if (alpha_pixels) {
    for (int i = 0; i < w * h; ++i) {
      rgba[i * 4 + 3] = alpha_pixels[i];
    }
  } else {
    std::fill(rgba.begin(), rgba.end(), 0);
  }
  GLuint tid = 0;
  glGenTextures(1, &tid);
  glBindTexture(GL_TEXTURE_2D, tid);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
  return tid;
}

void Renderer::update_alpha_texture(GLuint tex, int x, int y, int w, int h,
                                    const uint8_t* alpha_pixels) {
  std::vector<uint8_t> rgba(w * h * 4, 255);
  for (int i = 0; i < w * h; ++i) {
    rgba[i * 4 + 3] = alpha_pixels[i];
  }
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
}

void Renderer::draw_rect(float x, float y, float w, float h,
                         float r, float g, float b, float a) {
  if (a <= 0.001f) return;
  glDisable(GL_TEXTURE_2D);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glColor4f(r, g, b, a);
  glBegin(GL_QUADS);
  glVertex2f(x, y);
  glVertex2f(x + w, y);
  glVertex2f(x + w, y + h);
  glVertex2f(x, y + h);
  glEnd();
  glEnable(GL_TEXTURE_2D);
}

void Renderer::draw_texture(const Texture* tex, float x, float y, float w, float h,
                            float alpha, bool fliplr, bool flipud, BlendMode blend,
                            float tr, float tg, float tb) {
  if (!tex || tex->id == 0 || alpha <= 0.001f) return;
  float u0 = fliplr ? 1.0f : 0.0f;
  float u1 = fliplr ? 0.0f : 1.0f;
  float v0 = flipud ? 1.0f : 0.0f;
  float v1 = flipud ? 0.0f : 1.0f;

  switch (blend) {
    case BlendMode::Alpha:
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      break;
    case BlendMode::Additive:
      glBlendFunc(GL_SRC_ALPHA, GL_ONE);
      break;
    case BlendMode::Multiply:
      glBlendFunc(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA);
      break;
  }

  glBindTexture(GL_TEXTURE_2D, tex->id);
  glColor4f(tr, tg, tb, alpha);
  glBegin(GL_QUADS);
  glTexCoord2f(u0, v0); glVertex2f(x, y);
  glTexCoord2f(u1, v0); glVertex2f(x + w, y);
  glTexCoord2f(u1, v1); glVertex2f(x + w, y + h);
  glTexCoord2f(u0, v1); glVertex2f(x, y + h);
  glEnd();
}

void Renderer::draw_texture_uv(GLuint tex_id, float x, float y, float w, float h,
                               float u0, float v0, float u1, float v1,
                               float r, float g, float b, float a) {
  if (tex_id == 0 || a <= 0.001f) return;
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glBindTexture(GL_TEXTURE_2D, tex_id);
  glColor4f(r, g, b, a);
  glBegin(GL_QUADS);
  glTexCoord2f(u0, v0); glVertex2f(x, y);
  glTexCoord2f(u1, v0); glVertex2f(x + w, y);
  glTexCoord2f(u1, v1); glVertex2f(x + w, y + h);
  glTexCoord2f(u0, v1); glVertex2f(x, y + h);
  glEnd();
}

void Renderer::draw_rule_wipe(const Texture* next_tex, const Texture* rule_tex,
                              float x, float y, float w, float h,
                              float progress, float vague, bool fliplr, bool flipud) {
  if (!next_tex) return;
  if (!has_shaders_ || !rule_tex || rule_prog_ == 0) {
    draw_texture(next_tex, x, y, w, h, progress, fliplr, flipud);
    return;
  }

  // Orthogonal 960x544 projection matrix
  const float proj[16] = {
      2.0f / 960.0f, 0.0f, 0.0f, 0.0f,
      0.0f, -2.0f / 544.0f, 0.0f, 0.0f,
      0.0f, 0.0f, -1.0f, 0.0f,
      -1.0f, 1.0f, 0.0f, 1.0f,
  };

  float u0 = fliplr ? 1.0f : 0.0f;
  float u1 = fliplr ? 0.0f : 1.0f;
  float v0 = flipud ? 1.0f : 0.0f;
  float v1 = flipud ? 0.0f : 1.0f;

  const float verts[12] = {x, y, 0.0f, x + w, y, 0.0f, x + w, y + h, 0.0f, x, y + h, 0.0f};
  const float uvs[8] = {u0, v0, u1, v0, u1, v1, u0, v1};
  const float ruvs[8] = {0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};

  glUseProgram(rule_prog_);
  GLint u_proj = glGetUniformLocation(rule_prog_, "uProj");
  glUniformMatrix4fv(u_proj, 1, GL_FALSE, proj);
  glUniform1i(u_rule_tex_, 0);
  glUniform1i(u_rule_mask_, 1);
  glUniform1f(u_rule_prog_, progress);
  glUniform1f(u_rule_vague_, vague);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, next_tex->id);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, rule_tex->id);

  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, verts);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uvs);
  glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 0, ruvs);
  glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
  glDisableVertexAttribArray(0);
  glDisableVertexAttribArray(1);
  glDisableVertexAttribArray(2);

  glActiveTexture(GL_TEXTURE0);
  glUseProgram(0);
}

}  // namespace fsn
