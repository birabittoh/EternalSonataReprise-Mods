#include <rex/system/mod_plugin.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>

#include <imgui.h>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

#include <rexmod/eternalsonata_battle_api.h>

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

struct BattleApi {
  EternalSonataBattleAbiVersionFn version = nullptr;
  EternalSonataGetBattlePartyCountFn party_count = nullptr;
  EternalSonataGetBattleEnemyCountFn enemy_count = nullptr;
  EternalSonataGetBattlePartyUnitFn party_unit = nullptr;
  EternalSonataGetBattleEnemyFn enemy_unit = nullptr;
  EternalSonataGetBattleEnemyNameFn enemy_name = nullptr;
  EternalSonataGetBattleUnitPositionFn get_position = nullptr;
  EternalSonataSetBattleUnitPositionFn set_position = nullptr;
  EternalSonataGetBattleUnitFacingFn get_facing = nullptr;
  EternalSonataSetBattleUnitFacingFn set_facing = nullptr;

  BattleApi() {
    Resolve(version, "EternalSonataBattleAbiVersion");
    Resolve(party_count, "EternalSonataGetBattlePartyCount");
    Resolve(enemy_count, "EternalSonataGetBattleEnemyCount");
    Resolve(party_unit, "EternalSonataGetBattlePartyUnit");
    Resolve(enemy_unit, "EternalSonataGetBattleEnemy");
    Resolve(enemy_name, "EternalSonataGetBattleEnemyName");
    Resolve(get_position, "EternalSonataGetBattleUnitPosition");
    Resolve(set_position, "EternalSonataSetBattleUnitPosition");
    Resolve(get_facing, "EternalSonataGetBattleUnitFacing");
    Resolve(set_facing, "EternalSonataSetBattleUnitFacing");
  }

  bool ready() const { return get_position && set_position; }
  bool facing_ready() const { return get_facing && set_facing; }
};

class BattlePositionDialog : public rex::ui::ImGuiDialog {
 public:
  explicit BattlePositionDialog(rex::ui::ImGuiDrawer* drawer)
      : ImGuiDialog(drawer) {
    rex::ui::RegisterBind(
        "bind_battle_position_probe", "F4", "Toggle battle position probe",
        [this] { visible_ = !visible_; }, [this] { return visible_; },
        "Battle Position Probe##rex");
  }

  ~BattlePositionDialog() override {
    rex::ui::UnregisterBind("bind_battle_position_probe");
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!visible_) {
      return;
    }
    ImGui::SetNextWindowSize(ImVec2(460, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Battle Position Probe##rex", &visible_,
                      ImGuiWindowFlags_NoCollapse)) {
      ImGui::End();
      return;
    }
    if (!api_.ready()) {
      ImGui::TextUnformatted("Battle position API unavailable in this game build.");
      ImGui::Text("ABI version: %u  Expected: %u",
                  api_.version ? api_.version() : 0u,
                  ETERNALSONATA_BATTLE_ABI_VERSION);
      ImGui::Text("Getter: %s  Setter: %s", api_.get_position ? "found" : "missing",
                  api_.set_position ? "found" : "missing");
      ImGui::End();
      return;
    }
    ImGui::Text("Host ABI version: %u", api_.version ? api_.version() : 0u);

    const int party = api_.party_count ? api_.party_count() : 0;
    const int enemies = api_.enemy_count ? api_.enemy_count() : 0;
    if (party <= 0 && enemies <= 0) {
      ImGui::TextUnformatted("Waiting for a battle.");
      have_edit_ = false;
      pinned_ = false;
      ImGui::End();
      return;
    }
    ImGui::Text("Party %d  Enemies %d", party, enemies);

    ImGui::Separator();
    DrawSide(ETERNALSONATA_BATTLE_ACTOR_PARTY, "Party", party);
    DrawSide(ETERNALSONATA_BATTLE_ACTOR_ENEMY, "Enemies", enemies);

