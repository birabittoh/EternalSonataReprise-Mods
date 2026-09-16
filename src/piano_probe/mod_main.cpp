// piano_probe - an overlay (F9) that exercises every part of the game's Piano
// Music API, so the whole surface can be checked in one place while playing.
//
// It shows, live:
//
//   * the ABI version the running host implements, and which of the entry
//     points actually resolved out of it;
//   * EternalSonataIsPianoMusicAvailable / GetPianoMusicCount /
//     GetUnlockedPianoMusicCount, each with its raw return value, so a negative
//     "unavailable" is visible rather than being flattened into a zero;
//   * EternalSonataGetAllPianoMusic, run in the same two steps a real caller
//     uses (a sizing call with (NULL, 0) first, then a fill into an array of
//     that size, with both counts shown so a mismatch is obvious);
//   * every field of every piece, one row per piece, plus the invariants a
//     piece should always satisfy (flag in 80..86, title id == index, locked
//     title id == 9, story ids consecutive, reserved zeros, unlocked == saved ||
//     session) so a violation is flagged instead of just drawn;
//   * cross-checks between the entry points: GetPianoMusic(i) must agree with
//     GetAllPianoMusic's fill, IsPianoMusicUnlocked(i) with the struct field,
//     GetUnlockedPianoMusicCount with a sum over the list;
//   * EternalSonataSetPianoMusicUnlocked and SetAllPianoMusicUnlocked, with the
//     raw return value shown, so the "how many changed" semantics are visible;
//   * every error path the header declares, each on a button (out-of-range
//     index, null `out`, negative `max`) with the returned error code;
//   * a log of both piano music events off the mod registry bus, cross-checked
//     against what the probe itself observes by polling the list once a frame.
//
// The last section is the one that actually tests the events rather than just
// displaying them. The probe polls the pieces itself every frame and notes when
// it *observes* a piece unlock or lock; the bus events are counted separately.
// If the two ever disagree, the host published the wrong number of events, and
// the panel says so instead of quietly looking fine. That is also what makes a
// save-and-reload check meaningful: a load restores the flags without any
// event, so after one you press Reset baseline and the restored state is
// adopted, moving the observed count without a single added event.
//
// Polling every frame is fine here. Reading the list is a handful of
// guest-memory loads and nothing touches the filesystem or runs guest code, so
// there is no queued-write path to respect.
//
// Nothing here knows a guest address; it is all
// rexmod/eternalsonata_piano_music_api.h, copied verbatim from the game repo's
// src/.

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

#include <rexmod/eternalsonata_piano_music_api.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

// How many event lines to keep. Unlocking is rare in normal play, so this is a
// long history in practice (and a whole Unlock All burst is just seven lines).
constexpr size_t kMaxEventLines = 96;

// How many history pages each piece's story spans, in menu order. The guest
// builds its book pages from these; if a page count drifts the story screen
// will skip or overlap pages.
constexpr int kStoryPageCounts[ETERNALSONATA_PIANO_MUSIC_COUNT] = {8, 5, 5, 6, 7, 7, 9};

