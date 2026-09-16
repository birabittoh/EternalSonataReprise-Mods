// music_probe - an overlay (F11) that exercises every part of the game's Music
// API, so the whole surface can be checked in one place while playing.
//
// It shows, live:
//
//   * the ABI version the running host implements, and which of the entry
//     points actually resolved out of it;
//   * IsMusicAvailable / GetMusicTrackCount / GetUnlockedMusicTrackCount and
//     GetMusicTabTrackCount for each tab plus the unlisted bucket, each with
//     its raw return value, so a negative "unavailable" is visible rather than
//     being flattened into a zero;
//   * GetMusicTracks run the way a real caller runs it: a sizing call with
//     (NULL, 0) first, then a fill into an array of that size, once for the
//     whole list and once per tab, with both counts shown;
//   * every field of every track, one row per track, plus the invariants a
//     track should always satisfy (id in range, title id == id, number ==
//     row + 1, tab in -1..2, reserved zeros, a listed track has a title) so a
//     violation is flagged instead of just drawn;
//   * cross-checks between the entry points: GetMusicTrack(id) must agree with
//     the GetMusicTracks fill byte for byte, IsMusicTrackUnlocked with the
//     struct field, GetUnlockedMusicTrackCount with a sum over the list, and
//     each tab's fill with the placement the whole-list fill reports;
//   * SetMusicTrackUnlocked and SetAllMusicTracksUnlocked, with the raw return
//     value shown, so the "how many changed" semantics are visible;
//   * PlayMusicTrack, which is the one entry point that runs guest code and so
//     answers QUEUED rather than OK when it is called from here;
//   * every error path the header declares, each on a button (id 0, id 67,
//     null `out`, negative `max`, a bad tab, playing a locked track, playing
//     one of the four tracks no tab lists) with the returned error code;
//   * a log of both music events off the mod registry bus, cross-checked
//     against what the probe itself observes by polling the list once a frame.
//
// The last section is the one that actually tests the events rather than just
// displaying them. The probe polls the tracks itself every frame and notes when
// it *observes* one unlock or lock; the bus events are counted separately. If
// the two ever disagree, the host published the wrong number of events, and the
// panel says so instead of quietly looking fine. That is also what makes a
// save-and-reload check meaningful: a load restores the flags without any
// event, so after one you press Reset baseline and the restored state is
// adopted, moving the observed count without a single added event.
//
// Nothing here knows a guest address; it is all
// rexmod/eternalsonata_music_api.h, copied verbatim from the game repo's src/.

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

#include <rexmod/eternalsonata_music_api.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

constexpr size_t kMaxEventLines = 128;
constexpr int kCount = ETERNALSONATA_MUSIC_TRACK_COUNT;

// The four ids that no tab lists and that have no title in the game's blob.
// Documented in the game repo's docs/music.md; checked here rather than
// trusted, since the API reads the tab table out of the image at runtime.
bool ExpectedUnlisted(int track_id) {
  return track_id == 2 || track_id == 6 || track_id == 51 || track_id == 66;
}

// Rows per tab, from byte_822FF594. Same deal: the API reads them, the probe
// asserts them.
constexpr int kExpectedTabRows[ETERNALSONATA_MUSIC_TAB_COUNT] = {22, 31, 9};

