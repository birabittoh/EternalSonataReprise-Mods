// photo_probe - an overlay (F7) that exercises every part of the game's photo
// album API, so the whole surface can be checked in one place while playing.
//
// It shows, live:
//
//   * the ABI version the running host implements, and which of the entry
//     points actually resolved out of it;
//   * EternalSonataIsPhotoAlbumAvailable / GetPhotoCount / GetPhotoCapacity,
//     each with its raw return value, so a negative "unavailable" is visible
//     rather than being flattened into a zero;
//   * EternalSonataGetPhotos, run in the same two steps a real caller uses --
//     a sizing call with (NULL, 0) first, then a fill into an array of that
//     size -- with both counts shown so a mismatch is obvious;
//   * every field of every photograph, including the six subject entries and
//     their four undecoded values, since those are what a run is supposed to
//     start naming;
//   * a per-photograph cross-check that GetPhoto(index) and
//     GetPhotoByRecord(record) return the same struct, and that
//     GetPhotoDevelopment(index) agrees with the struct's own field;
//   * EternalSonataDevelopPhoto and EternalSonataDevelopAllPhotos, with the
//     value the album reports immediately afterwards;
//   * a log of the album events off the mod registry bus.
//
// The last section is the one that actually tests the events rather than just
// displaying them. The probe polls the album itself every frame and notes when
// it *observes* a photograph appear or finish developing; the bus events are
// counted separately. If the two ever disagree, the host published the wrong
// number of events, and the panel says so instead of quietly looking fine.
// That is also what makes the save-and-reload check meaningful: a load must
// move the observed count without producing a single added event.
//
// Polling every frame is fine here. Unlike the save API's listing, nothing in
// the photo API touches the filesystem or runs guest code; every read is a
// guest-memory load.
//
// Nothing here knows a guest address; it is all
// rexmod/eternalsonata_photo_api.h, copied verbatim from the game repo's src/.

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

#include <rexmod/eternalsonata_photo_api.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

// How many event lines to keep. Taking a photograph and developing it are both
// rare, so this is a long history in practice.
constexpr size_t kMaxEventLines = 96;

// Every entry point is resolved out of the host executable at load time and
// null-checked before use, so this mod still loads (and says exactly what is
// missing) on a host too old to have the photo API.
struct PhotoApi {
  EternalSonataPhotoAbiVersionFn abi_version = nullptr;
  EternalSonataIsPhotoAlbumAvailableFn available = nullptr;
  EternalSonataGetPhotoCountFn count = nullptr;
  EternalSonataGetPhotoCapacityFn capacity = nullptr;
  EternalSonataGetPhotoFn get = nullptr;
  EternalSonataGetPhotoByRecordFn get_by_record = nullptr;
  EternalSonataGetPhotosFn get_all = nullptr;
  EternalSonataGetPhotoDevelopmentFn development = nullptr;
  EternalSonataDevelopPhotoFn develop = nullptr;
  EternalSonataDevelopAllPhotosFn develop_all = nullptr;

