// flag_probe: an overlay (F11) that exercises every part of the game's Flag
// API, so the whole surface can be checked in one place while playing.
//
// It shows, live:
//
//   * the ABI version the running host implements, and which of the entry
//     points actually resolved out of it;
//   * how many flags are set right now, and which ones;
//   * a single-flag inspector that reads, sets, clears and toggles by index;
//   * snapshot and diff, which is the point of the whole thing: take a
//     snapshot, open a chest or watch a scene, press Diff, and the flag the
//     game just set is on screen. Watch mode does the same automatically
//     every frame and logs it;
//   * the scenario counter, read and written;
//   * every error path with the returned error code;
//   * a log of the three flag events off the mod registry bus, cross-checked
//     against the probe's own polling so a missing or extra event is flagged.
//
// Watch mode is also how the bit convention gets verified: set a known flag
// from the game and check the index the probe reports for it.
//
// Nothing here knows a guest address; it is all
// rexmod/eternalsonata_flag_api.h.

#include <rex/system/mod_plugin.h>

#include <atomic>
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

#include <rexmod/eternalsonata_flag_api.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

constexpr size_t kMaxEventLines = 256;
constexpr int kFlagBytes = ETERNALSONATA_FLAG_BYTES;

// Listing every set flag would be thousands of rows late in the game, and the
// list is only there to eyeball; past this the table says how many it hid.
constexpr int kMaxListedFlags = 512;

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

// Every entry point is resolved out of the host executable at load time and
// null-checked before use, so this mod still loads (and says exactly what is
// missing) on a host too old to have the Flag API.
struct FlagApi {
  EternalSonataFlagsAbiVersionFn abi_version = nullptr;
  EternalSonataAreFlagsAvailableFn available = nullptr;
  EternalSonataGetFlagCountFn count = nullptr;
  EternalSonataGetFlagFn get = nullptr;
  EternalSonataSetFlagFn set = nullptr;
  EternalSonataCopyFlagsFn copy = nullptr;
  EternalSonataResetFlagsFn reset = nullptr;
  EternalSonataGetScenarioCounterFn get_scenario = nullptr;
  EternalSonataSetScenarioCounterFn set_scenario = nullptr;

  bool ok() const { return abi_version && get && set; }
};

FlagApi LoadFlagApi() {
  FlagApi api;
  Resolve(api.abi_version, "EternalSonataFlagsAbiVersion");
  Resolve(api.available, "EternalSonataAreFlagsAvailable");
  Resolve(api.count, "EternalSonataGetFlagCount");
  Resolve(api.get, "EternalSonataGetFlag");
  Resolve(api.set, "EternalSonataSetFlag");
  Resolve(api.copy, "EternalSonataCopyFlags");
  Resolve(api.reset, "EternalSonataResetFlags");
  Resolve(api.get_scenario, "EternalSonataGetScenarioCounter");
  Resolve(api.set_scenario, "EternalSonataSetScenarioCounter");
  return api;
}

const char* ErrorName(int value) {
  switch (value) {
    case ETERNALSONATA_FLAG_OK:
      return "OK";
    case ETERNALSONATA_FLAG_ERR_UNAVAILABLE:
      return "ERR_UNAVAILABLE";
    case ETERNALSONATA_FLAG_ERR_INVALID_INDEX:
      return "ERR_INVALID_INDEX";
    case ETERNALSONATA_FLAG_ERR_INVALID_VALUE:
      return "ERR_INVALID_VALUE";
    default:
      return value > 0 ? "(value)" : "ERR_?";
  }
}

int CountSetBits(const std::vector<uint8_t>& bank) {
  int total = 0;
  for (uint8_t byte : bank) {
    for (int bit = 0; bit < 8; ++bit) {
      total += (byte >> bit) & 1;
    }
  }
  return total;
}

class FlagProbeDialog : public rex::ui::ImGuiDialog {
 public:
  FlagProbeDialog(rex::ui::ImGuiDrawer* drawer, rex::Runtime* runtime)
      : ImGuiDialog(drawer), runtime_(runtime), api_(LoadFlagApi()) {
    bank_.assign(kFlagBytes, 0);
    snapshot_.assign(kFlagBytes, 0);
    watch_.assign(kFlagBytes, 0);

    rex::ui::RegisterBind(
        "bind_flag_probe", "F11", "Toggle the flag API probe",
        [this] { visible_ = !visible_; }, [this] { return visible_; },
        "Flag API Probe");

    SubscribeToFlagEvents();
  }