// Every entry point is resolved out of the host executable at load time and
// null-checked before use, so this mod still loads (and says exactly what is
// missing) on a host too old to have the Music API.
struct MusicApi {
  EternalSonataMusicAbiVersionFn abi_version = nullptr;
  EternalSonataIsMusicAvailableFn available = nullptr;
  EternalSonataGetMusicTrackCountFn count = nullptr;
  EternalSonataGetMusicTabTrackCountFn tab_count = nullptr;
  EternalSonataGetUnlockedMusicTrackCountFn unlocked_count = nullptr;
  EternalSonataGetMusicTrackFn get = nullptr;
  EternalSonataGetMusicTracksFn get_many = nullptr;
  EternalSonataIsMusicTrackUnlockedFn is_unlocked = nullptr;
  EternalSonataGetMusicTrackNameFn name = nullptr;
  EternalSonataSetMusicTrackUnlockedFn set_unlocked = nullptr;
  EternalSonataSetAllMusicTracksUnlockedFn set_all = nullptr;
  EternalSonataPlayMusicTrackFn play = nullptr;

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

MusicApi LoadMusicApi() {
  MusicApi api;
  Resolve(api.abi_version, "EternalSonataMusicAbiVersion");
  Resolve(api.available, "EternalSonataIsMusicAvailable");
  Resolve(api.count, "EternalSonataGetMusicTrackCount");
  Resolve(api.tab_count, "EternalSonataGetMusicTabTrackCount");
  Resolve(api.unlocked_count, "EternalSonataGetUnlockedMusicTrackCount");
  Resolve(api.get, "EternalSonataGetMusicTrack");
  Resolve(api.get_many, "EternalSonataGetMusicTracks");
  Resolve(api.is_unlocked, "EternalSonataIsMusicTrackUnlocked");
  Resolve(api.name, "EternalSonataGetMusicTrackName");
  Resolve(api.set_unlocked, "EternalSonataSetMusicTrackUnlocked");
  Resolve(api.set_all, "EternalSonataSetAllMusicTracksUnlocked");
  Resolve(api.play, "EternalSonataPlayMusicTrack");
  return api;
}

const char* ErrorName(int value) {
  switch (value) {
    case ETERNALSONATA_MUSIC_OK:
      return "OK";
    case ETERNALSONATA_MUSIC_QUEUED:
      return "QUEUED";
    case ETERNALSONATA_MUSIC_ERR_UNAVAILABLE:
      return "ERR_UNAVAILABLE";
    case ETERNALSONATA_MUSIC_ERR_NO_SUCH_TRACK:
      return "ERR_NO_SUCH_TRACK";
    case ETERNALSONATA_MUSIC_ERR_NOT_LISTED:
      return "ERR_NOT_LISTED";
    case ETERNALSONATA_MUSIC_ERR_LOCKED:
      return "ERR_LOCKED";
    case ETERNALSONATA_MUSIC_ERR_INVALID_ARGUMENT:
      return "ERR_INVALID_ARGUMENT";
    default:
      return "ERR_?";
  }
}

const char* TabName(int tab) {
  switch (tab) {
    case ETERNALSONATA_MUSIC_TAB_EVENT:
      return "event";
    case ETERNALSONATA_MUSIC_TAB_FIELD:
      return "field";
    case ETERNALSONATA_MUSIC_TAB_BATTLE:
      return "battle";
    default:
      return "none";
  }
}

// A tri-state answer from the API: >= 0 is the value, < 0 is one of the ERR_*
// codes. Both are worth showing verbatim.
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

size_t SlotOf(int track_id) { return static_cast<size_t>(track_id - ETERNALSONATA_MUSIC_TRACK_ID_MIN); }

class MusicProbeDialog : public rex::ui::ImGuiDialog {
 public:
  MusicProbeDialog(rex::ui::ImGuiDrawer* drawer, rex::Runtime* runtime)
      : ImGuiDialog(drawer), runtime_(runtime), api_(LoadMusicApi()) {
    rex::ui::RegisterBind(
        "bind_music_probe", "F11", "Toggle the music API probe", [this] { visible_ = !visible_; },
        [this] { return visible_; }, "Music API Probe");

    SubscribeToMusicEvents();
  }

