// settings_probe: an overlay (F12) that exercises every part of the game's
// Settings API, so the whole surface can be checked in one place while
// playing.
//
// It shows, live:
//
//   * the ABI version the running host implements, and which of the entry
//     points actually resolved out of it;
//   * every setting with its current value, the range it accepts, and a
//     human-readable label for that value;
//   * per-setting write controls, one button per value, with manual input as
//     well. Both the values and their names come from the host, so the two
//     language settings show whatever the loaded mods added to them;
//   * the QUEUED vs OK distinction for volume writes;
//   * every error path (out-of-range value, invalid setting id) with the
//     returned error code;
//   * a log of setting.changed events off the mod registry bus, cross-
//     checked against the probe's own polling so a missing or extra event
//     is flagged.
//
// Nothing here knows a guest address; it is all
// rexmod/eternalsonata_settings_api.h.

#include <rex/system/mod_plugin.h>

#include <atomic>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/runtime.h>
#include <rex/system/mod_registry.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

#include <rexmod/eternalsonata_settings_api.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

constexpr size_t kMaxEventLines = 128;
constexpr int kSettingCount = ETERNALSONATA_SETTING_COUNT;

// Human-readable names for each setting, indexed by the enum value.
const char* SettingName(int setting) {
  switch (setting) {
    case ETERNALSONATA_SETTING_BATTLE_CAMERA:
      return "Battle Camera";
    case ETERNALSONATA_SETTING_ATTACK_BUTTON:
      return "Attack Button";
    case ETERNALSONATA_SETTING_VOLUME_MUSIC:
      return "Volume (Music)";
    case ETERNALSONATA_SETTING_VOLUME_SFX:
      return "Volume (SFX)";
    case ETERNALSONATA_SETTING_VOLUME_VOICE:
      return "Volume (Voice)";
    case ETERNALSONATA_SETTING_SUBTITLES:
      return "Subtitles";
    case ETERNALSONATA_SETTING_VOICE_LANGUAGE:
      return "Voice Language";
    case ETERNALSONATA_SETTING_AUDIO_OUTPUT:
      return "Audio Output";
    case ETERNALSONATA_SETTING_CONTROLLER_P1:
      return "Controller P1";
    case ETERNALSONATA_SETTING_CONTROLLER_P2:
      return "Controller P2";
    case ETERNALSONATA_SETTING_CONTROLLER_P3:
      return "Controller P3";
    case ETERNALSONATA_SETTING_TEXT_LANGUAGE:
      return "Text Language";
    default:
      return "???";
  }
}

// The struct for one setting's snapshot.
struct SettingInfo {
  int value;
  int range_min;
  int range_max;
};

// Every entry point is resolved out of the host executable at load time and
// null-checked before use, so this mod still loads (and says exactly what is
// missing) on a host too old to have the Settings API.
struct SettingsApi {
  EternalSonataSettingsAbiVersionFn abi_version = nullptr;
  EternalSonataGetSettingFn get = nullptr;
  EternalSonataSetSettingFn set = nullptr;
  EternalSonataGetSettingRangeFn get_range = nullptr;
  EternalSonataGetSettingValueNameFn value_name = nullptr;

  bool ok() const { return abi_version && get && set; }
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

SettingsApi LoadSettingsApi() {
  SettingsApi api;
  Resolve(api.abi_version, "EternalSonataSettingsAbiVersion");
  Resolve(api.get, "EternalSonataGetSetting");
  Resolve(api.set, "EternalSonataSetSetting");
  Resolve(api.get_range, "EternalSonataGetSettingRange");
  Resolve(api.value_name, "EternalSonataGetSettingValueName");
  return api;
}

const char* ErrorName(int value) {
  switch (value) {
    case ETERNALSONATA_SETTING_OK:
      return "OK";
    case ETERNALSONATA_SETTING_QUEUED:
      return "QUEUED";
    case ETERNALSONATA_SETTING_ERR_UNAVAILABLE:
      return "ERR_UNAVAILABLE";
    case ETERNALSONATA_SETTING_ERR_INVALID_SETTING:
      return "ERR_INVALID_SETTING";
    case ETERNALSONATA_SETTING_ERR_INVALID_VALUE:
      return "ERR_INVALID_VALUE";
    default:
      return "ERR_?";
  }
}

class SettingsProbeDialog : public rex::ui::ImGuiDialog {
 public:
  SettingsProbeDialog(rex::ui::ImGuiDrawer* drawer, rex::Runtime* runtime)
      : ImGuiDialog(drawer), runtime_(runtime), api_(LoadSettingsApi()) {
    rex::ui::RegisterBind(
        "bind_settings_probe", "F12", "Toggle the settings API probe",
        [this] { visible_ = !visible_; }, [this] { return visible_; },
        "Settings API Probe");

    SubscribeToSettingEvents();
  }