  ~FlagProbeDialog() override { rex::ui::UnregisterBind("bind_flag_probe"); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    (void)io;

    // Polled whether or not the window is open, so watch mode keeps working
    // (and keeps logging) with the overlay closed.
    Poll();

    if (!visible_) {
      return;
    }

    ImGui::SetNextWindowSize(ImVec2(900, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Flag API Probe", &visible_, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::End();
      return;
    }

    DrawHostSection();
    ImGui::Separator();
    DrawBankSection();
    ImGui::Separator();
    DrawSingleFlagSection();
    ImGui::Separator();
    DrawDiffSection();
    ImGui::Separator();
    DrawScenarioSection();
    ImGui::Separator();
    DrawDangerSection();
    ImGui::Separator();
    DrawErrorSection();
    ImGui::Separator();
    DrawEventSection();

    ImGui::End();
  }

 private:
  // -------------------------------------------------------------------------
  // Sections
  // -------------------------------------------------------------------------

  void DrawHostSection() {
    if (!api_.ok()) {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                         "This host does not export the Flag API.");
      ImGui::TextWrapped(
          "EternalSonataGetFlag / EternalSonataSetFlag were not found in the "
          "executable. Nothing below can be tested.");
      return;
    }

    ImGui::Text("Flags ABI version: %u (this mod was built against %u)", api_.abi_version(),
                ETERNALSONATA_FLAGS_ABI_VERSION);
    if (api_.abi_version() < ETERNALSONATA_FLAGS_ABI_VERSION) {
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                         "The host predates this mod's ABI version; the API surface may differ.");
    }

