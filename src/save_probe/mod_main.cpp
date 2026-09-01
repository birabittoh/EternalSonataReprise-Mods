// save_probe - an overlay (F6) that exercises every part of the game's save
// API, so the whole surface can be checked in one place while playing.
//
// It shows, live:
//
//   * the ABI version the running host implements, and which of the entry
//     points actually resolved out of it;
//   * EternalSonataCanSave / IsSaveInProgress / GetSaveState /
//     GetLastSaveSlot / GetSaveSlotCount, each with its raw return value, so a
//     negative "unavailable" is visible rather than being flattened into a no;
//   * EternalSonataGetSaveDirectory;
//   * EternalSonataListSaves, run in the same two steps a real caller uses --
//     a sizing call with (NULL, 0) first, then a fill into an array of that
//     size -- with both counts shown so a mismatch is obvious;
//   * a log of the save events off the mod registry bus.
//
// The listing is not refreshed every frame: EternalSonataListSaves walks the
// profile's save containers on the host filesystem, and doing that at frame
// rate would be I/O for nothing. It refreshes on demand, when the window is
// opened, and whenever a save completes.
//
// Nothing here knows a guest address; it is all
// rexmod/eternalsonata_save_api.h, copied verbatim from the game repo's src/.

#include <rex/system/mod_plugin.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
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

#include <rexmod/eternalsonata_save_api.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

// How many event lines to keep. The bus hands us one line per save, so this is
// a long history in practice.
constexpr size_t kMaxEventLines = 64;

// Every entry point is resolved out of the host executable at load time and
// null-checked before use, so this mod still loads (and says exactly what is
// missing) on a host too old to have the save API.
struct SaveApi {
  EternalSonataSaveAbiVersionFn abi_version = nullptr;
  EternalSonataCanSaveFn can_save = nullptr;
  EternalSonataSetSaveAlwaysAllowedFn set_always_allowed = nullptr;
  EternalSonataIsSaveAlwaysAllowedFn is_always_allowed = nullptr;
  EternalSonataIsSaveInProgressFn in_progress = nullptr;
  EternalSonataGetSaveStateFn state = nullptr;
  EternalSonataGetLastSaveSlotFn last_slot = nullptr;
  EternalSonataGetSaveSlotCountFn slot_count = nullptr;
  EternalSonataListSavesFn list = nullptr;
  EternalSonataGetSaveDirectoryFn directory = nullptr;

  bool ok() const { return abi_version && can_save && list; }
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

SaveApi LoadSaveApi() {
  SaveApi api;
  Resolve(api.abi_version, "EternalSonataSaveAbiVersion");
  Resolve(api.can_save, "EternalSonataCanSave");
  Resolve(api.set_always_allowed, "EternalSonataSetSaveAlwaysAllowed");
  Resolve(api.is_always_allowed, "EternalSonataIsSaveAlwaysAllowed");
  Resolve(api.in_progress, "EternalSonataIsSaveInProgress");
  Resolve(api.state, "EternalSonataGetSaveState");
  Resolve(api.last_slot, "EternalSonataGetLastSaveSlot");
  Resolve(api.slot_count, "EternalSonataGetSaveSlotCount");
  Resolve(api.list, "EternalSonataListSaves");
  Resolve(api.directory, "EternalSonataGetSaveDirectory");
  return api;
}

const char* StateName(int state) {
  switch (state) {
    case ETERNALSONATA_SAVE_STATE_NONE:
      return "none (no save attempted this run)";
    case ETERNALSONATA_SAVE_STATE_RUNNING:
      return "running";
    case ETERNALSONATA_SAVE_STATE_COMPLETED:
      return "completed";
    case ETERNALSONATA_SAVE_STATE_FAILED:
      return "failed";
    default:
      return "?";
  }
}

const char* FailureName(double reason) {
  const int code = static_cast<int>(reason);
  switch (code) {
    case ETERNALSONATA_SAVE_FAIL_NOT_STARTED:
      return "NOT_STARTED (the write never got off the ground)";
    case ETERNALSONATA_SAVE_FAIL_WRITE:
      return "WRITE (the write ran and reported failure)";
    default:
      return "unknown reason";
  }
}

// A tri-state answer from the API: >= 0 is the value, < 0 is one of the
// ETERNALSONATA_SAVE_ERR_* codes. Both are worth showing verbatim.
std::string Answer(int value, const char* yes, const char* no) {
  char buffer[128];
  if (value < 0) {
    const char* name = value == ETERNALSONATA_SAVE_ERR_UNAVAILABLE  ? "ERR_UNAVAILABLE"
                       : value == ETERNALSONATA_SAVE_ERR_INVALID_ARGUMENT
                           ? "ERR_INVALID_ARGUMENT"
                       : value == ETERNALSONATA_SAVE_ERR_IO ? "ERR_IO"
                                                            : "ERR_?";
    std::snprintf(buffer, sizeof(buffer), "%s (%d)", name, value);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%s (%d)", value ? yes : no, value);
  }
  return buffer;
}

std::string FormatTime(uint64_t unix_seconds) {
  if (!unix_seconds) {
    return "unknown";
  }
  const std::time_t t = static_cast<std::time_t>(unix_seconds);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buffer[32];
  if (!std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm)) {
    return "unknown";
  }
  return buffer;
}

