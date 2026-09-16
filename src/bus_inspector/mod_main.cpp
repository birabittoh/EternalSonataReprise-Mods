// bus_inspector mod - live log overlay (F5) for the shared mod event bus.
//
// Subscribes to every event name published by this project's own systems
// (asset, battle, enemy, equipment, flag, item, music, overworld, photo,
// piano music, save, settings) and renders a scrolling log of what it
// observed: event name, the u64/f64 fields, and the byte payload as text.
// Deliberately has no `requires` on any of them -- it just shows whatever
// traffic happens to exist, including none.
//
// rex::system::ModRegistry has no wildcard subscription, so "all events"
// means enumerating every ETERNALSONATA_*_EVENT_* constant by hand and
// subscribing to each individually. A new event added to one of the *_api.h
// headers needs a matching entry in kEventNames below, or this mod won't see
// it.
//
// Threading: Subscribe callbacks run on whatever thread published the event
// (the command-processor thread for every event in this project) -- never
// touch ImGui there. This mod only appends a formatted line to a
// mutex-guarded deque; OnDraw (UI thread) renders a snapshot under the same
// lock.

#include <rex/system/mod_plugin.h>

#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>

#include <imgui.h>

#include <rex/runtime.h>
#include <rex/system/mod_registry.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

#include <rexmod/eternalsonata_asset_api.h>
#include <rexmod/eternalsonata_battle_api.h>
#include <rexmod/eternalsonata_enemy_api.h>
#include <rexmod/eternalsonata_equipment_api.h>
#include <rexmod/eternalsonata_flag_api.h>
#include <rexmod/eternalsonata_item_api.h>
#include <rexmod/eternalsonata_music_api.h>
#include <rexmod/eternalsonata_overworld_api.h>
#include <rexmod/eternalsonata_photo_api.h>
#include <rexmod/eternalsonata_piano_music_api.h>
#include <rexmod/eternalsonata_save_api.h>
#include <rexmod/eternalsonata_settings_api.h>

