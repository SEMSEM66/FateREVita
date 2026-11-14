#include "audio.hpp"
#include "config.hpp"
#include "extras.hpp"
#include "log.hpp"
#include "renderer.hpp"
#include "script_vm.hpp"
#include "text.hpp"
#include "title_screen.hpp"

#include <psp2/apputil.h>
#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <psp2/system_param.h>
#include <psp2/touch.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

bool dir_exists(const char* path) {
  SceIoStat st{};
  return sceIoGetstat(path, &st) >= 0;
}

bool file_exists(const char* path) {
  SceIoStat st{};
  return sceIoGetstat(path, &st) >= 0;
}

std::string find_data_root() {
  if (dir_exists("ux0:data/FateReVita/manifest.json")) {
    return "ux0:data/FateReVita";
  }
  if (dir_exists("app0:data/manifest.json")) {
    return "app0:data";
  }
  return "ux0:data/FateReVita";
}

uint64_t parse_octal(const char* p, size_t len) {
  uint64_t val = 0;
  for (size_t i = 0; i < len; ++i) {
    char c = p[i];
    if (c == '\0' || c == ' ') {
      if (val > 0) break;
      continue;
    }
    if (c >= '0' && c <= '7') {
      val = (val << 3) | static_cast<uint64_t>(c - '0');
    }
  }
  return val;
}

void ensure_dir_cached(const std::string& dir, std::unordered_set<std::string>& seen) {
  if (dir.empty() || seen.count(dir)) return;
  size_t colon = dir.find(':');
  size_t pos = (colon == std::string::npos) ? 0 : colon + 1;
  while (pos <= dir.size()) {
    size_t slash = dir.find('/', pos);
    std::string sub = (slash == std::string::npos) ? dir : dir.substr(0, slash);
    if (!sub.empty() && !seen.count(sub)) {
      sceIoMkdir(sub.c_str(), 0777);
      seen.insert(sub);
    }
    if (slash == std::string::npos) break;
    pos = slash + 1;
  }
}

void draw_install_bar(fsn::Renderer& renderer, float progress, int step_idx, int total_steps) {
  sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
  renderer.begin_frame();
  renderer.draw_rect(0.0f, 0.0f, 960.0f, 544.0f, 0.05f, 0.07f, 0.13f, 1.0f);
  // Subtle multi-bar progress indicator (step bar top + overall fill bottom)
  float step_w = 560.0f / static_cast<float>(std::max(1, total_steps));
  renderer.draw_rect(200.0f + step_idx * step_w, 232.0f, std::max(8.0f, step_w - 4.0f),
                     10.0f, 0.35f, 0.72f, 1.0f, 0.9f);
  renderer.draw_rect(180.0f, 256.0f, 600.0f, 32.0f, 0.14f, 0.18f, 0.28f, 1.0f);
  renderer.draw_rect(184.0f, 260.0f,
                     592.0f * std::max(0.0f, std::min(1.0f, progress)), 24.0f,
                     0.28f, 0.62f, 0.96f, 1.0f);
  renderer.end_frame();
}

bool parse_ustar_header(const uint8_t* hdr, std::string& rel, uint64_t& fsize,
                        char& typeflag, size_t& pad) {
  bool all_zero = true;
  for (size_t i = 0; i < 512; ++i) {
    if (hdr[i] != 0) {
      all_zero = false;
      break;
    }
  }
  if (all_zero) return false;

  std::string prefix(reinterpret_cast<const char*>(hdr + 345),
                     strnlen(reinterpret_cast<const char*>(hdr + 345), 155));
  std::string name(reinterpret_cast<const char*>(hdr),
                   strnlen(reinterpret_cast<const char*>(hdr), 100));
  rel = prefix.empty() ? name : (prefix + "/" + name);
  while (!rel.empty() && (rel[0] == '.' || rel[0] == '/')) {
    rel.erase(0, 1);
  }

  fsize = parse_octal(reinterpret_cast<const char*>(hdr + 124), 12);
  typeflag = static_cast<char>(hdr[156]);
  pad = static_cast<size_t>((512 - (fsize % 512)) % 512);
  return true;
}

