// Exercises eternalsonata_cutscene_api.h: shows the running cutscene's state,
// skips it on demand, optionally auto-skips every skippable one, takes over
// its camera, and logs the started/ended bus events.
#include <rex/system/mod_plugin.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include <imgui.h>

#include <rex/runtime.h>
#include <rex/system/mod_registry.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

#include <rexmod/eternalsonata_cutscene_api.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

void* HostSymbol(const char* symbol) {
#if defined(_WIN32)
  return reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA(nullptr), symbol));
#else
  return dlsym(RTLD_DEFAULT, symbol);
#endif
}

template <typename Fn>
void Resolve(Fn& out, const char* symbol) {
  out = reinterpret_cast<Fn>(HostSymbol(symbol));
}

struct CutsceneApi {
  EternalSonataCutsceneAbiVersionFn version = nullptr;
  EternalSonataIsCutsceneActiveFn is_active = nullptr;
  EternalSonataGetCutsceneInfoFn get_info = nullptr;
  EternalSonataSkipCutsceneFn skip = nullptr;
  EternalSonataGetCutsceneCameraFn get_camera = nullptr;
  EternalSonataSetCutsceneCameraControlFn camera_control = nullptr;
  EternalSonataSetCutsceneCameraFn set_camera = nullptr;

  CutsceneApi() {
    Resolve(version, "EternalSonataCutsceneAbiVersion");
    Resolve(is_active, "EternalSonataIsCutsceneActive");
    Resolve(get_info, "EternalSonataGetCutsceneInfo");
    Resolve(skip, "EternalSonataSkipCutscene");
    Resolve(get_camera, "EternalSonataGetCutsceneCamera");
    Resolve(camera_control, "EternalSonataSetCutsceneCameraControl");
    Resolve(set_camera, "EternalSonataSetCutsceneCamera");
  }

  bool ready() const { return get_info && skip; }
  bool camera_ready() const {
    return get_camera && camera_control && set_camera;
  }
};

const char* SkipModeName(int mode) {
  switch (mode) {
    case ETERNALSONATA_CUTSCENE_SKIP_NONE: return "not skippable";
    case ETERNALSONATA_CUTSCENE_SKIP_PAUSE: return "pause, then A";
    case ETERNALSONATA_CUTSCENE_SKIP_ANY_BUTTON: return "any button";
    default: return "unknown";
  }
}

class CutsceneDialog : public rex::ui::ImGuiDialog {
 public:
  CutsceneDialog(rex::ui::ImGuiDrawer* drawer, rex::Runtime* runtime)
      : ImGuiDialog(drawer) {
    rex::ui::RegisterBind(
        "bind_cutscene_probe", "F8", "Toggle cutscene probe",
        [this] { visible_ = !visible_; }, [this] { return visible_; },
        "Cutscene Probe##rex");
    auto* registry = runtime ? runtime->mod_registry() : nullptr;
    if (!registry) {
      return;
    }
    registry->Subscribe(ETERNALSONATA_CUTSCENE_EVENT_STARTED,
                        [this](const auto& payload) { OnEvent("started", payload); });
    registry->Subscribe(ETERNALSONATA_CUTSCENE_EVENT_ENDED,
                        [this](const auto& payload) { OnEvent("ended", payload); });
  }

