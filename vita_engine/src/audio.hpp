#pragma once

#include <cstdint>
#include <string>

namespace fsn {

class AudioEngine {
 public:
  bool init();
  void shutdown();
  bool init_voice_archive(const std::string& data_root, const std::string& tar_path,
                          const std::string& idx_path);

  void play_bgm(const std::string& path, int fade_ms = 0, bool loop = true,
                int64_t loop_start = 0, int64_t loop_len = 0);
  void stop_bgm(int fade_ms = 0);
  void fade_bgm(int target_pct, int time_ms);

  void play_voice(const std::string& path);
  void stop_voice();
  bool is_voice_playing() const;

  void play_se(const std::string& name, const std::string& path, bool loop = false,
               int fade_in_ms = 0, float volume = 1.0f);
  void stop_se(const std::string& name, int fade_out_ms = 0);
  void stop_all_se(int fade_out_ms = 0);

  void set_master_volumes(float bgm, float se, float voice);
};

}  // namespace fsn