// Every entry point is resolved out of the host executable at load time and
// null-checked before use, so this mod still loads (and says exactly what is
// missing) on a host too old to have the Piano Music API.
struct PianoApi {
  EternalSonataPianoMusicAbiVersionFn abi_version = nullptr;
  EternalSonataIsPianoMusicAvailableFn available = nullptr;
  EternalSonataGetPianoMusicCountFn count = nullptr;
  EternalSonataGetUnlockedPianoMusicCountFn unlocked_count = nullptr;
  EternalSonataGetPianoMusicFn get = nullptr;
  EternalSonataGetAllPianoMusicFn get_all = nullptr;
  EternalSonataIsPianoMusicUnlockedFn is_unlocked = nullptr;
  EternalSonataSetPianoMusicUnlockedFn set_unlocked = nullptr;
  EternalSonataSetAllPianoMusicUnlockedFn set_all = nullptr;

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

PianoApi LoadPianoApi() {
  PianoApi api;
  Resolve(api.abi_version, "EternalSonataPianoMusicAbiVersion");
  Resolve(api.available, "EternalSonataIsPianoMusicAvailable");
  Resolve(api.count, "EternalSonataGetPianoMusicCount");
  Resolve(api.unlocked_count, "EternalSonataGetUnlockedPianoMusicCount");
  Resolve(api.get, "EternalSonataGetPianoMusic");
  Resolve(api.get_all, "EternalSonataGetAllPianoMusic");
  Resolve(api.is_unlocked, "EternalSonataIsPianoMusicUnlocked");
  Resolve(api.set_unlocked, "EternalSonataSetPianoMusicUnlocked");
  Resolve(api.set_all, "EternalSonataSetAllPianoMusicUnlocked");
  return api;
}

const char* ErrorName(int value) {
  switch (value) {
    case ETERNALSONATA_PIANO_MUSIC_OK:
      return "OK";
    case ETERNALSONATA_PIANO_MUSIC_ERR_UNAVAILABLE:
      return "ERR_UNAVAILABLE";
    case ETERNALSONATA_PIANO_MUSIC_ERR_NO_SUCH_PIECE:
      return "ERR_NO_SUCH_PIECE";
    case ETERNALSONATA_PIANO_MUSIC_ERR_INVALID_ARGUMENT:
      return "ERR_INVALID_ARGUMENT";
    default:
      return "ERR_?";
  }
}

// A tri-state answer from the API: >= 0 is the value, < 0 is one of the
// ERR_* codes. Both are worth showing verbatim.
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

class PianoProbeDialog : public rex::ui::ImGuiDialog {
 public:
  PianoProbeDialog(rex::ui::ImGuiDrawer* drawer, rex::Runtime* runtime)
      : ImGuiDialog(drawer), runtime_(runtime), api_(LoadPianoApi()) {
    rex::ui::RegisterBind(
        "bind_piano_probe", "F9", "Toggle the piano music API probe",
        [this] { visible_ = !visible_; }, [this] { return visible_; }, "Piano Music API Probe");

    SubscribeToPianoMusicEvents();
  }

  ~PianoProbeDialog() override { rex::ui::UnregisterBind("bind_piano_probe"); }

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

    ImGui::SetNextWindowSize(ImVec2(760, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Piano Music API Probe", &visible_, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::End();
      return;
    }

    DrawHostSection();
    ImGui::Separator();
    DrawStateSection();
    ImGui::Separator();
    DrawPieceTable();
    ImGui::Separator();
    DrawWritesSection();
    ImGui::Separator();
    DrawEventSection();

    ImGui::End();
  }

 private:
  // Which of the API actually resolved. A host without the Piano Music API at
  // all is the first thing to rule out when something below reads wrong.
  void DrawHostSection() {
    if (!api_.ok()) {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                         "This host does not export the Piano Music API.");
      ImGui::TextWrapped(
          "EternalSonataGetPianoMusicCount / EternalSonataGetPianoMusic were not found in "
          "the executable. Nothing below can be tested.");
      return;
    }

    ImGui::Text("Piano Music ABI version: %u (this mod was built against %u)",
                api_.abi_version(), ETERNALSONATA_PIANO_MUSIC_ABI_VERSION);

    if (api_.abi_version() < ETERNALSONATA_PIANO_MUSIC_ABI_VERSION) {
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                         "The host predates this mod's ABI version; the API surface may differ.");
    }

