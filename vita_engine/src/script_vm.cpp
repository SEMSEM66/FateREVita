#include "script_vm.hpp"
#include "extras.hpp"
#include "log.hpp"

#include <psp2/io/stat.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace fsn {

namespace {

bool read_json_file(const std::string& path, nlohmann::json& out) {
  std::ifstream f(path);
  if (!f.is_open()) {
    log::info("[VM] Cannot open JSON: %s", path.c_str());
    return false;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  out = nlohmann::json::parse(ss.str(), nullptr, false);
  if (out.is_discarded()) {
    log::info("[VM] JSON parse error in: %s", path.c_str());
    return false;
  }
  return true;
}

bool write_json_file(const std::string& path, const nlohmann::json& obj) {
  sceIoMkdir("ux0:data", 0777);
  sceIoMkdir("ux0:data/FateReVita", 0777);
  std::ofstream f(path, std::ios::trunc);
  if (!f.is_open()) return false;
  f << obj.dump(2) << '\n';
  return f.good();
}

std::string to_lower_ascii(std::string s) {
  for (char& c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

std::string strip_ext(const std::string& s) {
  auto pos = s.rfind('.');
  if (pos == std::string::npos) return s;
  std::string ext = to_lower_ascii(s.substr(pos));
  if (ext == ".wav" || ext == ".ogg" || ext == ".fcd" || ext == ".png" ||
      ext == ".webp" || ext == ".bmp" || ext == ".jpg" || ext == ".ks" || ext == ".fcf") {
    return s.substr(0, pos);
  }
  return s;
}

std::string normalize_asset_stem(const std::string& s) {
  std::string out = strip_ext(s);
  if (out.rfind("h/", 0) == 0 || out.rfind("H/", 0) == 0) {
    out = out.substr(2);
  }
  return out;
}

std::string format_scene_stem(const std::string& fcf_stem, int nid) {
  if (fcf_stem == "prologue") {
    return "\xE3\x83\x97\xE3\x83\xAD\xE3\x83\xAD\xE3\x83\xBC\xE3\x82\xB0" +
           std::to_string(nid) + "\xE6\x97\xA5\xE7\x9B\xAE";  // プロローグ<N>日目
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "-%02d", nid);
  return fcf_stem + buf;
}

float apply_accel(float t, float accel) {
  t = std::max(0.0f, std::min(1.0f, t));
  if (accel > 0.1f) {
    return std::pow(t, 1.0f + accel * 0.5f);
  }
  if (accel < -0.1f) {
    return 1.0f - std::pow(1.0f - t, 1.0f + (-accel) * 0.5f);
  }
  return t;
}

}  // namespace

bool ScriptVM::init(const std::string& data_root, Renderer* renderer, AudioEngine* audio,
                    TextRenderer* text, AppConfig* config, const std::string& global_flags_path,
                    ExtrasManager* extras) {
  data_root_ = data_root;
  renderer_ = renderer;
  audio_ = audio;
  text_ = text;
  config_ = config;
  extras_ = extras;
  global_flags_path_ = global_flags_path;

  if (!read_json_file(data_root_ + "/manifest.json", manifest_)) {
    return false;
  }

  correct_x_.clear();
  nlohmann::json corr;
  if (read_json_file(data_root_ + "/config/correct.json", corr) && corr.is_object()) {
    for (auto it = corr.begin(); it != corr.end(); ++it) {
      if (it.value().is_number()) {
        correct_x_[it.key()] = it.value().get<float>() * 0.5f;
      }
    }
  }
  load_global_flags();

  log::info("[VM] Initialized (scripts=%u scripts_h=%u flowcharts=%u voices=%u)",
            static_cast<unsigned>(manifest_.contains("scripts") ? manifest_["scripts"].size() : 0),
            static_cast<unsigned>(manifest_.contains("scripts_h") ? manifest_["scripts_h"].size() : 0),
            static_cast<unsigned>(manifest_.contains("flowcharts") ? manifest_["flowcharts"].size() : 0),
            static_cast<unsigned>(manifest_.contains("voice") ? manifest_["voice"].size() : 0));
  return true;
}

TitleBgmInfo ScriptVM::get_title_bgm_info() const {
  TitleBgmInfo info;
  if (manifest_.contains("bgm") && manifest_["bgm"].contains("bgm01")) {
    info.new_path = data_root_ + "/" + manifest_["bgm"]["bgm01"].get<std::string>();
  }
  if (manifest_.contains("bgm_old") && manifest_["bgm_old"].contains("bgm01")) {
    info.old_path = data_root_ + "/" + manifest_["bgm_old"]["bgm01"].get<std::string>();
  }
  if (manifest_.contains("bgm_old_loops") && manifest_["bgm_old_loops"].contains("bgm01")) {
    const auto& arr = manifest_["bgm_old_loops"]["bgm01"];
    if (arr.is_array() && arr.size() >= 2) {
      info.old_loop_start = arr[0].get<int64_t>();
      info.old_loop_len = arr[1].get<int64_t>();
    }
  }
  return info;
}

void ScriptVM::load_global_flags() {
  global_flags_.clear();
  nlohmann::json gf;
  if (!global_flags_path_.empty() && read_json_file(global_flags_path_, gf) && gf.is_object()) {
    for (auto it = gf.begin(); it != gf.end(); ++it) {
      if (it.value().is_number_integer()) {
        global_flags_[it.key()] = it.value().get<int>();
      }
    }
  }
}

void ScriptVM::save_global_flags() const {
  if (global_flags_path_.empty()) return;
  nlohmann::json gf = nlohmann::json::object();
  for (const auto& kv : global_flags_) {
    gf[kv.first] = kv.second;
  }
  write_json_file(global_flags_path_, gf);
}

int ScriptVM::get_flag(const std::string& name) const {
  if (name == "ghentai" || name == "gmature") {
    return (config_ && config_->content_mode != ContentMode::AllAges) ? 1 : 0;
  }
  if (config_ && config_->unlock_all_routes &&
      (name == "g\xE3\x82\xBB\xE3\x82\xA4\xE3\x83\x90\xE3\x83\xBC\xE3\x82\xAF\xE3\x83\xAA\xE3\x82\xA2" ||  // gセイバークリア
       name == "g\xE5\x87\x9B\xE3\x82\xAF\xE3\x83\xAA\xE3\x82\xA2")) {                                       // g凛クリア
    return 1;
  }
  if (!name.empty() && name[0] == 'g') {
    auto it = global_flags_.find(name);
    return (it != global_flags_.end()) ? it->second : 0;
  }
  auto it = route_flags_.find(name);
  return (it != route_flags_.end()) ? it->second : 0;
}

void ScriptVM::set_flag(const std::string& name, int val) {
  if (!name.empty() && name[0] == 'g') {
    global_flags_[name] = val;
    save_global_flags();
  } else {
    route_flags_[name] = val;
  }
}

bool ScriptVM::eval_flag_cond(const nlohmann::json& cond) const {
  std::string flag = cond.value("flag", "");
  int v0 = cond.value("v0", 0);
  int v1 = cond.value("v1", 0);
  int cur = get_flag(flag);
  switch (v0) {
    case 0: return cur == v1;
    case 1: return cur != v1;
    case 2: return cur < v1;
    case 3: return cur > v1;
    case 4: return cur <= v1;
    case 5: return cur >= v1;
    default: return false;
  }
}

void ScriptVM::apply_flag_ops(const nlohmann::json& ops) {
  if (!ops.is_array()) return;
  for (const auto& op : ops) {
    std::string flag = op.value("flag", "");
    int v0 = op.value("v0", 0);
    int v1 = op.value("v1", 0);
    if (flag.empty()) continue;
    int cur = get_flag(flag);
    if (v1 == 1) cur += v0;
    else if (v1 == 2) cur -= v0;
    else if (v1 == 3) cur = v0;
    set_flag(flag, cur);
  }
}

void ScriptVM::update_coord_transform() {
  int bw = 1920;
  if (script_.contains("coord_base") && script_["coord_base"].is_array() &&
      !script_["coord_base"].empty()) {
    bw = script_["coord_base"][0].get<int>();
  }
  if (bw <= 900) {
    coord_scale_ = 544.0f / 600.0f;
    coord_ox_ = (960.0f - 800.0f * coord_scale_) * 0.5f;
    coord_oy_ = 0.0f;
  } else {
    coord_scale_ = 0.5f;
    coord_ox_ = 0.0f;
    coord_oy_ = 0.0f;
  }
}

bool ScriptVM::start_preset(int preset_index, const std::string& choice_backup_path) {
  if (!choice_backup_path.empty()) choice_backup_path_ = choice_backup_path;
  route_flags_.clear();
  call_stack_.clear();
  layers_.clear();
  credits_lines_.clear();
  credits_active_ = false;
  credits_scroll_ = 0.0f;
  cur_bgm_stem_.clear();
  cur_bgm_playing_ = false;
  choice_active_ = false;
  tiger_retry_mode_ = false;
  backlog_open_ = false;
  backlog_scroll_ = 0;

  switch (preset_index) {
    case 0:
      return start_flowchart("prologue", 1);
    case 1:
      return start_flowchart("\xE3\x82\xBB\xE3\x82\xA4\xE3\x83\x90\xE3\x83\xBC\xE3\x83\xAB\xE3\x83\xBC\xE3\x83\x88\xE4\xB8\x80\xE6\x97\xA5\xE7\x9B\xAE", 0);  // セイバールート一日目
    case 2:
      return start_flowchart("\xE5\x87\x9B\xE3\x83\xAB\xE3\x83\xBC\xE3\x83\x88\xE4\xB8\x89\xE6\x97\xA5\xE7\x9B\xAE", 0);  // 凛ルート三日目
    case 3:
      return start_flowchart("\xE6\xA1\x9C\xE3\x83\xAB\xE3\x83\xBC\xE3\x83\x88\xE5\x9B\x9B\xE6\x97\xA5\xE7\x9B\xAE", 0);  // 桜ルート四日目
    case 4:
      in_flowchart_ = false;
      fcf_stem_.clear();
      return load_script_by_stem("\xE3\x82\xBB\xE3\x82\xA4\xE3\x83\x90\xE3\x83\xBC\xE3\x83\xAB\xE3\x83\xBC\xE3\x83\x88\xE5\x8D\x81\xE4\xB8\x80\xE6\x97\xA5\xE7\x9B\xAE-118");  // セイバールート十一日目-118
    case 5:
      in_flowchart_ = false;
      fcf_stem_.clear();
      return load_script_by_stem("\xE5\x87\x9B\xE3\x83\xAB\xE3\x83\xBC\xE3\x83\x88\xE5\x8D\x81\xE5\x9B\x9B\xE6\x97\xA5\xE7\x9B\xAE-108");  // 凛ルート十四日目-108
    case 6:
      in_flowchart_ = false;
      fcf_stem_.clear();
      return load_script_by_stem("\xE6\xA1\x9C\xE3\x83\xAB\xE3\x83\xBC\xE3\x83\x88\xE4\xB9\x9D\xE6\x97\xA5\xE7\x9B\xAE-130");  // 桜ルート九日目-130
    case 7:
      in_flowchart_ = false;
      fcf_stem_.clear();
      return load_script_by_stem("\xE3\x83\xA9\xE3\x82\xB9\xE3\x83\x88\xE3\x82\xA8\xE3\x83\x94\xE3\x82\xBD\xE3\x83\xBC\xE3\x83\x89");  // ラストエピソード
    default:
      return start_flowchart("prologue", 1);
  }
}

bool ScriptVM::start_flowchart(const std::string& fcf_stem, int start_node) {
  std::string normalized = (fcf_stem == "\xE3\x83\x97\xE3\x83\xAD\xE3\x83\xAD\xE3\x83\xBC\xE3\x82\xB0")
                               ? "prologue"
                               : fcf_stem;

  const bool want_h = (config_ && config_->content_mode != ContentMode::AllAges);
  std::string rel;
  if (want_h && manifest_.contains("flowcharts_h") && manifest_["flowcharts_h"].contains(normalized)) {
    rel = manifest_["flowcharts_h"][normalized].get<std::string>();
  } else if (manifest_.contains("flowcharts") && manifest_["flowcharts"].contains(normalized)) {
    rel = manifest_["flowcharts"][normalized].get<std::string>();
  } else if (manifest_.contains("flowcharts_h") && manifest_["flowcharts_h"].contains(normalized)) {
    rel = manifest_["flowcharts_h"][normalized].get<std::string>();
  }
  if (rel.empty() || !read_json_file(data_root_ + "/" + rel, flowchart_)) {
    log::info("[VM] Flowchart not found: %s", normalized.c_str());
    return false;
  }

  in_flowchart_ = true;
  fcf_stem_ = normalized;
  int target = (start_node >= 0) ? start_node : flowchart_.value("start", 0);
  if (normalized == "prologue" && start_node < 0) target = 1;
  log::info("[VM] Entering flowchart '%s' at node %d (H=%d)", fcf_stem_.c_str(), target, want_h ? 1 : 0);
  return enter_fcf_node(target);
}

bool ScriptVM::enter_fcf_node(int node_id) {
  for (int guard = 0; guard < 64; ++guard) {
    if (!flowchart_.contains("nodes")) {
      finished_ = true;
      return false;
    }
    const auto& nodes = flowchart_["nodes"];
    std::string key = std::to_string(node_id);
    if (!nodes.contains(key)) {
      log::info("[VM] FCF node %d missing in %s; finishing.", node_id, fcf_stem_.c_str());
      finished_ = true;
      return false;
    }

    fcf_node_id_ = node_id;
    const auto& node = nodes[key];
    std::string ntype = node.value("type", "");

    if (ntype == "OUTERLABEL") {
      std::string dest_file = node.value("file", "");
      int dest_node = node.value("target", 0);
      if (dest_file.empty()) {
        finished_ = true;
        return true;
      }
      return start_flowchart(dest_file, dest_node);
    }

    if (ntype == "SELECTER") {
      choices_.clear();
      choice_prompt_ = node.value("title", "Select an option");
      if (node.contains("choices") && node["choices"].is_array()) {
        for (const auto& c : node["choices"]) {
          ChoiceItem item;
          item.target = c.value("target", -1);
          item.text = c.value("text", "Choice");
          choices_.push_back(item);
        }
      }
      if (choices_.empty()) {
        finished_ = true;
        return true;
      }
      if (!choice_backup_path_.empty()) {
        write_quicksave(choice_backup_path_);
      }
      choice_selected_ = 0;
      choice_active_ = true;
      tiger_retry_mode_ = false;
      finished_ = false;
      return true;
    }

    if (ntype == "SCENE") {
      std::string sc_stem = format_scene_stem(fcf_stem_, node_id);
      if (load_script_by_stem(sc_stem, "", true)) {
        return true;
      }
      // Pure flag-check / pass-through SCENE node (no .ks file)
      if (node.contains("flag_ops")) {
        apply_flag_ops(node["flag_ops"]);
      }
      int next_id = -1;
      if (node.contains("jumps") && node["jumps"].is_array()) {
        for (const auto& j : node["jumps"]) {
          int link = j.value("link", 1);
          bool ok = true;
          if (j.contains("conds") && j["conds"].is_array() && !j["conds"].empty()) {
            if (link == 2) {
              ok = false;
              for (const auto& c : j["conds"]) {
                if (eval_flag_cond(c)) { ok = true; break; }
              }
            } else {
              for (const auto& c : j["conds"]) {
                if (!eval_flag_cond(c)) { ok = false; break; }
              }
            }
          }
          if (ok) {
            next_id = j.value("target", -1);
            break;
          }
        }
      }
      if (next_id < 0) {
        finished_ = true;
        return true;
      }
      node_id = next_id;
      continue;
    }

    finished_ = true;
    return false;
  }

  log::info("[VM] FCF loop guard exceeded in %s", fcf_stem_.c_str());
  finished_ = true;
  return false;
}

void ScriptVM::complete_fcf_scene() {
  if (!in_flowchart_ || !flowchart_.contains("nodes")) {
    finished_ = true;
    return;
  }
  std::string key = std::to_string(fcf_node_id_);
  if (!flowchart_["nodes"].contains(key)) {
    finished_ = true;
    return;
  }
  const auto& node = flowchart_["nodes"][key];
  if (node.contains("flag_ops")) {
    apply_flag_ops(node["flag_ops"]);
  }

  if (fcf_stem_ == "prologue") {
    if (fcf_node_id_ == 1) { enter_fcf_node(2); return; }
    if (fcf_node_id_ == 2) { enter_fcf_node(3); return; }
    if (fcf_node_id_ == 3) { enter_fcf_node(0); return; }
  }

  int next_id = -1;
  if (node.contains("jumps") && node["jumps"].is_array()) {
    for (const auto& j : node["jumps"]) {
      int link = j.value("link", 1);
      bool ok = true;
      if (j.contains("conds") && j["conds"].is_array() && !j["conds"].empty()) {
        if (link == 2) {
          ok = false;
          for (const auto& c : j["conds"]) {
            if (eval_flag_cond(c)) { ok = true; break; }
          }
        } else {
          for (const auto& c : j["conds"]) {
            if (!eval_flag_cond(c)) { ok = false; break; }
          }
        }
      }
      if (ok) {
        next_id = j.value("target", -1);
        break;
      }
    }
  }
  if (next_id < 0) {
    finished_ = true;
    return;
  }
  enter_fcf_node(next_id);
}

void ScriptVM::on_script_end() {
  if (!call_stack_.empty()) {
    CallFrame top = std::move(call_stack_.back());
    call_stack_.pop_back();
    cur_script_stem_ = top.script_stem;
    script_ = std::move(top.script);
    ip_ = top.ip;
    coord_scale_ = top.coord_scale;
    coord_ox_ = top.coord_ox;
    coord_oy_ = top.coord_oy;
    credits_active_ = false;
    credits_lines_.clear();
    return;
  }
  if (in_flowchart_) {
    complete_fcf_scene();
  } else {
    finished_ = true;
  }
}

bool ScriptVM::load_script(const std::string& rel_script_path) {
  if (!read_json_file(data_root_ + "/" + rel_script_path, script_)) {
    return false;
  }
  in_flowchart_ = false;
  fcf_stem_.clear();
  call_stack_.clear();
  cur_script_stem_ = strip_ext(rel_script_path);
  ip_ = 0;
  finished_ = false;
  wait_click_ = false;
  wait_page_ = false;
  wait_timer_ = 0.0f;
  wait_move_ = false;
  wait_trans_ = false;
  choice_active_ = false;
  text_visible_ = true;
  user_hid_text_ = false;
  interlude_mode_ = false;
  layers_.clear();
  bg_is_solid_ = true;
  bg_r_ = bg_g_ = bg_b_ = 0.0f;
  bg_name_.clear();
  bg_path_.clear();
  tint_r_ = tint_g_ = tint_b_ = 1.0f;
  darken_alpha_ = 0.0f;
  update_coord_transform();
  text_->clear_page();
  step_until_blocked();
  return true;
}

bool ScriptVM::load_script_by_stem(const std::string& raw_stem, const std::string& target_label,
                                     bool preserve_scene) {
  std::string stem = strip_ext(raw_stem);
  const bool want_h = (config_ && config_->content_mode != ContentMode::AllAges);
  std::string rel;
  if (want_h) {
    if (manifest_.contains("scripts_h") && manifest_["scripts_h"].contains(stem)) {
      rel = manifest_["scripts_h"][stem].get<std::string>();
    } else if (manifest_.contains("scripts") && manifest_["scripts"].contains(stem)) {
      rel = manifest_["scripts"][stem].get<std::string>();
    }
  } else {
    if (manifest_.contains("scripts") && manifest_["scripts"].contains(stem)) {
      rel = manifest_["scripts"][stem].get<std::string>();
    } else if (!in_flowchart_ && manifest_.contains("scripts_h") &&
               manifest_["scripts_h"].contains(stem)) {
      rel = manifest_["scripts_h"][stem].get<std::string>();
    }
  }
  if (rel.empty() || !read_json_file(data_root_ + "/" + rel, script_)) {
    return false;
  }

  cur_script_stem_ = stem;
  ip_ = 0;
  if (!target_label.empty() && script_.contains("labels") &&
      script_["labels"].contains(target_label)) {
    ip_ = script_["labels"][target_label].get<size_t>();
  }

  finished_ = false;
  wait_click_ = false;
  wait_page_ = false;
  wait_timer_ = 0.0f;
  wait_move_ = false;
  wait_trans_ = false;
  update_coord_transform();

  if (!preserve_scene) {
    layers_.clear();
    bg_is_solid_ = true;
    bg_r_ = bg_g_ = bg_b_ = 0.0f;
    bg_name_.clear();
    bg_path_.clear();
    tint_r_ = tint_g_ = tint_b_ = 1.0f;
    darken_alpha_ = 0.0f;
    text_visible_ = true;
    user_hid_text_ = false;
    interlude_mode_ = false;
    text_->clear_page();
  }
  step_until_blocked();
  return true;
}

bool ScriptVM::load_quicksave(const std::string& save_path) {
  nlohmann::json sv;
  if (!read_json_file(save_path, sv)) return false;

  route_flags_.clear();
  if (sv.contains("route_flags") && sv["route_flags"].is_object()) {
    for (auto it = sv["route_flags"].begin(); it != sv["route_flags"].end(); ++it) {
      if (it.value().is_number_integer()) route_flags_[it.key()] = it.value().get<int>();
    }
  }

  call_stack_.clear();
  bool has_fcf = sv.value("in_flowchart", false);
  std::string fstem = sv.value("fcf_stem", "");
  int fnode = sv.value("fcf_node", 0);
  std::string sc_stem = sv.value("script_stem", "");
  if (sc_stem.empty()) sc_stem = "プロローグ1日目";

  if (has_fcf && !fstem.empty()) {
    const bool want_h = (config_ && config_->content_mode != ContentMode::AllAges);
    std::string frel;
    if (want_h && manifest_.contains("flowcharts_h") && manifest_["flowcharts_h"].contains(fstem)) {
      frel = manifest_["flowcharts_h"][fstem].get<std::string>();
    } else if (manifest_.contains("flowcharts") && manifest_["flowcharts"].contains(fstem)) {
      frel = manifest_["flowcharts"][fstem].get<std::string>();
    } else if (manifest_.contains("flowcharts_h") && manifest_["flowcharts_h"].contains(fstem)) {
      frel = manifest_["flowcharts_h"][fstem].get<std::string>();
    }
    if (!frel.empty()) read_json_file(data_root_ + "/" + frel, flowchart_);
    in_flowchart_ = !flowchart_.is_null();
    fcf_stem_ = fstem;
    fcf_node_id_ = fnode;
  } else {
    in_flowchart_ = false;
    fcf_stem_.clear();
  }

  if (sv.value("choice_active", false) && in_flowchart_) {
    return enter_fcf_node(fcf_node_id_);
  }

  if (!load_script_by_stem(sc_stem, "", false)) {
    if (!load_script(sv.value("script", "scripts/prologue_day1.json"))) return false;
  }

  ip_ = sv.value("ip", static_cast<size_t>(0));
  bg_name_ = sv.value("bg_name", "");
  bg_path_ = resolve_image(bg_name_);
  bg_is_solid_ = bg_path_.empty();
  interlude_mode_ = sv.value("interlude", false);
  text_visible_ = sv.value("text_visible", true);

  layers_.clear();
  if (sv.contains("layers") && sv["layers"].is_array()) {
    for (const auto& jl : sv["layers"]) {
      std::string id = jl.value("id", "");
      if (id.empty()) continue;
      Layer l;
      l.visible = true;
      l.image_name = jl.value("image_name", "");
      l.image_path = resolve_image(l.image_name);
      l.x = jl.value("x", 0.0f);
      l.y = jl.value("y", 0.0f);
      l.alpha = jl.value("alpha", 1.0f);
      l.z_index = jl.value("z_index", 5000);
      l.fliplr = jl.value("fliplr", false);
      l.flipud = jl.value("flipud", false);
      l.blend = static_cast<BlendMode>(jl.value("blend", 0));
      layers_[id] = l;
    }
  }

  std::string bgm = sv.value("bgm", "");
  if (!bgm.empty()) play_bgm_stem(bgm, 300);

  text_->clear_page();
  wait_click_ = false;
  wait_page_ = false;
  wait_timer_ = 0.0f;
  choice_active_ = false;
  step_until_blocked();
  return true;
}

bool ScriptVM::write_quicksave(const std::string& save_path) const {
  nlohmann::json sv;
  sv["in_flowchart"] = in_flowchart_;
  sv["fcf_stem"] = fcf_stem_;
  sv["fcf_node"] = fcf_node_id_;
  sv["choice_active"] = choice_active_;
  sv["script_stem"] = cur_script_stem_;
  sv["ip"] = ip_;
  sv["bg_name"] = bg_name_;
  sv["interlude"] = interlude_mode_;
  sv["text_visible"] = text_visible_;
  sv["bgm"] = cur_bgm_playing_ ? cur_bgm_stem_ : "";

  nlohmann::json rf = nlohmann::json::object();
  for (const auto& kv : route_flags_) rf[kv.first] = kv.second;
  sv["route_flags"] = rf;

  nlohmann::json jlayers = nlohmann::json::array();
  for (const auto& kv : layers_) {
    if (!kv.second.visible) continue;
    nlohmann::json jl;
    jl["id"] = kv.first;
    jl["image_name"] = kv.second.image_name;
    jl["x"] = kv.second.x;
    jl["y"] = kv.second.y;
    jl["alpha"] = kv.second.alpha;
    jl["z_index"] = kv.second.z_index;
    jl["fliplr"] = kv.second.fliplr;
    jl["flipud"] = kv.second.flipud;
    jl["blend"] = static_cast<int>(kv.second.blend);
    jlayers.push_back(jl);
  }
  sv["layers"] = jlayers;

  return write_json_file(save_path, sv);
}

void ScriptVM::refresh_after_config_change() {
  if (!bg_name_.empty()) {
    bg_path_ = resolve_image(bg_name_);
    bg_is_solid_ = bg_path_.empty();
  }
  for (auto& kv : layers_) {
    if (!kv.second.image_name.empty()) {
      kv.second.image_path = resolve_image(kv.second.image_name);
    }
  }
  if (cur_bgm_playing_ && !cur_bgm_stem_.empty()) {
    play_bgm_stem(cur_bgm_stem_, 350);
  }
}

std::string ScriptVM::status_summary() const {
  if (in_flowchart_) {
    return fcf_stem_ + " #" + std::to_string(fcf_node_id_);
  }
  return cur_script_stem_;
}

std::string ScriptVM::resolve_image(const std::string& raw_name) const {
  std::string name = strip_ext(raw_name);
  if (name.empty()) return "";

  const ContentMode cm = config_ ? config_->content_mode : ContentMode::AllAges;
  auto lookup = [&](const char* dict_name) -> std::string {
    if (manifest_.contains(dict_name) && manifest_[dict_name].contains(name)) {
      return data_root_ + "/" + manifest_[dict_name][name]["path"].get<std::string>();
    }
    return "";
  };

  if (cm == ContentMode::HDemosaic) {
    if (auto p = lookup("images_demosaic"); !p.empty()) return p;
    if (auto p = lookup("images_h"); !p.empty()) return p;
    return lookup("images");
  }
  if (cm == ContentMode::HMosaic) {
    if (auto p = lookup("images_h"); !p.empty()) return p;
    return lookup("images");
  }
  if (auto p = lookup("images"); !p.empty()) return p;
  return lookup("images_h");
}

std::string ScriptVM::resolve_rule(const std::string& raw_name) const {
  std::string name = strip_ext(raw_name);
  if (name.empty() || !manifest_.contains("rules")) return "";
  const auto& rules = manifest_["rules"];
  if (rules.contains(name)) {
    return data_root_ + "/" + rules[name]["path"].get<std::string>();
  }
  return "";
}

std::string ScriptVM::resolve_bgm_for_ost(const std::string& raw_name, OstMode ost,
                                            int64_t* loop_start, int64_t* loop_len) const {
  if (loop_start) *loop_start = 0;
  if (loop_len) *loop_len = 0;
  std::string stem = to_lower_ascii(normalize_asset_stem(raw_name));
  if (stem.empty()) return "";

  if (ost == OstMode::Old && manifest_.contains("bgm_old") &&
      manifest_["bgm_old"].contains(stem)) {
    if (loop_start && loop_len && manifest_.contains("bgm_old_loops") &&
        manifest_["bgm_old_loops"].contains(stem)) {
      const auto& arr = manifest_["bgm_old_loops"][stem];
      if (arr.is_array() && arr.size() >= 2) {
        *loop_start = arr[0].get<int64_t>();
        *loop_len = arr[1].get<int64_t>();
      }
    }
    return data_root_ + "/" + manifest_["bgm_old"][stem].get<std::string>();
  }

  if (manifest_.contains("bgm") && manifest_["bgm"].contains(stem)) {
    return data_root_ + "/" + manifest_["bgm"][stem].get<std::string>();
  }
  return "";
}

std::string ScriptVM::resolve_audio(const std::string& cat, const std::string& raw_name,
                                    int64_t* loop_start, int64_t* loop_len) const {
  if (cat == "bgm") {
    return resolve_bgm_for_ost(raw_name, config_ ? config_->ost_mode : OstMode::New,
                               loop_start, loop_len);
  }
  if (loop_start) *loop_start = 0;
  if (loop_len) *loop_len = 0;
  std::string stem = to_lower_ascii(normalize_asset_stem(raw_name));
  if (stem.empty()) return "";

  if (!manifest_.contains(cat)) return "";
  const auto& map = manifest_[cat];
  if (map.contains(stem)) {
    return data_root_ + "/" + map[stem].get<std::string>();
  }
  return "";
}

std::string ScriptVM::resolve_ui(const std::string& alias) const {
  if (!manifest_.contains("ui")) return "";
  const auto& ui = manifest_["ui"];
  if (ui.contains(alias)) {
    return data_root_ + "/" + ui[alias]["path"].get<std::string>();
  }
  return "";
}

void ScriptVM::play_bgm_stem(const std::string& stem, int fade_ms) {
  std::string s = to_lower_ascii(normalize_asset_stem(stem));
  if (s.empty()) return;
  if (extras_) {
    extras_->unlock_bgm(s);
  }
  cur_bgm_stem_ = s;
  cur_bgm_playing_ = true;
  int64_t lstart = 0, llen = 0;
  std::string p = resolve_audio("bgm", s, &lstart, &llen);
  if (!p.empty()) {
    audio_->play_bgm(p, fade_ms, true, lstart, llen);
  }
}

float ScriptVM::arg_float(const nlohmann::json& args, const char* key, float def) {
  if (!args.contains(key)) return def;
  const auto& v = args[key];
  if (v.is_number()) return v.get<float>();
  if (v.is_string()) {
    char* end = nullptr;
    float res = std::strtof(v.get_ref<const std::string&>().c_str(), &end);
    return (end != v.get_ref<const std::string&>().c_str()) ? res : def;
  }
  return def;
}

int ScriptVM::arg_int(const nlohmann::json& args, const char* key, int def) {
  return static_cast<int>(arg_float(args, key, static_cast<float>(def)));
}

std::string ScriptVM::arg_str(const nlohmann::json& args, const char* key, const char* def) {
  if (!args.contains(key)) return def;
  const auto& v = args[key];
  return v.is_string() ? v.get<std::string>() : def;
}

bool ScriptVM::arg_bool(const nlohmann::json& args, const char* key, bool def) {
  if (!args.contains(key)) return def;
  const auto& v = args[key];
  if (v.is_boolean()) return v.get<bool>();
  if (v.is_string()) {
    const auto& s = v.get_ref<const std::string&>();
    return (s == "true" || s == "1" || s == "yes");
  }
  return def;
}

float ScriptVM::pos_to_center_x(const std::string& pos) const {
  if (coord_scale_ > 0.7f) {
    // 4:3 frame mapped centered [117 .. 842]
    if (pos == "l" || pos == "left") return 260.0f;
    if (pos == "lc" || pos == "left_center") return 360.0f;
    if (pos == "rc" || pos == "right_center") return 600.0f;
    if (pos == "r" || pos == "right") return 700.0f;
    return 480.0f;
  }
  if (pos == "l" || pos == "left") return 220.0f;
  if (pos == "lc" || pos == "left_center") return 320.0f;
  if (pos == "rc" || pos == "right_center") return 640.0f;
  if (pos == "r" || pos == "right") return 740.0f;
  return 480.0f;
}

void ScriptVM::clear_character_layers() {
  for (auto it = layers_.begin(); it != layers_.end();) {
    if (it->first.rfind("fg_", 0) == 0 || it->first == "chgfg" ||
        it->first.rfind("dash_", 0) == 0) {
      it = layers_.erase(it);
    } else {
      ++it;
    }
  }
}

void ScriptVM::load_fg_slot(const std::string& pos, const std::string& file, int z_idx,
                            bool fliplr, bool flipud, int time_ms) {
  if (file.empty()) return;
  Layer& l = layers_["fg_" + pos];
  l.image_name = file;
  l.image_path = resolve_image(file);
  const Texture* tex = l.image_path.empty() ? nullptr : renderer_->get_texture(l.image_path);
  float w = tex ? static_cast<float>(tex->w) : 230.0f;
  float h = tex ? static_cast<float>(tex->h) : 520.0f;
  float corr = 0.0f;
  auto it = correct_x_.find(strip_ext(file));
  if (it != correct_x_.end()) corr = it->second;

  if (w >= 720.0f && h >= 540.0f) {
    l.x = (960.0f - w) * 0.5f;
    l.y = (544.0f - h) * 0.5f;
  } else {
    l.x = pos_to_center_x(pos) - w * 0.5f + corr;
    l.y = 544.0f - h;
  }
  l.z_index = z_idx;
  l.fliplr = fliplr;
  l.flipud = flipud;
  l.visible = true;
  if (time_ms > 0) {
    l.moving = true;
    l.move_elapsed = 0.0f;
    l.move_duration = time_ms / 1000.0f;
    l.start_x = l.end_x = l.x;
    l.start_y = l.end_y = l.y;
    l.start_a = 0.0f;
    l.end_a = 1.0f;
    l.alpha = 0.0f;
  } else {
    l.alpha = 1.0f;
    l.moving = false;
  }
}

void ScriptVM::start_bg_transition(const std::string& target, const nlohmann::json& args,
                                     bool solid, float sr, float sg, float sb) {
  // Remove transient dash FX on any full scene/fade transition
  for (auto it = layers_.begin(); it != layers_.end();) {
    if (it->first.rfind("dash_", 0) == 0) it = layers_.erase(it);
    else ++it;
  }

  int ms = arg_int(args, "time", 300);
  bg_trans_.active = true;
  bg_trans_.elapsed = 0.0f;
  bg_trans_.duration = std::max(0.01f, ms / 1000.0f);
  bg_trans_.vague = std::max(4.0f, arg_float(args, "vague", 64.0f)) / 255.0f;
  bg_trans_.next_name = target;
  bg_trans_.next_path = solid ? "" : resolve_image(target);
  bg_trans_.rule_path = resolve_rule(arg_str(args, "rule"));
  bg_trans_.next_fliplr = arg_bool(args, "fliplr", false);
  bg_trans_.next_flipud = arg_bool(args, "flipud", false);
  bg_trans_.to_solid = solid;
  bg_trans_.solid_r = sr;
  bg_trans_.solid_g = sg;
  bg_trans_.solid_b = sb;
  if (ms <= 0 || (solid == false && bg_trans_.next_path.empty() && !target.empty())) {
    bg_name_ = bg_trans_.next_name;
    bg_path_ = bg_trans_.next_path;
    bg_fliplr_ = bg_trans_.next_fliplr;
    bg_flipud_ = bg_trans_.next_flipud;
    bg_is_solid_ = solid || bg_path_.empty();
    bg_r_ = sr;
    bg_g_ = sg;
    bg_b_ = sb;
    bg_trans_.active = false;
  } else {
    wait_trans_ = true;
  }
}

void ScriptVM::exec_cmd(const std::string& cmd, const nlohmann::json& args) {
  auto get_file_arg = [&]() {
    std::string f = arg_str(args, "file");
    if (f.empty()) f = arg_str(args, "storage");
    return f;
  };

  if (cmd == "pg" || cmd == "p" || cmd == "pgtg") {
    text_->set_wait_prompt(true);
    wait_click_ = true;
    wait_page_ = true;
  } else if (cmd == "l") {
    text_->set_wait_prompt(false);
    wait_click_ = true;
    wait_page_ = false;
  } else if (cmd == "r" || cmd == "rf" || cmd == "lr") {
    text_->add_newline();
  } else if (cmd == "wdash") {
    text_->append_raw_line("\xE2\x80\x94\xE2\x80\x94", interlude_mode_);
  } else if (cmd == "cm" || cmd == "pgnl") {
    text_->clear_page();
  } else if (cmd == "texton") {
    text_visible_ = true;
  } else if (cmd == "textoff" || cmd == "textwindowoff") {
    text_visible_ = false;
  } else if (cmd == "interlude_start" || cmd == "interlude_in" || cmd == "interlude_in_") {
    interlude_mode_ = true;
  } else if (cmd == "interlude_end" || cmd == "interlude_out" || cmd == "interlude_out_") {
    interlude_mode_ = false;
  } else if (cmd == "wait" || cmd == "waitt") {
    wait_timer_ = std::max(0.0f, arg_float(args, "time", 0.0f) / 1000.0f);
    wait_timer_skippable_ = arg_bool(args, "canskip", true);
  } else if (cmd == "wq" || cmd == "wshock") {
    if (quake_timer_ > 0.0f) {
      wait_timer_ = quake_timer_;
      wait_timer_skippable_ = true;
    }
  } else if (cmd == "wm") {
    for (const auto& kv : layers_) {
      if (kv.second.moving) {
        wait_move_ = true;
        break;
      }
    }
  } else if (cmd == "wt" || cmd == "transex_w") {
    if (bg_trans_.active) wait_trans_ = true;
  } else if (cmd == "play" || cmd == "play_" || cmd == "xchgbgm") {
    play_bgm_stem(get_file_arg(), arg_int(args, "time", 0));
  } else if (cmd == "playstop" || cmd == "playstop_") {
    cur_bgm_playing_ = false;
    cur_bgm_stem_.clear();
    audio_->stop_bgm(arg_int(args, "time", 500));
  } else if (cmd == "fadebgm") {
    audio_->fade_bgm(arg_int(args, "volume", 100), arg_int(args, "time", 500));
  } else if (cmd == "se" || cmd == "se_") {
    std::string f = get_file_arg();
    std::string p = resolve_audio("se", f);
    float vol = arg_float(args, "volume", 100.0f) / 100.0f;
    if (!p.empty()) audio_->play_se(normalize_asset_stem(f), p, false, 0, vol);
  } else if (cmd == "seloop" || cmd == "seloop_") {
    std::string f = get_file_arg();
    std::string p = resolve_audio("se", f);
    float vol = arg_float(args, "volume", 100.0f) / 100.0f;
    if (!p.empty()) audio_->play_se(normalize_asset_stem(f), p, true, arg_int(args, "time", 0), vol);
  } else if (cmd == "sestop" || cmd == "sestop_") {
    audio_->stop_se(normalize_asset_stem(get_file_arg()), arg_int(args, "time", 0));
  } else if (cmd == "fadese") {
    if (arg_int(args, "volume", 0) == 0) {
      audio_->stop_se(normalize_asset_stem(get_file_arg()), arg_int(args, "time", 500));
    }
  } else if (cmd == "say") {
    std::string p = resolve_audio("voice", arg_str(args, "storage"));
    if (!p.empty()) audio_->play_voice(p);
  } else if (cmd == "fadein" || cmd == "fadein4demo" || cmd == "sys_crossfadein" ||
             cmd == "a2a" || cmd == "a2at" || cmd == "a2a_fast" || cmd == "a2a_fastt" ||
             cmd == "i2i" || cmd == "i2it" || cmd == "i2i_fast" || cmd == "i2i_fastt" ||
             cmd == "i2o" || cmd == "i2ot" || cmd == "i2o_fast" || cmd == "i2o_fastt" ||
             cmd == "i2o_curtain") {
    if (cmd[0] == 'a' || cmd[0] == 'i') clear_character_layers();
    std::string f = get_file_arg();
    if (f == "black") start_bg_transition("", args, true, 0.0f, 0.0f, 0.0f);
    else if (f == "white") start_bg_transition("", args, true, 1.0f, 1.0f, 1.0f);
    else start_bg_transition(f, args, false);
  } else if (cmd == "bg") {
    std::string f = get_file_arg();
    if (f == "black") start_bg_transition("", args, true, 0.0f, 0.0f, 0.0f);
    else if (f == "white") start_bg_transition("", args, true, 1.0f, 1.0f, 1.0f);
    else start_bg_transition(f, args, false);
  } else if (cmd == "transex" || cmd == "trans") {
    std::string f = get_file_arg();
    if (!f.empty()) {
      start_bg_transition(f, args, false);
    } else if (bg_trans_.active) {
      wait_trans_ = (arg_int(args, "time", 300) > 0);
    }
  } else if (cmd == "stoptrans") {
    bg_trans_.active = false;
    wait_trans_ = false;
  } else if (cmd == "blackout" || cmd == "black") {
    clear_character_layers();
    start_bg_transition("", args, true, 0.0f, 0.0f, 0.0f);
  } else if (cmd == "flushover" || cmd == "white" || cmd == "simpleflush") {
    start_bg_transition("", args, true, 1.0f, 1.0f, 1.0f);
  } else if (cmd == "rep") {
    std::string bgf = arg_str(args, "bg");
    if (bgf.empty()) bgf = get_file_arg();
    if (!bgf.empty()) {
      if (bgf == "black") start_bg_transition("", args, true, 0.0f, 0.0f, 0.0f);
      else if (bgf == "white") start_bg_transition("", args, true, 1.0f, 1.0f, 1.0f);
      else start_bg_transition(bgf, args, false);
    }
    clear_character_layers();
    static const char* kSlots[] = {"l", "lc", "c", "rc", "r"};
    for (int i = 0; i < 5; ++i) {
      std::string sf = arg_str(args, kSlots[i]);
      if (!sf.empty()) {
        std::string ik = std::string("i") + kSlots[i];
        load_fg_slot(kSlots[i], sf, arg_int(args, ik.c_str(), 5000 + i),
                     arg_bool(args, "fliplr", false), arg_bool(args, "flipud", false),
                     arg_int(args, "time", 300));
      }
    }
  } else if (cmd == "ld" || cmd == "ld_auto" || cmd == "ld_notrans") {
    int ms = (cmd == "ld_notrans") ? 0 : arg_int(args, "time", 300);
    load_fg_slot(arg_str(args, "pos", "c"), get_file_arg(), arg_int(args, "index", 5000),
                 arg_bool(args, "fliplr", false), arg_bool(args, "flipud", false), ms);
  } else if (cmd == "chgfg") {
    std::string file = get_file_arg();
    if (!file.empty()) {
      if (args.contains("left") || args.contains("top")) {
        Layer& l = layers_["chgfg"];
        l.image_name = file;
        l.image_path = resolve_image(file);
        l.x = to_vita_x(arg_float(args, "left", 0.0f));
        l.y = to_vita_y(arg_float(args, "top", 0.0f));
        l.z_index = arg_int(args, "index", 5000);
        l.fliplr = arg_bool(args, "fliplr", false);
        l.flipud = arg_bool(args, "flipud", false);
        l.visible = true;
        int ms = arg_int(args, "time", 300);
        l.moving = ms > 0;
        l.move_elapsed = 0.0f;
        l.move_duration = ms / 1000.0f;
        l.start_x = l.end_x = l.x;
        l.start_y = l.end_y = l.y;
        l.start_a = ms > 0 ? 0.0f : 1.0f;
        l.end_a = 1.0f;
        l.alpha = l.start_a;
      } else {
        load_fg_slot(arg_str(args, "pos", "c"), file, arg_int(args, "index", 5000),
                     arg_bool(args, "fliplr", false), arg_bool(args, "flipud", false),
                     arg_int(args, "time", 300));
      }
    }
  } else if (cmd == "ldall" || cmd == "ldallt") {
    clear_character_layers();
    static const char* kSlots[] = {"l", "lc", "c", "rc", "r"};
    int ms = arg_int(args, "time", 300);
    for (int i = 0; i < 5; ++i) {
      std::string sf = arg_str(args, kSlots[i]);
      if (!sf.empty()) {
        std::string ik = std::string("i") + kSlots[i];
        load_fg_slot(kSlots[i], sf, arg_int(args, ik.c_str(), 5000 + i),
                     false, false, ms);
      }
    }
  } else if (cmd == "cl" || cmd == "cl_auto" || cmd == "cl_notrans") {
    std::string pos = arg_str(args, "pos", "all");
    if (pos == "all") {
      clear_character_layers();
    } else {
      layers_.erase("fg_" + pos);
    }
  } else if (cmd == "clfg") {
    layers_.clear();
  } else if (cmd == "superpose") {
    std::string st = get_file_arg();
    if (!st.empty()) {
      Layer& l = layers_["superpose"];
      l.image_name = st;
      l.image_path = resolve_image(st);
      l.x = 0.0f;
      l.y = 0.0f;
      l.alpha = arg_float(args, "opacity", 128.0f) / 255.0f;
      l.z_index = 6000;
      l.blend = BlendMode::Alpha;
      l.visible = true;
    }
  } else if (cmd == "superpose_off") {
    layers_.erase("superpose");
  } else if (cmd == "dash" || cmd == "dashcombo" || cmd == "dashcombot" ||
             cmd == "splinemovecombo" || cmd == "splinemovecombot") {
    std::string st = get_file_arg();
    if (!st.empty()) {
      std::string layer_name = arg_str(args, "layer", "0");
      Layer& l = layers_["dash_" + layer_name];
      l.image_name = st;
      l.image_path = resolve_image(st);
      l.x = 0.0f;
      l.y = 0.0f;
      l.alpha = arg_float(args, "opacity", 255.0f) / 255.0f;
      l.z_index = (layer_name == "base") ? 100 : 4500;
      l.blend = (layer_name == "base") ? BlendMode::Alpha : BlendMode::Additive;
      l.visible = true;
    }
  } else if (cmd == "stopdash") {
    for (auto it = layers_.begin(); it != layers_.end();) {
      if (it->first.rfind("dash_", 0) == 0) it = layers_.erase(it);
      else ++it;
    }
  } else if (cmd == "fg" || cmd == "image" || cmd == "imageex" || cmd == "image4demo") {
    std::string st = get_file_arg();
    if (arg_str(args, "layer") == "base" && st != "black" && st != "white") {
      bg_name_ = st;
      bg_path_ = resolve_image(st);
      bg_is_solid_ = bg_path_.empty();
    } else if (!st.empty()) {
      std::string lid = args.contains("layer") ? ("L_" + arg_str(args, "layer")) : ("S_" + st);
      Layer& l = layers_[lid];
      l.image_name = st;
      l.image_path = resolve_image(st);
      l.x = args.contains("left") ? to_vita_x(arg_float(args, "left", 0.0f)) : 0.0f;
      l.y = args.contains("top") ? to_vita_y(arg_float(args, "top", 0.0f)) : 0.0f;
      l.alpha = arg_float(args, "opacity", 255.0f) / 255.0f;
      l.z_index = arg_int(args, "index", 2000);
      l.fliplr = arg_bool(args, "fliplr", false);
      l.flipud = arg_bool(args, "flipud", false);
      l.visible = arg_bool(args, "visible", true);
      std::string mode = arg_str(args, "mode");
      l.blend = (mode == "add" || mode == "screen") ? BlendMode::Additive : BlendMode::Alpha;
    }
  } else if (cmd == "move") {
    std::string lid = "L_" + arg_str(args, "layer", "0");
    auto it = layers_.find(lid);
    if (it != layers_.end()) {
      Layer& l = it->second;
      std::string path = arg_str(args, "path");
      float tx = l.x, ty = l.y, ta = l.alpha * 255.0f;
      if (std::sscanf(path.c_str(), "(%f,%f,%f)", &tx, &ty, &ta) >= 2) {
        l.moving = true;
        l.move_elapsed = 0.0f;
        l.move_duration = std::max(0.01f, arg_float(args, "time", 300.0f) / 1000.0f);
        l.start_x = l.x;
        l.start_y = l.y;
        l.start_a = l.alpha;
        l.end_x = to_vita_x(tx);
        l.end_y = to_vita_y(ty);
        l.end_a = ta / 255.0f;
        l.accel = arg_float(args, "accel", 0.0f);
      }
    }
  } else if (cmd == "movefg") {
    std::string lid = "S_" + arg_str(args, "storage");
    auto it = layers_.find(lid);
    if (it != layers_.end()) {
      Layer& l = it->second;
      l.moving = true;
      l.move_elapsed = 0.0f;
      l.move_duration = std::max(0.01f, arg_float(args, "time", 300.0f) / 1000.0f);
      l.start_x = l.x;
      l.start_y = l.y;
      l.start_a = l.alpha;
      l.end_x = args.contains("left") ? to_vita_x(arg_float(args, "left", 0.0f)) : l.x;
      l.end_y = args.contains("top") ? to_vita_y(arg_float(args, "top", 0.0f)) : l.y;
      l.end_a = arg_float(args, "opacity", 255.0f) / 255.0f;
      l.accel = arg_float(args, "accel", 0.0f);
    }
  } else if (cmd == "stopmove") {
    for (auto& kv : layers_) kv.second.moving = false;
  } else if (cmd == "quake" || cmd == "quaket" || cmd == "lquake" ||
             cmd == "shock" || cmd == "shockt") {
    quake_timer_ = std::max(0.15f, arg_float(args, "time", 400.0f) / 1000.0f);
    quake_hmax_ = arg_float(args, "hmax", 24.0f) * 0.5f;
    quake_vmax_ = arg_float(args, "vmax", 16.0f) * 0.5f;
  } else if (cmd == "stopquake" || cmd == "stoplquake") {
    quake_timer_ = 0.0f;
  } else if (cmd == "monocro" || cmd == "monocrot") {
    tint_r_ = 0.85f; tint_g_ = 0.85f; tint_b_ = 0.85f;
  } else if (cmd == "sepia" || cmd == "sepiat") {
    tint_r_ = 1.0f; tint_g_ = 0.88f; tint_b_ = 0.72f;
  } else if (cmd == "nega" || cmd == "negat") {
    tint_r_ = 0.7f; tint_g_ = 0.75f; tint_b_ = 0.9f;
  } else if (cmd == "darken" || cmd == "darkent") {
    darken_alpha_ = 0.45f;
  } else if (cmd == "darkenoff" || cmd == "darkenofft" ||
             cmd == "condoff" || cmd == "condofft") {
    tint_r_ = tint_g_ = tint_b_ = 1.0f;
    darken_alpha_ = 0.0f;
  } else if (cmd == "staffrollinit") {
    credits_lines_.clear();
    credits_scroll_ = 0.0f;
    credits_active_ = true;
  } else if (cmd == "staffrolltext") {
    std::string txt = arg_str(args, "text");
    if (!txt.empty() && static_cast<unsigned char>(txt[0]) < 0xE3) {
      credits_lines_.push_back(txt);
    }
  } else if (cmd == "staffrollstart") {
    credits_active_ = true;
  } else if (cmd == "wstaffroll") {
    wait_timer_ = 2.0f;
    wait_timer_skippable_ = true;
  } else if (cmd == "staffrolluninit" || cmd == "erasestaffroll") {
    credits_active_ = false;
    credits_lines_.clear();
  } else if (cmd == "tiger_end") {
    if (!choice_backup_path_.empty()) {
      choices_.clear();
      choices_.push_back({1, "Retry Previous Choice"});
      choices_.push_back({0, "Continue / Return to Title"});
      choice_prompt_ = "BAD END - Retry from your last choice?";
      choice_selected_ = 0;
      choice_active_ = true;
      tiger_retry_mode_ = true;
    }
  } else if (cmd == "call") {
    std::string st = strip_ext(arg_str(args, "storage"));
    std::string trg = arg_str(args, "target");
    if (!trg.empty() && trg[0] == '*') trg = trg.substr(1);
    if (st == "StaffrollPlugin" || st == "petal") return;

    CallFrame frame;
    frame.script_stem = cur_script_stem_;
    frame.ip = ip_;
    frame.script = script_;
    frame.coord_scale = coord_scale_;
    frame.coord_ox = coord_ox_;
    frame.coord_oy = coord_oy_;
    call_stack_.push_back(std::move(frame));

    if (!st.empty()) {
      if (!load_script_by_stem(st, trg, true)) {
        call_stack_.pop_back();
      }
    } else if (!trg.empty() && script_.contains("labels") && script_["labels"].contains(trg)) {
      ip_ = script_["labels"][trg].get<size_t>();
    }
  } else if (cmd == "jump") {
    std::string st = strip_ext(arg_str(args, "storage"));
    std::string trg = arg_str(args, "target");
    if (!trg.empty() && trg[0] == '*') trg = trg.substr(1);
    if (st == "\xE3\x82\xBF\xE3\x82\xA4\xE3\x83\x88\xE3\x83\xAB" || st == "title") {
      finished_ = true;
      return;
    }
    if (!st.empty()) {
      load_script_by_stem(st, trg, true);
    } else if (!trg.empty() && script_.contains("labels") && script_["labels"].contains(trg)) {
      ip_ = script_["labels"][trg].get<size_t>();
    }
  } else if (cmd == "return") {
    on_script_end();
  }
}

void ScriptVM::step_until_blocked() {
  while (!finished_ && !choice_active_ && !wait_click_ &&
         wait_timer_ <= 0.0f && !wait_move_ && !wait_trans_) {
    if (!script_.contains("ops") || !script_["ops"].is_array()) {
      on_script_end();
      break;
    }
    const auto& ops = script_["ops"];
    if (ip_ >= ops.size()) {
      on_script_end();
      continue;
    }
    const auto op = ops[ip_++];
    const std::string kind = op.value("op", "");
    if (kind == "text") {
      text_->append_raw_line(op.value("raw", ""), interlude_mode_);
    } else if (kind == "cmd") {
      exec_cmd(op.value("cmd", ""), op.value("args", nlohmann::json::object()));
    }
  }
}

void ScriptVM::update(float dt_sec, bool btn_up, bool btn_down, bool btn_advance,
                      bool btn_skip_held, bool btn_toggle_text,
                      bool touch_tap, float touch_x, float touch_y) {
  anim_time_ += dt_sec;

  if (credits_active_ && !credits_lines_.empty()) {
    credits_scroll_ += dt_sec * 42.0f;
  }

  if (choice_active_) {
    if (btn_up && !choices_.empty()) {
      choice_selected_ = (choice_selected_ + static_cast<int>(choices_.size()) - 1) %
                         static_cast<int>(choices_.size());
      audio_->play_se("ui_move", data_root_ + "/audio/se/se_title_move.ogg");
    }
    if (btn_down && !choices_.empty()) {
      choice_selected_ = (choice_selected_ + 1) % static_cast<int>(choices_.size());
      audio_->play_se("ui_move", data_root_ + "/audio/se/se_title_move.ogg");
    }
    if (touch_tap && !choices_.empty()) {
      const float start_y = 160.0f;
      for (size_t i = 0; i < choices_.size(); ++i) {
        float cy = start_y + static_cast<float>(i) * 62.0f;
        if (touch_x >= 100.0f && touch_x <= 860.0f && touch_y >= cy && touch_y <= cy + 48.0f) {
          choice_selected_ = static_cast<int>(i);
          btn_advance = true;
          break;
        }
      }
    }
    if (btn_advance && !choices_.empty()) {
      int chosen_target = choices_[choice_selected_].target;
      bool was_tiger = tiger_retry_mode_;
      audio_->play_se("ui_decide", data_root_ + "/audio/se/se_title_decide.ogg");
      choice_active_ = false;
      tiger_retry_mode_ = false;
      text_->clear_page();

      if (was_tiger) {
        if (chosen_target == 1 && !choice_backup_path_.empty()) {
          load_quicksave(choice_backup_path_);
        } else {
          finished_ = true;
        }
        return;
      }
      enter_fcf_node(chosen_target);
    }
    return;
  }

  if (btn_toggle_text) {
    user_hid_text_ = !user_hid_text_;
  }
  if (backlog_open_) return;

  if (quake_timer_ > 0.0f) {
    quake_timer_ = std::max(0.0f, quake_timer_ - dt_sec);
  }

  // Advance background transition
  if (bg_trans_.active) {
    bg_trans_.elapsed += dt_sec * (btn_skip_held ? 4.0f : 1.0f);
    if (bg_trans_.elapsed >= bg_trans_.duration) {
      bg_name_ = bg_trans_.next_name;
      bg_path_ = bg_trans_.next_path;
      bg_fliplr_ = bg_trans_.next_fliplr;
      bg_flipud_ = bg_trans_.next_flipud;
      bg_is_solid_ = bg_trans_.to_solid || bg_path_.empty();
      bg_r_ = bg_trans_.solid_r;
      bg_g_ = bg_trans_.solid_g;
      bg_b_ = bg_trans_.solid_b;
      bg_trans_.active = false;
      wait_trans_ = false;
    }
  }

  // Advance layer tweens
  bool any_moving = false;
  for (auto& kv : layers_) {
    Layer& l = kv.second;
    if (!l.moving) continue;
    l.move_elapsed += dt_sec * (btn_skip_held ? 4.0f : 1.0f);
    float t = apply_accel(l.move_elapsed / std::max(0.01f, l.move_duration), l.accel);
    l.x = l.start_x + (l.end_x - l.start_x) * t;
    l.y = l.start_y + (l.end_y - l.start_y) * t;
    l.alpha = l.start_a + (l.end_a - l.start_a) * t;
    if (l.move_elapsed >= l.move_duration) {
      l.x = l.end_x;
      l.y = l.end_y;
      l.alpha = l.end_a;
      l.moving = false;
    } else {
      any_moving = true;
    }
  }
  if (!any_moving) wait_move_ = false;

  text_->update(dt_sec * (btn_skip_held ? 8.0f : 1.0f));

  if (wait_timer_ > 0.0f) {
    if ((btn_advance && wait_timer_skippable_) || btn_skip_held) {
      wait_timer_ = 0.0f;
    } else {
      wait_timer_ = std::max(0.0f, wait_timer_ - dt_sec);
    }
  }

  if (wait_click_) {
    if (btn_skip_held) {
      text_->reveal_all();
      wait_click_ = false;
      if (wait_page_) {
        text_->clear_page();
        wait_page_ = false;
      }
    } else if (btn_advance) {
      if (text_->is_revealing()) {
        text_->reveal_all();
      } else {
        wait_click_ = false;
        if (wait_page_) {
          text_->clear_page();
          wait_page_ = false;
        }
      }
    }
  }

  if (!wait_click_ && wait_timer_ <= 0.0f && !wait_move_ && !wait_trans_) {
    step_until_blocked();
  }
}

void ScriptVM::draw_layer(const Layer& l, float qx, float qy) {
  if (!l.visible || l.image_path.empty() || l.alpha <= 0.001f) return;
  const Texture* tex = renderer_->get_texture(l.image_path);
  if (!tex) return;
  float dx = l.x + qx;
  float dy = l.y + qy;
  if (l.x == 0.0f && l.y == 0.0f && tex->w >= 720 && tex->h >= 540) {
    dx = qx + (960.0f - tex->w) * 0.5f;
    dy = qy + (544.0f - tex->h) * 0.5f;
  }
  renderer_->draw_texture(tex, dx, dy, static_cast<float>(tex->w), static_cast<float>(tex->h),
                          l.alpha, l.fliplr, l.flipud, l.blend, tint_r_, tint_g_, tint_b_);
}

void ScriptVM::scroll_backlog(int delta) {
  int max_scroll = std::max(0, static_cast<int>(text_->history().size()) - 10);
  backlog_scroll_ = std::max(0, std::min(max_scroll, backlog_scroll_ + delta));
}

void ScriptVM::draw() {
  float qx = 0.0f, qy = 0.0f;
  if (quake_timer_ > 0.0f) {
    qx = std::sin(anim_time_ * 73.0f) * quake_hmax_;
    qy = std::cos(anim_time_ * 61.0f) * quake_vmax_;
  }

  // Ensure crisp black pillarboxes behind 4:3 H-CGs
  renderer_->draw_rect(0.0f, 0.0f, 960.0f, 544.0f, 0.0f, 0.0f, 0.0f, 1.0f);

  // 1. Current background
  if (bg_is_solid_) {
    renderer_->draw_rect(0.0f, 0.0f, 960.0f, 544.0f, bg_r_, bg_g_, bg_b_, 1.0f);
  } else if (!bg_path_.empty()) {
    const Texture* tex = renderer_->get_texture(bg_path_);
    if (tex) {
      float dx = qx + (960.0f - tex->w) * 0.5f;
      float dy = qy + (544.0f - tex->h) * 0.5f;
      renderer_->draw_texture(tex, dx, dy, static_cast<float>(tex->w), static_cast<float>(tex->h),
                              1.0f, bg_fliplr_, bg_flipud_, BlendMode::Alpha,
                              tint_r_, tint_g_, tint_b_);
    }
  }

  // 2. Active background transition
  if (bg_trans_.active) {
    float p = std::max(0.0f, std::min(1.0f, bg_trans_.elapsed / bg_trans_.duration));
    if (bg_trans_.to_solid) {
      renderer_->draw_rect(0.0f, 0.0f, 960.0f, 544.0f,
                           bg_trans_.solid_r, bg_trans_.solid_g, bg_trans_.solid_b, p);
    } else if (!bg_trans_.next_path.empty()) {
      const Texture* next_tex = renderer_->get_texture(bg_trans_.next_path);
      const Texture* rule_tex = bg_trans_.rule_path.empty()
                                    ? nullptr
                                    : renderer_->get_texture(bg_trans_.rule_path);
      if (next_tex) {
        float dx = qx + (960.0f - next_tex->w) * 0.5f;
        float dy = qy + (544.0f - next_tex->h) * 0.5f;
        if (rule_tex) {
          renderer_->draw_rule_wipe(next_tex, rule_tex, dx, dy,
                                    static_cast<float>(next_tex->w),
                                    static_cast<float>(next_tex->h),
                                    p, bg_trans_.vague, bg_trans_.next_fliplr,
                                    bg_trans_.next_flipud);
        } else {
          renderer_->draw_texture(next_tex, dx, dy, static_cast<float>(next_tex->w),
                                  static_cast<float>(next_tex->h), p,
                                  bg_trans_.next_fliplr, bg_trans_.next_flipud);
        }
      }
    }
  }

  // 3. Character & overlay layers sorted by z_index
  std::vector<const Layer*> sorted;
  sorted.reserve(layers_.size());
  for (const auto& kv : layers_) {
    if (kv.second.visible) sorted.push_back(&kv.second);
  }
  std::sort(sorted.begin(), sorted.end(),
            [](const Layer* a, const Layer* b) { return a->z_index < b->z_index; });
  for (const Layer* l : sorted) {
    draw_layer(*l, qx, qy);
  }

  if (darken_alpha_ > 0.001f) {
    renderer_->draw_rect(0.0f, 0.0f, 960.0f, 544.0f, 0.0f, 0.0f, 0.0f, darken_alpha_);
  }

  // Staffroll / credits overlay when running
  if (credits_active_ && !credits_lines_.empty()) {
    renderer_->draw_rect(0.0f, 0.0f, 960.0f, 544.0f, 1.0f, 1.0f, 1.0f, 0.85f);
    float base_y = 520.0f - credits_scroll_;
    for (size_t i = 0; i < credits_lines_.size(); ++i) {
      float cy = base_y + static_cast<float>(i) * 32.0f;
      if (cy >= -32.0f && cy <= 550.0f) {
        text_->draw_simple(credits_lines_[i], 260.0f, cy, 0.90f, 0.08f, 0.08f, 0.12f, 1.0f);
      }
    }
  }

  // 4. Text window + dialogue text
  const bool show_text =
      text_visible_ && !user_hid_text_ && !backlog_open_ && !choice_active_ && !credits_active_;
  if (show_text) {
    std::string tw_path = resolve_ui(interlude_mode_ ? "tw_interlude_us" : "tw_normal_us");
    if (!tw_path.empty()) {
      const Texture* tw = renderer_->get_texture(tw_path);
      if (tw) {
        renderer_->draw_texture(tw, 0.0f, 2.0f, static_cast<float>(tw->w),
                                static_cast<float>(tw->h), 0.92f);
      }
    } else {
      renderer_->draw_rect(24.0f, 20.0f, 912.0f, 504.0f, 0.04f, 0.05f, 0.09f, 0.72f);
    }

    const Texture* pb = renderer_->get_texture(resolve_ui("pagebreak_a"));
    const Texture* lb = renderer_->get_texture(resolve_ui("linebreak_a"));
    text_->draw(true, interlude_mode_, pb, lb, anim_time_);
  }

  // 5. Interactive Choice / Tiger Retry modal
  if (choice_active_) {
    renderer_->draw_rect(0.0f, 0.0f, 960.0f, 544.0f, 0.02f, 0.03f, 0.08f, 0.72f);
    text_->draw_simple(choice_prompt_, 100.0f, 88.0f, 0.90f, 0.85f, 0.92f, 1.0f, 1.0f);

    float start_y = 160.0f;
    for (size_t i = 0; i < choices_.size(); ++i) {
      float cy = start_y + static_cast<float>(i) * 62.0f;
      bool sel = (static_cast<int>(i) == choice_selected_);
      renderer_->draw_rect(100.0f, cy, 760.0f, 48.0f,
                           sel ? 0.22f : 0.08f,
                           sel ? 0.45f : 0.12f,
                           sel ? 0.85f : 0.22f,
                           sel ? 0.88f : 0.75f);
      renderer_->draw_rect(100.0f, cy, 4.0f, 48.0f, 0.50f, 0.78f, 1.0f, sel ? 1.0f : 0.3f);
      text_->draw_simple(choices_[i].text, 122.0f, cy + 12.0f, 0.90f,
                         1.0f, sel ? 0.98f : 0.85f, sel ? 0.75f : 0.92f, 1.0f);
    }
    text_->draw_simple("Up/Down: Choose  |  Confirm: Select",
                       100.0f, 496.0f, 0.75f, 0.78f, 0.86f, 0.95f, 0.90f);
  }

  // 6. Backlog modal
  if (backlog_open_) {
    renderer_->draw_rect(0.0f, 0.0f, 960.0f, 544.0f, 0.02f, 0.03f, 0.07f, 0.90f);
    text_->draw_simple("BACKLOG (Up/Down: Scroll | Back/Triangle: Close)",
                       40.0f, 24.0f, 0.95f, 0.75f, 0.85f, 1.0f, 1.0f);
    const auto& hist = text_->history();
    int total = static_cast<int>(hist.size());
    int start = std::max(0, total - 11 - backlog_scroll_);
    int end = std::min(total, start + 11);
    float y = 70.0f;
    for (int i = start; i < end; ++i) {
      text_->draw_simple(hist[i], 44.0f, y, 0.85f, 0.95f, 0.95f, 0.95f, 1.0f);
      y += 40.0f;
    }
  }
}

}  // namespace fsn