  ~SettingsProbeDialog() override { rex::ui::UnregisterBind("bind_settings_probe"); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    (void)io;

    // Polled whether or not the window is open, so a write with the overlay
    // closed is still observed and can still be compared against the events
    // that were published for it.
    Poll();

    if (!visible_) {
      return;
    }

    ImGui::SetNextWindowSize(ImVec2(880, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Settings API Probe", &visible_, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::End();
      return;
    }

    DrawHostSection();
    ImGui::Separator();
    DrawReadsSection();
    ImGui::Separator();
    DrawWriteSection();
    ImGui::Separator();
    DrawErrorSection();
    ImGui::Separator();
    DrawEventSection();

    ImGui::End();
  }

 private:
  void DrawHostSection() {
    if (!api_.ok()) {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                         "This host does not export the Settings API.");
      ImGui::TextWrapped(
          "EternalSonataGetSetting / EternalSonataSetSetting were not found in the "
          "executable. Nothing below can be tested.");
      return;
    }

    ImGui::Text("Settings ABI version: %u (this mod was built against %u)", api_.abi_version(),
                ETERNALSONATA_SETTINGS_ABI_VERSION);
    if (api_.abi_version() < ETERNALSONATA_SETTINGS_ABI_VERSION) {
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                         "The host predates this mod's ABI version; the API surface may differ.");
    }

    const char* missing[4];
    int missing_count = 0;
    if (!api_.get_range) missing[missing_count++] = "GetSettingRange";
    if (!api_.value_name) missing[missing_count++] = "GetSettingValueName";
    if (missing_count) {
      std::string list;
      for (int i = 0; i < missing_count; ++i) {
        if (i) {
          list += ", ";
        }
        list += missing[i];
      }
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Not exported by this host: %s",
                         list.c_str());
    }