    const char* missing[8];
    int missing_count = 0;
    if (!api_.available) missing[missing_count++] = "IsPianoMusicAvailable";
    if (!api_.unlocked_count) missing[missing_count++] = "GetUnlockedPianoMusicCount";
    if (!api_.get_all) missing[missing_count++] = "GetAllPianoMusic";
    if (!api_.is_unlocked) missing[missing_count++] = "IsPianoMusicUnlocked";
    if (!api_.set_unlocked) missing[missing_count++] = "SetPianoMusicUnlocked";
    if (!api_.set_all) missing[missing_count++] = "SetAllPianoMusicUnlocked";
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
                         "No mod registry: the Piano Music events cannot be received.");
    }
  }

  void DrawStateSection() {
    if (!api_.ok()) {
      return;
    }

    if (api_.available) {
      ImGui::Text("IsPianoMusicAvailable:   %s", Answer(api_.available(), "yes", "no").c_str());
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "0 before the title screen has finished coming up. Every read below\n"
            "answers ERR_UNAVAILABLE while this is 0.");
      }
    }
    ImGui::Text("GetPianoMusicCount:       %s", Answer(count_, nullptr, nullptr).c_str());
    if (count_ >= 0 && count_ != ETERNALSONATA_PIANO_MUSIC_COUNT) {
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                         "  (disagrees with the header's ETERNALSONATA_PIANO_MUSIC_COUNT)");
    }
    ImGui::Text("GetUnlockedPianoMusicCount: %s", Answer(unlocked_count_, nullptr, nullptr).c_str());

    if (api_.get_all && sizing_count_ >= 0 && fill_count_ >= 0 && sizing_count_ != fill_count_) {
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                         "GetAll sizing call returned %d, fill returned %d: the two disagree.",
                         sizing_count_, fill_count_);
    }

    // The cross-checks between the read entry points: count against the header
    // constant, unlocked count against a hand-rolled sum over the list, and the
    // single-piece getter against the getAll fill, byte for byte.
    if (cross_check_ok_) {
      ImGui::TextDisabled("Reads cross-check: count, unlocked sum, and Get(i) vs GetAll all agree.");
    } else {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                         "Reads cross-check FAILED: some entry points disagree about the same "
                         "piece.");
    }
  }

  void DrawPieceTable() {
    if (!api_.ok() || pieces_.empty()) {
      return;
    }

    if (ImGui::BeginTable("pieces", 11,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Idx", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Flag", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Unl", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Sv", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Ss", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Lock", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Story", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Pages", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Get", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("IsUnl", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableHeadersRow();

      for (size_t i = 0; i < pieces_.size(); ++i) {
        const EternalSonataPianoMusic& piece = pieces_[i];
        const unsigned bad = violations_[i];
        const ImVec4 bad_color(1.0f, 0.4f, 0.4f, 1.0f);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (bad & (1u << 0)) {
          ImGui::TextColored(bad_color, "%d!", piece.index);
        } else {
          ImGui::Text("%d", piece.index);
        }
        ImGui::TableNextColumn();
        if (bad & (1u << 1)) {
          ImGui::TextColored(bad_color, "%d", piece.flag_id);
        } else {
          ImGui::Text("%d", piece.flag_id);
        }
        ImGui::TableNextColumn();
        if (bad & (1u << 6)) {
          ImGui::TextColored(bad_color, "%d", piece.unlocked);
        } else {
          ImGui::Text("%d", piece.unlocked);
        }
        ImGui::TableNextColumn();
        ImGui::Text("%d", piece.unlocked_saved);
        ImGui::TableNextColumn();
        ImGui::Text("%d", piece.unlocked_session);
        ImGui::TableNextColumn();
        if (bad & (1u << 2)) {
          ImGui::TextColored(bad_color, "%d", piece.title_text_id);
        } else {
          ImGui::Text("%d", piece.title_text_id);
        }
        ImGui::TableNextColumn();
        if (bad & (1u << 3)) {
          ImGui::TextColored(bad_color, "%d", piece.locked_title_text_id);
        } else {
          ImGui::Text("%d", piece.locked_title_text_id);
        }
        ImGui::TableNextColumn();
        if (bad & (1u << 4)) {
          ImGui::TextColored(bad_color, "%d..%d", piece.first_story_text_id,
                             piece.first_story_text_id + piece.story_page_count - 1);
        } else {
          ImGui::Text("%d..%d", piece.first_story_text_id,
                      piece.first_story_text_id + piece.story_page_count - 1);
        }
        ImGui::TableNextColumn();
        if (bad & (1u << 5)) {
          ImGui::TextColored(bad_color, "%d", piece.story_page_count);
        } else {
          ImGui::Text("%d", piece.story_page_count);
        }
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(get_ok_[i] ? "OK" : "ERR");
        if (!get_ok_[i]) {
          ImGui::SameLine();
          ImGui::TextDisabled("(%d)", get_result_[i]);
        }
        ImGui::TableNextColumn();
        if (!is_unlocked_ok_[i]) {
          ImGui::TextColored(bad_color, "ERR");
        } else {
          ImGui::TextUnformatted("OK");
        }
      }
      ImGui::EndTable();
    }

    ImGui::TextDisabled(
        "Idx=menu order  Flag=collectible slot (80..86; also MP1<n>.wav)  Unl=unlocked  "
        "Sv=unlocked_saved  Ss=unlocked_session  Title=title_text_id  Lock=locked_title_text_id  "
        "Story=first..last story text id  Pages=story_page_count");
    ImGui::TextDisabled(
        "Get and IsUnl say OK when GetPianoMusic(i) and IsPianoMusicUnlocked(i) worked and "
        "agreed with the shared reads; a red cell is an invariant the struct broke. Violation "
        "code bits: 01=index 02=flag 04=title id 08=locked title id 10=story start 20=story "
        "pages 40=unlocked vs saved/session 80=reserved bytes.");

    // The invariants a piece must satisfy, with a per-bit marker so the table's
    // red cells have a text explanation.
    int first_bad = -1;
    for (size_t i = 0; i < pieces_.size(); ++i) {
      if (violations_[i] != 0) {
        first_bad = static_cast<int>(i);
        break;
      }
    }
    if (first_bad < 0) {
      return;
    }
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                       "Piece %d breaks an invariant (code %02X). The whole list cannot be "
                       "trusted until this is understood.",
                       first_bad, violations_[static_cast<size_t>(first_bad)]);
  }

  // The per-piece writes, the bulk writes, and the error paths. Everything here
  // runs the real entry point and shows the raw result.
  void DrawWritesSection() {
    if (!api_.ok()) {
      return;
    }
    if (!api_.set_unlocked && !api_.set_all) {
      ImGui::TextDisabled("This host exports no write entry points.");
      return;
    }

    ImGui::Text("Per-piece writes (SetPianoMusicUnlocked):");
    if (api_.set_unlocked) {
      bool any = false;
      for (size_t i = 0; i < pieces_.size(); ++i) {
        if (pieces_[i].index != static_cast<int>(i)) {
          continue;
        }
        const std::string unlock = std::string("Unlock ") + std::to_string(i);
        const std::string lock = std::string("Lock ") + std::to_string(i);
        if (any) {
          ImGui::SameLine();
        }
        if (ImGui::Button(unlock.c_str())) {
          last_write_result_ = WriteResult("SetPianoMusicUnlocked", api_.set_unlocked(i, 1));
        }
        ImGui::SameLine();
        if (ImGui::Button(lock.c_str())) {
          last_write_result_ = WriteResult("SetPianoMusicUnlocked", api_.set_unlocked(i, 0));
        }
        any = true;
      }
    }

    ImGui::Text("Bulk writes (SetAllPianoMusicUnlocked):");
    if (api_.set_all) {
      if (ImGui::Button("Unlock all")) {
        last_write_result_ = WriteResult("SetAllPianoMusicUnlocked", api_.set_all(1),
                                         "piece(s) changed");
      }
      ImGui::SameLine();
      if (ImGui::Button("Lock all")) {
        last_write_result_ = WriteResult("SetAllPianoMusicUnlocked", api_.set_all(0),
                                         "piece(s) changed");
      }
    }

    if (!last_write_result_.empty()) {
      ImGui::TextUnformatted(last_write_result_.c_str());
      ImGui::TextDisabled(
          "The menu should show the title or ??? straight away, and an event should follow "
          "on the next frame tick.");
    }

    ImGui::Text("Error paths (raw returns):");
    {
      EternalSonataPianoMusic scratch{};
      if (ImGui::Button("Get(-1)")) {
        last_error_result_ = ErrorResult("GetPianoMusic(-1)", api_.get(-1, &scratch));
      }
      ImGui::SameLine();
      if (ImGui::Button("Get(COUNT) NULL)")) {
        last_error_result_ =
            ErrorResult("GetPianoMusic(out-of-range, NULL)",
                        api_.get(ETERNALSONATA_PIANO_MUSIC_COUNT, &scratch));
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Get(0, NULL)")) {
      last_error_result_ = ErrorResult("GetPianoMusic(0, NULL)", api_.get(0, nullptr));
    }
    ImGui::SameLine();
    if (ImGui::Button("IsUnlocked(-1)")) {
      last_error_result_ = ErrorResult("IsPianoMusicUnlocked(-1)", api_.is_unlocked(-1));
    }
    ImGui::SameLine();
    if (ImGui::Button("SetUnlocked(-1)")) {
      last_error_result_ = ErrorResult("SetPianoMusicUnlocked(-1)", api_.set_unlocked(-1, 1));
    }
    ImGui::SameLine();
    if (ImGui::Button("GetAll(NULL, -1)")) {
      last_error_result_ =
          ErrorResult("GetAllPianoMusic(NULL, -1)", api_.get_all(nullptr, -1));
    }
    if (!last_error_result_.empty()) {
      ImGui::TextUnformatted(last_error_result_.c_str());
      ImGui::TextDisabled("Every one of these must be a negative ERR_ code, not OK.");
    }
  }

  void DrawEventSection() {
    ImGui::Text("Piano Music events: %d unlocked, %d locked", event_unlocked_.load(),
                event_locked_.load());
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear log")) {
      std::lock_guard<std::mutex> lock(events_mutex_);
      events_.clear();
    }

    // The actual test: what the probe saw happen, against what the host said
    // happened. These are counted from opposite ends - one by polling the list,
    // one off the bus - so a disagreement is a real defect.
    const int ev_unlocked = event_unlocked_.load();
    const int ev_locked = event_locked_.load();
    const bool unlocked_ok = observed_unlocked_ == ev_unlocked;
    const bool locked_ok = observed_locked_ == ev_locked;
    ImGui::Text("Probe observed:   %d unlocked, %d locked", observed_unlocked_,
                observed_locked_);
    if (unlocked_ok && locked_ok) {
      ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Events match what the probe observed.");
    } else {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                         "Mismatch: the host published %s%s%s than the probe observed.",
                         unlocked_ok ? "" : "the wrong number of unlocked events",
                         (!unlocked_ok && !locked_ok) ? " and " : "",
                         locked_ok ? "" : "the wrong number of locked events");
    }
    if (ImGui::SmallButton("Reset baseline")) {
      std::lock_guard<std::mutex> lock(events_mutex_);
      events_.emplace_back("baseline reset: current state adopted");
      while (events_.size() > kMaxEventLines) {
        events_.pop_front();
      }
      have_baseline_ = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled(
        "After loading a save, press Reset baseline: a load restores the flags without "
        "publishing any event.");

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

  // Runs the two-step GetAll a real caller makes (size with (NULL, 0), then
  // fill), reads every piece through the single-piece getter too, and works out
  // the cross-checks and observed transitions. Owns all of the state the
  // windows above draw, so it is how the "read" entry points get exercised
  // even when the overlay is closed.
  void Poll() {
    if (!api_.ok()) {
      return;
    }

    sizing_count_ = -1;
    fill_count_ = -1;
    pieces_.clear();
    violations_.fill(0);
    get_result_.fill(0);
    get_ok_.fill(false);
    is_unlocked_result_.fill(0);
    is_unlocked_ok_.fill(false);

    if (api_.count) {
      count_ = api_.count();
    }
    if (api_.unlocked_count) {
      unlocked_count_ = api_.unlocked_count();
    }

    if (api_.get_all) {
      sizing_count_ = api_.get_all(nullptr, 0);
      if (sizing_count_ > 0) {
        pieces_.resize(static_cast<size_t>(sizing_count_));
        fill_count_ = api_.get_all(pieces_.data(), sizing_count_);
        if (fill_count_ < 0) {
          pieces_.clear();
        } else {
          pieces_.resize(static_cast<size_t>(fill_count_));
        }
      } else {
        fill_count_ = sizing_count_;
      }
    }

    // The single-piece getter is exercised for every slot whatever happens,
    // both for its own OK/ERR column and so the getAll fill can be compared
    // against it in CrossCheckReads. A host without GetAll still needs the
    // rows, so the successful single reads fill the table then.
    for (int i = 0; i < ETERNALSONATA_PIANO_MUSIC_COUNT; ++i) {
      EternalSonataPianoMusic piece{};
      const int result = api_.get(i, &piece);
      get_result_[static_cast<size_t>(i)] = result;
      get_ok_[static_cast<size_t>(i)] = result == ETERNALSONATA_PIANO_MUSIC_OK;
      if (result != ETERNALSONATA_PIANO_MUSIC_OK) {
        continue;
      }
      if (pieces_.size() <= static_cast<size_t>(i)) {
        // GetAll produced nothing for this slot (or does not exist): adopt the
        // single read's row so the table still shows everything.
        pieces_.resize(static_cast<size_t>(i) + 1);
        pieces_[static_cast<size_t>(i)] = piece;
      }
    }

    CheckInvariants();
    CrossCheckReads();

    if (api_.is_unlocked) {
      for (int i = 0; i < ETERNALSONATA_PIANO_MUSIC_COUNT; ++i) {
        is_unlocked_result_[static_cast<size_t>(i)] = api_.is_unlocked(i);
        is_unlocked_ok_[static_cast<size_t>(i)] =
            is_unlocked_result_[static_cast<size_t>(i)] >= 0 &&
            is_unlocked_result_[static_cast<size_t>(i)] ==
                (pieces_.size() > static_cast<size_t>(i)
                     ? pieces_[static_cast<size_t>(i)].unlocked
                     : -1);
      }
    }

    ObserveTransitions();
  }

  void CheckInvariants() {
    for (size_t i = 0; i < pieces_.size(); ++i) {
      const EternalSonataPianoMusic& piece = pieces_[i];
      unsigned v = 0;
      if (piece.index != static_cast<int>(i)) v |= 1u << 0;
      if (piece.flag_id < 80 || piece.flag_id > 86) v |= 1u << 1;
      if (piece.title_text_id != static_cast<int>(i)) v |= 1u << 2;
      if (piece.locked_title_text_id != 9) v |= 1u << 3;
      if (piece.first_story_text_id != 10 * (static_cast<int>(i) + 1)) v |= 1u << 4;
      if (piece.story_page_count != kStoryPageCounts[i]) v |= 1u << 5;
      const bool unlocked = piece.unlocked != 0;
      const bool split = (piece.unlocked_saved != 0) || (piece.unlocked_session != 0);
      if (unlocked != split) v |= 1u << 6;
      for (int r = 0; r < 8; ++r) {
        if (piece.reserved[r] != 0) {
          v |= 1u << 7;
          break;
        }
      }
      violations_[i] = v;
    }
  }

  void CrossCheckReads() {
    cross_check_ok_ = true;
    if (count_ < 0) {
      cross_check_ok_ = false;
    } else if (count_ != ETERNALSONATA_PIANO_MUSIC_COUNT) {
      cross_check_ok_ = false;
    }
    int translated = 0;
    for (size_t i = 0; i < pieces_.size(); ++i) {
      if (pieces_[i].unlocked) {
        ++translated;
      }
    }
    if (unlocked_count_ != translated) {
      cross_check_ok_ = false;
    }
    // The one-piece getter's row must match the corresponding GetAll fill row.
    for (size_t i = 0; i < pieces_.size(); ++i) {
      EternalSonataPianoMusic directly{};
      if (api_.get(static_cast<int>(i), &directly) != ETERNALSONATA_PIANO_MUSIC_OK) {
        continue;
      }
      if (std::memcmp(&directly, &pieces_[i], sizeof(EternalSonataPianoMusic)) != 0) {
        cross_check_ok_ = false;
      }
    }
  }

  // The probe's own view of what changed, kept independently of the bus so the
  // two can be compared. Keyed by piece index, which is the stable identity.
  void ObserveTransitions() {
    // Mirror the host tick's guard: it drops its snapshot whenever the list is
    // not readable (before the title screen, during shutdown) and re-seeds it
    // silently on the next readable frame, so it never publishes across a gap.
    // The probe polls far more often than the host ticks, so without the same
    // guard a single unavailable frame reads every piece as locked and injects
    // a phantom lock+unlock pair per piece that no event ever matched, which is
    // why both counters drifted even though every button worked.
    const bool available = api_.available ? api_.available() != 0 : !pieces_.empty();
    if (!available) {
      have_baseline_ = false;
      return;
    }

    if (!have_baseline_) {
      have_baseline_ = true;
      last_state_.fill(false);
      for (size_t i = 0; i < pieces_.size(); ++i) {
        const int idx = pieces_[i].index;
        if (idx < 0 || idx >= ETERNALSONATA_PIANO_MUSIC_COUNT) {
          continue;
        }
        last_state_[static_cast<size_t>(idx)] = pieces_[i].unlocked != 0;
      }
      return;
    }

    std::array<bool, ETERNALSONATA_PIANO_MUSIC_COUNT> now{};
    for (size_t i = 0; i < pieces_.size(); ++i) {
      const int idx = pieces_[i].index;
      if (idx < 0 || idx >= ETERNALSONATA_PIANO_MUSIC_COUNT) {
        continue;
      }
      now[static_cast<size_t>(idx)] = pieces_[i].unlocked != 0;
    }
    for (size_t i = 0; i < now.size(); ++i) {
      if (now[i] == last_state_[i]) {
        continue;
      }
      char line[160];
      if (now[i]) {
        ++observed_unlocked_;
        std::snprintf(line, sizeof(line), "observed  unlocked  piece %zu", i);
      } else {
        ++observed_locked_;
        std::snprintf(line, sizeof(line), "observed  locked    piece %zu", i);
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
      std::snprintf(buffer, sizeof(buffer), "%s returned %s (%d)", name, ErrorName(result),
                    result);
    } else {
      std::snprintf(buffer, sizeof(buffer), "%s returned %d %s", name, result, suffix);
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
  void SubscribeToPianoMusicEvents() {
    if (!runtime_) {
      return;
    }
    auto* registry = runtime_->mod_registry();
    if (!registry) {
      return;
    }

    registry->Subscribe(ETERNALSONATA_PIANO_MUSIC_EVENT_UNLOCKED,
                        [this](const rex::system::ModRegistry::EventPayload& payload) {
                          event_unlocked_.fetch_add(1);
                          char line[160];
                          std::snprintf(line, sizeof(line),
                                        "event     unlocked  piece %llu, flag %d",
                                        static_cast<unsigned long long>(payload.u64),
                                        static_cast<int>(payload.f64));
                          PushEvent(line);
                        });

    registry->Subscribe(ETERNALSONATA_PIANO_MUSIC_EVENT_LOCKED,
                        [this](const rex::system::ModRegistry::EventPayload& payload) {
                          event_locked_.fetch_add(1);
                          char line[160];
                          std::snprintf(line, sizeof(line),
                                        "event     locked    piece %llu, flag %d",
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
  PianoApi api_;
  bool visible_ = false;

  // Snapshot, owned by the draw thread.
  std::vector<EternalSonataPianoMusic> pieces_;
  int count_ = -1;
  int unlocked_count_ = -1;
  int sizing_count_ = -1;
  int fill_count_ = -1;
  std::array<int, ETERNALSONATA_PIANO_MUSIC_COUNT> get_result_{};
  std::array<bool, ETERNALSONATA_PIANO_MUSIC_COUNT> get_ok_{};
  std::array<int, ETERNALSONATA_PIANO_MUSIC_COUNT> is_unlocked_result_{};
  std::array<bool, ETERNALSONATA_PIANO_MUSIC_COUNT> is_unlocked_ok_{};
  std::array<unsigned, ETERNALSONATA_PIANO_MUSIC_COUNT> violations_{};
  bool cross_check_ok_ = false;
  std::string last_write_result_;
  std::string last_error_result_;

  // The probe's own transition counters, also draw-thread only.
  bool have_baseline_ = false;
  std::array<bool, ETERNALSONATA_PIANO_MUSIC_COUNT> last_state_{};
  int observed_unlocked_ = 0;
  int observed_locked_ = 0;

  // Events, written by the registry tick.
  std::mutex events_mutex_;
  std::deque<std::string> events_;
  std::atomic<int> event_unlocked_{0};
  std::atomic<int> event_locked_{0};
};

class PianoProbeMod : public rex::system::IModPlugin {
 public:
  explicit PianoProbeMod(rex::Runtime* runtime) : runtime_(runtime) {}

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    dialog_ = std::make_unique<PianoProbeDialog>(drawer, runtime_);
  }

 private:
  rex::Runtime* runtime_ = nullptr;
  std::unique_ptr<PianoProbeDialog> dialog_;
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
  return new PianoProbeMod(ctx->runtime);
}