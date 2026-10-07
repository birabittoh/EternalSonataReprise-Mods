// costume_overlay - an overlay (F11) over the characters' costumes: pick the
// one each character wears, and lock or unlock each in the game in progress.
//
// The game changes costumes itself (X on the status page) but only among
// unlocked ones; this picks any of them, vanilla and modded alike, through
// rexmod/eternalsonata_costume_api.h. See the game's docs/costumes.md.

#include <rex/system/mod_plugin.h>

#include <cstdint>
#include <memory>
#include <string>

#include <imgui.h>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

#include <rexmod/eternalsonata_costume_api.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

constexpr char kBindName[] = "bind_costumes";
constexpr char kWindowTitle[] = "Costumes##costume_overlay";

constexpr const char* kCharacters[ETERNALSONATA_COSTUME_CHARACTER_COUNT] = {
    "Allegretto", "Polka", "Beat",     "Frederic", "Viola",     "Salsa",
    "Jazz",       "Falsetto", "Claves", "March",  "Crescendo", "Serenade",
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

struct CostumeApi {
  EternalSonataGetCostumeCountFn count = nullptr;
  EternalSonataGetCostumeLabelFn label = nullptr;
  EternalSonataGetWornCostumeFn worn = nullptr;
  EternalSonataWearCostumeFn wear = nullptr;
  EternalSonataIsCostumeUnlockedFn unlocked = nullptr;
  EternalSonataUnlockCostumeFn unlock = nullptr;
  EternalSonataLockCostumeFn lock = nullptr;
  bool ok() const { return count && label && worn && wear && unlocked && unlock && lock; }
};

CostumeApi LoadCostumeApi() {
  CostumeApi api;
  Resolve(api.count, "EternalSonataGetCostumeCount");
  Resolve(api.label, "EternalSonataGetCostumeLabel");
  Resolve(api.worn, "EternalSonataGetWornCostume");
  Resolve(api.wear, "EternalSonataWearCostume");
  Resolve(api.unlocked, "EternalSonataIsCostumeUnlocked");
  Resolve(api.unlock, "EternalSonataUnlockCostume");
  Resolve(api.lock, "EternalSonataLockCostume");
  return api;
}

class CostumeOverlayDialog : public rex::ui::ImGuiDialog {
 public:
  explicit CostumeOverlayDialog(rex::ui::ImGuiDrawer* drawer)
      : ImGuiDialog(drawer), api_(LoadCostumeApi()) {
    rex::ui::RegisterBind(
        kBindName, "F11", "Toggle costume overlay", [this] { visible_ = !visible_; },
        [this] { return visible_; }, kWindowTitle);
  }

  ~CostumeOverlayDialog() override { rex::ui::UnregisterBind(kBindName); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!visible_) {
      return;
    }
    rex::ui::SetNextWindowFittedSize(io, 460.0f, 260.0f);
    ImGui::SetNextWindowBgAlpha(0.9f);
    if (!ImGui::Begin(kWindowTitle, &visible_, ImGuiWindowFlags_NoCollapse)) {
      ImGui::End();
      return;
    }
    if (!api_.ok()) {
      ImGui::TextWrapped("This game build has no costume API.");
      ImGui::End();
      return;
    }

    if (ImGui::Button("Unlock all")) {
      SetAllLocks(true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Lock all")) {
      SetAllLocks(false);
    }
    ImGui::Separator();

    bool any = false;
    for (int c = 1; c <= ETERNALSONATA_COSTUME_CHARACTER_COUNT; ++c) {
      if (api_.count(c) > 1) {
        any = true;
        DrawCharacter(c);
      }
    }

    if (!any) {
      ImGui::TextWrapped("No costumes installed. Mods add them; see the game's docs/costumes.md.");
    } else {
      ImGui::Spacing();
      ImGui::TextDisabled("The field leader changes at once, outside cutscenes.");
    }
    if (failed_) {
      ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1.0f), "A costume failed to load; see the log.");
    }
    ImGui::End();
  }

 private:
  void DrawCharacter(int c) {
    const int count = api_.count(c);
    ImGui::PushID(c);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(kCharacters[c - 1]);
    ImGui::SameLine(110.0f);
    ImGui::SetNextItemWidth(-1.0f);
    const int worn = api_.worn(c);
    if (ImGui::BeginCombo("##costume", api_.label(c, worn))) {
      for (int i = 0; i < count; ++i) {
        const bool unlocked = api_.unlocked(c, i) == 1;
        const std::string label = std::string(api_.label(c, i)) +
                                  (unlocked ? "" : " (locked)") + "##" + std::to_string(i);
        if (ImGui::Selectable(label.c_str(), i == worn,
                              unlocked ? 0 : ImGuiSelectableFlags_Disabled) &&
            i != worn && api_.wear(c, i) < 0) {
          failed_ = true;
        }
      }
      ImGui::EndCombo();
    }
    DrawLocks(c, count);
    ImGui::PopID();
  }

  // One checkbox per costume, ticked when unlocked, wrapped to the window. The
  // default cannot be locked, so it has none.
  void DrawLocks(int c, int count) {
    ImGui::Indent(110.0f);
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    bool first = true;
    for (int i = ETERNALSONATA_COSTUME_DEFAULT + 1; i < count; ++i) {
      const std::string label = std::string(api_.label(c, i)) + "##lock" + std::to_string(i);
      const float width = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x +
                          ImGui::CalcTextSize(label.c_str(), nullptr, true).x;
      if (!first) {
        ImGui::SameLine();
        if (ImGui::GetCursorScreenPos().x + width > right) {
          ImGui::NewLine();
        }
      }
      first = false;
      bool unlocked = api_.unlocked(c, i) == 1;
      if (ImGui::Checkbox(label.c_str(), &unlocked)) {
        unlocked ? api_.unlock(c, i) : api_.lock(c, i);
      }
    }
    ImGui::Unindent(110.0f);
  }

  // The default cannot be locked, so it is skipped.
  void SetAllLocks(bool unlocked) {
    for (int c = 1; c <= ETERNALSONATA_COSTUME_CHARACTER_COUNT; ++c) {
      const int count = api_.count(c);
      for (int i = ETERNALSONATA_COSTUME_DEFAULT + 1; i < count; ++i) {
        unlocked ? api_.unlock(c, i) : api_.lock(c, i);
      }
    }
  }

  CostumeApi api_;
  bool visible_ = false;
  bool failed_ = false;
};

class CostumeOverlayMod : public rex::system::IModPlugin {
 public:
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    dialog_ = std::make_unique<CostumeOverlayDialog>(drawer);
  }

 private:
  std::unique_ptr<CostumeOverlayDialog> dialog_;
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
  return new CostumeOverlayMod();
}