  ~MusicProbeDialog() override { rex::ui::UnregisterBind("bind_music_probe"); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    (void)io;

    // Polled whether or not the window is open, so an unlock with the overlay
    // closed is still observed and can still be compared against the events
    // that were published for it.
    Poll();

    if (!visible_) {
      return;
    }

    ImGui::SetNextWindowSize(ImVec2(880, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Music API Probe", &visible_, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::End();
      return;
    }

    DrawHostSection();
    ImGui::Separator();
    DrawStateSection();
    ImGui::Separator();
    DrawTrackTable();
    ImGui::Separator();
    DrawWritesSection();
    ImGui::Separator();
    DrawErrorSection();
    ImGui::Separator();
    DrawEventSection();

    ImGui::End();
  }

 private:
  // Which of the API actually resolved. A host without the Music API at all is
  // the first thing to rule out when something below reads wrong.
  void DrawHostSection() {
    if (!api_.ok()) {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                         "This host does not export the Music API.");
      ImGui::TextWrapped(
          "EternalSonataGetMusicTrackCount / EternalSonataGetMusicTrack were not found in the "
          "executable. Nothing below can be tested.");
      return;
    }

    ImGui::Text("Music ABI version: %u (this mod was built against %u)", api_.abi_version(),
                ETERNALSONATA_MUSIC_ABI_VERSION);
    if (api_.abi_version() < ETERNALSONATA_MUSIC_ABI_VERSION) {
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                         "The host predates this mod's ABI version; the API surface may differ.");
    }

