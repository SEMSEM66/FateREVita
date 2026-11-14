#include "video_player.hpp"
#include "log.hpp"

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <psp2/touch.h>
#include <psp2/videodec.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace fsn {

namespace {

constexpr uint32_t kVidxMagic = 0x58444956u;  // "VIDX" little-endian
constexpr size_t kMaxSafeFrameBytes = 4 * 1024 * 1024;

struct VidxHeader {
  uint32_t magic = 0;
  uint32_t version = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t fps_num = 0;
  uint32_t fps_den = 0;
  uint32_t num_frames = 0;
};

struct VidxFrame {
  uint32_t offset = 0;
  uint32_t size = 0;
};

struct KernelMemBlock {
  SceUID uid = -1;
  void* ptr = nullptr;

  ~KernelMemBlock() { release(); }

  bool alloc(const char* name, uint32_t raw_size) {
    release();
    auto align_up = [](uint32_t v, uint32_t a) { return (v + a - 1u) & ~(a - 1u); };

    // Prefer CDRAM (256KB aligned) from our 16MB reserved threshold, then PHYCONT (1MB aligned).
    const struct {
      SceKernelMemBlockType type;
      uint32_t align;
    } attempts[2] = {
        {SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 0x40000u},
        {SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, 0x100000u},
    };

    for (const auto& a : attempts) {
      uint32_t sz = align_up(std::max(raw_size, a.align), a.align);
      SceUID id = sceKernelAllocMemBlock(name, a.type, sz, nullptr);
      if (id >= 0) {
        void* base = nullptr;
        if (sceKernelGetMemBlockBase(id, &base) >= 0 && base != nullptr) {
          uid = id;
          ptr = base;
          return true;
        }
        sceKernelFreeMemBlock(id);
      }
    }
    return false;
  }

  void release() {
    if (uid >= 0) {
      sceKernelFreeMemBlock(uid);
      uid = -1;
    }
    ptr = nullptr;
  }
};

std::string normalize_movie_stem(const std::string& raw) {
  std::string s = raw;
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  auto slash = s.rfind('/');
  if (slash != std::string::npos) s = s.substr(slash + 1);
  auto dot = s.rfind('.');
  if (dot != std::string::npos) s = s.substr(0, dot);
  return s;
}

bool read_vidx(const std::string& path, VidxHeader& hdr, std::vector<VidxFrame>& frames) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  if (std::fread(&hdr, sizeof(hdr), 1, f) != 1) {
    std::fclose(f);
    return false;
  }
  if (hdr.magic != kVidxMagic || hdr.version != 1 || hdr.width == 0 || hdr.height == 0 ||
      hdr.width > 1280 || hdr.height > 720 || hdr.fps_num == 0 || hdr.fps_den == 0 ||
      hdr.num_frames == 0 || hdr.num_frames > 200000) {
    std::fclose(f);
    return false;
  }
  frames.resize(hdr.num_frames);
  size_t got = std::fread(frames.data(), sizeof(VidxFrame), hdr.num_frames, f);
  std::fclose(f);
  return got == hdr.num_frames;
}

bool ensure_avcdec_loaded() {
  if (sceSysmoduleIsLoaded(SCE_SYSMODULE_AVCDEC) == 0) return true;
  int rc = sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC);
  if (rc < 0) {
    log::info("[Video] sceSysmoduleLoadModule(AVCDEC) failed: 0x%08X", rc);
    return false;
  }
  return true;
}

bool check_skip_pressed(SceCtrlData& prev_pad, bool& prev_touch) {
  SceCtrlData pad{};
  sceCtrlPeekBufferPositive(0, &pad, 1);
  uint32_t edge = pad.buttons & ~prev_pad.buttons;
  prev_pad = pad;

  SceTouchData touch{};
  sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
  bool touching = touch.reportNum > 0;
  bool tap = touching && !prev_touch;
  prev_touch = touching;

  constexpr uint32_t kSkipButtons = SCE_CTRL_CROSS | SCE_CTRL_CIRCLE | SCE_CTRL_START;
  return ((edge & kSkipButtons) != 0) || tap;
}

}  // namespace