namespace {

constexpr size_t kMaxLines = 200;

constexpr const char* kEventNames[] = {
    ETERNALSONATA_ASSET_EVENT_LOADING,
    ETERNALSONATA_ASSET_EVENT_PATCHED,
    ETERNALSONATA_BATTLE_EVENT_ATTACK,
    ETERNALSONATA_BATTLE_EVENT_ABILITY,
    ETERNALSONATA_BATTLE_EVENT_ITEM,
    ETERNALSONATA_BATTLE_EVENT_DAMAGE,
    ETERNALSONATA_BATTLE_EVENT_HEAL,
    ETERNALSONATA_BATTLE_EVENT_CRITICAL,
    ETERNALSONATA_BATTLE_EVENT_PARRY,
    ETERNALSONATA_BATTLE_EVENT_HARMONY_CHAIN,
    ETERNALSONATA_BATTLE_EVENT_COUNTERATTACK,
    ETERNALSONATA_ENEMY_EVENT_OVERRIDE_SET,
    ETERNALSONATA_ENEMY_EVENT_OVERRIDE_CLEARED,
    ETERNALSONATA_EQUIPMENT_EVENT_EQUIPPED,
    ETERNALSONATA_EQUIPMENT_EVENT_UNEQUIPPED,
    ETERNALSONATA_EQUIPMENT_EVENT_MAGIC_SET,
    ETERNALSONATA_FLAG_EVENT_CHANGED,
    ETERNALSONATA_FLAG_EVENT_BULK_CHANGED,
    ETERNALSONATA_ITEM_EVENT_GAINED,
    ETERNALSONATA_ITEM_EVENT_LOST,
    ETERNALSONATA_ITEM_EVENT_SET_ADDED,
    ETERNALSONATA_ITEM_EVENT_SET_REMOVED,
    ETERNALSONATA_SCORE_PIECE_EVENT_MARKED,
    ETERNALSONATA_MUSIC_EVENT_UNLOCKED,
    ETERNALSONATA_MUSIC_EVENT_LOCKED,
    ETERNALSONATA_OVERWORLD_EVENT_AREA_ENTERED,
    ETERNALSONATA_OVERWORLD_EVENT_ACTION,
    ETERNALSONATA_OVERWORLD_EVENT_DIALOGUE_STARTED,
    ETERNALSONATA_OVERWORLD_EVENT_BATTLE_STARTED,
    ETERNALSONATA_PHOTO_EVENT_ADDED,
    ETERNALSONATA_PHOTO_EVENT_DEVELOPED,
    ETERNALSONATA_PIANO_MUSIC_EVENT_UNLOCKED,
    ETERNALSONATA_PIANO_MUSIC_EVENT_LOCKED,
    ETERNALSONATA_SAVE_EVENT_STARTED,
    ETERNALSONATA_SAVE_EVENT_COMPLETED,
    ETERNALSONATA_SAVE_EVENT_FAILED,
    ETERNALSONATA_SETTING_EVENT_CHANGED,
};

// Payload bytes aren't guaranteed to be printable text for every event (they
// are for all the demo events in this project), so fall back to a length
// note rather than printing raw bytes as if they were a C string.
std::string DescribeBytes(std::span<const uint8_t> bytes) {
  for (uint8_t b : bytes) {
    if (b == 0 || b > 0x7E) {
      return "<" + std::to_string(bytes.size()) + " bytes>";
    }
  }
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

class BusInspectorDialog : public rex::ui::ImGuiDialog {
 public:
  BusInspectorDialog(rex::ui::ImGuiDrawer* drawer, rex::Runtime* runtime)
      : ImGuiDialog(drawer), runtime_(runtime) {
    rex::ui::RegisterBind(
        "bind_bus_inspector", "F5", "Toggle bus inspector overlay", [this] { visible_ = !visible_; },
        [this] { return visible_; }, "Bus Inspector");

    auto* registry = runtime_ ? runtime_->mod_registry() : nullptr;
    if (!registry) {
      return;
    }
    for (const char* event_name : kEventNames) {
      registry->Subscribe(event_name, [this, event_name](const auto& payload) {
        OnEvent(event_name, payload);
      });
    }
  }

  ~BusInspectorDialog() override { rex::ui::UnregisterBind("bind_bus_inspector"); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    (void)io;
    if (!visible_) {
      return;
    }

    ImGui::SetNextWindowSize(ImVec2(420, 280), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Bus Inspector", &visible_)) {
      ImGui::TextDisabled("Live log of eternalsonata.* mod-bus events");
      if (ImGui::Button("Clear")) {
        std::lock_guard lock(mutex_);
        lines_.clear();
      }
      ImGui::Separator();

      ImGui::BeginChild("bus_inspector_log", ImVec2(0, 0), ImGuiChildFlags_Borders);
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
    }
    ImGui::End();
  }

 private:
  void OnEvent(const char* event_name, const rex::system::ModRegistry::EventPayload& payload) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "[%s] u64=%llu f64=%.3f bytes=%s", event_name,
                 static_cast<unsigned long long>(payload.u64), payload.f64,
                 DescribeBytes(payload.bytes).c_str());

    std::lock_guard lock(mutex_);
    lines_.emplace_back(buf);
    if (lines_.size() > kMaxLines) {
      lines_.pop_front();
    }
  }

  rex::Runtime* runtime_ = nullptr;
  bool visible_ = false;

  std::mutex mutex_;
  std::deque<std::string> lines_;
};

class BusInspectorMod : public rex::system::IModPlugin {
 public:
  explicit BusInspectorMod(rex::Runtime* runtime) : runtime_(runtime) {}

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    dialog_ = std::make_unique<BusInspectorDialog>(drawer, runtime_);
  }

 private:
  rex::Runtime* runtime_ = nullptr;
  std::unique_ptr<BusInspectorDialog> dialog_;
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
  return new BusInspectorMod(ctx->runtime);
}