    const char* missing[12];
    int missing_count = 0;
    if (!api_.available) missing[missing_count++] = "IsMusicAvailable";
    if (!api_.tab_count) missing[missing_count++] = "GetMusicTabTrackCount";
    if (!api_.unlocked_count) missing[missing_count++] = "GetUnlockedMusicTrackCount";
    if (!api_.get_many) missing[missing_count++] = "GetMusicTracks";
    if (!api_.is_unlocked) missing[missing_count++] = "IsMusicTrackUnlocked";
    if (!api_.name) missing[missing_count++] = "GetMusicTrackName";
    if (!api_.set_unlocked) missing[missing_count++] = "SetMusicTrackUnlocked";
    if (!api_.set_all) missing[missing_count++] = "SetAllMusicTracksUnlocked";
    if (!api_.play) missing[missing_count++] = "PlayMusicTrack";
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
                         "No mod registry: the Music events cannot be received.");
    }
  }

  void DrawStateSection() {
    if (!api_.ok()) {
      return;
    }

    if (api_.available) {
      ImGui::Text("IsMusicAvailable:           %s", Answer(available_, "yes", "no").c_str());
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "0 before the title screen has finished coming up. Every read below\n"
            "answers ERR_UNAVAILABLE while this is 0.");
      }
    }
    ImGui::Text("GetMusicTrackCount:         %s", Answer(count_, nullptr, nullptr).c_str());
    if (count_ >= 0 && count_ != kCount) {
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                         "  (disagrees with the header's ETERNALSONATA_MUSIC_TRACK_COUNT)");
    }
    ImGui::Text("GetUnlockedMusicTrackCount: %s", Answer(unlocked_count_, nullptr, nullptr).c_str());

    if (api_.tab_count) {
      ImGui::Text("GetMusicTabTrackCount:      event %s, field %s, battle %s, unlisted %s",
                  Answer(tab_count_[0], nullptr, nullptr).c_str(),
                  Answer(tab_count_[1], nullptr, nullptr).c_str(),
                  Answer(tab_count_[2], nullptr, nullptr).c_str(),
                  Answer(unlisted_count_, nullptr, nullptr).c_str());
      for (int tab = 0; tab < ETERNALSONATA_MUSIC_TAB_COUNT; ++tab) {
        if (tab_count_[tab] >= 0 && tab_count_[tab] != kExpectedTabRows[tab]) {
          ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                             "  tab %d holds %d rows, docs/music.md says %d", tab,
                             tab_count_[tab], kExpectedTabRows[tab]);
        }
      }
      if (unlisted_count_ >= 0 && unlisted_count_ != 4) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                           "  %d unlisted tracks; docs/music.md says 4 (ids 2, 6, 51, 66)",
                           unlisted_count_);
      }
    }

    if (api_.get_many && sizing_count_ >= 0 && fill_count_ >= 0 && sizing_count_ != fill_count_) {
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                         "GetMusicTracks sizing call returned %d, fill returned %d: the two "
                         "disagree.",
                         sizing_count_, fill_count_);
    }

    if (cross_check_ok_) {
      ImGui::TextDisabled(
          "Reads cross-check: count, unlocked sum, Get(id) vs GetMusicTracks, and every tab's "
          "fill all agree.");
    } else {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                         "Reads cross-check FAILED: %s", cross_check_why_.c_str());
    }
  }

  void DrawTrackTable() {
    if (!api_.ok() || tracks_.empty()) {
      return;
    }

    ImGui::Checkbox("Only tracks that are still locked", &filter_locked_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::Combo("Tab filter", &filter_tab_, "all\0none (unlisted)\0event\0field\0battle\0");

    if (!ImGui::BeginTable("tracks", 8,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                           ImVec2(0.0f, 320.0f))) {
      return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Tab", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Row", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("No.", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Unl", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("TxtId", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Title");
    ImGui::TableSetupColumn("Checks");
    ImGui::TableHeadersRow();

    for (size_t i = 0; i < tracks_.size(); ++i) {
      const EternalSonataMusicTrack& track = tracks_[i];
      if (filter_locked_ && track.unlocked) {
        continue;
      }
      if (filter_tab_ == 1 && track.tab != ETERNALSONATA_MUSIC_TAB_NONE) {
        continue;
      }
      if (filter_tab_ >= 2 && track.tab != filter_tab_ - 2) {
        continue;
      }

      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      char label[32];
      std::snprintf(label, sizeof(label), "%d", track.track_id);
      if (ImGui::Selectable(label, selected_ == track.track_id,
                            ImGuiSelectableFlags_SpanAllColumns)) {
        selected_ = track.track_id;
      }
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(TabName(track.tab));
      ImGui::TableNextColumn();
      ImGui::Text("%d", track.row);
      ImGui::TableNextColumn();
      ImGui::Text("%d", track.number);
      ImGui::TableNextColumn();
      if (track.unlocked) {
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "yes");
      } else {
        ImGui::TextDisabled("no");
      }
      ImGui::TableNextColumn();
      ImGui::Text("%d", track.title_text_id);
      ImGui::TableNextColumn();
      const char* name = api_.name ? api_.name(track.track_id) : "";
      if (name && name[0]) {
        ImGui::TextUnformatted(name);
      } else {
        ImGui::TextDisabled("(no title)");
      }
      ImGui::TableNextColumn();
      const unsigned v = violations_[i];
      if (!v) {
        ImGui::TextDisabled("ok");
      } else {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", ViolationText(v).c_str());
      }
    }
    ImGui::EndTable();
  }

  std::string ViolationText(unsigned v) {
    std::string out;
    auto add = [&out](const char* what) {
      if (!out.empty()) {
        out += ", ";
      }
      out += what;
    };
    if (v & (1u << 0)) add("id out of range");
    if (v & (1u << 1)) add("title id != id");
    if (v & (1u << 2)) add("tab out of range");
    if (v & (1u << 3)) add("number != row + 1");
    if (v & (1u << 4)) add("reserved nonzero");
    if (v & (1u << 5)) add("listed but has no title");
    if (v & (1u << 6)) add("placement disagrees with docs");
    if (v & (1u << 7)) add("IsMusicTrackUnlocked disagrees");
    return out;
  }

  void DrawWritesSection() {
    if (!api_.ok()) {
      return;
    }

    ImGui::Text("Selected track: %d (%s)", selected_,
                api_.name && api_.name(selected_)[0] ? api_.name(selected_) : "no title");

    if (api_.set_unlocked) {
      if (ImGui::Button("Unlock selected")) {
        last_write_result_ =
            WriteResult("SetMusicTrackUnlocked(selected, 1)", api_.set_unlocked(selected_, 1));
      }
      ImGui::SameLine();
      if (ImGui::Button("Lock selected")) {
        last_write_result_ =
            WriteResult("SetMusicTrackUnlocked(selected, 0)", api_.set_unlocked(selected_, 0));
      }
      ImGui::SameLine();
    }
    if (api_.set_all) {
      if (ImGui::Button("Unlock all")) {
        last_write_result_ =
            WriteResult("SetAllMusicTracksUnlocked(1)", api_.set_all(1), "track(s) changed");
      }
      ImGui::SameLine();
      if (ImGui::Button("Lock all")) {
        last_write_result_ =
            WriteResult("SetAllMusicTracksUnlocked(0)", api_.set_all(0), "track(s) changed");
      }
    }
    if (api_.play) {
      if (ImGui::Button("Play selected")) {
        last_write_result_ = WriteResult("PlayMusicTrack(selected)", api_.play(selected_));
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "The only entry point that runs guest code, so from this thread it\n"
            "queues the call and answers QUEUED. The track starts on the guest's\n"
            "next frame and whatever normally owns the BGM takes it back at the\n"
            "next area or battle transition.");
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

    if (ImGui::Button("GetMusicTrack(0)")) {
      EternalSonataMusicTrack track{};
      last_error_result_ = ErrorResult("GetMusicTrack(0)", api_.get(0, &track));
    }
    ImGui::SameLine();
    if (ImGui::Button("GetMusicTrack(67)")) {
      EternalSonataMusicTrack track{};
      last_error_result_ = ErrorResult("GetMusicTrack(67)", api_.get(67, &track));
    }
    ImGui::SameLine();
    if (ImGui::Button("GetMusicTrack(1, NULL)")) {
      last_error_result_ = ErrorResult("GetMusicTrack(1, NULL)", api_.get(1, nullptr));
    }

    if (api_.get_many) {
      if (ImGui::Button("GetMusicTracks(buf, -1, ALL)")) {
        EternalSonataMusicTrack track{};
        last_error_result_ = ErrorResult("GetMusicTracks(buf, -1, ALL)",
                                         api_.get_many(&track, -1, ETERNALSONATA_MUSIC_TAB_ALL));
      }
      ImGui::SameLine();
      if (ImGui::Button("GetMusicTracks(buf, 1, 3)")) {
        EternalSonataMusicTrack track{};
        last_error_result_ =
            ErrorResult("GetMusicTracks(buf, 1, tab 3)", api_.get_many(&track, 1, 3));
      }
      ImGui::SameLine();
      if (ImGui::Button("GetMusicTracks(NULL, 1, ALL)")) {
        last_error_result_ = ErrorResult("GetMusicTracks(NULL, 1, ALL)",
                                         api_.get_many(nullptr, 1, ETERNALSONATA_MUSIC_TAB_ALL));
      }
    }

    if (api_.play) {
      if (ImGui::Button("Play an unlisted track (id 2)")) {
        last_error_result_ = ErrorResult("PlayMusicTrack(2)", api_.play(2));
      }
      ImGui::SameLine();
      if (ImGui::Button("Play the first locked track")) {
        int locked = -1;
        for (const auto& track : tracks_) {
          if (!track.unlocked && track.tab >= 0) {
            locked = track.track_id;
            break;
          }
        }
        if (locked < 0) {
          last_error_result_ = "no locked track to try; lock one first";
        } else {
          char name[64];
          std::snprintf(name, sizeof(name), "PlayMusicTrack(%d), locked", locked);
          last_error_result_ = ErrorResult(name, api_.play(locked));
        }
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

    const int events_unlocked = event_unlocked_.load();
    const int events_locked = event_locked_.load();
    ImGui::Text("Events on the bus: %d unlocked, %d locked", events_unlocked, events_locked);
    ImGui::Text("Observed by polling: %d unlocked, %d locked", observed_unlocked_, observed_locked_);
    if (events_unlocked == observed_unlocked_ && events_locked == observed_locked_) {
      ImGui::TextDisabled("The bus and the poll agree.");
    } else {
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                         "The bus and the poll DISAGREE. (A load adopts silently, so press Reset "
                         "baseline after one.)");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset baseline")) {
      have_baseline_ = false;
      observed_unlocked_ = 0;
      observed_locked_ = 0;
      event_unlocked_.store(0);
      event_locked_.store(0);
      std::lock_guard<std::mutex> lock(events_mutex_);
      events_.clear();
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

    available_ = api_.available ? api_.available() : 1;
    count_ = api_.count();
    unlocked_count_ = api_.unlocked_count ? api_.unlocked_count() : -1;
    if (api_.tab_count) {
      for (int tab = 0; tab < ETERNALSONATA_MUSIC_TAB_COUNT; ++tab) {
        tab_count_[tab] = api_.tab_count(tab);
      }
      unlisted_count_ = api_.tab_count(ETERNALSONATA_MUSIC_TAB_NONE);
    }

    tracks_.clear();
    sizing_count_ = -1;
    fill_count_ = -1;
    if (api_.get_many) {
      // The two-step a real caller makes: size, then fill.
      sizing_count_ = api_.get_many(nullptr, 0, ETERNALSONATA_MUSIC_TAB_ALL);
      if (sizing_count_ > 0) {
        tracks_.resize(static_cast<size_t>(sizing_count_));
        fill_count_ = api_.get_many(tracks_.data(), sizing_count_, ETERNALSONATA_MUSIC_TAB_ALL);
        if (fill_count_ >= 0) {
          tracks_.resize(static_cast<size_t>(fill_count_));
        } else {
          tracks_.clear();
        }
      }
    }

    // The single-track getter is exercised for every id whatever happens, both
    // for its own OK/ERR column and so the whole-list fill can be compared
    // against it. A host without GetMusicTracks still gets the rows.
    for (int id = ETERNALSONATA_MUSIC_TRACK_ID_MIN; id <= ETERNALSONATA_MUSIC_TRACK_ID_MAX; ++id) {
      EternalSonataMusicTrack track{};
      const int result = api_.get(id, &track);
      get_result_[SlotOf(id)] = result;
      if (result != ETERNALSONATA_MUSIC_OK) {
        continue;
      }
      if (tracks_.size() <= SlotOf(id)) {
        tracks_.resize(SlotOf(id) + 1);
        tracks_[SlotOf(id)] = track;
      }
    }

    PollTabs();
    CheckInvariants();
    CrossCheckReads();
    ObserveTransitions();
  }

  // Each tab's own fill, kept so the placement the whole-list read reports can
  // be checked against the order the menu would actually draw.
  void PollTabs() {
    for (int tab = 0; tab < ETERNALSONATA_MUSIC_TAB_COUNT; ++tab) {
      tab_tracks_[tab].clear();
      if (!api_.get_many) {
        continue;
      }
      const int sizing = api_.get_many(nullptr, 0, tab);
      if (sizing <= 0) {
        continue;
      }
      tab_tracks_[tab].resize(static_cast<size_t>(sizing));
      const int filled = api_.get_many(tab_tracks_[tab].data(), sizing, tab);
      if (filled < 0) {
        tab_tracks_[tab].clear();
      } else {
        tab_tracks_[tab].resize(static_cast<size_t>(filled));
      }
    }
  }

  void CheckInvariants() {
    violations_.assign(tracks_.size(), 0u);
    for (size_t i = 0; i < tracks_.size(); ++i) {
      const EternalSonataMusicTrack& track = tracks_[i];
      unsigned v = 0;
      if (track.track_id < ETERNALSONATA_MUSIC_TRACK_ID_MIN ||
          track.track_id > ETERNALSONATA_MUSIC_TRACK_ID_MAX) {
        v |= 1u << 0;
      }
      if (track.title_text_id != track.track_id) v |= 1u << 1;
      if (track.tab < ETERNALSONATA_MUSIC_TAB_NONE ||
          track.tab >= ETERNALSONATA_MUSIC_TAB_COUNT) {
        v |= 1u << 2;
      }
      const int expected_number = track.row >= 0 ? track.row + 1 : 0;
      if (track.number != expected_number) v |= 1u << 3;
      for (int r = 0; r < 6; ++r) {
        if (track.reserved[r] != 0) {
          v |= 1u << 4;
          break;
        }
      }
      if (track.tab >= 0 && api_.name && !api_.name(track.track_id)[0]) v |= 1u << 5;
      if ((track.tab == ETERNALSONATA_MUSIC_TAB_NONE) != ExpectedUnlisted(track.track_id)) {
        v |= 1u << 6;
      }
      if (api_.is_unlocked && api_.is_unlocked(track.track_id) != track.unlocked) v |= 1u << 7;
      violations_[i] = v;
    }
  }

  void CrossCheckReads() {
    cross_check_ok_ = true;
    cross_check_why_.clear();
    auto fail = [this](const char* why) {
      if (cross_check_ok_) {
        cross_check_why_ = why;
      }
      cross_check_ok_ = false;
    };

    if (count_ != kCount) {
      fail("GetMusicTrackCount is not 66");
    }
    int sum = 0;
    for (const auto& track : tracks_) {
      if (track.unlocked) {
        ++sum;
      }
    }
    if (unlocked_count_ != sum) {
      fail("GetUnlockedMusicTrackCount does not match a sum over the list");
    }

    // The one-track getter's row must match the whole-list fill, byte for byte.
    for (const auto& track : tracks_) {
      EternalSonataMusicTrack directly{};
      if (api_.get(track.track_id, &directly) != ETERNALSONATA_MUSIC_OK) {
        continue;
      }
      if (std::memcmp(&directly, &track, sizeof(EternalSonataMusicTrack)) != 0) {
        fail("GetMusicTrack(id) disagrees with the GetMusicTracks fill");
        break;
      }
    }

    // Each tab's fill must be in menu order and must agree with the placement
    // the whole-list read gave the same track.
    for (int tab = 0; tab < ETERNALSONATA_MUSIC_TAB_COUNT; ++tab) {
      const auto& rows = tab_tracks_[tab];
      if (api_.get_many && tab_count_[tab] >= 0 &&
          static_cast<int>(rows.size()) != tab_count_[tab]) {
        fail("a tab's fill is not as long as GetMusicTabTrackCount says");
        break;
      }
      for (size_t row = 0; row < rows.size(); ++row) {
        if (rows[row].tab != tab || rows[row].row != static_cast<int>(row)) {
          fail("a tab's fill is out of menu order");
          break;
        }
        const size_t slot = SlotOf(rows[row].track_id);
        if (slot < tracks_.size() &&
            std::memcmp(&rows[row], &tracks_[slot], sizeof(EternalSonataMusicTrack)) != 0) {
          fail("a tab's fill disagrees with the whole-list fill");
          break;
        }
      }
    }
  }

  // The probe's own view of what changed, kept independently of the bus so the
  // two can be compared. Keyed by track id, which is the stable identity.
  void ObserveTransitions() {
    // Mirror the host tick's guard: it drops its snapshot whenever the list is
    // not readable and re-seeds it silently on the next readable frame, so it
    // never publishes across a gap. Without the same guard here a single
    // unavailable frame would read every track as locked and inject a phantom
    // lock/unlock pair per track that no event ever matched.
    if (!available_ || tracks_.empty()) {
      have_baseline_ = false;
      return;
    }

    std::array<bool, kCount> now{};
    for (const auto& track : tracks_) {
      const size_t slot = SlotOf(track.track_id);
      if (slot < now.size()) {
        now[slot] = track.unlocked != 0;
      }
    }

    if (!have_baseline_) {
      have_baseline_ = true;
      last_state_ = now;
      return;
    }

    for (size_t i = 0; i < now.size(); ++i) {
      if (now[i] == last_state_[i]) {
        continue;
      }
      const int id = static_cast<int>(i) + ETERNALSONATA_MUSIC_TRACK_ID_MIN;
      char line[160];
      if (now[i]) {
        ++observed_unlocked_;
        std::snprintf(line, sizeof(line), "observed  unlocked  track %d", id);
      } else {
        ++observed_locked_;
        std::snprintf(line, sizeof(line), "observed  locked    track %d", id);
      }
      PushEvent(line);
    }
    last_state_ = now;
  }

  // Formats one raw call result with its symbolic name, for the write buttons
  // and the error-path buttons.
  std::string WriteResult(const char* name, int result, const char* suffix = "") {
    char buffer[256];
    if (result < 0) {
      std::snprintf(buffer, sizeof(buffer), "%s returned %s (%d)", name, ErrorName(result), result);
    } else {
      std::snprintf(buffer, sizeof(buffer), "%s returned %d (%s) %s", name, result,
                    ErrorName(result), suffix);
    }
    return buffer;
  }

  std::string ErrorResult(const char* name, int result) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%s -> %s (%d)", name, ErrorName(result), result);
    return buffer;
  }

  // The events arrive on the mod registry's frame tick, which is not this
  // thread, so everything they touch is either a mutex-guarded deque or an
  // atomic. Nothing here calls into ImGui.
  void SubscribeToMusicEvents() {
    if (!runtime_) {
      return;
    }
    auto* registry = runtime_->mod_registry();
    if (!registry) {
      return;
    }

    registry->Subscribe(ETERNALSONATA_MUSIC_EVENT_UNLOCKED,
                        [this](const rex::system::ModRegistry::EventPayload& payload) {
                          event_unlocked_.fetch_add(1);
                          char line[160];
                          std::snprintf(line, sizeof(line), "event     unlocked  track %llu, tab %d",
                                        static_cast<unsigned long long>(payload.u64),
                                        static_cast<int>(payload.f64));
                          PushEvent(line);
                        });

    registry->Subscribe(ETERNALSONATA_MUSIC_EVENT_LOCKED,
                        [this](const rex::system::ModRegistry::EventPayload& payload) {
                          event_locked_.fetch_add(1);
                          char line[160];
                          std::snprintf(line, sizeof(line), "event     locked    track %llu, tab %d",
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
  MusicApi api_;
  bool visible_ = false;

  // Snapshot, owned by the draw thread.
  std::vector<EternalSonataMusicTrack> tracks_;  // by id, index = id - 1
  std::array<std::vector<EternalSonataMusicTrack>, ETERNALSONATA_MUSIC_TAB_COUNT> tab_tracks_;
  int available_ = 0;
  int count_ = -1;
  int unlocked_count_ = -1;
  int unlisted_count_ = -1;
  std::array<int, ETERNALSONATA_MUSIC_TAB_COUNT> tab_count_{};
  int sizing_count_ = -1;
  int fill_count_ = -1;
  std::array<int, kCount> get_result_{};
  std::vector<unsigned> violations_;
  bool cross_check_ok_ = false;
  std::string cross_check_why_;
  std::string last_write_result_;
  std::string last_error_result_;

  int selected_ = ETERNALSONATA_MUSIC_TRACK_ID_MIN;
  bool filter_locked_ = false;
  int filter_tab_ = 0;

  // The probe's own transition counters, also draw-thread only.
  bool have_baseline_ = false;
  std::array<bool, kCount> last_state_{};
  int observed_unlocked_ = 0;
  int observed_locked_ = 0;

  // Events, written by the registry tick.
  std::mutex events_mutex_;
  std::deque<std::string> events_;
  std::atomic<int> event_unlocked_{0};
  std::atomic<int> event_locked_{0};
};

class MusicProbeMod : public rex::system::IModPlugin {
 public:
  explicit MusicProbeMod(rex::Runtime* runtime) : runtime_(runtime) {}

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    dialog_ = std::make_unique<MusicProbeDialog>(drawer, runtime_);
  }

 private:
  rex::Runtime* runtime_ = nullptr;
  std::unique_ptr<MusicProbeDialog> dialog_;
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
  return new MusicProbeMod(ctx->runtime);
}
