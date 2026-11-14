#include "audio.hpp"
#include "log.hpp"

#include <psp2/audioout.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>
#include <vorbis/vorbisfile.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace fsn {

namespace {

constexpr int kSampleRate = 48000;
constexpr int kFrameSamples = 1024;  // multiple of 64 required by sceAudioOut
constexpr int kMaxSeChannels = 8;

struct VoiceTarSlice {
  uint64_t offset = 0;
  uint32_t size = 0;
};

struct MemOggSource {
  std::vector<uint8_t> data;
  size_t pos = 0;
};

size_t mem_ogg_read(void* ptr, size_t size, size_t nmemb, void* datasource) {
  if (!ptr || size == 0 || nmemb == 0) return 0;
  auto* src = static_cast<MemOggSource*>(datasource);
  size_t avail = (src->pos < src->data.size()) ? (src->data.size() - src->pos) : 0;
  size_t count = std::min(nmemb, avail / size);
  size_t bytes = count * size;
  if (bytes > 0) {
    std::memcpy(ptr, src->data.data() + src->pos, bytes);
    src->pos += bytes;
  }
  return count;
}

int mem_ogg_seek(void* datasource, ogg_int64_t offset, int whence) {
  auto* src = static_cast<MemOggSource*>(datasource);
  int64_t next = 0;
  if (whence == SEEK_SET) {
    next = offset;
  } else if (whence == SEEK_CUR) {
    next = static_cast<int64_t>(src->pos) + offset;
  } else if (whence == SEEK_END) {
    next = static_cast<int64_t>(src->data.size()) + offset;
  } else {
    return -1;
  }
  if (next < 0 || static_cast<uint64_t>(next) > src->data.size()) return -1;
  src->pos = static_cast<size_t>(next);
  return 0;
}

int mem_ogg_close(void* /*datasource*/) {
  return 0;
}

long mem_ogg_tell(void* datasource) {
  return static_cast<long>(static_cast<MemOggSource*>(datasource)->pos);
}

const ov_callbacks kMemOggCallbacks = {
    mem_ogg_read,
    mem_ogg_seek,
    mem_ogg_close,
    mem_ogg_tell,
};

bool has_wav_extension(const std::string& path) {
  if (path.size() < 4) return false;
  std::string ext = path.substr(path.size() - 4);
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext == ".wav";
}

uint16_t read_le16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t read_le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

struct AudioStream {
  OggVorbis_File vf{};
  bool open = false;
  bool is_wav = false;
  bool loop = false;
  int channels = 2;
  int rate = 48000;

  int64_t loop_start = 0;
  int64_t loop_end = 0;
  int64_t pcm_cursor = 0;

  // Ogg staging buffer (up to 1024 stereo int16 samples)
  std::array<int16_t, 2048> ogg_buf{};
  int ogg_pos = 0;
  int ogg_frames = 0;

  // Small buffered PCM WAV data (e.g., h/se410.wav)
  std::vector<int16_t> wav_pcm;
  size_t wav_frame_pos = 0;

  // In-memory Ogg data (used for archived voice.tar slices)
  MemOggSource mem_ogg;

  // Linear resampler state (when rate != kSampleRate)
  float cur_l = 0.0f;
  float cur_r = 0.0f;
  float next_l = 0.0f;
  float next_r = 0.0f;
  bool resampler_primed = false;
  bool next_eof = false;
  bool stream_finished = false;
  double src_phase = 0.0;

  float volume = 1.0f;
  float fade_start = 1.0f;
  float fade_target = 1.0f;
  int fade_samples_total = 0;
  int fade_samples_done = 0;
  bool stop_after_fade = false;
  std::string tag;

  void close() {
    if (open && !is_wav) {
      ov_clear(&vf);
    }
    open = false;
    is_wav = false;
    wav_pcm.clear();
    wav_frame_pos = 0;
    mem_ogg.data.clear();
    mem_ogg.pos = 0;
    ogg_pos = 0;
    ogg_frames = 0;
    pcm_cursor = 0;
    loop_start = 0;
    loop_end = 0;
    resampler_primed = false;
    next_eof = false;
    stream_finished = false;
    src_phase = 0.0;
    tag.clear();
  }

