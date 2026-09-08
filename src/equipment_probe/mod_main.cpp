// equipment_probe - an overlay (F10) that exercises every part of the game's
// Equipment API, so the whole surface can be checked in one place while
// playing. The companion to item_probe, and built the same way.
//
// It shows, live:
//
//   * the ABI version the running host implements, and which of the entry
//     points actually resolved out of it;
//   * EternalSonataIsEquipmentSystemAvailable, with its raw return value;
//   * all forty slots (ten characters times four), read the same two-step way
//     a real caller uses EternalSonataGetAllEquipment (a sizing call with
//     (NULL, 0) first, then a fill), with the invariants a row should always
//     satisfy flagged instead of silently accepted: the character and slot
//     fields echoing the request, an empty slot zeroing every derived field,
//     name_text_id == description_text_id == item_id - 1, the category
//     matching the slot it is in, EternalSonataCanEquip saying yes about
//     something the character is actually wearing, and the reserved words
//     staying zero;
//   * cross-checks between the entry points: EternalSonataGetEquipment(c, s)
//     must agree byte for byte with the GetAllEquipment fill, and
//     EternalSonataGetEquippedItem with the struct's item_id;
//   * EternalSonataGetEquippableItems for a chosen character and slot, again
//     sized then filled, with every id it returns re-checked through
//     EternalSonataCanEquipInSlot and EternalSonataGetEquipmentSlotForItem;
//   * **the preview self-test**, which is the part worth watching. The host
//     computes EternalSonataPreviewEquip itself rather than calling the
//     game's routine, so previewing the item that is *already* in a slot has
//     to reproduce, exactly, the numbers the game's own recompute left in the
//     stat struct - which is what EternalSonataGetEquipStats reads back. The
//     probe runs that comparison for all ten characters every frame and says
//     which stat drifted if any of them does;
//   * EternalSonataEquip / Unequip / UnequipAll for a character and slot typed
//     in, with the raw return value shown, so the QUEUED-vs-error semantics
//     are visible;
//   * every error path the header declares (bad character, bad slot, bad item,
//     NULL out, not equippable, wrong slot, not owned, slot empty), each on a
//     button with the returned error code;
//   * a log of both equipment events off the mod registry bus, cross-checked
//     against what the probe itself observes by polling the forty slots once a
//     frame.
//
// That last section is the one that actually tests the events rather than
// just displaying them. The probe polls every slot itself and notes when it
// *observes* something come off or go on; the bus events are counted
// separately. If the two ever disagree, the host published the wrong number of
// events, and the panel says so instead of quietly looking fine. That is also
// what makes a save-and-reload check meaningful: a load restores the state
// without any event, so after one you press Reset baseline and the restored
// state is adopted, moving the observed counts without a single added event.
//
// A save-and-reload check is worth doing here in particular. Equipment writes
// into the stat structs the whole game reads and both of them are serialized
// into the save, so the sequence that matters is: equip something, check the
// game's own status screen agrees with this panel, save, reload, press Reset
// baseline, and confirm the slot still holds it.
//
// Polling every frame is fine: reading forty slots is a handful of guest
// memory loads, and so is a preview, which is host-side arithmetic. Nothing
// here touches the filesystem or runs guest code. Writes (equip, unequip) do
// queue onto the guest main thread, which is exactly the QUEUED-vs-immediate
// distinction the header describes and this probe surfaces verbatim.
//
// Nothing here knows a guest address; it is all
// rexmod/eternalsonata_equipment_api.h, copied verbatim from the game repo's
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

#include <rexmod/eternalsonata_equipment_api.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

// How many event lines to keep.
constexpr size_t kMaxEventLines = 128;

constexpr int kCharacterMin = ETERNALSONATA_EQUIPMENT_CHARACTER_MIN;
constexpr int kCharacterMax = ETERNALSONATA_EQUIPMENT_CHARACTER_MAX;
constexpr int kSlotCount = ETERNALSONATA_EQUIPMENT_SLOT_COUNT;
constexpr int kSlotTotal = kCharacterMax * kSlotCount;

// The cast in character-number order, so a row says who it is without the
// probe having to resolve the Party API too.
constexpr const char* kCharacterNames[kCharacterMax + 1] = {
    "",      "Allegretto", "Polka", "Beat",     "Frederic", "Viola",
    "Salsa", "Jazz",       "Falsetto", "Claves", "March"};

constexpr const char* kSlotNames[kSlotCount] = {"weapon", "armor", "accessory 1",
                                                "accessory 2"};

// What the header says each slot takes. A row whose category disagrees means
// either the host put the wrong item in the slot or the two tables have
// drifted apart.
constexpr int kSlotCategory[kSlotCount] = {1, 2, 3, 3};

