// area_overlay - an overlay that lists every known field area and
// force-loads the one the player clicks.
//
// There is no reverse engineering and no UI policy left in the game for
// this. The game exports the primitives through the overworld API
// (rexmod/eternalsonata_overworld_api.h, copied verbatim from the game
// repo's src/): enumerating the area table, asking whether a warp can be
// queued right now, and queueing it. The overlay itself - the window, the
// filter box, the "show all maps" toggle and the clickable list - lives in
// this mod, so the game stays a thin API surface and each mod can present
// (or not) the feature however it likes.

#include <rex/system/mod_plugin.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

#include <rexmod/eternalsonata_overworld_api.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

// Every entry point is resolved out of the host executable at load time and
// null-checked before use, so this mod still loads (and simply says so) on a
// host too old to have the API.
struct OverworldApi {
  EternalSonataOverworldAbiVersionFn abi_version = nullptr;
  EternalSonataIsAreaLoadingAvailableFn is_loading_available = nullptr;
  EternalSonataForceLoadAreaFn force_load_area = nullptr;
  EternalSonataGetAreaCountFn area_count = nullptr;
  EternalSonataGetAreaInfoFn area_info = nullptr;
  // Optional: only the origin button needs it.
  EternalSonataSetFieldPositionFn set_field_position = nullptr;

  bool ok() const {
    return abi_version && is_loading_available && force_load_area && area_count && area_info;
  }
};

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

OverworldApi LoadOverworldApi() {
  OverworldApi api;
  Resolve(api.abi_version, "EternalSonataOverworldAbiVersion");
  Resolve(api.is_loading_available, "EternalSonataIsAreaLoadingAvailable");
  Resolve(api.force_load_area, "EternalSonataForceLoadArea");
  Resolve(api.area_count, "EternalSonataGetAreaCount");
  Resolve(api.area_info, "EternalSonataGetAreaInfo");
  Resolve(api.set_field_position, "EternalSonataSetFieldPosition");
  return api;
}

struct Area {
  std::string id;
  std::string name;
};

std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

class DebugAreaOverlayDialog : public rex::ui::ImGuiDialog {
 public:
  explicit DebugAreaOverlayDialog(rex::ui::ImGuiDrawer* drawer)
      : ImGuiDialog(drawer), api_(LoadOverworldApi()) {
    rex::ui::RegisterBind(
        "bind_area_overlay", "F5", "Toggle area overlay",
        [this] { visible_ = !visible_; }, [this] { return visible_; },
        "Area Overlay##rex");
  }

  ~DebugAreaOverlayDialog() override { rex::ui::UnregisterBind("bind_area_overlay"); }

 protected:
  void OnDraw(ImGuiIO& /*io*/) override {
    if (!visible_) {
      return;
    }
    DrawOverlay();
  }

 private:
  void DrawOverlay() {
    if (!api_.ok()) {
      ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
      if (!ImGui::Begin("Area Overlay", &visible_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
      }
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                         "This build of the game has no overworld API.");
      ImGui::End();
      return;
    }

    ImGui::SetNextWindowSize(ImVec2(420, 480), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.9f);
    if (!ImGui::Begin("Area Overlay##rex", &visible_, ImGuiWindowFlags_NoCollapse)) {
      ImGui::End();
      return;
    }

    // Warping mid-battle would tear the field out from under the battle, so
    // the whole window is disabled for the duration. The API gate says when
    // area loading is available, which is exactly that distinction.
    const bool can_load = api_.is_loading_available() != 0;

    ImGui::BeginDisabled(!can_load);
    if (!can_load) {
      ImGui::TextUnformatted("Area loading is unavailable during a battle.");
    }

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##filter", "Filter by id or name... or paste to load",
                                 filter_buf_, sizeof(filter_buf_),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
      RequestLoad(filter_buf_);
    }

    ImGui::Checkbox("Show all maps", &show_all_maps_);
    if (api_.set_field_position) {
      ImGui::SameLine();
      if (ImGui::Button("Move to origin")) {
        const EternalSonataFieldPosition origin{0.0f, 0.0f, 0.0f};
        api_.set_field_position(&origin);
      }
    }

    ImGui::BeginChild("##arealist", ImVec2(0, 0), ImGuiChildFlags_Borders);
    EnsureAreas();
    const std::string filter = ToLower(filter_buf_);
    for (const auto& area : areas_) {
      // Filter out unnamed areas unless "Show all maps" is enabled.
      if (!show_all_maps_ && area.name.empty()) {
        continue;
      }
      if (!filter.empty() && ToLower(area.id).find(filter) == std::string::npos &&
          ToLower(area.name).find(filter) == std::string::npos) {
        continue;
      }
      const std::string label = !area.name.empty() ? area.id + " - " + area.name : area.id;
      if (ImGui::Selectable(label.c_str())) {
        RequestLoad(area.id);
      }
    }
    ImGui::EndChild();
    ImGui::EndDisabled();

    ImGui::End();
  }

  // The area table is static, so it is enumerated once and reused ever after.
  void EnsureAreas() {
    if (areas_loaded_ || !api_.ok()) {
      return;
    }
    areas_.clear();
    const int count = api_.area_count();
    if (count <= 0) {
      areas_loaded_ = true;
      return;
    }
    areas_.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
      char id[64] = {};
      char name[160] = {};
      if (api_.area_info(i, id, sizeof(id), name, sizeof(name)) == ETERNALSONATA_OVERWORLD_OK) {
        areas_.push_back({id, name});
      }
    }
    areas_loaded_ = true;
  }

  void RequestLoad(const std::string& id) {
    if (id.empty()) {
      return;
    }
    api_.force_load_area(id.c_str());
  }

  const OverworldApi api_;
  bool visible_ = false;
  bool show_all_maps_ = false;
  bool areas_loaded_ = false;
  std::vector<Area> areas_;
  char filter_buf_[64] = {};
};

class DebugAreaOverlayMod : public rex::system::IModPlugin {
 public:
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    dialog_ = std::make_unique<DebugAreaOverlayDialog>(drawer);
  }

 private:
  std::unique_ptr<DebugAreaOverlayDialog> dialog_;
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
  return new DebugAreaOverlayMod();
}