  bool open_wav(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
      log::info("[Audio] Failed to open WAV: %s", path.c_str());
      return false;
    }
    uint8_t hdr[12];
    if (std::fread(hdr, 1, 12, f) != 12 || std::memcmp(hdr, "RIFF", 4) != 0 ||
        std::memcmp(hdr + 8, "WAVE", 4) != 0) {
      std::fclose(f);
      log::info("[Audio] Invalid WAV header: %s", path.c_str());
      return false;
    }

    bool got_fmt = false;
    bool got_data = false;
    uint16_t audio_fmt = 0;
    uint16_t bits = 0;
    uint16_t wav_ch = 2;
    uint32_t wav_rate = kSampleRate;

    uint8_t chdr[8];
    while (std::fread(chdr, 1, 8, f) == 8) {
      uint32_t sz = read_le32(chdr + 4);
      if (std::memcmp(chdr, "fmt ", 4) == 0 && sz >= 16) {
        uint8_t fmt[16];
        if (std::fread(fmt, 1, 16, f) != 16) break;
        audio_fmt = read_le16(fmt + 0);
        wav_ch = read_le16(fmt + 2);
        wav_rate = read_le32(fmt + 4);
        bits = read_le16(fmt + 14);
        if (sz > 16) {
          std::fseek(f, static_cast<long>(sz - 16), SEEK_CUR);
        }
        got_fmt = true;
      } else if (std::memcmp(chdr, "data", 4) == 0 && got_fmt) {
        if (audio_fmt != 1 || (wav_ch != 1 && wav_ch != 2) ||
            (bits != 16 && bits != 8) || sz == 0 || sz > 32 * 1024 * 1024) {
          log::info("[Audio] Unsupported WAV format (fmt=%u ch=%u bits=%u sz=%u): %s",
                    audio_fmt, wav_ch, bits, sz, path.c_str());
          break;
        }
        if (bits == 16) {
          size_t count = sz / 2;
          wav_pcm.resize(count);
          size_t read_n = std::fread(wav_pcm.data(), 2, count, f);
          wav_pcm.resize(read_n);
        } else {
          std::vector<uint8_t> raw(sz);
          size_t read_n = std::fread(raw.data(), 1, sz, f);
          wav_pcm.resize(read_n);
          for (size_t i = 0; i < read_n; ++i) {
            wav_pcm[i] = static_cast<int16_t>((static_cast<int>(raw[i]) - 128) << 8);
          }
        }
        got_data = !wav_pcm.empty();
        break;
      } else {
        std::fseek(f, static_cast<long>(sz), SEEK_CUR);
      }
      if (sz & 1) std::fseek(f, 1, SEEK_CUR);
    }
    std::fclose(f);

    if (!got_fmt || !got_data) {
      wav_pcm.clear();
      return false;
    }
    is_wav = true;
    open = true;
    channels = wav_ch;
    rate = static_cast<int>(wav_rate);
    wav_frame_pos = 0;
    return true;
  }

  void apply_playback_params(bool should_loop, float vol, int fade_in_ms,
                             int64_t lp_start, int64_t lp_len) {
    loop = should_loop;
    loop_start = (should_loop && lp_start > 0) ? lp_start : 0;
    loop_end = (should_loop && lp_len > 0) ? (loop_start + lp_len) : 0;
    pcm_cursor = 0;

    if (fade_in_ms > 0) {
      volume = 0.0f;
      fade_start = 0.0f;
      fade_target = vol;
      fade_samples_total = (fade_in_ms * kSampleRate) / 1000;
      fade_samples_done = 0;
      stop_after_fade = false;
    } else {
      volume = vol;
      fade_start = vol;
      fade_target = vol;
      fade_samples_total = 0;
      fade_samples_done = 0;
      stop_after_fade = false;
    }
  }

  bool finish_vorbis_open(const char* label, bool should_loop, float vol, int fade_in_ms,
                          int64_t lp_start, int64_t lp_len) {
    vorbis_info* vi = ov_info(&vf, -1);
    channels = vi ? vi->channels : 2;
    rate = vi ? static_cast<int>(vi->rate) : kSampleRate;
    if (channels != 1 && channels != 2) {
      log::info("[Audio] Unsupported channel count %d: %s", channels, label);
      ov_clear(&vf);
      mem_ogg.data.clear();
      mem_ogg.pos = 0;
      return false;
    }
    is_wav = false;
    open = true;
    apply_playback_params(should_loop, vol, fade_in_ms, lp_start, lp_len);
    return true;
  }