// Every entry point is resolved out of the host executable at load time and
// null-checked before use, so this mod still loads (and says exactly what is
// missing) on a host too old to have the Equipment API.
struct EquipmentApi {
  EternalSonataEquipmentAbiVersionFn abi_version = nullptr;
  EternalSonataIsEquipmentSystemAvailableFn available = nullptr;
  EternalSonataGetEquipmentFn get_equipment = nullptr;
  EternalSonataGetAllEquipmentFn get_all = nullptr;
  EternalSonataGetEquippedItemFn get_equipped = nullptr;
  EternalSonataGetEquipmentNameFn name = nullptr;
  EternalSonataGetEquipmentDescriptionFn description = nullptr;
  EternalSonataCanEquipFn can_equip = nullptr;
  EternalSonataGetEquipmentSlotForItemFn slot_for_item = nullptr;
  EternalSonataCanEquipInSlotFn can_equip_in_slot = nullptr;
  EternalSonataGetEquippableItemsFn equippable_items = nullptr;
  EternalSonataPreviewEquipFn preview = nullptr;
  EternalSonataGetEquipStatsFn equip_stats = nullptr;
  EternalSonataEquipFn equip = nullptr;
  EternalSonataUnequipFn unequip = nullptr;
  EternalSonataUnequipAllFn unequip_all = nullptr;

  bool ok() const { return abi_version && available && get_equipment && get_all; }
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

EquipmentApi LoadEquipmentApi() {
  EquipmentApi api;
  Resolve(api.abi_version, "EternalSonataEquipmentAbiVersion");
  Resolve(api.available, "EternalSonataIsEquipmentSystemAvailable");
  Resolve(api.get_equipment, "EternalSonataGetEquipment");
  Resolve(api.get_all, "EternalSonataGetAllEquipment");
  Resolve(api.get_equipped, "EternalSonataGetEquippedItem");
  Resolve(api.name, "EternalSonataGetEquipmentName");
  Resolve(api.description, "EternalSonataGetEquipmentDescription");
  Resolve(api.can_equip, "EternalSonataCanEquip");
  Resolve(api.slot_for_item, "EternalSonataGetEquipmentSlotForItem");
  Resolve(api.can_equip_in_slot, "EternalSonataCanEquipInSlot");
  Resolve(api.equippable_items, "EternalSonataGetEquippableItems");
  Resolve(api.preview, "EternalSonataPreviewEquip");
  Resolve(api.equip_stats, "EternalSonataGetEquipStats");
  Resolve(api.equip, "EternalSonataEquip");
  Resolve(api.unequip, "EternalSonataUnequip");
  Resolve(api.unequip_all, "EternalSonataUnequipAll");
  return api;
}

const char* ErrorName(int value) {
  switch (value) {
    case ETERNALSONATA_EQUIPMENT_OK:
      return "OK";
    case ETERNALSONATA_EQUIPMENT_QUEUED:
      return "QUEUED";
    case ETERNALSONATA_EQUIPMENT_ERR_UNAVAILABLE:
      return "ERR_UNAVAILABLE";
    case ETERNALSONATA_EQUIPMENT_ERR_INVALID_CHARACTER:
      return "ERR_INVALID_CHARACTER";
    case ETERNALSONATA_EQUIPMENT_ERR_INVALID_SLOT:
      return "ERR_INVALID_SLOT";
    case ETERNALSONATA_EQUIPMENT_ERR_INVALID_ITEM:
      return "ERR_INVALID_ITEM";
    case ETERNALSONATA_EQUIPMENT_ERR_NOT_EQUIPPABLE:
      return "ERR_NOT_EQUIPPABLE";
    case ETERNALSONATA_EQUIPMENT_ERR_WRONG_SLOT:
      return "ERR_WRONG_SLOT";
    case ETERNALSONATA_EQUIPMENT_ERR_NOT_OWNED:
      return "ERR_NOT_OWNED";
    case ETERNALSONATA_EQUIPMENT_ERR_SLOT_EMPTY:
      return "ERR_SLOT_EMPTY";
    case ETERNALSONATA_EQUIPMENT_ERR_INVALID_ARGUMENT:
      return "ERR_INVALID_ARGUMENT";
    default:
      return "ERR_?";
  }
}

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

const ImVec4 kBad(1.0f, 0.4f, 0.4f, 1.0f);
const ImVec4 kWarn(1.0f, 0.8f, 0.3f, 1.0f);
const ImVec4 kGood(0.4f, 0.9f, 0.4f, 1.0f);

class EquipmentProbeDialog : public rex::ui::ImGuiDialog {
 public:
  EquipmentProbeDialog(rex::ui::ImGuiDrawer* drawer, rex::Runtime* runtime)
      : ImGuiDialog(drawer), runtime_(runtime), api_(LoadEquipmentApi()) {
    rex::ui::RegisterBind(
        "bind_equipment_probe", "F10", "Toggle the equipment API probe",
        [this] { visible_ = !visible_; }, [this] { return visible_; }, "Equipment API Probe");

    SubscribeToEquipmentEvents();
  }