  bool ok() const { return abi_version && count && get; }
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

PhotoApi LoadPhotoApi() {
  PhotoApi api;
  Resolve(api.abi_version, "EternalSonataPhotoAbiVersion");
  Resolve(api.available, "EternalSonataIsPhotoAlbumAvailable");
  Resolve(api.count, "EternalSonataGetPhotoCount");
  Resolve(api.capacity, "EternalSonataGetPhotoCapacity");
  Resolve(api.get, "EternalSonataGetPhoto");
  Resolve(api.get_by_record, "EternalSonataGetPhotoByRecord");
  Resolve(api.get_all, "EternalSonataGetPhotos");
  Resolve(api.development, "EternalSonataGetPhotoDevelopment");
  Resolve(api.develop, "EternalSonataDevelopPhoto");
  Resolve(api.develop_all, "EternalSonataDevelopAllPhotos");
  return api;
}

const char* ErrorName(int value) {
  switch (value) {
    case ETERNALSONATA_PHOTO_ERR_UNAVAILABLE:
      return "ERR_UNAVAILABLE";
    case ETERNALSONATA_PHOTO_ERR_NO_SUCH_PHOTO:
      return "ERR_NO_SUCH_PHOTO";
    case ETERNALSONATA_PHOTO_ERR_INVALID_ARGUMENT:
      return "ERR_INVALID_ARGUMENT";
    default:
      return "ERR_?";
  }
}

const char* RankName(int rank) {
  switch (rank) {
    case ETERNALSONATA_PHOTO_RANK_BEST:
      return "best";
    case ETERNALSONATA_PHOTO_RANK_MIDDLE:
      return "middle";
    case ETERNALSONATA_PHOTO_RANK_WORST:
      return "worst";
    default:
      return "?";
  }
}

// A tri-state answer from the API: >= 0 is the value, < 0 is one of the
// ETERNALSONATA_PHOTO_ERR_* codes. Both are worth showing verbatim.
std::string Answer(int value, const char* yes, const char* no) {
  char buffer[128];
  if (value < 0) {
    std::snprintf(buffer, sizeof(buffer), "%s (%d)", ErrorName(value), value);
  } else if (yes && no) {
    std::snprintf(buffer, sizeof(buffer), "%s (%d)", value ? yes : no, value);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%d", value);
  }
  return buffer;
}

bool SameStruct(const EternalSonataPhoto& a, const EternalSonataPhoto& b) {
  return std::memcmp(&a, &b, sizeof(EternalSonataPhoto)) == 0;
}

class PhotoProbeDialog : public rex::ui::ImGuiDialog {
 public:
  PhotoProbeDialog(rex::ui::ImGuiDrawer* drawer, rex::Runtime* runtime)
      : ImGuiDialog(drawer), runtime_(runtime), api_(LoadPhotoApi()) {
    rex::ui::RegisterBind(
        "bind_photo_probe", "F7", "Toggle the photo API probe",
        [this] { visible_ = !visible_; }, [this] { return visible_; }, "Photo API Probe");

    SubscribeToPhotoEvents();
  }