    std::string missing;
    auto note = [&missing](bool present, const char* name) {
      if (present) {
        return;
      }
      if (!missing.empty()) {
        missing += ", ";
      }
      missing += name;
    };
    note(api_.available != nullptr, "AreFlagsAvailable");
    note(api_.count != nullptr, "GetFlagCount");
    note(api_.copy != nullptr, "CopyFlags");
    note(api_.reset != nullptr, "ResetFlags");
    note(api_.get_scenario != nullptr, "GetScenarioCounter");
    note(api_.set_scenario != nullptr, "SetScenarioCounter");
    if (!missing.empty()) {
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Not exported by this host: %s",
                         missing.c_str());
    }

    if (available_) {
      ImGui::Text("Bank available: yes.  Flags: %d.  Bytes: %d.", flag_count_, kFlagBytes);
    } else {
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                         "Bank not readable yet (the title screen has not brought it up).");
    }

    if (!runtime_ || !runtime_->mod_registry()) {
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                         "No mod registry: the flag events cannot be received.");
    }
  }

  void DrawBankSection() {
    if (!api_.ok()) {
      return;
    }
    if (!api_.copy) {
      ImGui::TextDisabled("CopyFlags is not exported; the bank view is unavailable.");
      return;
    }

    ImGui::Text("Flags set: %d of %d", set_count_, flag_count_);
    ImGui::SameLine();
    ImGui::Checkbox("Show the set flags", &show_set_list_);

    if (!show_set_list_ || !bank_valid_) {
      return;
    }

    if (ImGui::BeginChild("setflags", ImVec2(0.0f, 140.0f), true)) {
      int listed = 0;
      std::string line;
      for (int byte = 0; byte < kFlagBytes && listed < kMaxListedFlags; ++byte) {
        if (!bank_[static_cast<size_t>(byte)]) {
          continue;
        }
        for (int bit = 0; bit < 8 && listed < kMaxListedFlags; ++bit) {
          if (!((bank_[static_cast<size_t>(byte)] >> bit) & 1)) {
            continue;
          }
          char item[32];
          std::snprintf(item, sizeof(item), "%5d ", byte * 8 + bit);
          line += item;
          ++listed;
          if (listed % 12 == 0) {
            ImGui::TextUnformatted(line.c_str());
            line.clear();
          }
        }
      }
      if (!line.empty()) {
        ImGui::TextUnformatted(line.c_str());
      }
      if (set_count_ > listed) {
        ImGui::TextDisabled("... and %d more", set_count_ - listed);
      }
    }
    ImGui::EndChild();
  }

  void DrawSingleFlagSection() {
    if (!api_.ok()) {
      return;
    }

    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputInt("Flag index", &single_index_);

    const int value = api_.get(single_index_);
    if (value < 0) {
      ImGui::Text("Get(%d) -> %s (%d)", single_index_, ErrorName(value), value);
    } else {
      ImGui::Text("Get(%d) -> %d   (byte %d, bit %d)", single_index_, value, single_index_ >> 3,
                  single_index_ & 7);
    }

    if (ImGui::Button("Set 1")) {
      last_write_ = WriteResult("SetFlag(1)", api_.set(single_index_, 1));
    }
    ImGui::SameLine();
    if (ImGui::Button("Set 0")) {
      last_write_ = WriteResult("SetFlag(0)", api_.set(single_index_, 0));
    }
    ImGui::SameLine();
    if (ImGui::Button("Toggle")) {
      const int current = api_.get(single_index_);
      if (current < 0) {
        last_write_ = WriteResult("SetFlag(toggle)", current);
      } else {
        last_write_ = WriteResult("SetFlag(toggle)", api_.set(single_index_, current ? 0 : 1));
      }
    }

    if (!last_write_.empty()) {
      ImGui::TextDisabled("%s", last_write_.c_str());
    }
  }

  void DrawDiffSection() {
    if (!api_.ok() || !api_.copy) {
      return;
    }

    ImGui::TextDisabled(
        "Snapshot, do the thing in game, then Diff: the flags that moved are the ones "
        "that event owns.");

    if (ImGui::Button("Take snapshot")) {
      snapshot_ = bank_;
      have_snapshot_ = bank_valid_;
      diff_lines_.clear();
      diff_lines_.emplace_back(have_snapshot_ ? "snapshot taken"
                                              : "snapshot NOT taken: bank unreadable");
    }
    ImGui::SameLine();
    if (ImGui::Button("Diff now") && have_snapshot_) {
      BuildDiff();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Watch (log every change as it happens)", &watching_);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Compares the bank every frame and appends each changed flag to the\n"
          "event log below, marked \"observed\". This is the cross-check for\n"
          "the bus events, and for the bit numbering.");
    }

    if (ImGui::BeginChild("diff", ImVec2(0.0f, 120.0f), true)) {
      for (const auto& line : diff_lines_) {
        ImGui::TextUnformatted(line.c_str());
      }
    }
    ImGui::EndChild();
  }

  void DrawScenarioSection() {
    if (!api_.ok() || !api_.get_scenario) {
      return;
    }

    if (scenario_ < 0) {
      ImGui::Text("Scenario counter: %s (%d)", ErrorName(scenario_), scenario_);
    } else {
      ImGui::Text("Scenario counter: %d", scenario_);
    }

    if (!api_.set_scenario) {
      return;
    }
    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputInt("##scenario", &scenario_input_);
    ImGui::SameLine();
    if (ImGui::Button("Write scenario counter")) {
      last_write_ = WriteResult("SetScenarioCounter", api_.set_scenario(scenario_input_));
    }
  }

  void DrawDangerSection() {
    if (!api_.ok() || !api_.reset) {
      return;
    }

    ImGui::Checkbox("Arm reset", &reset_armed_);
    ImGui::SameLine();
    ImGui::BeginDisabled(!reset_armed_);
    if (ImGui::Button("Reset ALL flags")) {
      last_write_ = WriteResult("ResetFlags", api_.reset());
      reset_armed_ = false;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("(clears the whole bank: the scripts forget the entire playthrough)");
  }

  void DrawErrorSection() {
    if (!api_.ok()) {
      return;
    }
    ImGui::TextDisabled("Error paths");

    if (ImGui::Button("Get(-1)")) {
      last_error_ = ErrorResult("GetFlag(-1)", api_.get(-1));
    }
    ImGui::SameLine();
    if (ImGui::Button("Get(count)")) {
      last_error_ = ErrorResult("GetFlag(count)", api_.get(flag_count_));
    }
    ImGui::SameLine();
    if (ImGui::Button("Set(0, 2)")) {
      last_error_ = ErrorResult("SetFlag(0, 2)", api_.set(0, 2));
    }
    ImGui::SameLine();
    if (ImGui::Button("Set(count, 1)")) {
      last_error_ = ErrorResult("SetFlag(count, 1)", api_.set(flag_count_, 1));
    }

    if (api_.copy) {
      uint8_t scratch[8];
      if (ImGui::Button("CopyFlags(null)")) {
        last_error_ = ErrorResult("CopyFlags(null, 0, 8)", api_.copy(nullptr, 0, 8));
      }
      ImGui::SameLine();
      if (ImGui::Button("CopyFlags past the end")) {
        last_error_ =
            ErrorResult("CopyFlags(dst, bytes-4, 8)", api_.copy(scratch, kFlagBytes - 4, 8));
      }
      ImGui::SameLine();
      if (ImGui::Button("CopyFlags(-1)")) {
        last_error_ = ErrorResult("CopyFlags(dst, -1, 8)", api_.copy(scratch, -1, 8));
      }
    }

    if (api_.set_scenario) {
      if (ImGui::Button("SetScenarioCounter(-1)")) {
        last_error_ = ErrorResult("SetScenarioCounter(-1)", api_.set_scenario(-1));
      }
      ImGui::SameLine();
      if (ImGui::Button("SetScenarioCounter(65536)")) {
        last_error_ = ErrorResult("SetScenarioCounter(65536)", api_.set_scenario(65536));
      }
    }

    if (!last_error_.empty()) {
      ImGui::TextDisabled("%s", last_error_.c_str());
    }
  }

  void DrawEventSection() {
    if (!api_.ok()) {
      return;
    }

    ImGui::Text("flag.changed on the bus: %d   bulkchanged: %d   scenario.changed: %d",
                event_count_.load(), bulk_count_.load(), scenario_event_count_.load());
    ImGui::Text("Observed by polling: %d changes", observed_changes_);
    if (!watching_) {
      ImGui::TextDisabled("Turn on Watch to compare the bus against the poll.");
    } else if (event_count_.load() == observed_changes_) {
      ImGui::TextDisabled("The bus and the poll agree.");
    } else {
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                         "The bus and the poll DISAGREE. (A load is adopted silently, and a "
                         "big change publishes bulkchanged instead, so press Reset counters "
                         "after either.)");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset counters")) {
      observed_changes_ = 0;
      event_count_.store(0);
      bulk_count_.store(0);
      scenario_event_count_.store(0);
      std::lock_guard<std::mutex> lock(events_mutex_);
      events_.clear();
    }

    if (ImGui::BeginChild("events", ImVec2(0.0f, 180.0f), true)) {
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

    flag_count_ = api_.count ? api_.count() : ETERNALSONATA_FLAG_COUNT;
    available_ = api_.available ? api_.available() != 0 : true;
    scenario_ = api_.get_scenario ? api_.get_scenario() : -1;

    bank_valid_ = false;
    if (api_.copy && api_.copy(bank_.data(), 0, kFlagBytes) == ETERNALSONATA_FLAG_OK) {
      bank_valid_ = true;
      set_count_ = CountSetBits(bank_);
    }

    if (watching_ && bank_valid_) {
      ObserveTransitions();
    } else if (bank_valid_) {
      // Keep the watch baseline current while watching is off, so turning it
      // on does not report everything that happened in the meantime.
      watch_ = bank_;
      have_watch_ = true;
    }
  }

  void ObserveTransitions() {
    if (!have_watch_) {
      watch_ = bank_;
      have_watch_ = true;
      return;
    }

    for (int byte = 0; byte < kFlagBytes; ++byte) {
      const uint8_t diff = bank_[static_cast<size_t>(byte)] ^ watch_[static_cast<size_t>(byte)];
      if (!diff) {
        continue;
      }
      for (int bit = 0; bit < 8; ++bit) {
        if (!((diff >> bit) & 1)) {
          continue;
        }
        const int index = byte * 8 + bit;
        const int value = (bank_[static_cast<size_t>(byte)] >> bit) & 1;
        ++observed_changes_;
        char line[128];
        std::snprintf(line, sizeof(line), "observed  flag %d = %d", index, value);
        PushEvent(line);
      }
    }
    watch_ = bank_;
  }

  void BuildDiff() {
    diff_lines_.clear();
    if (!bank_valid_) {
      diff_lines_.emplace_back("bank unreadable");
      return;
    }

    int changes = 0;
    for (int byte = 0; byte < kFlagBytes; ++byte) {
      const uint8_t diff =
          bank_[static_cast<size_t>(byte)] ^ snapshot_[static_cast<size_t>(byte)];
      if (!diff) {
        continue;
      }
      for (int bit = 0; bit < 8; ++bit) {
        if (!((diff >> bit) & 1)) {
          continue;
        }
        const int index = byte * 8 + bit;
        const int value = (bank_[static_cast<size_t>(byte)] >> bit) & 1;
        ++changes;
        if (diff_lines_.size() < kMaxEventLines) {
          char line[128];
          std::snprintf(line, sizeof(line), "flag %d: %d -> %d", index, value ? 0 : 1, value);
          diff_lines_.emplace_back(line);
        }
      }
    }
    if (!changes) {
      diff_lines_.emplace_back("no flag changed since the snapshot");
    } else {
      char summary[64];
      std::snprintf(summary, sizeof(summary), "%d flag(s) changed", changes);
      diff_lines_.emplace_back(summary);
    }
  }

  std::string WriteResult(const char* name, int result) {
    char buffer[192];
    std::snprintf(buffer, sizeof(buffer), "%s returned %s (%d)", name, ErrorName(result),
                  result);
    return buffer;
  }

  std::string ErrorResult(const char* name, int result) {
    char buffer[192];
    std::snprintf(buffer, sizeof(buffer), "%s -> %s (%d)", name, ErrorName(result), result);
    return buffer;
  }

  // -------------------------------------------------------------------------
  // Events
  // -------------------------------------------------------------------------

  void SubscribeToFlagEvents() {
    if (!runtime_) {
      return;
    }
    auto* registry = runtime_->mod_registry();
    if (!registry) {
      return;
    }

    registry->Subscribe(ETERNALSONATA_FLAG_EVENT_CHANGED,
                        [this](const rex::system::ModRegistry::EventPayload& payload) {
                          event_count_.fetch_add(1);
                          char line[128];
                          std::snprintf(line, sizeof(line), "event     flag %d = %d",
                                        static_cast<int>(payload.u64),
                                        static_cast<int>(payload.f64));
                          PushEvent(line);
                        });

    registry->Subscribe(ETERNALSONATA_FLAG_EVENT_BULK_CHANGED,
                        [this](const rex::system::ModRegistry::EventPayload& payload) {
                          bulk_count_.fetch_add(1);
                          char line[128];
                          std::snprintf(line, sizeof(line), "event     bulk change: %d flags",
                                        static_cast<int>(payload.u64));
                          PushEvent(line);
                        });

    registry->Subscribe(ETERNALSONATA_FLAG_EVENT_SCENARIO_CHANGED,
                        [this](const rex::system::ModRegistry::EventPayload& payload) {
                          scenario_event_count_.fetch_add(1);
                          char line[128];
                          std::snprintf(line, sizeof(line), "event     scenario counter = %d",
                                        static_cast<int>(payload.f64));
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
  FlagApi api_;
  bool visible_ = false;

  // Snapshot of the host's answers, owned by the draw thread.
  std::vector<uint8_t> bank_;
  bool bank_valid_ = false;
  bool available_ = false;
  int flag_count_ = ETERNALSONATA_FLAG_COUNT;
  int set_count_ = 0;
  int scenario_ = 0;

  // Controls.
  bool show_set_list_ = false;
  int single_index_ = 0;
  int scenario_input_ = 0;
  bool reset_armed_ = false;
  std::string last_write_;
  std::string last_error_;

  // Manual snapshot/diff.
  std::vector<uint8_t> snapshot_;
  bool have_snapshot_ = false;
  std::vector<std::string> diff_lines_;

  // Watch mode, also the probe's own change counter.
  bool watching_ = false;
  std::vector<uint8_t> watch_;
  bool have_watch_ = false;
  int observed_changes_ = 0;

  // Events, written by the registry tick.
  std::mutex events_mutex_;
  std::deque<std::string> events_;
  std::atomic<int> event_count_{0};
  std::atomic<int> bulk_count_{0};
  std::atomic<int> scenario_event_count_{0};
};

class FlagProbeMod : public rex::system::IModPlugin {
 public:
  explicit FlagProbeMod(rex::Runtime* runtime) : runtime_(runtime) {}

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    dialog_ = std::make_unique<FlagProbeDialog>(drawer, runtime_);
  }

 private:
  rex::Runtime* runtime_ = nullptr;
  std::unique_ptr<FlagProbeDialog> dialog_;
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
  return new FlagProbeMod(ctx->runtime);
}