  ~EquipmentProbeDialog() override { rex::ui::UnregisterBind("bind_equipment_probe"); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    (void)io;

    // Polled whether or not the window is open, so a change made elsewhere
    // (the game's own equipment screen) is still observed and can still be
    // compared against the events published for it.
    Poll();

    if (!visible_) {
      return;
    }

    ImGui::SetNextWindowSize(ImVec2(940, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Equipment API Probe", &visible_, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::End();
      return;
    }

    DrawHostSection();
    ImGui::Separator();
    DrawStateSection();
    ImGui::Separator();
    DrawSlotTable();
    ImGui::Separator();
    DrawPreviewSection();
    ImGui::Separator();
    DrawEquippableSection();
    ImGui::Separator();
    DrawActionSection();
    ImGui::Separator();
    DrawErrorPathsSection();
    ImGui::Separator();
    DrawEventSection();

    ImGui::End();
  }

 private:
  void DrawHostSection() {
    if (!api_.ok()) {
      ImGui::TextColored(kBad, "This host does not export the Equipment API.");
      ImGui::TextWrapped(
          "EternalSonataIsEquipmentSystemAvailable / EternalSonataGetEquipment / "
          "EternalSonataGetAllEquipment were not found in the executable. Nothing below can be "
          "tested.");
      return;
    }

    ImGui::Text("Equipment ABI version: %u (this mod was built against %u)", api_.abi_version(),
                ETERNALSONATA_EQUIPMENT_ABI_VERSION);
    if (api_.abi_version() < ETERNALSONATA_EQUIPMENT_ABI_VERSION) {
      ImGui::TextColored(kWarn,
                         "The host predates this mod's ABI version; the API surface may differ.");
    }

    const char* missing[16];
    int missing_count = 0;
    if (!api_.get_equipped) missing[missing_count++] = "GetEquippedItem";
    if (!api_.name) missing[missing_count++] = "GetEquipmentName";
    if (!api_.description) missing[missing_count++] = "GetEquipmentDescription";
    if (!api_.can_equip) missing[missing_count++] = "CanEquip";
    if (!api_.slot_for_item) missing[missing_count++] = "GetEquipmentSlotForItem";
    if (!api_.can_equip_in_slot) missing[missing_count++] = "CanEquipInSlot";
    if (!api_.equippable_items) missing[missing_count++] = "GetEquippableItems";
    if (!api_.preview) missing[missing_count++] = "PreviewEquip";
    if (!api_.equip_stats) missing[missing_count++] = "GetEquipStats";
    if (!api_.equip) missing[missing_count++] = "Equip";
    if (!api_.unequip) missing[missing_count++] = "Unequip";
    if (!api_.unequip_all) missing[missing_count++] = "UnequipAll";
    if (missing_count) {
      std::string list;
      for (int i = 0; i < missing_count; ++i) {
        if (i) list += ", ";
        list += missing[i];
      }
      ImGui::TextColored(kWarn, "Not exported by this host: %s", list.c_str());
    }

    if (!runtime_ || !runtime_->mod_registry()) {
      ImGui::TextColored(kWarn, "No mod registry: the equipment events cannot be received.");
    }
  }

  void DrawStateSection() {
    if (!api_.ok()) {
      return;
    }
    ImGui::Text("IsEquipmentSystemAvailable: %s", Answer(api_.available(), "yes", "no").c_str());
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "0 before a save is loaded or a new game started. Every read below answers\n"
          "ERR_UNAVAILABLE (or an equivalent negative count) while this is 0.");
    }
    ImGui::Text("GetAllEquipment sizing call: %s (expected %d)",
                Answer(sizing_count_, nullptr, nullptr).c_str(), kSlotCount);
    if (sizing_count_ >= 0 && sizing_count_ != kSlotCount) {
      ImGui::TextColored(kBad, "The sizing call disagrees with ETERNALSONATA_EQUIPMENT_SLOT_COUNT.");
    }

    if (cross_check_ok_) {
      ImGui::TextDisabled(
          "Reads cross-check: GetEquipment(c, s), the GetAllEquipment fill and "
          "GetEquippedItem all agree.");
    } else {
      ImGui::TextColored(kBad,
                         "Reads cross-check FAILED: some entry points disagree about the same "
                         "slot.");
    }
  }

