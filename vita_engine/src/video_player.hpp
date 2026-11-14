#pragma once

#include "audio.hpp"
#include "renderer.hpp"

#include <json.hpp>
#include <string>

namespace fsn {

/**
 * Hardware-accelerated H.264 + Vorbis video player for KAG @playmovie commands.
 * Degrades cleanly (returns false and skips) if files are absent or decoder init fails.
 */
class VideoPlayer {
 public:
  static bool play(const std::string& storage, bool canskip,
                   const std::string& data_root, const nlohmann::json& manifest,
                   Renderer* renderer, AudioEngine* audio);
};

}  // namespace fsn
