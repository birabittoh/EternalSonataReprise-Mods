// party_overlay - an overlay (F5) over the game's party: who is in it, in what
// order, with what stats, plus adding, removing, reordering and healing.
//
// There is no reverse engineering left in this mod. Every guest address, the
// join sequence, the battle-party resync and the battle-safety gate that used
// to live here now live in the game itself and are reached through the party
// API it exports (see rexmod/eternalsonata_party_api.h, copied from the game
// repo's src/eternalsonata_party_api.h, and the game's docs/party-system.md
// for the structures underneath). This file is UI.

#include <rex/system/mod_plugin.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/runtime.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

#include <rexmod/eternalsonata_party_api.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

// Every entry point is resolved out of the host executable at load time and
// null-checked before use, so this mod still loads (and simply says so) on a
// host too old to have the party API.
struct PartyApi {
  EternalSonataPartyAbiVersionFn abi_version = nullptr;
  EternalSonataIsPartyAvailableFn is_available = nullptr;
  EternalSonataIsPartyEditableFn is_editable = nullptr;
  EternalSonataGetCharacterNameFn name = nullptr;
  EternalSonataGetPartyMembersFn members = nullptr;
  EternalSonataIsCharacterInPartyFn in_party = nullptr;
  EternalSonataGetCharacterPositionFn position = nullptr;
  EternalSonataGetPartyLevelFn level = nullptr;
  EternalSonataGetPartyLevelBudgetFn budget = nullptr;
  EternalSonataGetPartyLevelBudgetUsedFn budget_used = nullptr;
  EternalSonataGetPartyLevelBudgetFreeFn budget_free = nullptr;
  EternalSonataGetCharacterStatsFn stats = nullptr;
  EternalSonataGetCharacterBaseStatsFn base_stats = nullptr;
  EternalSonataSetCharacterStatsFn set_stats = nullptr;
  EternalSonataHealCharacterFn heal = nullptr;
  EternalSonataHealPartyFn heal_party = nullptr;
  EternalSonataAddCharacterToPartyFn add = nullptr;
  EternalSonataRemoveCharacterFromPartyFn remove = nullptr;
  EternalSonataSetCharacterPositionFn set_position = nullptr;
  EternalSonataSetPartyLevelFn set_level = nullptr;
  // The added slots are vacant unless some other mod has defined them, and a
  // vacant slot has no name and cannot join a party, so it is left out of every
  // list here rather than shown as a row that does nothing.
  EternalSonataIsCharacterDefinedFn is_defined = nullptr;

  bool ok() const { return abi_version && is_available && name && members && add && remove; }
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

PartyApi LoadPartyApi() {
  PartyApi api;
  Resolve(api.abi_version, "EternalSonataPartyAbiVersion");
  Resolve(api.is_available, "EternalSonataIsPartyAvailable");
  Resolve(api.is_editable, "EternalSonataIsPartyEditable");
  Resolve(api.name, "EternalSonataGetCharacterName");
  Resolve(api.members, "EternalSonataGetPartyMembers");
  Resolve(api.in_party, "EternalSonataIsCharacterInParty");
  Resolve(api.position, "EternalSonataGetCharacterPosition");
  Resolve(api.level, "EternalSonataGetPartyLevel");
  Resolve(api.budget, "EternalSonataGetPartyLevelBudget");
  Resolve(api.budget_used, "EternalSonataGetPartyLevelBudgetUsed");
  Resolve(api.budget_free, "EternalSonataGetPartyLevelBudgetFree");
  Resolve(api.stats, "EternalSonataGetCharacterStats");
  Resolve(api.base_stats, "EternalSonataGetCharacterBaseStats");
  Resolve(api.set_stats, "EternalSonataSetCharacterStats");
  Resolve(api.heal, "EternalSonataHealCharacter");
  Resolve(api.heal_party, "EternalSonataHealParty");
  Resolve(api.add, "EternalSonataAddCharacterToParty");
  Resolve(api.remove, "EternalSonataRemoveCharacterFromParty");
  Resolve(api.set_position, "EternalSonataSetCharacterPosition");
  Resolve(api.set_level, "EternalSonataSetPartyLevel");
  Resolve(api.is_defined, "EternalSonataIsCharacterDefined");
  return api;
}

class PartyOverlayDialog : public rex::ui::ImGuiDialog {
 public:
  explicit PartyOverlayDialog(rex::ui::ImGuiDrawer* drawer)
      : ImGuiDialog(drawer), api_(LoadPartyApi()) {
    rex::ui::RegisterBind(
        "bind_party_overlay", "F5", "Toggle party overlay", [this] { visible_ = !visible_; },
        [this] { return visible_; }, "Party Overlay");
  }