  bool open_mem_ogg(std::vector<uint8_t>&& bytes, const char* label, bool should_loop, float vol,
                    int fade_in_ms, int64_t lp_start = 0, int64_t lp_len = 0) {
    close();
    mem_ogg.data = std::move(bytes);
    mem_ogg.pos = 0;
    if (ov_open_callbacks(&mem_ogg, &vf, nullptr, 0, kMemOggCallbacks) < 0) {
      log::info("[Audio] Failed to open Ogg slice: %s", label);
      mem_ogg.data.clear();
      mem_ogg.pos = 0;
      return false;
    }
    return finish_vorbis_open(label, should_loop, vol, fade_in_ms, lp_start, lp_len);
  }

  bool open_file(const std::string& path, bool should_loop, float vol, int fade_in_ms,
                 int64_t lp_start = 0, int64_t lp_len = 0) {
    close();
    if (has_wav_extension(path)) {
      if (!open_wav(path)) return false;
      apply_playback_params(should_loop, vol, fade_in_ms, lp_start, lp_len);
      return true;
    }
    if (ov_fopen(path.c_str(), &vf) < 0) {
      log::info("[Audio] Failed to open Ogg: %s", path.c_str());
      return false;
    }
    return finish_vorbis_open(path.c_str(), should_loop, vol, fade_in_ms, lp_start, lp_len);
  }

  void seek_to_sample(int64_t target) {
    ogg_pos = 0;
    ogg_frames = 0;
    if (is_wav) {
      size_t total_frames = wav_pcm.size() / static_cast<size_t>(channels);
      wav_frame_pos = (target > 0 && static_cast<size_t>(target) < total_frames)
                          ? static_cast<size_t>(target)
                          : 0;
      pcm_cursor = static_cast<int64_t>(wav_frame_pos);
      return;
    }
    int rc = (target > 0) ? ov_pcm_seek(&vf, target) : ov_raw_seek(&vf, 0);
    if (rc < 0) {
      ov_raw_seek(&vf, 0);
      pcm_cursor = 0;
    } else {
      pcm_cursor = target;
    }
  }

  bool decode_one_source_frame(float& out_l, float& out_r) {
    if (!open) return false;
    if (loop && loop_end > 0 && pcm_cursor >= loop_end) {
      seek_to_sample(loop_start);
    }

    if (is_wav) {
      size_t total_frames = wav_pcm.size() / static_cast<size_t>(channels);
      if (wav_frame_pos >= total_frames) {
        if (!loop || total_frames == 0) return false;
        seek_to_sample(loop_start);
        if (wav_frame_pos >= total_frames) return false;
      }
      size_t idx = wav_frame_pos * static_cast<size_t>(channels);
      out_l = wav_pcm[idx] / 32768.0f;
      out_r = (channels == 1) ? out_l : (wav_pcm[idx + 1] / 32768.0f);
      ++wav_frame_pos;
      ++pcm_cursor;
      return true;
    }

    int retries = 0;
    while (ogg_pos >= ogg_frames) {
      int section = 0;
      int max_bytes = 1024 * channels * static_cast<int>(sizeof(int16_t));
      long got = ov_read(&vf, reinterpret_cast<char*>(ogg_buf.data()), max_bytes,
                         0, 2, 1, &section);
      if (got == 0) {
        if (!loop || retries > 0) {
          return false;
        }
        seek_to_sample(loop_start);
        ++retries;
        continue;
      }
      if (got < 0) {
        if (++retries > 8) return false;
        continue;
      }
      ogg_pos = 0;
      ogg_frames = static_cast<int>(got) / (channels * static_cast<int>(sizeof(int16_t)));
    }

    int idx = ogg_pos * channels;
    out_l = ogg_buf[idx] / 32768.0f;
    out_r = (channels == 1) ? out_l : (ogg_buf[idx + 1] / 32768.0f);
    ++ogg_pos;
    ++pcm_cursor;
    return true;
  }