    if (!runtime_ || !runtime_->mod_registry()) {
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                         "No mod registry: the setting.changed events cannot be received.");
    }
  }

  void DrawReadsSection() {
    if (!api_.ok()) {
      return;
    }

    if (!ImGui::BeginTable("settings", 5,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_SizingStretchProp)) {
      return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Range", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Label");
    ImGui::TableHeadersRow();

    for (int i = 0; i < kSettingCount; ++i) {
      const SettingInfo& info = settings_[i];
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("%d", i);
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(SettingName(i));
      ImGui::TableNextColumn();
      ImGui::Text("%d", info.value);
      ImGui::TableNextColumn();
      if (info.range_min == info.range_max) {
        ImGui::TextDisabled("fixed");
      } else {
        ImGui::Text("[%d, %d]", info.range_min, info.range_max);
      }
      ImGui::TableNextColumn();
      const char* label = ValueName(i, info.value);
      if (label) {
        ImGui::TextUnformatted(label);
      } else {
        ImGui::TextDisabled("(continuous)");
      }
    }
    ImGui::EndTable();
  }

  void DrawWriteSection() {
    if (!api_.ok()) {
      return;
    }

    ImGui::Text("Selected setting: %d (%s)", selected_, SettingName(selected_));

    const SettingInfo& info = settings_[selected_];
    ImGui::Text("Range: [%d, %d]", info.range_min, info.range_max);

    // One button per value, named by the host. The language settings are the
    // reason this is a loop and not a table of literals: how many values they
    // have, and what they are called, depends on the mods that are loaded.
    for (int value = info.range_min; value <= info.range_max; ++value) {
      const char* name = ValueName(selected_, value);
      if (!name) {
        break;  // continuous; the manual input below covers it
      }
      if (value > info.range_min) {
        ImGui::SameLine();
      }
      ImGui::PushID(value);
      if (ImGui::Button(name)) {
        last_write_result_ = WriteResult(SettingName(selected_), api_.set(selected_, value));
      }
      ImGui::PopID();
    }

    // Manual value input for all settings.
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::InputInt("##value", &manual_value_, 1, 100,
                        ImGuiInputTextFlags_EnterReturnsTrue)) {
      last_write_result_ =
          WriteResult(SettingName(selected_), api_.set(selected_, manual_value_));
    }
    ImGui::SameLine();
    if (ImGui::Button("Write")) {
      last_write_result_ =
          WriteResult(SettingName(selected_), api_.set(selected_, manual_value_));
    }

    // Convenience buttons for volumes.
    if (selected_ >= ETERNALSONATA_SETTING_VOLUME_MUSIC &&
        selected_ <= ETERNALSONATA_SETTING_VOLUME_VOICE) {
      if (ImGui::Button("0 (mute)")) {
        manual_value_ = 0;
        last_write_result_ =
            WriteResult(SettingName(selected_), api_.set(selected_, 0));
      }
      ImGui::SameLine();
      if (ImGui::Button("50")) {
        manual_value_ = 50;
        last_write_result_ =
            WriteResult(SettingName(selected_), api_.set(selected_, 50));
      }
      ImGui::SameLine();
      if (ImGui::Button("100 (max)")) {
        manual_value_ = 100;
        last_write_result_ =
            WriteResult(SettingName(selected_), api_.set(selected_, 100));
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Volume writes from this thread go through the guest main thread\n"
            "and answer QUEUED rather than OK. The value lands on the next\n"
            "guest frame.");
      }
    }

    if (!last_write_result_.empty()) {
      ImGui::TextDisabled("%s", last_write_result_.c_str());
    }
  }

  void DrawErrorSection() {
    if (!api_.ok()) {
      return;
    }
    ImGui::TextDisabled("Error paths");

    if (ImGui::Button("Get(-1) bad id")) {
      last_error_result_ =
          ErrorResult("EternalSonataGetSetting(-1)", api_.get(-1));
    }
    ImGui::SameLine();
    if (ImGui::Button("Get(COUNT) bad id")) {
      last_error_result_ =
          ErrorResult("EternalSonataGetSetting(COUNT)", api_.get(kSettingCount));
    }
    ImGui::SameLine();
    if (ImGui::Button("Set(COUNT, 0) bad id")) {
      last_error_result_ =
          ErrorResult("EternalSonataSetSetting(COUNT, 0)", api_.set(kSettingCount, 0));
    }

    if (api_.get_range) {
      if (ImGui::Button("GetRange(-1)")) {
        int min, max;
        int result = api_.get_range(-1, &min, &max);
        char label[128];
        std::snprintf(label, sizeof(label), "GetSettingRange(-1) -> %s (%d)",
                      ErrorName(result), result);
        last_error_result_ = label;
      }
      ImGui::SameLine();
      if (ImGui::Button("GetRange(COUNT)")) {
        int min, max;
        int result = api_.get_range(kSettingCount, &min, &max);
        char label[128];
        std::snprintf(label, sizeof(label), "GetSettingRange(COUNT) -> %s (%d)",
                      ErrorName(result), result);
        last_error_result_ = label;
      }
    }

    if (api_.value_name) {
      if (ImGui::Button("ValueName(VolMusic, 50)")) {
        const char* name = api_.value_name(ETERNALSONATA_SETTING_VOLUME_MUSIC, 50);
        last_error_result_ = std::string("ValueName(VolMusic, 50) -> ") +
                             (name ? name : "null (continuous, as expected)");
      }
      ImGui::SameLine();
      if (ImGui::Button("ValueName(Voice, COUNT)")) {
        const int past = settings_[ETERNALSONATA_SETTING_VOICE_LANGUAGE].range_max + 1;
        const char* name = api_.value_name(ETERNALSONATA_SETTING_VOICE_LANGUAGE, past);
        last_error_result_ = std::string("ValueName(Voice, past the end) -> ") +
                             (name ? name : "null (as expected)");
      }
    }

    if (api_.set) {
      if (ImGui::Button("Set(0, -1) below range")) {
        last_error_result_ =
            ErrorResult("Set(BattleCamera, -1)", api_.set(0, -1));
      }
      ImGui::SameLine();
      if (ImGui::Button("Set(0, 2) above range")) {
        last_error_result_ =
            ErrorResult("Set(BattleCamera, 2)", api_.set(0, 2));
      }
      ImGui::SameLine();
      if (ImGui::Button("Set(2, 150) above range")) {
        last_error_result_ =
            ErrorResult("Set(VolMusic, 150)", api_.set(2, 150));
      }
    }

    if (!last_error_result_.empty()) {
      ImGui::TextDisabled("%s", last_error_result_.c_str());
    }
  }

  void DrawEventSection() {
    if (!api_.ok()) {
      return;
    }

    const int events_received = event_count_.load();
    ImGui::Text("Events on the bus: %d", events_received);
    ImGui::Text("Observed by polling: %d changes", observed_changes_);
    if (events_received == observed_changes_) {
      ImGui::TextDisabled("The bus and the poll agree.");
    } else {
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                         "The bus and the poll DISAGREE. (A load replaces values silently, "
                         "so press Reset baseline after one.)");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset baseline")) {
      have_baseline_ = false;
      observed_changes_ = 0;
      event_count_.store(0);
      std::lock_guard<std::mutex> lock(events_mutex_);
      events_.clear();
      for (int i = 0; i < kSettingCount; ++i) {
        baseline_[i] = settings_[i].value;
      }
    }

    if (ImGui::BeginChild("events", ImVec2(0.0f, 160.0f), true)) {
      std::lock_guard<std::mutex> lock(events_mutex_);
      for (const auto& line : events_) {
        ImGui::TextUnformatted(line.c_str());
      }
    }
    ImGui::EndChild();
  }

  // -------------------------------------------------------------------------
  // Polling
  // -------------------------------------------------------------------------

  void Poll() {
    if (!api_.ok()) {
      return;
    }

    for (int i = 0; i < kSettingCount; ++i) {
      settings_[i].value = api_.get(i);
      if (api_.get_range) {
        int min, max;
        if (api_.get_range(i, &min, &max) == ETERNALSONATA_SETTING_OK) {
          settings_[i].range_min = min;
          settings_[i].range_max = max;
        }
      }
    }

    ObserveTransitions();
  }

  void ObserveTransitions() {
    if (!have_baseline_) {
      have_baseline_ = true;
      for (int i = 0; i < kSettingCount; ++i) {
        baseline_[i] = settings_[i].value;
      }
      return;
    }

    for (int i = 0; i < kSettingCount; ++i) {
      if (settings_[i].value < 0 || baseline_[i] < 0) {
        continue;  // skip unavailable settings
      }
      if (settings_[i].value != baseline_[i]) {
        ++observed_changes_;
        char line[160];
        std::snprintf(line, sizeof(line), "observed  %s: %d -> %d", SettingName(i),
                      baseline_[i], settings_[i].value);
        PushEvent(line);
        baseline_[i] = settings_[i].value;
      }
    }
  }

  // Null for a continuous setting, an out-of-range value, or a host too old to
  // export the call.
  const char* ValueName(int setting, int value) const {
    return api_.value_name ? api_.value_name(setting, value) : nullptr;
  }

  std::string WriteResult(const char* name, int result) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%s returned %s (%d)", name,
                  ErrorName(result), result);
    return buffer;
  }

  std::string ErrorResult(const char* name, int result) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%s -> %s (%d)", name, ErrorName(result), result);
    return buffer;
  }

  void SubscribeToSettingEvents() {
    if (!runtime_) {
      return;
    }
    auto* registry = runtime_->mod_registry();
    if (!registry) {
      return;
    }

    registry->Subscribe(ETERNALSONATA_SETTING_EVENT_CHANGED,
                        [this](const rex::system::ModRegistry::EventPayload& payload) {
                          event_count_.fetch_add(1);
                          const int setting = static_cast<int>(payload.u64);
                          const int value = static_cast<int>(payload.f64);
                          char line[160];
                          std::snprintf(line, sizeof(line), "event     %s = %d",
                                        SettingName(setting), value);
                          PushEvent(line);
                        });
  }

  void PushEvent(const char* line) {
    std::lock_guard<std::mutex> lock(events_mutex_);
    events_.emplace_back(line);
    while (events_.size() > kMaxEventLines) {
      events_.pop_front();
    }
  }

  rex::Runtime* runtime_ = nullptr;
  SettingsApi api_;
  bool visible_ = false;

  // Snapshot, owned by the draw thread.
  std::array<SettingInfo, kSettingCount> settings_{};
  std::string last_write_result_;
  std::string last_error_result_;
  int selected_ = 0;
  int manual_value_ = 0;

  // The probe's own transition counters, also draw-thread only.
  bool have_baseline_ = false;
  std::array<int, kSettingCount> baseline_{};
  int observed_changes_ = 0;

  // Events, written by the registry tick.
  std::mutex events_mutex_;
  std::deque<std::string> events_;
  std::atomic<int> event_count_{0};
};

class SettingsProbeMod : public rex::system::IModPlugin {
 public:
  explicit SettingsProbeMod(rex::Runtime* runtime) : runtime_(runtime) {}

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    dialog_ = std::make_unique<SettingsProbeDialog>(drawer, runtime_);
  }

 private:
  rex::Runtime* runtime_ = nullptr;
  std::unique_ptr<SettingsProbeDialog> dialog_;
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
  return new SettingsProbeMod(ctx->runtime);
}