  ~PhotoProbeDialog() override { rex::ui::UnregisterBind("bind_photo_probe"); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    (void)io;

    // Polled whether or not the window is open, so a photograph taken with the
    // overlay closed is still counted and can still be compared against the
    // events that were published for it.
    Poll();

    if (!visible_) {
      return;
    }

    ImGui::SetNextWindowSize(ImVec2(720, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Photo API Probe", &visible_, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::End();
      return;
    }

    DrawHostSection();
    ImGui::Separator();
    DrawAlbumSection();
    ImGui::Separator();
    DrawPhotoTable();
    ImGui::Separator();
    DrawDetailSection();
    ImGui::Separator();
    DrawEventSection();

    ImGui::End();
  }

 private:
  // Which of the API actually resolved. A host without the photo API at all is
  // the first thing to rule out when something below reads wrong.
  void DrawHostSection() {
    if (!api_.ok()) {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                         "This host does not export the photo API.");
      ImGui::TextWrapped(
          "EternalSonataGetPhotoCount / EternalSonataGetPhoto were not found in "
          "the executable. Nothing below can be tested.");
      return;
    }

    ImGui::Text("Photo ABI version: %u (this mod was built against %u)", api_.abi_version(),
                ETERNALSONATA_PHOTO_ABI_VERSION);

    const char* missing[8];
    int missing_count = 0;
    if (!api_.available) missing[missing_count++] = "IsPhotoAlbumAvailable";
    if (!api_.capacity) missing[missing_count++] = "GetPhotoCapacity";
    if (!api_.get_by_record) missing[missing_count++] = "GetPhotoByRecord";
    if (!api_.get_all) missing[missing_count++] = "GetPhotos";
    if (!api_.development) missing[missing_count++] = "GetPhotoDevelopment";
    if (!api_.develop) missing[missing_count++] = "DevelopPhoto";
    if (!api_.develop_all) missing[missing_count++] = "DevelopAllPhotos";
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
                         "No mod registry: the album events cannot be received.");
    }
  }

  void DrawAlbumSection() {
    if (!api_.ok()) {
      return;
    }

    if (api_.available) {
      ImGui::Text("IsPhotoAlbumAvailable: %s", Answer(api_.available(), "yes", "no").c_str());
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "0 before the title screen has finished coming up. Every read below\n"
            "answers ERR_UNAVAILABLE while this is 0.");
      }
    }
    ImGui::Text("GetPhotoCount:         %s", Answer(count_, nullptr, nullptr).c_str());
    if (api_.capacity) {
      const int capacity = api_.capacity();
      ImGui::Text("GetPhotoCapacity:      %d%s", capacity,
                  capacity == ETERNALSONATA_PHOTO_CAPACITY ? "" : "  (disagrees with the header!)");
    }

    ImGui::Text("GetPhotos sizing call returned %d, fill returned %d", sizing_count_, fill_count_);
    if (sizing_count_ >= 0 && fill_count_ >= 0 && sizing_count_ != fill_count_) {
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                         "The two disagree: the album changed between the calls, or the "
                         "sizing call is wrong.");
    }
  }

  void DrawPhotoTable() {
    if (!api_.ok()) {
      return;
    }
    if (photos_.empty()) {
      ImGui::TextDisabled("The album is empty. Take a photograph with the camera.");
      return;
    }

    if (ImGui::BeginTable("photos", 11,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Idx", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Rec", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Flags", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Score");
      ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Rank", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Price");
      ImGui::TableSetupColumn("Taken");
      ImGui::TableSetupColumn("Devel");
      ImGui::TableSetupColumn("Done", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Subj", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableHeadersRow();

      for (size_t i = 0; i < photos_.size(); ++i) {
        const EternalSonataPhoto& photo = photos_[i];
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        char label[16];
        std::snprintf(label, sizeof(label), "%d", photo.index);
        if (ImGui::Selectable(label, selected_index_ == photo.index,
                              ImGuiSelectableFlags_SpanAllColumns)) {
          selected_index_ = photo.index;
        }

        ImGui::TableNextColumn();
        ImGui::Text("%d", photo.record);
        ImGui::TableNextColumn();
        if (photo.is_blank) {
          ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%08X blank", photo.flags);
        } else {
          ImGui::Text("%08X", photo.flags);
        }
        ImGui::TableNextColumn();
        ImGui::Text("%.4f", photo.score);
        ImGui::TableNextColumn();
        ImGui::Text("%d", photo.score_percent);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(RankName(photo.rank));
        ImGui::TableNextColumn();
        ImGui::Text("%.4f", photo.price_scale);
        ImGui::TableNextColumn();
        ImGui::Text("t=%u p=%d", photo.taken_time, photo.taken_progress);
        ImGui::TableNextColumn();
        ImGui::Text("%.4f", photo.development);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(photo.is_developed ? "yes" : "no");
        ImGui::TableNextColumn();
        ImGui::Text("%d", photo.subject_count);
      }
      ImGui::EndTable();
    }

    ImGui::TextDisabled(
        "Score and %% should match the number the Photos tab prints; rank should match its icon.");
  }

  // Everything that needs one photograph picked: the two cross-checks, the six
  // subject entries, and the two write calls.
  void DrawDetailSection() {
    if (!api_.ok()) {
      return;
    }

    const EternalSonataPhoto* selected = nullptr;
    for (const EternalSonataPhoto& photo : photos_) {
      if (photo.index == selected_index_) {
        selected = &photo;
        break;
      }
    }
    if (!selected) {
      ImGui::TextDisabled("Select a photograph above for its subjects and the write calls.");
      return;
    }

    ImGui::Text("Photograph %d (record slot %d)", selected->index, selected->record);

    // Same photograph by its other key. These must be identical: the display
    // index moves as photographs are added and sold, the record slot does not.
    if (api_.get_by_record) {
      EternalSonataPhoto by_record{};
      const int result = api_.get_by_record(selected->record, &by_record);
      if (result != ETERNALSONATA_PHOTO_OK) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "GetPhotoByRecord(%d) failed: %s",
                           selected->record, ErrorName(result));
      } else if (!SameStruct(*selected, by_record)) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "GetPhotoByRecord(%d) disagrees with GetPhoto(%d).", selected->record,
                           selected->index);
      } else {
        ImGui::TextDisabled("GetPhotoByRecord agrees with GetPhoto.");
      }
    }

    // The shorthand accessor against the field it is shorthand for.
    if (api_.development) {
      const float shorthand = api_.development(selected->index);
      if (shorthand != selected->development) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "GetPhotoDevelopment returned %.6f, the struct says %.6f.", shorthand,
                           selected->development);
      } else {
        ImGui::TextDisabled("GetPhotoDevelopment agrees with the struct (%.6f).", shorthand);
      }
    }

    ImGui::Text("image_handle: %08X", selected->image_handle);

    if (ImGui::BeginTable("subjects", 7,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Attr", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("values[0]");
      ImGui::TableSetupColumn("values[1]");
      ImGui::TableSetupColumn("values[2]");
      ImGui::TableSetupColumn("values[3]");
      ImGui::TableHeadersRow();

      for (int i = 0; i < ETERNALSONATA_PHOTO_SUBJECT_SLOTS; ++i) {
        const EternalSonataPhotoSubject& subject = selected->subjects[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%d", i);
        ImGui::TableNextColumn();
        if (subject.id < 0) {
          ImGui::TextDisabled("empty");
        } else {
          ImGui::Text("%d", subject.id);
        }
        ImGui::TableNextColumn();
        ImGui::Text("%d", subject.attribute);
        for (int v = 0; v < 4; ++v) {
          ImGui::TableNextColumn();
          ImGui::Text("%d (%08X)", subject.values[v],
                      static_cast<unsigned>(subject.values[v]));
        }
      }
      ImGui::EndTable();
    }
    ImGui::TextDisabled(
        "The four values are undecoded; this table is where naming them starts.");

    if (api_.develop) {
      if (ImGui::Button("DevelopPhoto(selected)")) {
        const int result = api_.develop(selected->index);
        char line[160];
        std::snprintf(line, sizeof(line), "DevelopPhoto(%d) returned %s", selected->index,
                      result == ETERNALSONATA_PHOTO_OK ? "OK" : ErrorName(result));
        last_write_result_ = line;
      }
      ImGui::SameLine();
    }
    if (api_.develop_all) {
      if (ImGui::Button("DevelopAllPhotos()")) {
        const int result = api_.develop_all();
        char line[160];
        if (result < 0) {
          std::snprintf(line, sizeof(line), "DevelopAllPhotos() returned %s", ErrorName(result));
        } else {
          std::snprintf(line, sizeof(line), "DevelopAllPhotos() changed %d photograph(s)", result);
        }
        last_write_result_ = line;
      }
    }
    if (!last_write_result_.empty()) {
      ImGui::TextUnformatted(last_write_result_.c_str());
      ImGui::TextDisabled(
          "The album should draw the photograph clean immediately, and a developed event "
          "should follow on the next frame.");
    }
  }

  void DrawEventSection() {
    ImGui::Text("Album events: %d added, %d developed", event_added_.load(),
                event_developed_.load());
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
      std::lock_guard<std::mutex> lock(events_mutex_);
      events_.clear();
    }

    // The actual test: what the probe saw happen, against what the host said
    // happened. These are counted from opposite ends - one by polling the
    // album, one off the bus - so a disagreement is a real defect.
    ImGui::Text("Probe observed:  %d added, %d developed", observed_added_, observed_developed_);
    const bool added_ok = observed_added_ == event_added_.load();
    const bool developed_ok = observed_developed_ == event_developed_.load();
    if (added_ok && developed_ok) {
      ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Events match what the probe observed.");
    } else {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                         "Mismatch: the host published %s%s%s than the probe observed.",
                         added_ok ? "" : "the wrong number of added events",
                         (!added_ok && !developed_ok) ? " and " : "",
                         developed_ok ? "" : "the wrong number of developed events");
    }
    ImGui::TextDisabled(
        "Loading a save must move neither counter: the restored album is adopted silently.");

    std::vector<std::string> lines;
    {
      std::lock_guard<std::mutex> lock(events_mutex_);
      lines.assign(events_.begin(), events_.end());
    }
    if (lines.empty()) {
      ImGui::TextDisabled("(nothing yet)");
      return;
    }

    if (ImGui::BeginChild("events", ImVec2(0, 180), true)) {
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
  void Poll() {
    if (!api_.ok()) {
      return;
    }

    count_ = api_.count();
    photos_.clear();
    sizing_count_ = -1;
    fill_count_ = -1;

    if (api_.get_all) {
      sizing_count_ = api_.get_all(nullptr, 0);
      if (sizing_count_ > 0) {
        photos_.resize(static_cast<size_t>(sizing_count_));
        fill_count_ = api_.get_all(photos_.data(), sizing_count_);
        if (fill_count_ >= 0) {
          photos_.resize(static_cast<size_t>(fill_count_));
        } else {
          photos_.clear();
        }
      } else {
        fill_count_ = sizing_count_;
      }
    } else {
      // A host without GetPhotos still has the single-photograph read, and the
      // rest of the window works off that.
      for (int i = 0; i < count_; ++i) {
        EternalSonataPhoto photo{};
        if (api_.get(i, &photo) == ETERNALSONATA_PHOTO_OK) {
          photos_.push_back(photo);
        }
      }
    }

    ObserveTransitions();
  }

  // The probe's own view of what changed, kept independently of the bus so the
  // two can be compared. Keyed by record slot, which is the identity that does
  // not move when the album is reordered.
  void ObserveTransitions() {
    std::array<bool, ETERNALSONATA_PHOTO_CAPACITY> present{};
    std::array<bool, ETERNALSONATA_PHOTO_CAPACITY> developed{};
    for (const EternalSonataPhoto& photo : photos_) {
      if (photo.record < 0 || photo.record >= ETERNALSONATA_PHOTO_CAPACITY) {
        continue;
      }
      const auto slot = static_cast<size_t>(photo.record);
      present[slot] = true;
      developed[slot] = photo.is_developed != 0;
    }

    if (!have_baseline_) {
      have_baseline_ = true;
      present_ = present;
      developed_ = developed;
      return;
    }

    // A load restores a whole album at once. That is not a sequence of
    // additions, and the host is not supposed to report it as one, so it is
    // adopted here too rather than counted - otherwise the comparison in the
    // event panel would fire on every load and mean nothing.
    int appeared = 0;
    for (size_t slot = 0; slot < present.size(); ++slot) {
      if (present[slot] && !present_[slot]) {
        ++appeared;
      }
    }
    const bool bulk_change = appeared > 1;

    for (size_t slot = 0; slot < present.size(); ++slot) {
      const bool is_new = present[slot] && !present_[slot];
      if (is_new && !bulk_change) {
        ++observed_added_;
        char line[160];
        std::snprintf(line, sizeof(line), "observed  added     record %zu", slot);
        PushEvent(line);
      }
      // A photograph that was already developed when it arrived never crossed
      // anything, so only a record that was present and undeveloped counts.
      if (present[slot] && present_[slot] && developed[slot] && !developed_[slot]) {
        ++observed_developed_;
        char line[160];
        std::snprintf(line, sizeof(line), "observed  developed record %zu", slot);
        PushEvent(line);
      }
    }
    if (bulk_change) {
      PushEvent("observed  album restored (adopted, not counted as additions)");
    }

    present_ = present;
    developed_ = developed;
  }

  // The events arrive on the mod registry's frame tick, which is not this
  // thread, so everything they touch is either a mutex-guarded deque or an
  // atomic. Nothing here calls into ImGui.
  void SubscribeToPhotoEvents() {
    if (!runtime_) {
      return;
    }
    auto* registry = runtime_->mod_registry();
    if (!registry) {
      return;
    }

    registry->Subscribe(ETERNALSONATA_PHOTO_EVENT_ADDED,
                        [this](const rex::system::ModRegistry::EventPayload& payload) {
                          event_added_.fetch_add(1);
                          char line[160];
                          std::snprintf(line, sizeof(line), "event     added     record %llu, index %d",
                                        static_cast<unsigned long long>(payload.u64),
                                        static_cast<int>(payload.f64));
                          PushEvent(line);
                        });

    registry->Subscribe(ETERNALSONATA_PHOTO_EVENT_DEVELOPED,
                        [this](const rex::system::ModRegistry::EventPayload& payload) {
                          event_developed_.fetch_add(1);
                          char line[160];
                          std::snprintf(line, sizeof(line), "event     developed record %llu, index %d",
                                        static_cast<unsigned long long>(payload.u64),
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
  PhotoApi api_;
  bool visible_ = false;

  // Album snapshot, owned by the draw thread.
  std::vector<EternalSonataPhoto> photos_;
  int count_ = -1;
  int sizing_count_ = -1;
  int fill_count_ = -1;
  int selected_index_ = 0;
  std::string last_write_result_;

  // The probe's own transition counters, also draw-thread only.
  bool have_baseline_ = false;
  std::array<bool, ETERNALSONATA_PHOTO_CAPACITY> present_{};
  std::array<bool, ETERNALSONATA_PHOTO_CAPACITY> developed_{};
  int observed_added_ = 0;
  int observed_developed_ = 0;

  // Events, written by the registry tick.
  std::mutex events_mutex_;
  std::deque<std::string> events_;
  std::atomic<int> event_added_{0};
  std::atomic<int> event_developed_{0};
};

class PhotoProbeMod : public rex::system::IModPlugin {
 public:
  explicit PhotoProbeMod(rex::Runtime* runtime) : runtime_(runtime) {}

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    dialog_ = std::make_unique<PhotoProbeDialog>(drawer, runtime_);
  }

 private:
  rex::Runtime* runtime_ = nullptr;
  std::unique_ptr<PhotoProbeDialog> dialog_;
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
  return new PhotoProbeMod(ctx->runtime);
}