void clear_dir_files(const std::string& dir) {
  SceUID dfd = sceIoDopen(dir.c_str());
  if (dfd < 0) return;
  std::vector<std::string> names;
  SceIoDirent ent{};
  while (sceIoDread(dfd, &ent) > 0) {
    if (ent.d_name[0] == '.' &&
        (ent.d_name[1] == '\0' || (ent.d_name[1] == '.' && ent.d_name[2] == '\0'))) {
      continue;
    }
    if (!SCE_S_ISDIR(ent.d_stat.st_mode)) {
      names.emplace_back(ent.d_name);
    }
  }
  sceIoDclose(dfd);
  for (size_t i = 0; i < names.size(); ++i) {
    if ((i & 63) == 0) sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
    std::string p = dir + "/" + names[i];
    sceIoRemove(p.c_str());
  }
}

void install_voice_tar_archive(fsn::Renderer& renderer, const char* src_tar,
                               const std::string& dst_tar, const std::string& dst_idx,
                               const std::string& stale_voice_dir,
                               int step_idx, int total_steps) {
  std::unordered_set<std::string> dirs;
  if (file_exists(src_tar)) {
    size_t sl = dst_tar.rfind('/');
    if (sl != std::string::npos) ensure_dir_cached(dst_tar.substr(0, sl), dirs);
    sceIoRemove(dst_tar.c_str());
    sceIoRemove(dst_idx.c_str());
    int rc = sceIoRename(src_tar, dst_tar.c_str());
    if (rc < 0) {
      fsn::log::info("[Install] Failed to move %s -> %s: 0x%08X", src_tar, dst_tar.c_str(), rc);
      return;
    }
    fsn::log::info("[Install] Moved %s -> %s", src_tar, dst_tar.c_str());
    clear_dir_files(stale_voice_dir);
  }

  if (!file_exists(dst_tar.c_str()) || file_exists(dst_idx.c_str())) return;

  SceUID fd = sceIoOpen(dst_tar.c_str(), SCE_O_RDONLY, 0);
  if (fd < 0) return;

  SceOff total_bytes = sceIoLseek(fd, 0, SCE_SEEK_END);
  sceIoLseek(fd, 0, SCE_SEEK_SET);
  if (total_bytes <= 512) {
    sceIoClose(fd);
    return;
  }

  fsn::log::info("[Install] Indexing %s (%llu bytes)...",
                 dst_tar.c_str(), static_cast<unsigned long long>(total_bytes));

  std::string idx_text;
  idx_text.reserve(2 * 1024 * 1024);
  SceOff pos = 0;
  int files_done = 0;
  int next_ui_files = 0;
  uint8_t hdr[512];

  while (sceIoRead(fd, hdr, 512) == 512) {
    pos += 512;
    std::string rel;
    uint64_t fsize = 0;
    char typeflag = '\0';
    size_t pad = 0;
    if (!parse_ustar_header(hdr, rel, fsize, typeflag, pad)) break;

    if ((typeflag == '0' || typeflag == '\0') && !rel.empty() &&
        rel.find("..") == std::string::npos) {
      idx_text += rel;
      idx_text += '\t';
      idx_text += std::to_string(static_cast<unsigned long long>(pos));
      idx_text += '\t';
      idx_text += std::to_string(static_cast<unsigned long long>(fsize));
      idx_text += '\n';
      ++files_done;
    }

    SceOff skip = static_cast<SceOff>(fsize + pad);
    if (skip > 0) {
      sceIoLseek(fd, skip, SCE_SEEK_CUR);
      pos += skip;
    }

    if (files_done >= next_ui_files) {
      float prog = static_cast<float>(pos) / static_cast<float>(total_bytes);
      draw_install_bar(renderer, prog, step_idx, total_steps);
      next_ui_files = files_done + 256;
    }
  }
  sceIoClose(fd);

  std::string idx_tmp = dst_idx + ".tmp";
  SceUID out_fd = sceIoOpen(idx_tmp.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (out_fd >= 0) {
    size_t written = 0;
    while (written < idx_text.size()) {
      size_t chunk = std::min<size_t>(idx_text.size() - written, 256 * 1024);
      int w = sceIoWrite(out_fd, idx_text.data() + written, static_cast<SceSize>(chunk));
      if (w <= 0) break;
      written += static_cast<size_t>(w);
    }
    sceIoClose(out_fd);
    sceIoRemove(dst_idx.c_str());
    sceIoRename(idx_tmp.c_str(), dst_idx.c_str());
  }

  draw_install_bar(renderer, 1.0f, step_idx, total_steps);
  fsn::log::info("[Install] Indexed %s (%d files -> %s)",
                 dst_tar.c_str(), files_done, dst_idx.c_str());
}

void unpack_tar_if_exists(fsn::Renderer& renderer, const char* tar_path,
                          const std::string& dst_root, int step_idx, int total_steps) {
  if (!file_exists(tar_path)) return;
  SceUID fd = sceIoOpen(tar_path, SCE_O_RDONLY, 0);
  if (fd < 0) return;

  SceOff total_bytes = sceIoLseek(fd, 0, SCE_SEEK_END);
  sceIoLseek(fd, 0, SCE_SEEK_SET);
  if (total_bytes <= 512) {
    sceIoClose(fd);
    return;
  }

  fsn::log::info("[Install] Unpacking %s (%llu bytes)...",
                 tar_path, static_cast<unsigned long long>(total_bytes));

  std::unordered_set<std::string> created_dirs;
  ensure_dir_cached(dst_root, created_dirs);

  static constexpr size_t kBufSize = 256 * 1024;
  std::vector<uint8_t> io_buf(kBufSize);
  SceOff bytes_read = 0;
  SceOff next_ui_bytes = 0;
  int files_done = 0;

  uint8_t hdr[512];
  while (true) {
    int n = sceIoRead(fd, hdr, 512);
    if (n < 512) break;
    bytes_read += 512;

    std::string rel;
    uint64_t fsize = 0;
    char typeflag = '\0';
    size_t pad = 0;
    if (!parse_ustar_header(hdr, rel, fsize, typeflag, pad)) break;

    if (typeflag == '5' || (!rel.empty() && rel.back() == '/')) {
      if (!rel.empty() && rel.back() == '/') rel.pop_back();
      if (!rel.empty()) ensure_dir_cached(dst_root + "/" + rel, created_dirs);
    } else if ((typeflag == '0' || typeflag == '\0') && !rel.empty() &&
               rel.find("..") == std::string::npos) {
      std::string full_dst = dst_root + "/" + rel;
      size_t sl = full_dst.rfind('/');
      if (sl != std::string::npos) {
        ensure_dir_cached(full_dst.substr(0, sl), created_dirs);
      }
      SceUID out_fd = sceIoOpen(full_dst.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
      uint64_t rem = fsize;
      while (rem > 0) {
        size_t chunk = static_cast<size_t>(std::min<uint64_t>(rem, kBufSize));
        int got = sceIoRead(fd, io_buf.data(), chunk);
        if (got <= 0) break;
        if (out_fd >= 0) sceIoWrite(out_fd, io_buf.data(), got);
        rem -= static_cast<uint64_t>(got);
        bytes_read += got;
      }
      if (out_fd >= 0) sceIoClose(out_fd);
      if (pad > 0) {
        sceIoLseek(fd, static_cast<SceOff>(pad), SCE_SEEK_CUR);
        bytes_read += static_cast<SceOff>(pad);
      }
      ++files_done;
    } else {
      SceOff skip = static_cast<SceOff>(fsize + pad);
      if (skip > 0) {
        sceIoLseek(fd, skip, SCE_SEEK_CUR);
        bytes_read += skip;
      }
    }

    if (bytes_read >= next_ui_bytes) {
      float prog = static_cast<float>(bytes_read) / static_cast<float>(total_bytes);
      draw_install_bar(renderer, prog, step_idx, total_steps);
      next_ui_bytes = bytes_read + (2 * 1024 * 1024);
    }
  }

  sceIoClose(fd);
  sceIoRemove(tar_path);
  draw_install_bar(renderer, 1.0f, step_idx, total_steps);
  fsn::log::info("[Install] Finished %s (%d files extracted)", tar_path, files_done);
}

void draw_pause_menu(fsn::Renderer& renderer, fsn::TextRenderer& text,
                     const fsn::AppConfig& config, const fsn::ScriptVM& vm,
                     int row, const std::string& feedback, bool circle_confirm) {
  renderer.draw_rect(0.0f, 0.0f, 960.0f, 544.0f, 0.02f, 0.03f, 0.08f, 0.78f);
  renderer.draw_rect(140.0f, 70.0f, 680.0f, 404.0f, 0.06f, 0.09f, 0.17f, 0.94f);
  renderer.draw_rect(140.0f, 70.0f, 680.0f, 3.0f, 0.35f, 0.62f, 0.95f, 0.9f);

  text.draw_simple("PAUSE MENU", 170.0f, 88.0f, 0.98f, 0.85f, 0.92f, 1.0f, 1.0f);
  text.draw_simple("Scene: " + vm.status_summary(), 170.0f, 120.0f,
                   0.72f, 0.70f, 0.78f, 0.90f, 0.90f);

  const std::string items[7] = {
      "Resume Game",
      "Open Story Flowchart",
      std::string("Soundtrack:       < ") + config.ost_label() + " >",
      std::string("Content Edition:  < ") + config.content_label() + " >",
      "Quick Save Now",
      "Open Backlog",
      "Save & Return to Title",
  };

  for (int i = 0; i < 7; ++i) {
    float ry = 152.0f + i * 39.0f;
    bool sel = (row == i);
    renderer.draw_rect(164.0f, ry, 632.0f, 33.0f,
                       sel ? 0.22f : 0.10f,
                       sel ? 0.45f : 0.14f,
                       sel ? 0.85f : 0.24f,
                       sel ? 0.85f : 0.45f);
    text.draw_simple(items[i], 184.0f, ry + 6.0f, 0.83f,
                     1.0f, sel ? 0.98f : 0.88f, sel ? 0.75f : 0.95f, 1.0f);
  }

  if (!feedback.empty()) {
    text.draw_simple(feedback, 170.0f, 434.0f, 0.80f, 0.55f, 1.0f, 0.65f, 1.0f);
  } else {
    text.draw_simple(circle_confirm
                         ? "D-Pad: Navigate/Switch | Circle: Select | Cross/Start: Resume"
                         : "D-Pad: Navigate/Switch | Cross: Select | Circle/Start: Resume",
                     170.0f, 434.0f, 0.72f, 0.78f, 0.84f, 0.94f, 0.85f);
  }
}

}  // namespace

int main(int /*argc*/, char* /*argv*/[]) {
  scePowerSetArmClockFrequency(444);
  scePowerSetBusClockFrequency(222);
  scePowerSetGpuClockFrequency(222);
  scePowerSetGpuXbarClockFrequency(166);

  SceAppUtilInitParam init_param{};
  SceAppUtilBootParam boot_param{};
  sceAppUtilInit(&init_param, &boot_param);
  int enter_btn = SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
  sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enter_btn);
  const bool circle_confirm = (enter_btn == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE);

  fsn::log::init("ux0:data/FateReVita/log.txt");
  fsn::log::info("=== FateReVita starting (enter=%s) ===", circle_confirm ? "CIRCLE" : "CROSS");

  sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
  sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);

  fsn::Renderer renderer;
  if (!renderer.init()) {
    fsn::log::close();
    sceKernelExitProcess(1);
    return 1;
  }

  unpack_tar_if_exists(renderer, "ux0:data/FateReVita_base.tar", "ux0:data/FateReVita", 0, 2);
  install_voice_tar_archive(renderer, "ux0:data/FateReVita_voice.tar",
                            "ux0:data/FateReVita/audio/voice.tar",
                            "ux0:data/FateReVita/audio/voice.idx",
                            "ux0:data/FateReVita/audio/voice", 1, 2);

  const std::string data_root = find_data_root();
  fsn::log::info("[Main] Using data root: %s", data_root.c_str());

  const std::string ux0_dir = "ux0:data/FateReVita";
  const std::string config_path = ux0_dir + "/config.json";
  const std::string save_path = ux0_dir + "/save0.json";
  const std::string global_flags_path = ux0_dir + "/global_flags.json";
  const std::string choice_backup_path = ux0_dir + "/choice_autosave.json";
  const std::string extras_path = ux0_dir + "/extras.json";

  fsn::AppConfig config;
  config.load(config_path);

  fsn::AudioEngine audio;
  audio.init();
  audio.init_voice_archive(data_root, data_root + "/audio/voice.tar",
                           data_root + "/audio/voice.idx");
  audio.set_master_volumes(config.bgm_vol, config.se_vol, config.voice_vol);

  fsn::TextRenderer text;
  if (!text.init(&renderer, data_root + "/fonts/HelveticaNeueWorld-55Roman.otf", 22)) {
    fsn::log::info("[Main] Font load failed; check data root");
  }

  fsn::ExtrasManager extras;
  fsn::ScriptVM vm;
  bool vm_ok = vm.init(data_root, &renderer, &audio, &text, &config, global_flags_path, &extras);
  if (vm_ok) {
    extras.init(data_root, extras_path, vm.manifest());
  }

  fsn::TitleScreen title;
  title.init(data_root, &renderer, &audio, &text, &config, config_path,
             vm.get_title_bgm_info(), file_exists(save_path.c_str()));

  fsn::MusicRoomScreen music_room;
  music_room.init(data_root, &renderer, &audio, &text, &config, config_path,
                  &extras, &vm, circle_confirm);

  fsn::CgGalleryScreen cg_gallery;
  cg_gallery.init(data_root, &renderer, &audio, &text, &config, config_path,
                  &extras, &vm, &title, circle_confirm);

  fsn::FlowchartScreen flowchart;
  flowchart.init(data_root, &renderer, &audio, &text, &config, config_path,
                 &extras, &vm, circle_confirm);

  enum class AppMode { BootNotice, Title, Flowchart, MusicRoom, CgGallery, Game };
  AppMode mode = AppMode::BootNotice;
  float boot_timer = 0.0f;

  bool pause_open = false;
  int pause_row = 0;
  std::string pause_feedback;

  SceCtrlData prev_pad{};
  bool prev_touch = false;
  uint64_t prev_tick = sceKernelGetProcessTimeWide();
  bool running = true;

  while (running) {
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
    uint64_t now = sceKernelGetProcessTimeWide();
    float dt = static_cast<float>(now - prev_tick) / 1000000.0f;
    prev_tick = now;
    if (dt > 0.05f) dt = 0.05f;
    if (dt < 0.0f) dt = 0.0f;

    SceCtrlData pad{};
    sceCtrlPeekBufferPositive(0, &pad, 1);
    uint32_t pressed = pad.buttons & ~prev_pad.buttons;
    prev_pad = pad;

    SceTouchData touch{};
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
    bool touching = touch.reportNum > 0;
    bool touch_tap = touching && !prev_touch;
    float touch_x = touching ? (touch.report[0].x / 2.0f) : 0.0f;
    float touch_y = touching ? (touch.report[0].y / 2.0f) : 0.0f;
    prev_touch = touching;

    bool btn_up = (pressed & SCE_CTRL_UP) != 0;
    bool btn_down = (pressed & SCE_CTRL_DOWN) != 0;
    bool btn_left = (pressed & SCE_CTRL_LEFT) != 0;
    bool btn_right = (pressed & SCE_CTRL_RIGHT) != 0;
    bool btn_cross = (pressed & SCE_CTRL_CROSS) != 0;
    bool btn_circle = (pressed & SCE_CTRL_CIRCLE) != 0;
    bool btn_triangle = (pressed & SCE_CTRL_TRIANGLE) != 0;

    bool btn_sys_confirm = circle_confirm ? btn_circle : btn_cross;
    bool btn_sys_back = circle_confirm ? btn_cross : btn_circle;
    bool btn_any_confirm = btn_cross || btn_circle;
    bool btn_advance = btn_any_confirm || touch_tap;
    bool btn_skip_held = (pad.buttons & SCE_CTRL_RTRIGGER) != 0;
    bool btn_toggle_text = (pressed & SCE_CTRL_SQUARE) != 0;
    bool btn_l = (pressed & SCE_CTRL_LTRIGGER) != 0;
    bool btn_r = (pressed & SCE_CTRL_RTRIGGER) != 0;
    bool btn_start = (pressed & SCE_CTRL_START) != 0;
    bool btn_select = (pressed & SCE_CTRL_SELECT) != 0;

    renderer.begin_frame();

    if (mode == AppMode::BootNotice) {
      boot_timer += dt;
      const fsn::Texture* boot_tex =
          renderer.get_texture(data_root + "/images/ui/boot_attention_us.png");
      if (boot_tex) {
        float bx = (960.0f - boot_tex->w) * 0.5f;
        float by = (544.0f - boot_tex->h) * 0.5f;
        renderer.draw_texture(boot_tex, bx, by, static_cast<float>(boot_tex->w),
                              static_cast<float>(boot_tex->h), 1.0f);
      }
      if (boot_timer > 2.5f || btn_advance) {
        mode = AppMode::Title;
        title.enter(file_exists(save_path.c_str()));
      }
    } else if (mode == AppMode::Title) {
      if (!vm_ok) {
        text.draw_simple("Missing data in ux0:data/FateReVita/ (see log.txt)",
                         60.0f, 250.0f, 1.0f, 1.0f, 0.4f, 0.4f, 1.0f);
        if (btn_any_confirm) running = false;
      } else {
        bool confirm_in = title.is_main() ? btn_any_confirm : btn_sys_confirm;
        bool back_in = title.is_main() ? false : (btn_sys_back || btn_triangle);
        fsn::TitleAction act = title.update(
            dt, btn_up, btn_down, btn_left, btn_right, confirm_in, back_in,
            touch_tap, touch_x, touch_y);
        title.draw();
        if (act == fsn::TitleAction::NewGame) {
          audio.stop_bgm(500);
          pause_open = false;
          if (vm.start_preset(0, choice_backup_path)) {
            mode = AppMode::Game;
          } else {
            fsn::log::info("[Main] Failed to start New Game");
            title.enter(file_exists(save_path.c_str()));
          }
        } else if (act == fsn::TitleAction::Continue) {
          audio.stop_bgm(400);
          pause_open = false;
          if (vm.load_quicksave(save_path)) {
            mode = AppMode::Game;
          }
        } else if (act == fsn::TitleAction::Flowchart) {
          flowchart.enter(false, "", -1);
          mode = AppMode::Flowchart;
        } else if (act == fsn::TitleAction::MusicRoom) {
          music_room.enter();
          mode = AppMode::MusicRoom;
        } else if (act == fsn::TitleAction::CgGallery) {
          cg_gallery.enter();
          mode = AppMode::CgGallery;
        } else if (act == fsn::TitleAction::Exit) {
          running = false;
        }
      }
    } else if (mode == AppMode::Flowchart) {
      bool jumped = false;
      bool stay = flowchart.update(
          dt, btn_up, btn_down, btn_left, btn_right,
          btn_sys_confirm, btn_sys_back, btn_triangle, btn_toggle_text,
          btn_l, btn_r, btn_select,
          touch_tap, touch_x, touch_y, jumped);
      if (flowchart.opened_from_game()) {
        vm.draw();
      }
      flowchart.draw();
      if (!stay) {
        if (jumped) {
          pause_open = false;
          mode = AppMode::Game;
        } else if (flowchart.opened_from_game()) {
          pause_open = true;
          mode = AppMode::Game;
        } else {
          mode = AppMode::Title;
        }
      }
    } else if (mode == AppMode::MusicRoom) {
      bool stay = music_room.update(
          dt, btn_up, btn_down, btn_left, btn_right,
          btn_sys_confirm, btn_sys_back, btn_triangle, btn_toggle_text,
          btn_l, btn_r, btn_start, btn_select,
          touch_tap, touch_x, touch_y);
      music_room.draw();
      if (!stay) {
        mode = AppMode::Title;
        title.enter(file_exists(save_path.c_str()));
      }
    } else if (mode == AppMode::CgGallery) {
      bool stay = cg_gallery.update(
          dt, btn_up, btn_down, btn_left, btn_right,
          btn_sys_confirm, btn_sys_back, btn_triangle, btn_toggle_text,
          btn_l, btn_r, btn_select,
          touch_tap, touch_x, touch_y);
      cg_gallery.draw();
      if (!stay) {
        mode = AppMode::Title;
        title.enter(file_exists(save_path.c_str()));
      }
    } else if (mode == AppMode::Game) {
      if (pause_open) {
        if (btn_up) {
          pause_row = (pause_row + 6) % 7;
          pause_feedback.clear();
          audio.play_se("ui_move", data_root + "/audio/se/se_title_move.ogg");
        }
        if (btn_down) {
          pause_row = (pause_row + 1) % 7;
          pause_feedback.clear();
          audio.play_se("ui_move", data_root + "/audio/se/se_title_move.ogg");
        }
        bool adj = (btn_left || btn_right || btn_sys_confirm) && (pause_row == 2 || pause_row == 3);
        if (adj) {
          int dir = btn_left ? -1 : 1;
          if (pause_row == 2) {
            config.ost_mode = (config.ost_mode == fsn::OstMode::New)
                                  ? fsn::OstMode::Old
                                  : fsn::OstMode::New;
          } else {
            int cm = (static_cast<int>(config.content_mode) + dir + 3) % 3;
            config.content_mode = static_cast<fsn::ContentMode>(cm);
          }
          config.save(config_path);
          vm.refresh_after_config_change();
          pause_feedback = "Applied & saved to config.json";
          audio.play_se("ui_move", data_root + "/audio/se/se_title_move.ogg");
        } else if (btn_sys_confirm) {
          if (pause_row == 0) {
            pause_open = false;
          } else if (pause_row == 1) {
            std::string cur_fcf = vm.in_flowchart() ? vm.fcf_stem() : vm.status_summary();
            flowchart.enter(true, cur_fcf, vm.fcf_node_id());
            pause_open = false;
            mode = AppMode::Flowchart;
          } else if (pause_row == 4) {
            vm.write_quicksave(save_path);
            pause_feedback = "Quick-saved to save0.json!";
            audio.play_se("ui_decide", data_root + "/audio/se/se_title_decide.ogg");
          } else if (pause_row == 5) {
            pause_open = false;
            vm.set_backlog_open(true);
          } else if (pause_row == 6) {
            vm.write_quicksave(save_path);
            pause_open = false;
            audio.stop_all_se(200);
            audio.stop_voice();
            mode = AppMode::Title;
            title.enter(true);
          }
        } else if (btn_sys_back || btn_triangle || btn_start) {
          pause_open = false;
        }
      } else if (vm.is_backlog_open()) {
        if (btn_triangle || btn_sys_back) vm.set_backlog_open(false);
        if (btn_up) vm.scroll_backlog(1);
        if (btn_down) vm.scroll_backlog(-1);
      } else {
        if (btn_start) {
          pause_open = true;
          pause_row = 0;
          pause_feedback.clear();
        } else if (btn_triangle && !vm.is_choice_active()) {
          vm.set_backlog_open(true);
        } else {
          vm.update(dt, btn_up, btn_down, btn_advance, btn_skip_held,
                    btn_toggle_text, touch_tap, touch_x, touch_y);
          if (vm.is_finished()) {
            audio.stop_all_se(300);
            audio.stop_bgm(500);
            mode = AppMode::Title;
            title.enter(file_exists(save_path.c_str()));
          }
        }
      }

      vm.draw();
      if (pause_open) {
        draw_pause_menu(renderer, text, config, vm, pause_row, pause_feedback, circle_confirm);
      }
    }

    renderer.end_frame();
  }

  audio.shutdown();
  text.shutdown();
  renderer.shutdown();
  sceAppUtilShutdown();
  fsn::log::close();
  sceKernelExitProcess(0);
  return 0;
}