  ~CutsceneDialog() override {
    ReleaseCamera();
    rex::ui::UnregisterBind("bind_cutscene_probe");
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!visible_) {
      ReleaseCamera();
      return;
    }
    ImGui::SetNextWindowSize(ImVec2(440, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Cutscene Probe##rex", &visible_, ImGuiWindowFlags_NoCollapse)) {
      ImGui::End();
      return;
    }
    if (!api_.ready()) {
      ImGui::TextUnformatted("Cutscene API unavailable in this game build.");
      ImGui::End();
      return;
    }
    ImGui::Text("Host ABI version: %u", api_.version ? api_.version() : 0u);

    EternalSonataCutsceneInfo info{};
    const int result = api_.get_info(&info);
    if (result != ETERNALSONATA_CUTSCENE_OK) {
      ImGui::Text("GetCutsceneInfo failed: %d", result);
    } else if (!info.active) {
      ImGui::TextUnformatted("No cutscene running.");
    } else {
      ImGui::Text("Script %s in %s (handle 0x%08x)", info.script_id, info.area_id,
                  info.handle);
      ImGui::Text("Skip: %s%s%s", SkipModeName(info.skip_mode),
                  info.skipping ? ", skipping" : "", info.paused ? ", paused" : "");
    }

    ImGui::BeginDisabled(!info.active || info.skip_mode == ETERNALSONATA_CUTSCENE_SKIP_NONE);
    if (ImGui::Button("Skip cutscene")) {
      skip_result_ = api_.skip();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Text("result %d", skip_result_);

    ImGui::Checkbox("Auto-skip every skippable cutscene", &auto_skip_);
    if (auto_skip_ && info.active && !info.skipping &&
        info.skip_mode != ETERNALSONATA_CUTSCENE_SKIP_NONE &&
        auto_skipped_handle_ != info.handle) {
      auto_skipped_handle_ = info.handle;
      skip_result_ = api_.skip();
    }

    ImGui::Separator();
    DrawCamera(io, info);

    ImGui::Separator();
    ImGui::TextDisabled("Bus events");
    ImGui::BeginChild("cutscene_probe_log", ImVec2(0, 0), ImGuiChildFlags_Borders);
    {
      std::lock_guard lock(mutex_);
      for (const auto& line : lines_) {
        ImGui::TextUnformatted(line.c_str());
      }
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
      ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
  }

 private:
  // Control is host state that the cutscene's end clears on its own; the
  // checkbox follows the host's answer rather than the other way round, so
  // a scene change never leaves it lying.
  void DrawCamera(ImGuiIO& io, const EternalSonataCutsceneInfo& info) {
    ImGui::TextDisabled("Camera");
    if (!api_.camera_ready()) {
      ImGui::TextUnformatted("Camera API unavailable in this host.");
      return;
    }
    EternalSonataFieldCamera live{};
    const int read = api_.get_camera(&live);
    if (read == ETERNALSONATA_CUTSCENE_OK) {
      ImGui::Text("Live position  %.3f  %.3f  %.3f", live.position.x,
                  live.position.y, live.position.z);
      ImGui::Text("Live rotation  %.3f  %.3f  %.3f rad", live.rotation.x,
                  live.rotation.y, live.rotation.z);
    } else {
      ImGui::Text("No readable camera (%d).", read);
    }
    camera_enabled_ = info.active && info.camera_controlled;

    ImGui::BeginDisabled(!info.active);
    bool want_control = camera_enabled_;
    if (ImGui::Checkbox("Camera control", &want_control)) {
      camera_result_ = api_.camera_control(want_control ? 1 : 0);
      if (want_control && read == ETERNALSONATA_CUTSCENE_OK) {
        target_ = live;
        have_target_ = true;
      }
    }
    ImGui::Text("control %d  set %d", camera_result_, set_result_);
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!camera_enabled_);
    if (ImGui::Button("Capture live") && read == ETERNALSONATA_CUTSCENE_OK) {
      target_ = live;
      have_target_ = true;
    }
    ImGui::SliderFloat("Move speed", &move_speed_, 0.1f, 30.0f, "%.1f units/s");
    ImGui::SliderFloat("Turn speed", &turn_speed_, 0.05f, 3.0f, "%.2f rad/s");
    if (camera_enabled_ && !have_target_ && read == ETERNALSONATA_CUTSCENE_OK) {
      target_ = live;
      have_target_ = true;
    }
    auto hold = [](const char* label) {
      ImGui::Button(label, ImVec2(105, 0));
      return ImGui::IsItemActive();
    };
    const float dt = std::min(io.DeltaTime, 0.05f);
    if (hold("X left")) target_.position.x -= move_speed_ * dt;
    ImGui::SameLine();
    if (hold("X right")) target_.position.x += move_speed_ * dt;
    if (hold("Y down")) target_.position.y -= move_speed_ * dt;
    ImGui::SameLine();
    if (hold("Y up")) target_.position.y += move_speed_ * dt;
    if (hold("Z back")) target_.position.z -= move_speed_ * dt;
    ImGui::SameLine();
    if (hold("Z forward")) target_.position.z += move_speed_ * dt;
    if (hold("Pitch down")) target_.rotation.x -= turn_speed_ * dt;
    ImGui::SameLine();
    if (hold("Pitch up")) target_.rotation.x += turn_speed_ * dt;
    if (hold("Yaw left")) target_.rotation.y -= turn_speed_ * dt;
    ImGui::SameLine();
    if (hold("Yaw right")) target_.rotation.y += turn_speed_ * dt;
    if (camera_enabled_ && have_target_) {
      set_result_ = api_.set_camera(&target_);
      ImGui::Text("Target  %.3f  %.3f  %.3f", target_.position.x,
                  target_.position.y, target_.position.z);
    }
    ImGui::EndDisabled();
    if (!camera_enabled_) {
      have_target_ = false;
    }
  }

  void ReleaseCamera() {
    if (camera_enabled_ && api_.camera_control) {
      api_.camera_control(0);
      camera_enabled_ = false;
    }
    have_target_ = false;
  }

  void Append(const char* line) {
    std::lock_guard lock(mutex_);
    lines_.emplace_back(line);
    if (lines_.size() > 64) {
      lines_.pop_front();
    }
  }

  void OnEvent(const char* edge, const rex::system::ModRegistry::EventPayload& payload) {
    if (payload.bytes.size() != sizeof(EternalSonataCutsceneEvent)) {
      return;
    }
    EternalSonataCutsceneEvent event{};
    std::memcpy(&event, payload.bytes.data(), sizeof(event));
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s  %s in %s  skip mode %d%s", edge, event.script_id,
                  event.area_id, event.skip_mode, event.skipped ? "  (skipped)" : "");
    Append(buf);
  }

  CutsceneApi api_;
  bool visible_ = false;
  bool auto_skip_ = false;
  uint32_t auto_skipped_handle_ = 0;
  int skip_result_ = 0;
  bool camera_enabled_ = false;
  bool have_target_ = false;
  EternalSonataFieldCamera target_{};
  float move_speed_ = 5.0f;
  float turn_speed_ = 1.0f;
  int camera_result_ = 0;
  int set_result_ = 0;
  std::mutex mutex_;
  std::deque<std::string> lines_;
};

class CutsceneProbe : public rex::system::IModPlugin {
 public:
  explicit CutsceneProbe(rex::Runtime* runtime) : runtime_(runtime) {}

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    dialog_ = std::make_unique<CutsceneDialog>(drawer, runtime_);
  }

 private:
  rex::Runtime* runtime_ = nullptr;
  std::unique_ptr<CutsceneDialog> dialog_;
};

}  // namespace

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t rex_mod_abi_version(void) {
  return rex::system::kModPluginAbiVersion;
}

extern "C" REX_MOD_PLUGIN_EXPORT rex::system::IModPlugin* rex_mod_create(
    uint32_t abi_version, const rex::system::ModHostContext* ctx) {
  if (abi_version != rex::system::kModPluginAbiVersion || !ctx) {
    return nullptr;
  }
  return new CutsceneProbe(ctx->runtime);
}