    ImGui::Separator();
    DrawEditor(io);
    ImGui::End();
  }

 private:
  const char* UnitLabel(int side, int slot) {
    static char label[96];
    EternalSonataBattleUnit unit{};
    if (side == ETERNALSONATA_BATTLE_ACTOR_PARTY) {
      if (api_.party_unit && api_.party_unit(slot, &unit) == ETERNALSONATA_BATTLE_OK) {
        std::snprintf(label, sizeof(label), "P%d  character %d", slot, unit.character);
        return label;
      }
    } else if (api_.enemy_name) {
      std::snprintf(label, sizeof(label), "E%d  %s", slot, api_.enemy_name(slot));
      return label;
    }
    std::snprintf(label, sizeof(label), "%c%d", side ? 'E' : 'P', slot);
    return label;
  }

  void DrawSide(int side, const char* title, int count) {
    ImGui::TextUnformatted(title);
    for (int slot = 0; slot < count; ++slot) {
      EternalSonataBattlePosition position{};
      const int result = api_.get_position(side, slot, &position);
      const bool selected = side_ == side && slot_ == slot;
      char row[160];
      if (result == ETERNALSONATA_BATTLE_OK) {
        EternalSonataBattleFacing facing{};
        const bool have_facing =
            api_.get_facing &&
            api_.get_facing(side, slot, &facing) == ETERNALSONATA_BATTLE_OK;
        std::snprintf(row, sizeof(row), "%s   %.3f  %.3f  %.3f   %.1f deg##%d_%d",
                      UnitLabel(side, slot), position.x, position.y, position.z,
                      have_facing ? facing.yaw * 57.2957795f : 0.0f, side, slot);
      } else {
        std::snprintf(row, sizeof(row), "%s   no position (%d)##%d_%d",
                      UnitLabel(side, slot), result, side, slot);
      }
      if (ImGui::Selectable(row, selected)) {
        side_ = side;
        slot_ = slot;
        have_edit_ = false;
        pinned_ = false;
        have_facing_edit_ = false;
        pinned_facing_ = false;
      }
    }
  }

  void DrawEditor(ImGuiIO& io) {
    ImGui::Text("Selected: %s", UnitLabel(side_, slot_));
    EternalSonataBattlePosition live{};
    const int read_result = api_.get_position(side_, slot_, &live);
    if (read_result != ETERNALSONATA_BATTLE_OK) {
      ImGui::Text("No live position for this unit (result %d).", read_result);
      have_edit_ = false;
      pinned_ = false;
      return;
    }
    ImGui::Text("Live  X %.3f  Y %.3f  Z %.3f", live.x, live.y, live.z);
    if (!have_edit_) {
      edit_ = live;
      have_edit_ = true;
    }
    if (ImGui::Button("Capture live")) {
      edit_ = live;
    }
    ImGui::InputFloat("X", &edit_.x, 0.0f, 0.0f, "%.3f");
    ImGui::InputFloat("Y", &edit_.y, 0.0f, 0.0f, "%.3f");
    ImGui::InputFloat("Z", &edit_.z, 0.0f, 0.0f, "%.3f");
    if (ImGui::Button("Set position")) {
      set_result_ = api_.set_position(side_, slot_, &edit_);
    }
    ImGui::SameLine();
    ImGui::Text("Last set result: %d", set_result_);

    ImGui::Checkbox("Pin (re-apply every frame)", &pinned_);
    ImGui::SameLine();
    ImGui::TextDisabled("(the game keeps driving the unit otherwise)");
    ImGui::SliderFloat("Speed", &speed_, 0.1f, 30.0f, "%.1f units per second");

    const float step = speed_ * std::min(io.DeltaTime, 0.05f);
    auto hold = [](const char* label) {
      ImGui::Button(label, ImVec2(105, 0));
      return ImGui::IsItemActive();
    };
    bool moved = false;
    if (hold("X left")) { edit_.x -= step; moved = true; }
    ImGui::SameLine();
    if (hold("X right")) { edit_.x += step; moved = true; }
    if (hold("Y down")) { edit_.y -= step; moved = true; }
    ImGui::SameLine();
    if (hold("Y up")) { edit_.y += step; moved = true; }
    if (hold("Z back")) { edit_.z -= step; moved = true; }
    ImGui::SameLine();
    if (hold("Z forward")) { edit_.z += step; moved = true; }

    if (pinned_ || moved) {
      set_result_ = api_.set_position(side_, slot_, &edit_);
    }

    ImGui::Separator();
    DrawFacing(io);
  }

  void DrawFacing(ImGuiIO& io) {
    ImGui::TextUnformatted("Facing");
    if (!api_.facing_ready()) {
      ImGui::TextUnformatted("Facing API unavailable in this game build.");
      return;
    }
    EternalSonataBattleFacing live{};
    if (api_.get_facing(side_, slot_, &live) != ETERNALSONATA_BATTLE_OK) {
      ImGui::TextUnformatted("No live facing for this unit.");
      have_facing_edit_ = false;
      pinned_facing_ = false;
      return;
    }
    constexpr float kToDegrees = 57.2957795f;
    ImGui::Text("Yaw       %.2f rad  (%.1f deg)", live.yaw, live.yaw * kToDegrees);
    ImGui::Text("Forward   %.3f  %.3f  %.3f", live.forward.x, live.forward.y,
                live.forward.z);
    ImGui::Text("Rotation  %.3f  %.3f  %.3f rad", live.rotation.x, live.rotation.y,
                live.rotation.z);
    if (!have_facing_edit_) {
      facing_edit_ = live.rotation;
      have_facing_edit_ = true;
    }
    if (ImGui::Button("Capture live facing")) {
      facing_edit_ = live.rotation;
    }
    ImGui::InputFloat("Pitch", &facing_edit_.x, 0.0f, 0.0f, "%.3f");
    ImGui::InputFloat("Yaw", &facing_edit_.y, 0.0f, 0.0f, "%.3f");
    ImGui::InputFloat("Roll", &facing_edit_.z, 0.0f, 0.0f, "%.3f");
    if (ImGui::Button("Set facing")) {
      facing_result_ = api_.set_facing(side_, slot_, &facing_edit_);
    }
    ImGui::SameLine();
    ImGui::Text("Last facing result: %d", facing_result_);
    ImGui::Checkbox("Pin facing", &pinned_facing_);
    ImGui::SliderFloat("Turn speed", &turn_speed_, 0.05f, 6.0f,
                       "%.2f rad per second");

    const float step = turn_speed_ * std::min(io.DeltaTime, 0.05f);
    auto hold = [](const char* label) {
      ImGui::Button(label, ImVec2(105, 0));
      return ImGui::IsItemActive();
    };
    bool turned = false;
    if (hold("Yaw left")) { facing_edit_.y -= step; turned = true; }
    ImGui::SameLine();
    if (hold("Yaw right")) { facing_edit_.y += step; turned = true; }
    if (pinned_facing_ || turned) {
      facing_result_ = api_.set_facing(side_, slot_, &facing_edit_);
    }
  }

  BattleApi api_;
  bool visible_ = false;
  int side_ = ETERNALSONATA_BATTLE_ACTOR_PARTY;
  int slot_ = 0;
  bool have_edit_ = false;
  bool pinned_ = false;
  EternalSonataBattlePosition edit_{};
  float speed_ = 3.0f;
  int set_result_ = ETERNALSONATA_BATTLE_ERR_UNAVAILABLE;
  bool have_facing_edit_ = false;
  bool pinned_facing_ = false;
  EternalSonataBattlePosition facing_edit_{};
  float turn_speed_ = 1.5f;
  int facing_result_ = ETERNALSONATA_BATTLE_ERR_UNAVAILABLE;
};

class BattlePositionProbe : public rex::system::IModPlugin {
 public:
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    dialog_ = std::make_unique<BattlePositionDialog>(drawer);
  }

 private:
  std::unique_ptr<BattlePositionDialog> dialog_;
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
  return new BattlePositionProbe();
}