bool VideoPlayer::play(const std::string& storage, bool canskip,
                       const std::string& data_root, const nlohmann::json& manifest,
                       Renderer* renderer, AudioEngine* audio) {
  std::string stem = normalize_movie_stem(storage);
  if (stem.empty() || !renderer || !audio) return false;

  std::string rel_h264 = "video/" + stem + ".h264";
  std::string rel_vidx = "video/" + stem + ".vidx";
  std::string rel_ogg = "video/" + stem + ".ogg";

  if (manifest.contains("videos") && manifest["videos"].contains(stem)) {
    const auto& vm = manifest["videos"][stem];
    rel_h264 = vm.value("h264", rel_h264);
    rel_vidx = vm.value("vidx", rel_vidx);
    rel_ogg = vm.value("audio", vm.value("ogg", rel_ogg));
  }

  const std::string path_h264 = data_root + "/" + rel_h264;
  const std::string path_vidx = data_root + "/" + rel_vidx;
  const std::string path_ogg = data_root + "/" + rel_ogg;

  VidxHeader hdr{};
  std::vector<VidxFrame> frames;
  if (!read_vidx(path_vidx, hdr, frames)) {
    log::info("[Video] Movie index missing/invalid (%s); skipping '%s'",
              path_vidx.c_str(), stem.c_str());
    return false;
  }

  FILE* fh264 = std::fopen(path_h264.c_str(), "rb");
  if (!fh264) {
    log::info("[Video] Movie H.264 stream missing (%s); skipping '%s'",
              path_h264.c_str(), stem.c_str());
    return false;
  }

  if (!ensure_avcdec_loaded()) {
    std::fclose(fh264);
    return false;
  }

  // Free cached scene textures before allocating hardware decoder buffers.
  renderer->clear_cache();

  const uint32_t aligned_w = (hdr.width + 15u) & ~15u;
  const uint32_t aligned_h = (hdr.height + 15u) & ~15u;

  SceVideodecQueryInitInfoHwAvcdec init_info{};
  init_info.size = sizeof(SceVideodecQueryInitInfoHwAvcdec);
  init_info.horizontal = aligned_w;
  init_info.vertical = aligned_h;
  init_info.numOfRefFrames = 2;
  init_info.numOfStreams = 1;

  int rc = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, &init_info);
  if (rc < 0) {
    log::info("[Video] sceVideodecInitLibrary failed: 0x%08X", rc);
    std::fclose(fh264);
    return false;
  }

  bool played_ok = false;
  do {
    SceAvcdecQueryDecoderInfo dec_query{};
    dec_query.horizontal = aligned_w;
    dec_query.vertical = aligned_h;
    dec_query.numOfRefFrames = 2;

    SceAvcdecDecoderInfo dec_info{};
    rc = sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, &dec_query, &dec_info);
    if (rc < 0 || dec_info.frameMemSize == 0) {
      log::info("[Video] sceAvcdecQueryDecoderMemSize failed: 0x%08X", rc);
      break;
    }

    KernelMemBlock dec_mem;
    if (!dec_mem.alloc("fsn_avc_dec", dec_info.frameMemSize)) {
      log::info("[Video] Failed to allocate %u bytes for AVC decoder", dec_info.frameMemSize);
      break;
    }

    const uint32_t rgba_bytes = hdr.width * hdr.height * 4u;
    KernelMemBlock rgba_mem;
    if (!rgba_mem.alloc("fsn_avc_rgba", rgba_bytes)) {
      log::info("[Video] Failed to allocate %u bytes for RGBA frame buffer", rgba_bytes);
      break;
    }
    std::memset(rgba_mem.ptr, 0, rgba_bytes);

    SceAvcdecCtrl decoder{};
    decoder.frameBuf.pBuf = dec_mem.ptr;
    decoder.frameBuf.size = dec_info.frameMemSize;
    rc = sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, &decoder, &dec_query);
    if (rc < 0) {
      log::info("[Video] sceAvcdecCreateDecoder failed: 0x%08X", rc);
      break;
    }

    GLuint tex_id = 0;
    glGenTextures(1, &tex_id);
    glBindTexture(GL_TEXTURE_2D, tex_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(hdr.width),
                 static_cast<GLsizei>(hdr.height), 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba_mem.ptr);
    Texture frame_tex{tex_id, static_cast<int>(hdr.width), static_cast<int>(hdr.height)};

    // Start companion Vorbis track after decoder allocation succeeds so audio stays synced.
    audio->play_bgm(path_ogg, 0, false, 0, 0);
    const uint64_t start_us = sceKernelGetProcessTimeWide();
    const double frame_dur = static_cast<double>(hdr.fps_den) / static_cast<double>(hdr.fps_num);

    SceCtrlData prev_pad{};
    sceCtrlPeekBufferPositive(0, &prev_pad, 1);
    SceTouchData prev_t{};
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &prev_t, 1);
    bool prev_touch = prev_t.reportNum > 0;

    // 64-byte aligned ES scratch buffer via over-allocated vector
    std::vector<uint8_t> es_storage(128 * 1024 + 64, 0);
    bool have_frame = false;

    for (uint32_t fi = 0; fi < hdr.num_frames; ++fi) {
      sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
      if (canskip && check_skip_pressed(prev_pad, prev_touch)) {
        log::info("[Video] Playback of '%s' skipped at frame %u/%u", stem.c_str(), fi, hdr.num_frames);
        break;
      }

      const VidxFrame& fr = frames[fi];
      if (fr.size == 0 || fr.size > kMaxSafeFrameBytes) continue;
      if (es_storage.size() < fr.size + 64u) {
        es_storage.resize(fr.size + 64u);
      }
      uintptr_t raw_addr = reinterpret_cast<uintptr_t>(es_storage.data());
      uint8_t* aligned_es = reinterpret_cast<uint8_t*>((raw_addr + 63u) & ~uintptr_t{63u});

      if (std::fseek(fh264, static_cast<long>(fr.offset), SEEK_SET) != 0 ||
          std::fread(aligned_es, 1, fr.size, fh264) != fr.size) {
        log::info("[Video] Read error in '%s' at frame %u", stem.c_str(), fi);
        break;
      }

      SceAvcdecAu au{};
      au.pts.upper = 0xFFFFFFFFu;
      au.pts.lower = 0xFFFFFFFFu;
      au.dts.upper = 0xFFFFFFFFu;
      au.dts.lower = 0xFFFFFFFFu;
      au.es.pBuf = aligned_es;
      au.es.size = fr.size;

      SceAvcdecPicture pic{};
      pic.size = sizeof(SceAvcdecPicture);
      pic.frame.pixelType = SCE_AVCDEC_PIXELFORMAT_RGBA8888;
      pic.frame.framePitch = hdr.width;
      pic.frame.frameWidth = hdr.width;
      pic.frame.frameHeight = hdr.height;
      pic.frame.horizontalSize = hdr.width;
      pic.frame.verticalSize = hdr.height;
      pic.frame.opt.rgba.alpha = 0xFF;
      pic.frame.pPicture[0] = rgba_mem.ptr;

      SceAvcdecPicture* pPic = &pic;
      SceAvcdecArrayPicture arr_pic{};
      arr_pic.numOfOutput = 0;
      arr_pic.numOfElm = 1;
      arr_pic.pPicture = &pPic;

      int dec_rc = sceAvcdecDecode(&decoder, &au, &arr_pic);
      if (dec_rc < 0) {
        log::info("[Video] sceAvcdecDecode error at frame %u: 0x%08X", fi, dec_rc);
        break;
      }
      if (arr_pic.numOfOutput > 0) {
        have_frame = true;
      }

      auto clock_sec = [&]() -> double {
        if (audio->is_bgm_playing()) return audio->get_bgm_time_sec();
        return static_cast<double>(sceKernelGetProcessTimeWide() - start_us) / 1000000.0;
      };

      const double target_sec = (static_cast<double>(fi) + 1.0) * frame_dur;
      double now_sec = clock_sec();

      // Drop upload/present when far behind audio clock so A/V stays locked.
      if (have_frame && (now_sec - target_sec) <= frame_dur * 2.0) {
        glBindTexture(GL_TEXTURE_2D, tex_id);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(hdr.width),
                        static_cast<GLsizei>(hdr.height), GL_RGBA, GL_UNSIGNED_BYTE,
                        rgba_mem.ptr);

        renderer->begin_frame();
        renderer->draw_texture(&frame_tex, 0.0f, 0.0f, 960.0f, 544.0f, 1.0f);
        renderer->end_frame();
      }

      while (true) {
        now_sec = clock_sec();
        double rem = target_sec - now_sec;
        if (rem <= 0.002) break;
        if (canskip && check_skip_pressed(prev_pad, prev_touch)) {
          fi = hdr.num_frames;  // break outer loop too
          break;
        }
        uint32_t sleep_us = static_cast<uint32_t>(std::min(rem, 0.008) * 1000000.0);
        if (sleep_us > 500u) sceKernelDelayThread(sleep_us);
      }
    }

    audio->stop_bgm(0);
    if (tex_id != 0) glDeleteTextures(1, &tex_id);
    sceAvcdecDeleteDecoder(&decoder);
    played_ok = true;
  } while (false);

  sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
  std::fclose(fh264);
  return played_ok;
}

}  // namespace fsn