class SaveProbeDialog : public rex::ui::ImGuiDialog {
 public:
  SaveProbeDialog(rex::ui::ImGuiDrawer* drawer, rex::Runtime* runtime)
      : ImGuiDialog(drawer), runtime_(runtime), api_(LoadSaveApi()) {
    rex::ui::RegisterBind(
        "bind_save_probe", "F6", "Toggle the save API probe",
        [this] {
          visible_ = !visible_;
          if (visible_) {
            RefreshSaves();
          }
        },
        [this] { return visible_; }, "Save API Probe");

    SubscribeToSaveEvents();
  }

  ~SaveProbeDialog() override { rex::ui::UnregisterBind("bind_save_probe"); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    (void)io;

    // A save can complete while the window is closed, and the listing it
    // produced should still be current the next time the window opens, so the
    // refresh is driven by the event rather than by visibility.
    if (refresh_pending_.exchange(false)) {
      RefreshSaves();
    }
    if (!visible_) {
      return;
    }

    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Save API Probe", &visible_, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::End();
      return;
    }

    DrawHostSection();
    ImGui::Separator();
    DrawStateSection();
    ImGui::Separator();
    DrawListSection();
    ImGui::Separator();
    DrawEventSection();

    ImGui::End();
  }

 private:
  // Which of the API actually resolved. A host without the save API at all is
  // the first thing to rule out when something below reads wrong.
  void DrawHostSection() {
    if (!api_.ok()) {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                         "This host does not export the save API.");
      ImGui::TextWrapped(
          "EternalSonataCanSave / EternalSonataListSaves were not found in the "
          "executable. Nothing below can be tested.");
      return;
    }

    ImGui::Text("Save ABI version: %u (this mod was built against %u)", api_.abi_version(),
                ETERNALSONATA_SAVE_ABI_VERSION);

    const char* missing[8];
    int missing_count = 0;
    if (!api_.in_progress) missing[missing_count++] = "IsSaveInProgress";
    if (!api_.state) missing[missing_count++] = "GetSaveState";
    if (!api_.last_slot) missing[missing_count++] = "GetLastSaveSlot";
    if (!api_.slot_count) missing[missing_count++] = "GetSaveSlotCount";
    if (!api_.directory) missing[missing_count++] = "GetSaveDirectory";
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
                         "No mod registry: the save events cannot be received.");
    }
  }

  // The five scalar reads, each with its raw value.
  void DrawStateSection() {
    if (!api_.ok()) {
      return;
    }

    ImGui::Text("CanSave:          %s", Answer(api_.can_save(), "yes", "no").c_str());
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "1 when the pause menu's Save entry is selectable, i.e. the party is\n"
          "at a save point (or a mod is forcing that flag). Walk onto and off a\n"
          "save point with this window open to watch it flip.");
    }

    // The one write in the API, and the thing save_anywhere is a switch for.
    // Toggling it here should make the pause menu's last entry, Save, stop
    // being greyed the next time the menu is opened, anywhere on the map.
    if (api_.set_always_allowed && api_.is_always_allowed) {
      bool forced = api_.is_always_allowed() != 0;
      if (ImGui::Checkbox("SetSaveAlwaysAllowed (force the menu's save gate)", &forced)) {
        const int result = api_.set_always_allowed(forced ? 1 : 0);
        if (result < 0) {
          last_toggle_error_ = Answer(result, "", "");
        } else {
          last_toggle_error_.clear();
        }
      }
      if (!last_toggle_error_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "  refused: %s",
                           last_toggle_error_.c_str());
      }
    }

    if (api_.in_progress) {
      ImGui::Text("IsSaveInProgress: %s", Answer(api_.in_progress(), "yes", "no").c_str());
    }
    if (api_.state) {
      const int state = api_.state();
      ImGui::Text("GetSaveState:     %s (%d)", StateName(state), state);
    }
    if (api_.last_slot) {
      const int slot = api_.last_slot();
      if (slot < 0) {
        ImGui::TextUnformatted("GetLastSaveSlot:  none yet (-1)");
      } else {
        ImGui::Text("GetLastSaveSlot:  %d", slot);
      }
    }
    if (api_.slot_count) {
      ImGui::Text("GetSaveSlotCount: %d", api_.slot_count());
    }
  }

  void DrawListSection() {
    if (!api_.ok()) {
      return;
    }

    if (ImGui::Button("Refresh listing")) {
      RefreshSaves();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(does filesystem I/O; not run every frame)");

    if (api_.directory) {
      ImGui::TextWrapped("Save directory: %s", save_directory_.c_str());
    }

    ImGui::Text("ListSaves sizing call returned %d, fill returned %d", sizing_count_, fill_count_);
    if (sizing_count_ >= 0 && fill_count_ >= 0 && sizing_count_ != fill_count_) {
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                         "The two disagree: a save was written between the calls, or the "
                         "sizing call is wrong.");
    }

    if (saves_.empty()) {
      ImGui::TextDisabled("No save containers on disk for this profile.");
      return;
    }

    if (ImGui::BeginTable("saves", 5,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Container", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Display name");
      ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableHeadersRow();

      for (const EternalSonataSaveSlot& save : saves_) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (save.slot < 0) {
          ImGui::TextDisabled("-");
        } else {
          ImGui::Text("%d", save.slot);
        }
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(save.file_name);
        ImGui::TableNextColumn();
        if (save.display_name[0]) {
          ImGui::TextUnformatted(save.display_name);
        } else {
          ImGui::TextDisabled("(empty)");
        }
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(FormatTime(save.modified_time).c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%llu", static_cast<unsigned long long>(save.size_bytes));
      }
      ImGui::EndTable();
    }
  }

  void DrawEventSection() {
    ImGui::Text("Save events (%d received)", event_count_.load());
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
      std::lock_guard<std::mutex> lock(events_mutex_);
      events_.clear();
    }

    ImGui::TextDisabled(
        "Save at a save point to see started, then completed or failed.");

    std::vector<std::string> lines;
    {
      std::lock_guard<std::mutex> lock(events_mutex_);
      lines.assign(events_.begin(), events_.end());
    }
    if (lines.empty()) {
      ImGui::TextDisabled("(nothing yet)");
      return;
    }

    if (ImGui::BeginChild("events", ImVec2(0, 160), true)) {
      for (const std::string& line : lines) {
        ImGui::TextUnformatted(line.c_str());
      }
      // Keep the newest line in view as they arrive.
      if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
        ImGui::SetScrollHereY(1.0f);
      }
    }
    ImGui::EndChild();
  }

  // The two-step call a real caller makes: ask for the count, then fill an
  // array of exactly that size. Both results are kept so the window can show
  // that they agree.
  void RefreshSaves() {
    saves_.clear();
    sizing_count_ = -1;
    fill_count_ = -1;

    if (!api_.list) {
      return;
    }

    sizing_count_ = api_.list(nullptr, 0);
    if (sizing_count_ > 0) {
      saves_.resize(static_cast<size_t>(sizing_count_));
      fill_count_ = api_.list(saves_.data(), sizing_count_);
      if (fill_count_ >= 0) {
        saves_.resize(static_cast<size_t>(fill_count_));
      } else {
        saves_.clear();
      }
    } else {
      fill_count_ = sizing_count_;
    }

    save_directory_ = "(not available)";
    if (api_.directory) {
      char path[1024];
      const int written = api_.directory(path, static_cast<int>(sizeof(path)));
      if (written >= 0) {
        save_directory_.assign(path, static_cast<size_t>(written));
      } else {
        save_directory_ = Answer(written, "", "");
      }
    }
  }

  // The events arrive on guest threads (the menu thread for started, the
  // content worker for completed/failed), never on this one, so everything
  // they touch is either a mutex-guarded deque or an atomic. Nothing here
  // calls into ImGui or does I/O; the refresh is only requested, and OnDraw
  // performs it on the draw thread.
  void SubscribeToSaveEvents() {
    if (!runtime_) {
      return;
    }
    auto* registry = runtime_->mod_registry();
    if (!registry) {
      return;
    }

    registry->Subscribe(ETERNALSONATA_SAVE_EVENT_STARTED,
                        [this](const rex::system::ModRegistry::EventPayload& payload) {
                          char line[160];
                          std::snprintf(line, sizeof(line), "started   slot %llu",
                                        static_cast<unsigned long long>(payload.u64));
                          PushEvent(line);
                        });

    registry->Subscribe(ETERNALSONATA_SAVE_EVENT_COMPLETED,
                        [this](const rex::system::ModRegistry::EventPayload& payload) {
                          char line[160];
                          std::snprintf(line, sizeof(line), "completed slot %llu",
                                        static_cast<unsigned long long>(payload.u64));
                          PushEvent(line);
                          // The slot on disk just changed; pick it up on the
                          // next frame rather than doing I/O on this thread.
                          refresh_pending_.store(true);
                        });

    registry->Subscribe(ETERNALSONATA_SAVE_EVENT_FAILED,
                        [this](const rex::system::ModRegistry::EventPayload& payload) {
                          char line[160];
                          std::snprintf(line, sizeof(line), "failed    slot %llu: %s",
                                        static_cast<unsigned long long>(payload.u64),
                                        FailureName(payload.f64));
                          PushEvent(line);
                        });
  }

  void PushEvent(const char* line) {
    event_count_.fetch_add(1);
    std::lock_guard<std::mutex> lock(events_mutex_);
    events_.emplace_back(line);
    while (events_.size() > kMaxEventLines) {
      events_.pop_front();
    }
  }

  rex::Runtime* runtime_ = nullptr;
  SaveApi api_;
  bool visible_ = false;

  // Listing, owned by the draw thread.
  std::vector<EternalSonataSaveSlot> saves_;
  std::string save_directory_ = "(not available)";
  int sizing_count_ = -1;
  int fill_count_ = -1;
  std::string last_toggle_error_;

  // Events, written by guest threads.
  std::mutex events_mutex_;
  std::deque<std::string> events_;
  std::atomic<int> event_count_{0};
  std::atomic<bool> refresh_pending_{false};
};

class SaveProbeMod : public rex::system::IModPlugin {
 public:
  explicit SaveProbeMod(rex::Runtime* runtime) : runtime_(runtime) {}

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    dialog_ = std::make_unique<SaveProbeDialog>(drawer, runtime_);
  }

 private:
  rex::Runtime* runtime_ = nullptr;
  std::unique_ptr<SaveProbeDialog> dialog_;
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
  return new SaveProbeMod(ctx->runtime);
}