  void DrawSlotTable() {
    if (!api_.ok()) {
      return;
    }
    ImGui::Text("All %d slots:", kSlotTotal);
    if (ImGui::BeginTable("slots", 9,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0, 320))) {
      ImGui::TableSetupColumn("Character", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Cat", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("HP", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Atk", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Mag/Def/Spd", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Bad", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableHeadersRow();
      ImGui::TableSetupScrollFreeze(0, 1);

      for (int index = 0; index < kSlotTotal; ++index) {
        const EternalSonataEquipment& row = slots_[index];
        const unsigned bad = violations_[index];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%d %s", row.character,
                    (row.character >= kCharacterMin && row.character <= kCharacterMax)
                        ? kCharacterNames[row.character]
                        : "?");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted((row.slot >= 0 && row.slot < kSlotCount) ? kSlotNames[row.slot]
                                                                       : "?");
        ImGui::TableNextColumn();
        ImGui::Text("%d", row.item_id);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(row.item_id && api_.name ? api_.name(row.item_id) : "");
        ImGui::TableNextColumn();
        ImGui::Text("%d", row.category);
        ImGui::TableNextColumn();
        ImGui::Text("%d", row.hp);
        ImGui::TableNextColumn();
        ImGui::Text("%d", row.attack);
        ImGui::TableNextColumn();
        ImGui::Text("%d/%d/%d", row.magic, row.defense, row.speed);
        ImGui::TableNextColumn();
        if (bad) {
          ImGui::TextColored(kBad, "%02X", bad);
        } else {
          ImGui::TextDisabled("-");
        }
      }
      ImGui::EndTable();
    }
    ImGui::TextDisabled(
        "Violation bits: 01=character/slot field does not echo the request 02=empty slot has "
        "nonzero derived fields 04=text ids != item_id-1 08=category wrong for this slot "
        "10=CanEquip says no about something being worn 20=reserved words nonzero "
        "40=GetEquipment / GetEquippedItem disagree with this row.");
  }

  // The headline test. PreviewEquip is computed host-side rather than by
  // calling the game's routine, so previewing what is already worn has to
  // reproduce exactly what the game's own recompute left behind, which is what
  // GetEquipStats reads back out of the live stat struct.
  void DrawPreviewSection() {
    if (!api_.ok() || !api_.preview || !api_.equip_stats) {
      return;
    }
    ImGui::Text("Preview self-test (preview of what is already worn == live stats):");
    if (preview_checked_ == 0) {
      ImGui::TextDisabled("(nothing to check yet)");
    } else if (preview_mismatches_ == 0) {
      ImGui::TextColored(kGood, "%d/%d characters match exactly.", preview_checked_,
                         preview_checked_);
    } else {
      ImGui::TextColored(kBad, "%d of %d characters MISMATCH: %s", preview_mismatches_,
                         preview_checked_, preview_detail_.c_str());
    }
    ImGui::TextDisabled(
        "A mismatch means the host's reimplementation of the game's preview maths has drifted "
        "from what the game itself computes. It is the one thing here that cannot be seen by "
        "eye in-game.");

    ImGui::Separator();
    ImGui::Text("Preview a swap:");
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("Character##preview", &character_);
    ClampCharacter();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("Slot##preview", &slot_);
    ClampSlot();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("Item id##preview", &item_id_);
    ClampItem();

    EternalSonataEquipStats now{};
    EternalSonataEquipStats after{};
    const int now_result = api_.equip_stats(character_, &now);
    const int after_result = api_.preview(character_, slot_, item_id_, &after);
    ImGui::Text("GetEquipStats -> %s   PreviewEquip -> %s", ErrorName(now_result),
                ErrorName(after_result));
    if (now_result == ETERNALSONATA_EQUIPMENT_OK) {
      ImGui::Text("            HPmax   Atk   Mag   Def   Spd    HP");
      ImGui::Text("now       %7d %5d %5d %5d %5d %5d", now.hp_max, now.attack, now.magic,
                  now.defense, now.speed, now.hp);
      ImGui::Text("preview   %7d %5d %5d %5d %5d %5d", after.hp_max, after.attack, after.magic,
                  after.defense, after.speed, after.hp);
      ImGui::Text("delta     %7d %5d %5d %5d %5d %5d", after.hp_max - now.hp_max,
                  after.attack - now.attack, after.magic - now.magic,
                  after.defense - now.defense, after.speed - now.speed, after.hp - now.hp);
    }
    ImGui::TextDisabled(
        "Item id 0 previews emptying the slot. An item the character cannot wear answers "
        "ERR_NOT_EQUIPPABLE and fills the struct with the current stats, which is what the "
        "game's own comparison does.");
  }

  void DrawEquippableSection() {
    if (!api_.ok() || !api_.equippable_items) {
      return;
    }
    ImGui::Text("GetEquippableItems(character %d, slot %s): sized %d, filled %d", character_,
                (slot_ >= 0 && slot_ < kSlotCount) ? kSlotNames[slot_] : "?",
                equippable_sizing_, equippable_fill_);
    if (equippable_sizing_ >= 0 && equippable_fill_ >= 0 &&
        equippable_sizing_ != equippable_fill_) {
      ImGui::TextColored(kBad, "The sizing call and the fill disagree.");
    }
    if (equippable_.empty()) {
      ImGui::TextDisabled(
          "(nothing in the inventory fits this slot for this character; worn equipment is out "
          "of the inventory, so what is already on somebody is correctly absent)");
    } else if (ImGui::BeginTable("equippable", 4,
                                 ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                                 ImVec2(0, 160))) {
      ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("CanEquipInSlot", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("SlotForItem", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableHeadersRow();
      ImGui::TableSetupScrollFreeze(0, 1);
      for (size_t i = 0; i < equippable_.size(); ++i) {
        const int id = equippable_[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%d", id);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(api_.name ? api_.name(id) : "");
        ImGui::TableNextColumn();
        const int allowed =
            api_.can_equip_in_slot ? api_.can_equip_in_slot(character_, slot_, id) : 0;
        if (allowed == ETERNALSONATA_EQUIPMENT_OK) {
          ImGui::TextUnformatted("OK");
        } else {
          ImGui::TextColored(kBad, "%s", ErrorName(allowed));
        }
        ImGui::TableNextColumn();
        const int fits = api_.slot_for_item ? api_.slot_for_item(id) : -1;
        // An accessory always answers slot 2, since either accessory slot
        // takes it, so slot 3 agreeing with an answer of 2 is correct.
        const bool fits_ok =
            fits == slot_ || (slot_ == 3 && fits == 2) || (slot_ == 2 && fits == 2);
        if (fits_ok) {
          ImGui::Text("%d", fits);
        } else {
          ImGui::TextColored(kBad, "%d", fits);
        }
      }
      ImGui::EndTable();
    }
    if (!equippable_.empty() && api_.equip && ImGui::Button("Equip the first one")) {
      last_write_result_ =
          Result("Equip", api_.equip(character_, slot_, equippable_.front()));
    }
  }

  void DrawActionSection() {
    if (!api_.ok()) {
      return;
    }
    ImGui::Text("Equip / unequip (character %d, slot %s, item %d):", character_,
                (slot_ >= 0 && slot_ < kSlotCount) ? kSlotNames[slot_] : "?", item_id_);
    if (api_.can_equip_in_slot && ImGui::Button("CanEquipInSlot?")) {
      last_write_result_ =
          Result("CanEquipInSlot", api_.can_equip_in_slot(character_, slot_, item_id_));
    }
    ImGui::SameLine();
    if (api_.equip && ImGui::Button("Equip")) {
      last_write_result_ = Result("Equip", api_.equip(character_, slot_, item_id_));
    }
    ImGui::SameLine();
    if (api_.unequip && ImGui::Button("Unequip")) {
      last_write_result_ = Result("Unequip", api_.unequip(character_, slot_));
    }
    ImGui::SameLine();
    if (api_.unequip_all && ImGui::Button("Unequip all")) {
      last_write_result_ = Result("UnequipAll", api_.unequip_all(character_));
    }
    ImGui::SameLine();
    if (api_.can_equip && ImGui::Button("CanEquip?")) {
      last_write_result_ = Result("CanEquip", api_.can_equip(character_, item_id_));
    }

    if (!last_write_result_.empty()) {
      ImGui::TextUnformatted(last_write_result_.c_str());
      ImGui::TextDisabled(
          "QUEUED means it will apply on the guest thread's next frame; an equipment event "
          "should follow. Equipping takes the item out of the inventory and puts whatever was "
          "in the slot back into it.");
    }
  }

  void DrawErrorPathsSection() {
    if (!api_.ok()) {
      return;
    }
    ImGui::Text("Error paths (raw returns):");
    EternalSonataEquipment scratch{};
    if (ImGui::Button("GetEquipment(0, 0)")) {
      last_error_result_ = Result("GetEquipment(0, 0)", api_.get_equipment(0, 0, &scratch));
    }
    ImGui::SameLine();
    if (ImGui::Button("GetEquipment(11, 0)")) {
      last_error_result_ = Result("GetEquipment(11, 0)", api_.get_equipment(11, 0, &scratch));
    }
    ImGui::SameLine();
    if (ImGui::Button("GetEquipment(1, 4)")) {
      last_error_result_ = Result("GetEquipment(1, 4)", api_.get_equipment(1, 4, &scratch));
    }
    ImGui::SameLine();
    if (ImGui::Button("GetEquipment(1, 0, NULL)")) {
      last_error_result_ = Result("GetEquipment(1, 0, NULL)", api_.get_equipment(1, 0, nullptr));
    }

    if (api_.equip && ImGui::Button("Equip(1, 0, 0)")) {
      last_error_result_ = Result("Equip(1, 0, 0)", api_.equip(1, 0, 0));
    }
    ImGui::SameLine();
    if (api_.equip && ImGui::Button("Equip(1, 0, 513)")) {
      last_error_result_ = Result("Equip(1, 0, 513)", api_.equip(1, 0, 513));
    }
    ImGui::SameLine();
    if (api_.equip && ImGui::Button("Equip(1, 0, <a consumable>)")) {
      // Floral Powder: an item with no equipment bit at all.
      last_error_result_ = Result("Equip(1, 0, 209)", api_.equip(1, 0, 209));
    }
    ImGui::SameLine();
    if (api_.equip && ImGui::Button("Equip a weapon into the armor slot")) {
      const int weapon = FirstEquippable(character_, 0);
      last_error_result_ =
          weapon > 0 ? Result("Equip(weapon into slot 1)", api_.equip(character_, 1, weapon))
                     : std::string("no spare weapon in the inventory to try it with");
    }

    if (api_.equip && ImGui::Button("Equip somebody else's weapon")) {
      const int other = ForeignEquippable(character_);
      last_error_result_ =
          other > 0
              ? Result("Equip(another character's weapon)", api_.equip(character_, 0, other))
              : std::string("no weapon in the inventory that this character cannot wear");
    }
    ImGui::SameLine();
    if (api_.unequip && ImGui::Button("Unequip an already-empty slot")) {
      const int empty = FirstEmptySlot(character_);
      last_error_result_ = empty >= 0
                               ? Result("Unequip(empty slot)", api_.unequip(character_, empty))
                               : std::string("this character has no empty slot to try it with");
    }
    ImGui::SameLine();
    if (api_.unequip && ImGui::Button("Unequip(1, 4)")) {
      last_error_result_ = Result("Unequip(1, 4)", api_.unequip(1, 4));
    }

    if (!last_error_result_.empty()) {
      ImGui::TextUnformatted(last_error_result_.c_str());
      ImGui::TextDisabled(
          "Every one of these must be a negative ERR_ code. A QUEUED here means the host "
          "accepted something it should have refused up front, which is the failure mode worth "
          "watching for.");
    }
  }

  void DrawEventSection() {
    ImGui::Text("Equipment events: %d equipped, %d unequipped", event_equipped_.load(),
                event_unequipped_.load());
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear log")) {
      std::lock_guard<std::mutex> lock(events_mutex_);
      events_.clear();
    }

    const bool equipped_ok = observed_equipped_ == event_equipped_.load();
    const bool unequipped_ok = observed_unequipped_ == event_unequipped_.load();
    ImGui::Text("Probe observed:   %d equipped, %d unequipped", observed_equipped_,
                observed_unequipped_);
    if (equipped_ok && unequipped_ok) {
      ImGui::TextColored(kGood, "Events match what the probe observed.");
    } else {
      ImGui::TextColored(kBad,
                         "Mismatch: the host published a different number of events than the "
                         "probe observed.");
    }
    if (ImGui::SmallButton("Reset baseline")) {
      PushEvent("baseline reset: current state adopted");
      have_baseline_ = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled(
        "After loading a save, press Reset baseline: a load restores state without publishing "
        "any event.");

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
      if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
        ImGui::SetScrollHereY(1.0f);
      }
    }
    ImGui::EndChild();
  }

  // --- polling ---------------------------------------------------------------

  void Poll() {
    if (!api_.ok()) {
      return;
    }

    sizing_count_ = api_.get_all(character_, nullptr, 0);

    for (int character = kCharacterMin; character <= kCharacterMax; ++character) {
      EternalSonataEquipment filled[kSlotCount];
      std::memset(filled, 0, sizeof(filled));
      const int written = api_.get_all(character, filled, kSlotCount);
      for (int slot = 0; slot < kSlotCount; ++slot) {
        const int index = SlotIndex(character, slot);
        if (written == kSlotCount) {
          slots_[index] = filled[slot];
        } else {
          std::memset(&slots_[index], 0, sizeof(slots_[index]));
        }
      }
    }

    CheckInvariants();
    CrossCheckReads();
    CheckPreview();
    PollEquippable();
    ObserveTransitions();
  }

  void CheckInvariants() {
    for (int character = kCharacterMin; character <= kCharacterMax; ++character) {
      for (int slot = 0; slot < kSlotCount; ++slot) {
        const int index = SlotIndex(character, slot);
        const EternalSonataEquipment& row = slots_[index];
        unsigned v = 0;

        if (row.character != character || row.slot != slot) {
          v |= 1u << 0;
        }
        if (row.item_id == 0) {
          if (row.name_text_id || row.description_text_id || row.category || row.hp ||
              row.attack || row.magic || row.defense || row.speed) {
            v |= 1u << 1;
          }
        } else {
          if (row.name_text_id != row.item_id - 1 ||
              row.description_text_id != row.item_id - 1) {
            v |= 1u << 2;
          }
          if (row.category != kSlotCategory[slot]) {
            v |= 1u << 3;
          }
          if (api_.can_equip && api_.can_equip(character, row.item_id) != 1) {
            v |= 1u << 4;
          }
        }
        for (int r = 0; r < 8; ++r) {
          if (row.reserved[r] != 0) {
            v |= 1u << 5;
            break;
          }
        }
        violations_[index] = v;
      }
    }
  }

  void CrossCheckReads() {
    cross_check_ok_ = true;
    if (sizing_count_ >= 0 && sizing_count_ != kSlotCount) {
      cross_check_ok_ = false;
    }
    for (int character = kCharacterMin; character <= kCharacterMax; ++character) {
      for (int slot = 0; slot < kSlotCount; ++slot) {
        const int index = SlotIndex(character, slot);
        EternalSonataEquipment directly{};
        if (api_.get_equipment(character, slot, &directly) != ETERNALSONATA_EQUIPMENT_OK) {
          cross_check_ok_ = false;
          violations_[index] |= 1u << 6;
          continue;
        }
        if (std::memcmp(&directly, &slots_[index], sizeof(EternalSonataEquipment)) != 0) {
          cross_check_ok_ = false;
          violations_[index] |= 1u << 6;
        }
        if (api_.get_equipped && api_.get_equipped(character, slot) != slots_[index].item_id) {
          cross_check_ok_ = false;
          violations_[index] |= 1u << 6;
        }
      }
    }
  }

  // Previewing the item already in a slot must land on the numbers the game
  // itself computed, which GetEquipStats reads back. Slot 0 is used because
  // every character has one and the choice does not matter: the preview
  // rebuilds all four slots either way.
  void CheckPreview() {
    preview_checked_ = 0;
    preview_mismatches_ = 0;
    preview_detail_.clear();
    if (!api_.preview || !api_.equip_stats || !api_.available || !api_.available()) {
      return;
    }
    for (int character = kCharacterMin; character <= kCharacterMax; ++character) {
      EternalSonataEquipStats live{};
      if (api_.equip_stats(character, &live) != ETERNALSONATA_EQUIPMENT_OK) {
        continue;
      }
      // An untouched character slot has no stats at all; comparing zeroes
      // would be meaningless.
      if (live.hp_max <= 0) {
        continue;
      }
      const int worn = slots_[SlotIndex(character, 0)].item_id;
      EternalSonataEquipStats predicted{};
      const int result = api_.preview(character, 0, worn, &predicted);
      if (result != ETERNALSONATA_EQUIPMENT_OK) {
        continue;
      }
      ++preview_checked_;
      const char* which = nullptr;
      if (predicted.hp_max != live.hp_max) {
        which = "HPmax";
      } else if (predicted.attack != live.attack) {
        which = "attack";
      } else if (predicted.magic != live.magic) {
        which = "magic";
      } else if (predicted.defense != live.defense) {
        which = "defense";
      } else if (predicted.speed != live.speed) {
        which = "speed";
      } else if (predicted.hp != live.hp) {
        which = "HP";
      }
      if (which) {
        ++preview_mismatches_;
        if (preview_detail_.empty()) {
          char buffer[192];
          std::snprintf(buffer, sizeof(buffer), "%s differs for %s (preview %d, live %d)", which,
                        kCharacterNames[character],
                        StatByName(predicted, which), StatByName(live, which));
          preview_detail_ = buffer;
        }
      }
    }
  }

  static int StatByName(const EternalSonataEquipStats& stats, const char* which) {
    if (std::strcmp(which, "HPmax") == 0) return stats.hp_max;
    if (std::strcmp(which, "attack") == 0) return stats.attack;
    if (std::strcmp(which, "magic") == 0) return stats.magic;
    if (std::strcmp(which, "defense") == 0) return stats.defense;
    if (std::strcmp(which, "speed") == 0) return stats.speed;
    return stats.hp;
  }

  void PollEquippable() {
    equippable_.clear();
    equippable_sizing_ = -1;
    equippable_fill_ = -1;
    if (!api_.equippable_items) {
      return;
    }
    equippable_sizing_ = api_.equippable_items(character_, slot_, nullptr, 0);
    if (equippable_sizing_ > 0) {
      equippable_.resize(static_cast<size_t>(equippable_sizing_));
      equippable_fill_ =
          api_.equippable_items(character_, slot_, equippable_.data(), equippable_sizing_);
      if (equippable_fill_ < 0) {
        equippable_.clear();
      } else {
        equippable_.resize(static_cast<size_t>(equippable_fill_));
      }
    } else {
      equippable_fill_ = equippable_sizing_;
    }
  }

  // Diffs the forty slots against the previous poll to build the probe's own
  // view of what changed, independent of the bus.
  void ObserveTransitions() {
    const bool available = api_.available ? api_.available() != 0 : true;
    if (!available) {
      have_baseline_ = false;
      return;
    }

    std::array<int, kSlotTotal> now{};
    for (int i = 0; i < kSlotTotal; ++i) {
      now[static_cast<size_t>(i)] = slots_[i].item_id;
    }

    if (!have_baseline_) {
      have_baseline_ = true;
      last_worn_ = now;
      return;
    }

    for (int character = kCharacterMin; character <= kCharacterMax; ++character) {
      for (int slot = 0; slot < kSlotCount; ++slot) {
        const size_t index = static_cast<size_t>(SlotIndex(character, slot));
        const int before = last_worn_[index];
        const int after = now[index];
        if (before == after) {
          continue;
        }
        // A swap is both: the old item came off and the new one went on, which
        // is exactly what the host publishes.
        if (before != 0) {
          ++observed_unequipped_;
          char line[160];
          std::snprintf(line, sizeof(line), "observed  unequipped %s %s: item %d",
                        kCharacterNames[character], kSlotNames[slot], before);
          PushEvent(line);
        }
        if (after != 0) {
          ++observed_equipped_;
          char line[160];
          std::snprintf(line, sizeof(line), "observed  equipped   %s %s: item %d",
                        kCharacterNames[character], kSlotNames[slot], after);
          PushEvent(line);
        }
      }
    }
    last_worn_ = now;
  }

  // --- helpers ---------------------------------------------------------------

  static int SlotIndex(int character, int slot) {
    return (character - kCharacterMin) * kSlotCount + slot;
  }

  void ClampCharacter() {
    if (character_ < kCharacterMin) character_ = kCharacterMin;
    if (character_ > kCharacterMax) character_ = kCharacterMax;
  }
  void ClampSlot() {
    if (slot_ < 0) slot_ = 0;
    if (slot_ >= kSlotCount) slot_ = kSlotCount - 1;
  }
  void ClampItem() {
    if (item_id_ < 0) item_id_ = 0;
    if (item_id_ > ETERNALSONATA_EQUIPMENT_ITEM_ID_MAX) {
      item_id_ = ETERNALSONATA_EQUIPMENT_ITEM_ID_MAX;
    }
  }

  // The first spare item that fits `slot` on `character`, for the error-path
  // buttons that need a real id to misuse.
  int FirstEquippable(int character, int slot) const {
    if (!api_.equippable_items) {
      return 0;
    }
    int ids[64];
    const int count = api_.equippable_items(character, slot, ids, 64);
    return count > 0 ? ids[0] : 0;
  }

  // A weapon in the inventory that `character` specifically cannot wear, for
  // the NOT_EQUIPPABLE path. Found by asking every other character what it
  // could wear and keeping the first answer this one is refused.
  int ForeignEquippable(int character) const {
    if (!api_.equippable_items || !api_.can_equip) {
      return 0;
    }
    for (int other = kCharacterMin; other <= kCharacterMax; ++other) {
      if (other == character) {
        continue;
      }
      int ids[64];
      const int count = api_.equippable_items(other, 0, ids, 64);
      for (int i = 0; i < count; ++i) {
        if (api_.can_equip(character, ids[i]) == 0) {
          return ids[i];
        }
      }
    }
    return 0;
  }

  int FirstEmptySlot(int character) const {
    for (int slot = 0; slot < kSlotCount; ++slot) {
      if (slots_[SlotIndex(character, slot)].item_id == 0) {
        return slot;
      }
    }
    return -1;
  }

  std::string Result(const char* name, int result) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%s -> %s (%d)", name, ErrorName(result), result);
    return buffer;
  }

  void SubscribeToEquipmentEvents() {
    if (!runtime_) {
      return;
    }
    auto* registry = runtime_->mod_registry();
    if (!registry) {
      return;
    }

    registry->Subscribe(ETERNALSONATA_EQUIPMENT_EVENT_EQUIPPED,
                        [this](const rex::system::ModRegistry::EventPayload& payload) {
                          event_equipped_.fetch_add(1);
                          char line[160];
                          std::snprintf(line, sizeof(line),
                                        "event     equipped   character %llu, item %d",
                                        static_cast<unsigned long long>(payload.u64),
                                        static_cast<int>(payload.f64));
                          PushEvent(line);
                        });
    registry->Subscribe(ETERNALSONATA_EQUIPMENT_EVENT_UNEQUIPPED,
                        [this](const rex::system::ModRegistry::EventPayload& payload) {
                          event_unequipped_.fetch_add(1);
                          char line[160];
                          std::snprintf(line, sizeof(line),
                                        "event     unequipped character %llu, item %d",
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
  EquipmentApi api_;
  bool visible_ = false;

  // Snapshot, owned by the draw thread.
  int sizing_count_ = -1;
  std::array<EternalSonataEquipment, kSlotTotal> slots_{};
  std::array<unsigned, kSlotTotal> violations_{};
  bool cross_check_ok_ = false;

  int preview_checked_ = 0;
  int preview_mismatches_ = 0;
  std::string preview_detail_;

  int equippable_sizing_ = -1;
  int equippable_fill_ = -1;
  std::vector<int> equippable_;

  int character_ = 1;  // Allegretto
  int slot_ = 0;       // weapon
  int item_id_ = 1;    // Hunting Knife, his starting weapon
  std::string last_write_result_;
  std::string last_error_result_;

  // The probe's own transition counters, draw-thread only.
  bool have_baseline_ = false;
  std::array<int, kSlotTotal> last_worn_{};
  int observed_equipped_ = 0;
  int observed_unequipped_ = 0;

  // Events, written by the registry tick.
  std::mutex events_mutex_;
  std::deque<std::string> events_;
  std::atomic<int> event_equipped_{0};
  std::atomic<int> event_unequipped_{0};
};

class EquipmentProbeMod : public rex::system::IModPlugin {
 public:
  explicit EquipmentProbeMod(rex::Runtime* runtime) : runtime_(runtime) {}

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    dialog_ = std::make_unique<EquipmentProbeDialog>(drawer, runtime_);
  }

 private:
  rex::Runtime* runtime_ = nullptr;
  std::unique_ptr<EquipmentProbeDialog> dialog_;
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
  return new EquipmentProbeMod(ctx->runtime);
}