  ~PartyOverlayDialog() override { rex::ui::UnregisterBind("bind_party_overlay"); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    (void)io;
    if (!visible_) {
      return;
    }

    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Party Overlay", &visible_, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::End();
      return;
    }

    if (!api_.ok()) {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                         "This build of the game has no party API.");
      ImGui::TextDisabled("Eternal Sonata: Reprise 1.2.0 or newer is required.");
      ImGui::End();
      return;
    }

    if (!api_.is_available()) {
      ImGui::TextDisabled("Start or load a game to view party state.");
      ImGui::End();
      return;
    }

    DrawHeader();
    ImGui::Separator();
    DrawMembers();
    DrawBench();

    ImGui::End();

    for (int character = 1; character <= ETERNALSONATA_CHARACTER_COUNT; ++character) {
      if (detail_open_[character]) {
        DrawDetailWindow(character);
      }
    }
  }

 private:
  void DrawHeader() {
    const int level = api_.level ? api_.level() : -1;
    if (level > 0) {
      ImGui::Text("Party Lv. %d", level);
      if (api_.set_level) {
        ImGui::SameLine();
        if (ImGui::SmallButton("-##level") && level > 1) {
          api_.set_level(level - 1);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("+##level") && level < 6) {
          api_.set_level(level + 1);
        }
      }
    }
    if (api_.budget && api_.budget_used && api_.budget_free) {
      const int cap = api_.budget();
      if (cap >= 0) {
        ImGui::Text("Budget %d used, %d free, of %d", api_.budget_used(), api_.budget_free(), cap);
      }
    }
    if (api_.heal_party && ImGui::Button("Heal party")) {
      api_.heal_party();
    }
    if (api_.is_editable && !api_.is_editable()) {
      ImGui::SameLine();
      ImGui::TextDisabled("(battle in progress: no party edits)");
    }
  }

  void DrawMembers() {
    int ids[ETERNALSONATA_CHARACTER_COUNT] = {};
    const int count = api_.members(ids, ETERNALSONATA_CHARACTER_COUNT);
    if (count <= 0) {
      ImGui::TextDisabled("No party members.");
      return;
    }

    // One fixed column for the buttons so they line up whatever the names are.
    const float button_column =
        ImGui::CalcTextSize("  #12 Allegretto__").x + 3.0f * ImGui::GetStyle().ItemSpacing.x;
    const bool editable = api_.is_editable && api_.is_editable();

    ImGui::Text("Active party:");
    for (int i = 0; i < count; ++i) {
      if (i == ETERNALSONATA_ACTIVE_PARTY_SIZE) {
        ImGui::Text("Reserves:");
      }
      const int character = ids[i];
      ImGui::Text("  #%d %s", i + 1, api_.name(character));
      ImGui::SameLine(button_column);
      if (ImGui::Button(("Stats##" + std::to_string(character)).c_str())) {
        detail_open_[character] = !detail_open_[character];
      }
      if (!editable) {
        continue;
      }
      if (api_.set_position && i > 0) {
        ImGui::SameLine();
        if (ImGui::Button(("Up##" + std::to_string(character)).c_str())) {
          api_.set_position(character, i);
        }
      }
      if (api_.set_position && i + 1 < count) {
        ImGui::SameLine();
        if (ImGui::Button(("Down##" + std::to_string(character)).c_str())) {
          api_.set_position(character, i + 2);
        }
      }
      // The party has to keep somebody in it; the game has no concept of an
      // empty one and its own menu will not let you get there either.
      if (count > 1) {
        ImGui::SameLine();
        if (ImGui::Button(("Remove##" + std::to_string(character)).c_str())) {
          api_.remove(character);
        }
      }
    }
  }

  void DrawBench() {
    if (!api_.is_editable || !api_.is_editable()) {
      return;
    }
    std::vector<int> missing;
    for (int character = 1; character <= ETERNALSONATA_CHARACTER_COUNT; ++character) {
      if (api_.is_defined && !api_.is_defined(character)) {
        continue;  // a vacant added slot: no mod has given it any content
      }
      if (!api_.in_party(character)) {
        missing.push_back(character);
      }
    }
    if (missing.empty()) {
      return;
    }

    ImGui::Separator();
    ImGui::Text("Add to party:");
    for (size_t i = 0; i < missing.size(); ++i) {
      if ((i % 2) != 0) {
        ImGui::SameLine();
      }
      const int character = missing[i];
      const std::string label = std::string(api_.name(character)) + "##add" + std::to_string(character);
      if (ImGui::Button(label.c_str())) {
        api_.add(character);
      }
    }
  }

  // Scratch values for the edit form, filled when edit mode is entered so an
  // in-progress edit is not clobbered by the next frame's re-read.
  struct EditBuffer {
    EternalSonataCharacterStats stats{};
  };

  void DrawDetailWindow(int character) {
    const std::string title =
        std::string(api_.name(character)) + " Stats##detail" + std::to_string(character);

    bool open = detail_open_[character];
    ImGui::SetNextWindowSize(ImVec2(240, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize)) {
      EternalSonataCharacterStats stats{};
      // Read the character's own stats, not the equipment-adjusted ones: those
      // are what a write goes back into, so editing them round-trips cleanly.
      const bool have =
          api_.base_stats && api_.base_stats(character, &stats) == ETERNALSONATA_PARTY_OK;
      if (!have) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "stats unavailable");
      } else if (!edit_mode_[character]) {
        const float label_column =
            ImGui::CalcTextSize("Level").x + 2.0f * ImGui::GetStyle().ItemSpacing.x;
        auto row = [&](const char* label, const std::string& value) {
          ImGui::Text("%s", label);
          ImGui::SameLine(label_column);
          ImGui::Text("%s", value.c_str());
        };
        row("Level", std::to_string(stats.level));
        row("HP", std::to_string(stats.hp) + " / " + std::to_string(stats.hp_max));
        row("ATK", std::to_string(stats.attack));
        row("MGC", std::to_string(stats.magic));
        row("DEF", std::to_string(stats.defense));
        row("SPD", std::to_string(stats.speed));
        if (api_.set_stats && ImGui::Button("Edit")) {
          edit_[character].stats = stats;
          edit_mode_[character] = true;
        }
        if (api_.heal) {
          ImGui::SameLine();
          if (ImGui::Button("Heal")) {
            api_.heal(character);
          }
        }
      } else {
        EternalSonataCharacterStats& b = edit_[character].stats;
        auto field = [](const char* label, int32_t* value) {
          ImGui::SetNextItemWidth(100.0f);
          int scratch = static_cast<int>(*value);
          if (ImGui::InputInt(label, &scratch)) {
            *value = static_cast<int32_t>(scratch);
          }
        };
        field("Level", &b.level);
        field("HP", &b.hp);
        field("HP max", &b.hp_max);
        field("ATK", &b.attack);
        field("MGC", &b.magic);
        field("DEF", &b.defense);
        field("SPD", &b.speed);
        if (ImGui::Button("Save")) {
          api_.set_stats(character, &b);
          edit_mode_[character] = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
          edit_mode_[character] = false;
        }
      }
    }
    ImGui::End();

    detail_open_[character] = open;
    if (!open) {
      edit_mode_[character] = false;
    }
  }

  PartyApi api_;
  bool visible_ = false;
  // Indexed by character number (1..10); index 0 unused.
  bool detail_open_[ETERNALSONATA_CHARACTER_COUNT + 1] = {};
  bool edit_mode_[ETERNALSONATA_CHARACTER_COUNT + 1] = {};
  EditBuffer edit_[ETERNALSONATA_CHARACTER_COUNT + 1];
};

class PartyOverlayMod : public rex::system::IModPlugin {
 public:
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    dialog_ = std::make_unique<PartyOverlayDialog>(drawer);
  }

 private:
  std::unique_ptr<PartyOverlayDialog> dialog_;
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
  return new PartyOverlayMod();
}