  bool next_output_sample(float& out_l, float& out_r) {
    if (!open || stream_finished) return false;
    if (rate == kSampleRate || rate <= 0) {
      return decode_one_source_frame(out_l, out_r);
    }

    if (!resampler_primed) {
      if (!decode_one_source_frame(cur_l, cur_r)) {
        stream_finished = true;
        return false;
      }
      next_eof = !decode_one_source_frame(next_l, next_r);
      resampler_primed = true;
      src_phase = 0.0;
    }

    float frac = static_cast<float>(src_phase);
    if (next_eof) {
      out_l = cur_l;
      out_r = cur_r;
    } else {
      out_l = cur_l + (next_l - cur_l) * frac;
      out_r = cur_r + (next_r - cur_r) * frac;
    }

    src_phase += static_cast<double>(rate) / static_cast<double>(kSampleRate);
    while (src_phase >= 1.0) {
      src_phase -= 1.0;
      if (next_eof) {
        stream_finished = true;
        break;
      }
      cur_l = next_l;
      cur_r = next_r;
      next_eof = !decode_one_source_frame(next_l, next_r);
    }
    return true;
  }

  void start_fade(float target, int ms, bool stop_on_end) {
    if (!open) return;
    if (ms <= 0) {
      volume = target;
      fade_samples_total = 0;
      if (stop_on_end) close();
      return;
    }
    fade_start = volume;
    fade_target = target;
    fade_samples_total = (ms * kSampleRate) / 1000;
    fade_samples_done = 0;
    stop_after_fade = stop_on_end;
  }

  // Decode up to `frames` stereo samples (interleaved L,R) and add into `mix`.
  void mix_into(float* mix, int frames, float bus_gain) {
    if (!open) return;
    for (int i = 0; i < frames; ++i) {
      float sl = 0.0f;
      float sr = 0.0f;
      if (!next_output_sample(sl, sr)) {
        close();
        break;
      }
      if (fade_samples_total > 0) {
        float t = static_cast<float>(fade_samples_done) / fade_samples_total;
        if (t >= 1.0f) {
          volume = fade_target;
          fade_samples_total = 0;
          if (stop_after_fade) {
            close();
            break;
          }
        } else {
          volume = fade_start + (fade_target - fade_start) * t;
          ++fade_samples_done;
        }
      }
      float g = volume * bus_gain;
      int out_idx = i * 2;
      mix[out_idx] += sl * g;
      mix[out_idx + 1] += sr * g;
    }
  }
};

struct AudioState {
  SceUID mutex = -1;
  SceUID thread = -1;
  int port = -1;
  std::atomic<bool> running{false};

  AudioStream bgm;
  AudioStream voice;
  std::array<AudioStream, kMaxSeChannels> se;

  std::atomic<float> gain_bgm{0.75f};
  std::atomic<float> gain_se{0.85f};
  std::atomic<float> gain_voice{1.0f};

  SceUID voice_tar_fd = -1;
  std::string voice_data_root;
  std::unordered_map<std::string, VoiceTarSlice> voice_index;
};

AudioState g_audio;

int audio_thread_entry(SceSize /*args*/, void* /*argp*/) {
  std::vector<float> mix_buf(kFrameSamples * 2, 0.0f);
  std::vector<int16_t> out_buf(kFrameSamples * 2, 0);

  while (g_audio.running.load()) {
    std::fill(mix_buf.begin(), mix_buf.end(), 0.0f);

    sceKernelLockMutex(g_audio.mutex, 1, nullptr);
    g_audio.bgm.mix_into(mix_buf.data(), kFrameSamples, g_audio.gain_bgm.load());
    g_audio.voice.mix_into(mix_buf.data(), kFrameSamples, g_audio.gain_voice.load());
    for (auto& ch : g_audio.se) {
      ch.mix_into(mix_buf.data(), kFrameSamples, g_audio.gain_se.load());
    }
    sceKernelUnlockMutex(g_audio.mutex, 1);

    for (size_t i = 0; i < mix_buf.size(); ++i) {
      float v = std::max(-1.0f, std::min(1.0f, mix_buf[i]));
      out_buf[i] = static_cast<int16_t>(v * 32767.0f);
    }

    sceAudioOutOutput(g_audio.port, out_buf.data());
  }
  return 0;
}

}  // namespace

bool AudioEngine::init() {
  g_audio.port = sceAudioOutOpenPort(
      SCE_AUDIO_OUT_PORT_TYPE_BGM, kFrameSamples, kSampleRate, SCE_AUDIO_OUT_MODE_STEREO);
  if (g_audio.port < 0) {
    log::info("[Audio] sceAudioOutOpenPort failed: 0x%08X", g_audio.port);
    return false;
  }

  g_audio.mutex = sceKernelCreateMutex("fsn_audio_mtx", 0, 0, nullptr);
  g_audio.running.store(true);
  g_audio.thread = sceKernelCreateThread(
      "fsn_audio_thr", audio_thread_entry, 0x10000100, 0x20000, 0, 0, nullptr);
  if (g_audio.thread < 0) {
    log::info("[Audio] sceKernelCreateThread failed: 0x%08X", g_audio.thread);
    g_audio.running.store(false);
    sceAudioOutReleasePort(g_audio.port);
    g_audio.port = -1;
    return false;
  }
  sceKernelStartThread(g_audio.thread, 0, nullptr);
  log::info("[Audio] Initialized 48kHz stereo mixer (with resampler, SLI loop, WAV)");
  return true;
}

void AudioEngine::shutdown() {
  if (!g_audio.running.load()) return;
  g_audio.running.store(false);
  if (g_audio.thread >= 0) {
    sceKernelWaitThreadEnd(g_audio.thread, nullptr, nullptr);
    sceKernelDeleteThread(g_audio.thread);
    g_audio.thread = -1;
  }
  sceKernelLockMutex(g_audio.mutex, 1, nullptr);
  g_audio.bgm.close();
  g_audio.voice.close();
  for (auto& ch : g_audio.se) ch.close();
  sceKernelUnlockMutex(g_audio.mutex, 1);

  if (g_audio.mutex >= 0) {
    sceKernelDeleteMutex(g_audio.mutex);
    g_audio.mutex = -1;
  }
  if (g_audio.port >= 0) {
    sceAudioOutReleasePort(g_audio.port);
    g_audio.port = -1;
  }
  if (g_audio.voice_tar_fd >= 0) {
    sceIoClose(g_audio.voice_tar_fd);
    g_audio.voice_tar_fd = -1;
  }
  g_audio.voice_index.clear();
}

bool AudioEngine::init_voice_archive(const std::string& data_root, const std::string& tar_path,
                                     const std::string& idx_path) {
  if (g_audio.voice_tar_fd >= 0) {
    sceIoClose(g_audio.voice_tar_fd);
    g_audio.voice_tar_fd = -1;
  }
  g_audio.voice_index.clear();
  g_audio.voice_data_root = data_root;
  while (!g_audio.voice_data_root.empty() && g_audio.voice_data_root.back() == '/') {
    g_audio.voice_data_root.pop_back();
  }

  FILE* f = std::fopen(idx_path.c_str(), "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  long fsz = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (fsz <= 0) {
    std::fclose(f);
    return false;
  }

  std::string buf(static_cast<size_t>(fsz), '\0');
  size_t got = std::fread(&buf[0], 1, buf.size(), f);
  std::fclose(f);
  buf.resize(got);

  g_audio.voice_index.reserve(30000);
  size_t pos = 0;
  while (pos < buf.size()) {
    size_t nl = buf.find('\n', pos);
    size_t end = (nl == std::string::npos) ? buf.size() : nl;
    size_t t1 = buf.find('\t', pos);
    if (t1 != std::string::npos && t1 < end) {
      size_t t2 = buf.find('\t', t1 + 1);
      if (t2 != std::string::npos && t2 < end) {
        std::string key = buf.substr(pos, t1 - pos);
        uint64_t off = std::strtoull(buf.c_str() + t1 + 1, nullptr, 10);
        uint32_t sz = static_cast<uint32_t>(std::strtoull(buf.c_str() + t2 + 1, nullptr, 10));
        if (!key.empty() && sz > 0) {
          g_audio.voice_index.emplace(std::move(key), VoiceTarSlice{off, sz});
        }
      }
    }
    pos = (nl == std::string::npos) ? buf.size() : (nl + 1);
  }

  if (g_audio.voice_index.empty()) return false;

  g_audio.voice_tar_fd = sceIoOpen(tar_path.c_str(), SCE_O_RDONLY, 0);
  if (g_audio.voice_tar_fd < 0) {
    log::info("[Audio] Failed to open voice archive %s: 0x%08X", tar_path.c_str(),
              g_audio.voice_tar_fd);
    g_audio.voice_index.clear();
    return false;
  }

  log::info("[Audio] Loaded voice TAR index (%u entries)",
            static_cast<unsigned>(g_audio.voice_index.size()));
  return true;
}

void AudioEngine::play_bgm(const std::string& path, int fade_ms, bool loop,
                           int64_t loop_start, int64_t loop_len) {
  sceKernelLockMutex(g_audio.mutex, 1, nullptr);
  g_audio.bgm.open_file(path, loop, 1.0f, fade_ms, loop_start, loop_len);
  sceKernelUnlockMutex(g_audio.mutex, 1);
}

void AudioEngine::stop_bgm(int fade_ms) {
  sceKernelLockMutex(g_audio.mutex, 1, nullptr);
  g_audio.bgm.start_fade(0.0f, fade_ms, true);
  sceKernelUnlockMutex(g_audio.mutex, 1);
}

void AudioEngine::fade_bgm(int target_pct, int time_ms) {
  sceKernelLockMutex(g_audio.mutex, 1, nullptr);
  g_audio.bgm.start_fade(std::max(0, std::min(100, target_pct)) / 100.0f, time_ms, false);
  sceKernelUnlockMutex(g_audio.mutex, 1);
}

void AudioEngine::play_voice(const std::string& path) {
  if (g_audio.voice_tar_fd >= 0 && !g_audio.voice_index.empty()) {
    std::string rel;
    std::string prefix = g_audio.voice_data_root + "/";
    if (path.compare(0, prefix.size(), prefix) == 0) {
      rel = path.substr(prefix.size());
    } else {
      size_t p = path.find("audio/voice/");
      if (p != std::string::npos) rel = path.substr(p);
    }

    auto it = g_audio.voice_index.find(rel);
    if (it != g_audio.voice_index.end() && it->second.size > 0 &&
        it->second.size <= 32 * 1024 * 1024) {
      std::vector<uint8_t> bytes(it->second.size);
      if (sceIoLseek(g_audio.voice_tar_fd, static_cast<SceOff>(it->second.offset), SCE_SEEK_SET) >=
          0) {
        size_t total = 0;
        while (total < bytes.size()) {
          int got = sceIoRead(g_audio.voice_tar_fd, bytes.data() + total,
                              static_cast<SceSize>(bytes.size() - total));
          if (got <= 0) break;
          total += static_cast<size_t>(got);
        }
        if (total == bytes.size()) {
          sceKernelLockMutex(g_audio.mutex, 1, nullptr);
          g_audio.voice.open_mem_ogg(std::move(bytes), rel.c_str(), false, 1.0f, 0);
          sceKernelUnlockMutex(g_audio.mutex, 1);
          return;
        }
      }
    }
  }

  sceKernelLockMutex(g_audio.mutex, 1, nullptr);
  g_audio.voice.open_file(path, false, 1.0f, 0);
  sceKernelUnlockMutex(g_audio.mutex, 1);
}

void AudioEngine::stop_voice() {
  sceKernelLockMutex(g_audio.mutex, 1, nullptr);
  g_audio.voice.close();
  sceKernelUnlockMutex(g_audio.mutex, 1);
}

bool AudioEngine::is_voice_playing() const {
  sceKernelLockMutex(g_audio.mutex, 1, nullptr);
  bool playing = g_audio.voice.open;
  sceKernelUnlockMutex(g_audio.mutex, 1);
  return playing;
}

void AudioEngine::play_se(const std::string& name, const std::string& path, bool loop,
                          int fade_in_ms, float volume) {
  sceKernelLockMutex(g_audio.mutex, 1, nullptr);
  int slot = -1;
  for (int i = 0; i < kMaxSeChannels; ++i) {
    if (!g_audio.se[i].open) {
      slot = i;
      break;
    }
  }
  if (slot < 0) slot = 0;
  if (g_audio.se[slot].open_file(path, loop, volume, fade_in_ms)) {
    g_audio.se[slot].tag = name;
  }
  sceKernelUnlockMutex(g_audio.mutex, 1);
}

void AudioEngine::stop_se(const std::string& name, int fade_out_ms) {
  sceKernelLockMutex(g_audio.mutex, 1, nullptr);
  for (auto& ch : g_audio.se) {
    if (ch.open && (name.empty() || ch.tag == name)) {
      ch.start_fade(0.0f, fade_out_ms, true);
    }
  }
  sceKernelUnlockMutex(g_audio.mutex, 1);
}

void AudioEngine::stop_all_se(int fade_out_ms) {
  stop_se("", fade_out_ms);
}

void AudioEngine::set_master_volumes(float bgm, float se, float voice) {
  g_audio.gain_bgm.store(bgm);
  g_audio.gain_se.store(se);
  g_audio.gain_voice.store(voice);
}

}  // namespace fsn
