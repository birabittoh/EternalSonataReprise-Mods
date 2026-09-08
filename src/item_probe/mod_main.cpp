// item_probe - an overlay (F8) that exercises every part of the game's Item
// API, so the whole surface can be checked in one place while playing.
//
// It shows, live:
//
//   * the ABI version the running host implements, and which of the entry
//     points actually resolved out of it;
//   * EternalSonataIsItemSystemAvailable, with its raw return value;
//   * EternalSonataGetOwnedItemCount / EternalSonataGetOwnedItems, run the
//     same two-step way a real caller uses (a sizing call with (NULL, 0)
//     first, then a fill into an array of that size), with both counts shown
//     so a mismatch is obvious;
//   * every owned item, one row per item, with the invariants a row should
//     always satisfy (id in range, free_count = count - reserved_count,
//     set_entry_count == reserved_count, category in range, reserved zeros)
//     flagged instead of silently accepted;
//   * cross-checks between the entry points: EternalSonataGetItem(id) must
//     agree with the GetOwnedItems fill, EternalSonataGetItemCount with the
//     struct's count field;
//   * the Item Set: its 32 entries, the party level and the budget
//     accounting (used + free == total), with EternalSonataGetItemSetSlot
//     and EternalSonataIsItemInSet cross-checked against the struct;
//   * EternalSonataGiveItem / TakeItem for an id typed in, and
//     EternalSonataCanAddItemToSet / AddItemToSet / RemoveItemFromSet /
//     RemoveItemSetSlot / ClearItemSet, all with the raw return value shown,
//     so the QUEUED-vs-error semantics are visible;
//   * every error path the header declares (out-of-range id, set full,
//     budget too small, not owned, not in set), each on a button with the
//     returned error code;
//   * the 32 score pieces: which are collected, in what order, which one the
//     Mark is on, and the invariants that tie them to the item half of the API
//     (item_id == number + 349, GetItemCount is 1 exactly when collected, slot
//     is always -1, never in the Item Set), with unlock / lock / mark / clear
//     buttons and the raw return value of each;
//   * a log of all four item/set events plus the score piece Mark event off the
//     mod registry bus,
//     cross-checked against what the probe itself observes by polling the
//     inventory and set once a frame.
//
// The last section is the one that actually tests the events rather than
// just displaying them. The probe polls the inventory and set itself every
// frame and notes when it *observes* a gain/loss/set-add/set-remove; the bus
// events are counted separately. If the two ever disagree, the host
// published the wrong number of events, and the panel says so instead of
// quietly looking fine. That is also what makes a save-and-reload check
// meaningful: a load restores the state without any event, so after one you
// press Reset baseline and the restored state is adopted, moving the
// observed counts without a single added event.
//
// Polling every frame is fine here: reading the inventory and set is a
// handful of guest-memory loads and nothing touches the filesystem or runs
// guest code, so there is no queued-write path to respect for reads. Writes
// (give/take, set add/remove) do queue onto the guest main thread, which is
// exactly the QUEUED-vs-immediate distinction the header describes and this
// probe surfaces verbatim.
//
// Nothing here knows a guest address; it is all
// rexmod/eternalsonata_item_api.h, copied verbatim from the game repo's src/.

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

#include <rexmod/eternalsonata_item_api.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

// How many event lines to keep.
constexpr size_t kMaxEventLines = 128;

// Every entry point is resolved out of the host executable at load time and
// null-checked before use, so this mod still loads (and says exactly what is
// missing) on a host too old to have the Item API.
struct ItemApi {
  EternalSonataItemAbiVersionFn abi_version = nullptr;
  EternalSonataIsItemSystemAvailableFn available = nullptr;
  EternalSonataGetItemFn get_item = nullptr;
  EternalSonataGetOwnedItemCountFn owned_count = nullptr;
  EternalSonataGetOwnedItemsFn owned_items = nullptr;
  EternalSonataGetItemCountFn item_count = nullptr;
  EternalSonataGetItemNameFn item_name = nullptr;
  EternalSonataGetItemDescriptionFn item_description = nullptr;
  EternalSonataIsRealItemFn is_real_item = nullptr;
  EternalSonataGetItemCatalogCountFn catalog_count = nullptr;
  EternalSonataGetItemCatalogFn catalog = nullptr;
  EternalSonataRegisterCustomItemFn register_custom_item = nullptr;
  EternalSonataUnregisterCustomItemFn unregister_custom_item = nullptr;
  EternalSonataGiveItemFn give_item = nullptr;
  EternalSonataTakeItemFn take_item = nullptr;
  EternalSonataGetItemSetFn get_set = nullptr;
  EternalSonataGetItemSetCountFn set_count = nullptr;
  EternalSonataGetItemSetSlotFn set_slot = nullptr;
  EternalSonataIsItemInSetFn is_in_set = nullptr;
  EternalSonataCanAddItemToSetFn can_add = nullptr;
  EternalSonataAddItemToSetFn add_to_set = nullptr;
  EternalSonataRemoveItemFromSetFn remove_from_set = nullptr;
  EternalSonataRemoveItemSetSlotFn remove_set_slot = nullptr;
  EternalSonataClearItemSetFn clear_set = nullptr;
  EternalSonataIsScorePieceFn is_score_piece = nullptr;
  EternalSonataGetScorePieceFn get_score_piece = nullptr;
  EternalSonataGetAllScorePiecesFn all_score_pieces = nullptr;
  EternalSonataGetScorePieceCountFn score_piece_count = nullptr;
  EternalSonataIsScorePieceUnlockedFn score_piece_unlocked = nullptr;
  EternalSonataSetScorePieceUnlockedFn set_score_piece_unlocked = nullptr;
  EternalSonataSetAllScorePiecesUnlockedFn set_all_score_pieces = nullptr;
  EternalSonataGetMarkedScorePieceFn marked_score_piece = nullptr;
  EternalSonataSetMarkedScorePieceFn set_marked_score_piece = nullptr;

  bool ok() const { return abi_version && available && get_item && owned_items; }
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

ItemApi LoadItemApi() {
  ItemApi api;
  Resolve(api.abi_version, "EternalSonataItemAbiVersion");
  Resolve(api.available, "EternalSonataIsItemSystemAvailable");
  Resolve(api.get_item, "EternalSonataGetItem");
  Resolve(api.owned_count, "EternalSonataGetOwnedItemCount");
  Resolve(api.owned_items, "EternalSonataGetOwnedItems");
  Resolve(api.item_count, "EternalSonataGetItemCount");
  Resolve(api.item_name, "EternalSonataGetItemName");
  Resolve(api.item_description, "EternalSonataGetItemDescription");
  Resolve(api.is_real_item, "EternalSonataIsRealItem");
  Resolve(api.catalog_count, "EternalSonataGetItemCatalogCount");
  Resolve(api.catalog, "EternalSonataGetItemCatalog");
  Resolve(api.register_custom_item, "EternalSonataRegisterCustomItem");
  Resolve(api.unregister_custom_item, "EternalSonataUnregisterCustomItem");
  Resolve(api.give_item, "EternalSonataGiveItem");
  Resolve(api.take_item, "EternalSonataTakeItem");
  Resolve(api.get_set, "EternalSonataGetItemSet");
  Resolve(api.set_count, "EternalSonataGetItemSetCount");
  Resolve(api.set_slot, "EternalSonataGetItemSetSlot");
  Resolve(api.is_in_set, "EternalSonataIsItemInSet");
  Resolve(api.can_add, "EternalSonataCanAddItemToSet");
  Resolve(api.add_to_set, "EternalSonataAddItemToSet");
  Resolve(api.remove_from_set, "EternalSonataRemoveItemFromSet");
  Resolve(api.remove_set_slot, "EternalSonataRemoveItemSetSlot");
  Resolve(api.clear_set, "EternalSonataClearItemSet");
  Resolve(api.is_score_piece, "EternalSonataIsScorePiece");
  Resolve(api.get_score_piece, "EternalSonataGetScorePiece");
  Resolve(api.all_score_pieces, "EternalSonataGetAllScorePieces");
  Resolve(api.score_piece_count, "EternalSonataGetScorePieceCount");
  Resolve(api.score_piece_unlocked, "EternalSonataIsScorePieceUnlocked");
  Resolve(api.set_score_piece_unlocked, "EternalSonataSetScorePieceUnlocked");
  Resolve(api.set_all_score_pieces, "EternalSonataSetAllScorePiecesUnlocked");
  Resolve(api.marked_score_piece, "EternalSonataGetMarkedScorePiece");
  Resolve(api.set_marked_score_piece, "EternalSonataSetMarkedScorePiece");
  return api;
}

const char* ErrorName(int value) {
  switch (value) {
    case ETERNALSONATA_ITEM_OK:
      return "OK";
    case ETERNALSONATA_ITEM_QUEUED:
      return "QUEUED";
    case ETERNALSONATA_ITEM_ERR_UNAVAILABLE:
      return "ERR_UNAVAILABLE";
    case ETERNALSONATA_ITEM_ERR_INVALID_ITEM:
      return "ERR_INVALID_ITEM";
    case ETERNALSONATA_ITEM_ERR_NOT_OWNED:
      return "ERR_NOT_OWNED";
    case ETERNALSONATA_ITEM_ERR_SET_FULL:
      return "ERR_SET_FULL";
    case ETERNALSONATA_ITEM_ERR_BUDGET:
      return "ERR_BUDGET";
    case ETERNALSONATA_ITEM_ERR_NOT_IN_SET:
      return "ERR_NOT_IN_SET";
    case ETERNALSONATA_ITEM_ERR_INVALID_ARGUMENT:
      return "ERR_INVALID_ARGUMENT";
    default:
      return "ERR_?";
  }
}

const char* CategoryName(int32_t category) {
  switch (category) {
    case ETERNALSONATA_ITEM_CATEGORY_CONSUMABLE:
      return "consumable";
    case ETERNALSONATA_ITEM_CATEGORY_WEAPON:
      return "weapon";
    case ETERNALSONATA_ITEM_CATEGORY_ARMOR:
      return "armor";
    case ETERNALSONATA_ITEM_CATEGORY_ACCESSORY:
      return "accessory";
    default:
      return "?";
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

class ItemProbeDialog : public rex::ui::ImGuiDialog {
 public:
  ItemProbeDialog(rex::ui::ImGuiDrawer* drawer, rex::Runtime* runtime)
      : ImGuiDialog(drawer), runtime_(runtime), api_(LoadItemApi()) {
    rex::ui::RegisterBind(
        "bind_item_probe", "F8", "Toggle the item API probe",
        [this] { visible_ = !visible_; }, [this] { return visible_; }, "Item API Probe");

    SubscribeToItemEvents();
  }

  ~ItemProbeDialog() override { rex::ui::UnregisterBind("bind_item_probe"); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    (void)io;

    // Polled whether or not the window is open, so a change made elsewhere
    // (shop, battle) is still observed and can still be compared against the
    // events published for it.
    Poll();

    if (!visible_) {
      return;
    }

    ImGui::SetNextWindowSize(ImVec2(900, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Item API Probe", &visible_, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::End();
      return;
    }

    DrawHostSection();
    ImGui::Separator();
    DrawStateSection();
    ImGui::Separator();
    DrawInventoryTable();
    ImGui::Separator();
    DrawCatalogSection();
    ImGui::Separator();
    DrawCustomItemRegistrationSection();
    ImGui::Separator();
    DrawItemSetSection();
    ImGui::Separator();
    DrawScorePieceSection();
    ImGui::Separator();
    DrawGiveTakeSection();
    ImGui::Separator();
    DrawErrorPathsSection();
    ImGui::Separator();
    DrawEventSection();

    ImGui::End();
  }

 private:
  void DrawHostSection() {
    if (!api_.ok()) {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                         "This host does not export the Item API.");
      ImGui::TextWrapped(
          "EternalSonataIsItemSystemAvailable / EternalSonataGetItem / "
          "EternalSonataGetOwnedItems were not found in the executable. Nothing below can be "
          "tested.");
      return;
    }

    ImGui::Text("Item ABI version: %u (this mod was built against %u)", api_.abi_version(),
               ETERNALSONATA_ITEM_ABI_VERSION);
    if (api_.abi_version() < ETERNALSONATA_ITEM_ABI_VERSION) {
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                         "The host predates this mod's ABI version; the API surface may differ.");
    }

    const char* missing[32];
    int missing_count = 0;
    if (!api_.item_count) missing[missing_count++] = "GetItemCount";
    if (!api_.item_name) missing[missing_count++] = "GetItemName";
    if (!api_.item_description) missing[missing_count++] = "GetItemDescription";
    if (!api_.give_item) missing[missing_count++] = "GiveItem";
    if (!api_.take_item) missing[missing_count++] = "TakeItem";
    if (!api_.get_set) missing[missing_count++] = "GetItemSet";
    if (!api_.set_count) missing[missing_count++] = "GetItemSetCount";
    if (!api_.set_slot) missing[missing_count++] = "GetItemSetSlot";
    if (!api_.is_in_set) missing[missing_count++] = "IsItemInSet";
    if (!api_.can_add) missing[missing_count++] = "CanAddItemToSet";
    if (!api_.add_to_set) missing[missing_count++] = "AddItemToSet";
    if (!api_.remove_from_set) missing[missing_count++] = "RemoveItemFromSet";
    if (!api_.remove_set_slot) missing[missing_count++] = "RemoveItemSetSlot";
    if (!api_.clear_set) missing[missing_count++] = "ClearItemSet";
    if (!api_.all_score_pieces) missing[missing_count++] = "GetAllScorePieces";
    if (!api_.set_score_piece_unlocked) missing[missing_count++] = "SetScorePieceUnlocked";
    if (!api_.set_marked_score_piece) missing[missing_count++] = "SetMarkedScorePiece";
    if (missing_count) {
      std::string list;
      for (int i = 0; i < missing_count; ++i) {
        if (i) list += ", ";
        list += missing[i];
      }
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Not exported by this host: %s",
                         list.c_str());
    }

    if (!runtime_ || !runtime_->mod_registry()) {
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                         "No mod registry: the item/set events cannot be received.");
    }
  }

  void DrawStateSection() {
    if (!api_.ok()) {
      return;
    }

    ImGui::Text("IsItemSystemAvailable: %s", Answer(api_.available(), "yes", "no").c_str());
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "0 before a save is loaded or a new game started. Every read below answers\n"
          "ERR_UNAVAILABLE (or an equivalent negative count) while this is 0.");
    }
    ImGui::Text("GetOwnedItemCount:      %s", Answer(owned_count_, nullptr, nullptr).c_str());
    if (api_.owned_items && sizing_count_ >= 0 && fill_count_ >= 0 && sizing_count_ != fill_count_) {
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                         "GetOwnedItems sizing call returned %d, fill returned %d: the two "
                         "disagree.",
                         sizing_count_, fill_count_);
    }
    if (owned_count_ >= 0 && fill_count_ >= 0 && owned_count_ != fill_count_) {
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                         "GetOwnedItemCount (%d) disagrees with GetOwnedItems' fill (%d).",
                         owned_count_, fill_count_);
    }

    if (cross_check_ok_) {
      ImGui::TextDisabled("Reads cross-check: owned count, GetItem(id) vs GetOwnedItems, and "
                          "GetItemCount all agree.");
    } else {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                         "Reads cross-check FAILED: some entry points disagree about the same "
                         "item.");
    }
  }

  void DrawInventoryTable() {
    if (!api_.ok()) {
      return;
    }
    ImGui::Text("Owned items (%zu):", items_.size());
    if (items_.empty()) {
      ImGui::TextDisabled("(nothing owned, or the system is unavailable)");
      return;
    }

    if (ImGui::BeginTable("owned_items", 9,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0, 280))) {
      ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Rsvd", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Free", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("SetCnt", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Cost", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableHeadersRow();
      ImGui::TableSetupScrollFreeze(0, 1);

      const ImVec4 bad_color(1.0f, 0.4f, 0.4f, 1.0f);
      for (size_t i = 0; i < items_.size(); ++i) {
        const EternalSonataItem& item = items_[i];
        const unsigned bad = violations_.size() > i ? violations_[i] : 0;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (bad & (1u << 0)) {
          ImGui::TextColored(bad_color, "%d!", item.id);
        } else {
          ImGui::Text("%d", item.id);
        }
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(api_.item_name ? api_.item_name(item.id) : "");
        ImGui::TableNextColumn();
        ImGui::Text("%d", item.slot);
        ImGui::TableNextColumn();
        ImGui::Text("%d", item.count);
        ImGui::TableNextColumn();
        ImGui::Text("%d", item.reserved_count);
        ImGui::TableNextColumn();
        if (bad & (1u << 1)) {
          ImGui::TextColored(bad_color, "%d", item.free_count);
        } else {
          ImGui::Text("%d", item.free_count);
        }
        ImGui::TableNextColumn();
        if (bad & (1u << 2)) {
          ImGui::TextColored(bad_color, "%d", item.set_entry_count);
        } else {
          ImGui::Text("%d", item.set_entry_count);
        }
        ImGui::TableNextColumn();
        if (bad & (1u << 3)) {
          ImGui::TextColored(bad_color, "%s", CategoryName(item.category));
        } else {
          ImGui::Text("%s", CategoryName(item.category));
        }
        ImGui::TableNextColumn();
        ImGui::Text("%d", item.cost);
      }
      ImGui::EndTable();
    }
    ImGui::TextDisabled(
        "Rsvd=reserved_count  Free=free_count  SetCnt=set_entry_count. Violation bits: "
        "01=id out of range 02=free_count != count-reserved 04=set_entry_count != "
        "reserved_count 08=category out of range 10=reserved bytes nonzero.");

    int first_bad = -1;
    for (size_t i = 0; i < violations_.size(); ++i) {
      if (violations_[i] != 0) {
        first_bad = static_cast<int>(i);
        break;
      }
    }
    if (first_bad >= 0) {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                         "Row %d breaks an invariant (code %02X).", first_bad,
                         violations_[static_cast<size_t>(first_bad)]);
    }
  }

  void DrawCatalogSection() {
    if (!api_.ok() || !api_.catalog_count || !api_.catalog) {
      return;
    }

    ImGui::Text("Item Catalog:");

    const int catalog_count = api_.catalog_count();
    if (catalog_count < 0) {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Error: %d", catalog_count);
      return;
    }

    ImGui::Text("Total real items (excluding characters): %d", catalog_count);

    static int selected_category = -1;
    const char* category_names[] = {"All", "Consumable", "Weapon", "Armor", "Accessory"};
    if (ImGui::BeginCombo("Filter by category##catalog", category_names[selected_category + 1])) {
      for (int i = -1; i < 4; ++i) {
        const bool is_selected = (selected_category == i);
        if (ImGui::Selectable(category_names[i + 1], is_selected)) {
          selected_category = i;
        }
        if (is_selected) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }

    const int count = api_.catalog(nullptr, 0, selected_category);
    if (count < 0) {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Error getting catalog: %d", count);
      return;
    }

    ImGui::Text("Items in selected filter: %d", count);

    if (count == 0) {
      ImGui::TextDisabled("(no items in this category)");
      return;
    }

    std::vector<EternalSonataItem> catalog(count);
    const int filled = api_.catalog(catalog.data(), count, selected_category);
    if (filled < 0 || filled != count) {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                        "Error: requested %d, got %d", count, filled);
      return;
    }

    if (ImGui::BeginTable("catalog", 5,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0, 200))) {
      ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("IsReal", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("IsChar", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableHeadersRow();
      ImGui::TableSetupScrollFreeze(0, 1);

      for (const auto& item : catalog) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%d", item.id);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(api_.item_name ? api_.item_name(item.id) : "");
        ImGui::TableNextColumn();
        ImGui::Text("%s", item.is_real ? "yes" : "no");
        ImGui::TableNextColumn();
        ImGui::Text("%s", item.is_character ? "yes" : "no");
        ImGui::TableNextColumn();
        ImGui::Text("%s", CategoryName(item.category));
      }
      ImGui::EndTable();
    }
  }

  void DrawCustomItemRegistrationSection() {
    if (!api_.ok() || !api_.register_custom_item) {
      return;
    }

    ImGui::Text("Custom Item Registration:");

    static char name_buf[256] = "Test Item";
    static char desc_buf[512] = "A test custom item";
    static int icon_id = 0;
    static int category = ETERNALSONATA_ITEM_CATEGORY_CONSUMABLE;
    static int buy_price = 100;
    static int sell_price = 50;
    static int cost = 1;
    static int last_registered_id = -1;

    ImGui::InputText("Name##custom", name_buf, sizeof(name_buf));
    ImGui::InputTextMultiline("Description##custom", desc_buf, sizeof(desc_buf), ImVec2(0, 60));
    ImGui::DragInt("Icon ID##custom", &icon_id, 1, 0, 67);
    if (ImGui::BeginCombo("Category##custom", CategoryName(category))) {
      for (int i = 0; i < 4; ++i) {
        const bool is_selected = (category == i);
        if (ImGui::Selectable(CategoryName(i), is_selected)) {
          category = i;
        }
        if (is_selected) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }
    ImGui::DragInt("Buy Price##custom", &buy_price, 10, 0, 9999);
    ImGui::DragInt("Sell Price##custom", &sell_price, 10, 0, 9999);
    ImGui::DragInt("Item Set Cost##custom", &cost, 1, 0, 99);

    if (ImGui::Button("Register Item")) {
      EternalSonataCustomItemData data;
      data.name = name_buf;
      data.description = desc_buf;
      data.icon_id = icon_id;
      data.category = category;
      data.buy_price = buy_price;
      data.sell_price = sell_price;
      data.cost = cost;

      last_registered_id = api_.register_custom_item(&data);
    }

    if (last_registered_id > 0) {
      ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Registered item ID: %d", last_registered_id);

      ImGui::SameLine();
      if (ImGui::Button("Give x1##custom")) {
        if (api_.give_item) {
          api_.give_item(last_registered_id, 1);
        }
      }
      ImGui::SameLine();
      if (ImGui::Button("Unregister##custom")) {
        if (api_.unregister_custom_item) {
          api_.unregister_custom_item(last_registered_id);
          last_registered_id = -1;
        }
      }
    } else if (last_registered_id < 0) {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Registration failed: error %d", last_registered_id);
    }
  }

  void DrawItemSetSection() {
    if (!api_.ok() || !api_.get_set) {
      return;
    }
    ImGui::Text("Item Set: %d/%d entries, party level %d", set_.count,
               ETERNALSONATA_ITEM_SET_CAPACITY, set_.party_level);
    ImGui::Text("Budget: total %d, used %d, free %d%s", set_.budget_total, set_.budget_used,
               set_.budget_free,
               (set_.budget_used + set_.budget_free == set_.budget_total) ? "" : "  MISMATCH!");

    if (ImGui::BeginTable("item_set", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("SlotFn/IsInSet agree", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableHeadersRow();
      for (int i = 0; i < set_.count && i < ETERNALSONATA_ITEM_SET_CAPACITY; ++i) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%d", i);
        ImGui::TableNextColumn();
        ImGui::Text("%d", set_.entries[i]);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(api_.item_name ? api_.item_name(set_.entries[i]) : "");
        ImGui::TableNextColumn();
        const bool slot_ok = !api_.set_slot || api_.set_slot(i) == set_.entries[i];
        const bool in_set_ok = !api_.is_in_set || api_.is_in_set(set_.entries[i]) > 0;
        if (slot_ok && in_set_ok) {
          ImGui::TextUnformatted("OK");
        } else {
          ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "MISMATCH");
        }
      }
      ImGui::EndTable();
    }

    ImGui::Text("Register / remove by id:");
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("Item id##set", &set_item_id_);
    if (set_item_id_ < ETERNALSONATA_ITEM_ID_MIN) set_item_id_ = ETERNALSONATA_ITEM_ID_MIN;
    if (set_item_id_ > ETERNALSONATA_ITEM_ID_MAX) set_item_id_ = ETERNALSONATA_ITEM_ID_MAX;
    ImGui::SameLine();
    if (api_.can_add && ImGui::Button("CanAdd?")) {
      last_write_result_ = WriteResult("CanAddItemToSet", api_.can_add(set_item_id_));
    }
    ImGui::SameLine();
    if (api_.add_to_set && ImGui::Button("Add to set")) {
      last_write_result_ = WriteResult("AddItemToSet", api_.add_to_set(set_item_id_));
    }
    ImGui::SameLine();
    if (api_.remove_from_set && ImGui::Button("Remove from set (by id)")) {
      last_write_result_ = WriteResult("RemoveItemFromSet", api_.remove_from_set(set_item_id_));
    }

    ImGui::Text("Remove / clear:");
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("Slot##set", &set_slot_index_);
    if (set_slot_index_ < 0) set_slot_index_ = 0;
    if (set_slot_index_ >= ETERNALSONATA_ITEM_SET_CAPACITY) {
      set_slot_index_ = ETERNALSONATA_ITEM_SET_CAPACITY - 1;
    }
    ImGui::SameLine();
    if (api_.remove_set_slot && ImGui::Button("Remove slot")) {
      last_write_result_ = WriteResult("RemoveItemSetSlot", api_.remove_set_slot(set_slot_index_));
    }
    ImGui::SameLine();
    if (api_.clear_set && ImGui::Button("Clear set")) {
      last_write_result_ = WriteResult("ClearItemSet", api_.clear_set());
    }
  }

  void DrawScorePieceSection() {
    if (!api_.ok() || !api_.all_score_pieces) {
      return;
    }

    char mark_text[32];
    if (marked_piece_ > 0) {
      std::snprintf(mark_text, sizeof(mark_text), "piece %d", marked_piece_);
    } else {
      std::snprintf(mark_text, sizeof(mark_text), "nothing");
    }
    ImGui::Text("Score pieces: %d/%d collected, mark on %s", collected_pieces_,
               ETERNALSONATA_SCORE_PIECE_COUNT, mark_text);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Score pieces are items 350..381 but live outside the inventory, so none of\n"
          "these calls queues onto the guest thread: they all return a final result.\n"
          "The Score Pieces menu writes its own copy of the mark back when it closes,\n"
          "so a mark set while that menu is open is overwritten on exit.");
    }
    if (api_.score_piece_count && api_.score_piece_count() != collected_pieces_) {
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                         "GetScorePieceCount (%d) disagrees with GetAllScorePieces (%d).",
                         api_.score_piece_count(), collected_pieces_);
    }

    ImGui::Checkbox("Collected order##pieces", &pieces_collected_order_);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Off: all 32 by piece number. On: only the collected ones, in the order the\n"
          "game's own Score Pieces menu lists them.");
    }

    if (ImGui::BeginTable("score_pieces", 7,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0, 220))) {
      ImGui::TableSetupColumn("No", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("ItemId", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Collected", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Order", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Mark", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("Checks", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableHeadersRow();
      ImGui::TableSetupScrollFreeze(0, 1);

      const ImVec4 bad_color(1.0f, 0.4f, 0.4f, 1.0f);
      for (size_t i = 0; i < pieces_.size(); ++i) {
        const EternalSonataScorePiece& piece = pieces_[i];
        const unsigned bad = piece_violations_.size() > i ? piece_violations_[i] : 0;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%d", piece.number);
        ImGui::TableNextColumn();
        ImGui::Text("%d", piece.item_id);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(api_.item_name ? api_.item_name(piece.item_id) : "");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(piece.unlocked ? "yes" : "no");
        ImGui::TableNextColumn();
        ImGui::Text("%d", piece.order);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(piece.marked ? "MARK" : "");
        ImGui::TableNextColumn();
        if (bad) {
          ImGui::TextColored(bad_color, "%02X", bad);
        } else {
          ImGui::TextUnformatted("OK");
        }
      }
      ImGui::EndTable();
    }
    ImGui::TextDisabled(
        "Check bits: 01=item_id != number+349 02=order/unlocked disagree "
        "04=GetItemCount != collected 08=slot != -1 or is_score_piece unset "
        "10=in the Item Set 20=GetScorePiece disagrees with the list.");

    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("Piece##pieces", &piece_number_);
    if (piece_number_ < 1) piece_number_ = 1;
    if (piece_number_ > ETERNALSONATA_SCORE_PIECE_COUNT) {
      piece_number_ = ETERNALSONATA_SCORE_PIECE_COUNT;
    }
    ImGui::SameLine();
    if (api_.set_score_piece_unlocked && ImGui::Button("Unlock##pieces")) {
      last_piece_result_ =
          ErrorResult("SetScorePieceUnlocked(n, 1)", api_.set_score_piece_unlocked(piece_number_, 1));
    }
    ImGui::SameLine();
    if (api_.set_score_piece_unlocked && ImGui::Button("Lock##pieces")) {
      last_piece_result_ =
          ErrorResult("SetScorePieceUnlocked(n, 0)", api_.set_score_piece_unlocked(piece_number_, 0));
    }
    ImGui::SameLine();
    if (api_.set_marked_score_piece && ImGui::Button("Mark##pieces")) {
      last_piece_result_ =
          ErrorResult("SetMarkedScorePiece(n)", api_.set_marked_score_piece(piece_number_));
    }
    ImGui::SameLine();
    if (api_.set_marked_score_piece && ImGui::Button("Clear mark##pieces")) {
      last_piece_result_ =
          ErrorResult("SetMarkedScorePiece(0)", api_.set_marked_score_piece(0));
    }

    if (api_.set_all_score_pieces && ImGui::Button("Unlock all##pieces")) {
      last_piece_result_ =
          ErrorResult("SetAllScorePiecesUnlocked(1)", api_.set_all_score_pieces(1));
    }
    ImGui::SameLine();
    if (api_.set_all_score_pieces && ImGui::Button("Lock all##pieces")) {
      last_piece_result_ =
          ErrorResult("SetAllScorePiecesUnlocked(0)", api_.set_all_score_pieces(0));
    }
    ImGui::SameLine();
    if (api_.give_item && ImGui::Button("GiveItem(id)##pieces")) {
      last_piece_result_ =
          ErrorResult("GiveItem(piece)", api_.give_item(piece_number_ + 349, 1));
    }
    ImGui::SameLine();
    if (api_.take_item && ImGui::Button("TakeItem(id)##pieces")) {
      last_piece_result_ =
          ErrorResult("TakeItem(piece)", api_.take_item(piece_number_ + 349, 1));
    }

    // The error paths that only exist for score pieces.
    if (api_.set_marked_score_piece && ImGui::Button("Mark an uncollected piece##pieces")) {
      const int number = UncollectedPieceNumber();
      last_piece_result_ = number
                               ? ErrorResult("SetMarkedScorePiece(uncollected)",
                                             api_.set_marked_score_piece(number))
                               : std::string("every piece is collected: nothing to test with");
    }
    ImGui::SameLine();
    if (api_.get_score_piece && ImGui::Button("GetScorePiece(33)##pieces")) {
      EternalSonataScorePiece scratch{};
      last_piece_result_ = ErrorResult("GetScorePiece(33)", api_.get_score_piece(33, &scratch));
    }
    ImGui::SameLine();
    if (api_.can_add && ImGui::Button("CanAddItemToSet(350)##pieces")) {
      last_piece_result_ = ErrorResult("CanAddItemToSet(350)", api_.can_add(350));
    }

    if (!last_piece_result_.empty()) {
      ImGui::TextUnformatted(last_piece_result_.c_str());
      ImGui::TextDisabled(
          "None of these may return QUEUED: score pieces need no guest call. Marking an "
          "uncollected piece must be ERR_NOT_OWNED, GetScorePiece(33) ERR_INVALID_ITEM, and "
          "CanAddItemToSet(350) ERR_INVALID_ITEM.");
    }
  }

  void DrawGiveTakeSection() {
    if (!api_.ok() || (!api_.give_item && !api_.take_item)) {
      return;
    }
    ImGui::Text("Give / take:");
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("Item id##givetake", &give_item_id_);
    if (give_item_id_ < ETERNALSONATA_ITEM_ID_MIN) give_item_id_ = ETERNALSONATA_ITEM_ID_MIN;
    if (give_item_id_ > ETERNALSONATA_ITEM_ID_MAX) give_item_id_ = ETERNALSONATA_ITEM_ID_MAX;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    ImGui::InputInt("Count##givetake", &give_count_);
    ImGui::SameLine();
    if (api_.give_item && ImGui::Button("Give")) {
      last_write_result_ = WriteResult("GiveItem", api_.give_item(give_item_id_, give_count_));
    }
    ImGui::SameLine();
    if (api_.take_item && ImGui::Button("Take")) {
      last_write_result_ = WriteResult("TakeItem", api_.take_item(give_item_id_, give_count_));
    }

    if (!last_write_result_.empty()) {
      ImGui::TextUnformatted(last_write_result_.c_str());
      ImGui::TextDisabled(
          "QUEUED means it will apply on the guest thread's next frame; an item/set event "
          "should follow.");
    }
  }

  void DrawErrorPathsSection() {
    if (!api_.ok()) {
      return;
    }
    ImGui::Text("Error paths (raw returns):");
    EternalSonataItem scratch{};
    if (ImGui::Button("GetItem(0)")) {
      last_error_result_ = ErrorResult("GetItem(0)", api_.get_item(0, &scratch));
    }
    ImGui::SameLine();
    if (ImGui::Button("GetItem(513)")) {
      last_error_result_ = ErrorResult("GetItem(513)", api_.get_item(513, &scratch));
    }
    ImGui::SameLine();
    if (ImGui::Button("GetItem(1, NULL)")) {
      last_error_result_ = ErrorResult("GetItem(1, NULL)", api_.get_item(1, nullptr));
    }
    if (api_.give_item && ImGui::Button("GiveItem(0, 1)")) {
      last_error_result_ = ErrorResult("GiveItem(0, 1)", api_.give_item(0, 1));
    }
    ImGui::SameLine();
    if (api_.take_item && ImGui::Button("TakeItem(<unowned>, 1)")) {
      last_error_result_ = ErrorResult("TakeItem(unowned, 1)", api_.take_item(UnownedItemId(), 1));
    }
    ImGui::SameLine();
    if (api_.add_to_set && ImGui::Button("AddItemToSet(<unowned>)")) {
      last_error_result_ =
          ErrorResult("AddItemToSet(unowned)", api_.add_to_set(UnownedItemId()));
    }
    ImGui::SameLine();
    if (api_.remove_from_set && ImGui::Button("RemoveItemFromSet(<not in set>)")) {
      last_error_result_ =
          ErrorResult("RemoveItemFromSet(not in set)", api_.remove_from_set(UnownedItemId()));
    }
    if (api_.remove_set_slot && ImGui::Button("RemoveItemSetSlot(31)")) {
      last_error_result_ = ErrorResult("RemoveItemSetSlot(31)", api_.remove_set_slot(31));
    }
    if (!last_error_result_.empty()) {
      ImGui::TextUnformatted(last_error_result_.c_str());
      ImGui::TextDisabled(
          "Every one of these must be a negative ERR_ code (or QUEUED for an accepted "
          "unowned/not-in-set no-op is still wrong -- watch for a false OK/QUEUED here).");
    }
  }

  void DrawEventSection() {
    ImGui::Text("Item events: %d gained, %d lost, %d set-added, %d set-removed, %d mark",
               event_gained_.load(), event_lost_.load(), event_set_added_.load(),
               event_set_removed_.load(), event_marked_.load());
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear log")) {
      std::lock_guard<std::mutex> lock(events_mutex_);
      events_.clear();
    }

    const bool gained_ok = observed_gained_ == event_gained_.load();
    const bool lost_ok = observed_lost_ == event_lost_.load();
    const bool set_added_ok = observed_set_added_ == event_set_added_.load();
    const bool set_removed_ok = observed_set_removed_ == event_set_removed_.load();
    const bool marked_ok = observed_marked_ == event_marked_.load();
    ImGui::Text("Probe observed:   %d gained, %d lost, %d set-added, %d set-removed, %d mark",
               observed_gained_, observed_lost_, observed_set_added_, observed_set_removed_,
               observed_marked_);
    if (gained_ok && lost_ok && set_added_ok && set_removed_ok && marked_ok) {
      ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Events match what the probe observed.");
    } else {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                         "Mismatch: the host published a different number of events than the "
                         "probe observed.");
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
        "After loading a save, press Reset baseline: a load restores state without "
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
      if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
        ImGui::SetScrollHereY(1.0f);
      }
    }
    ImGui::EndChild();
  }

  // An id the player is very unlikely to hold, for exercising the
  // not-owned / not-in-set error paths without disturbing real state.
  int UnownedItemId() const { return 512; }

  void Poll() {
    if (!api_.ok()) {
      return;
    }

    if (api_.owned_count) {
      owned_count_ = api_.owned_count();
    }

    sizing_count_ = -1;
    fill_count_ = -1;
    items_.clear();
    if (api_.owned_items) {
      sizing_count_ = api_.owned_items(nullptr, 0);
      if (sizing_count_ > 0) {
        items_.resize(static_cast<size_t>(sizing_count_));
        fill_count_ = api_.owned_items(items_.data(), sizing_count_);
        if (fill_count_ < 0) {
          items_.clear();
        } else {
          items_.resize(static_cast<size_t>(fill_count_));
        }
      } else {
        fill_count_ = sizing_count_;
      }
    }

    CheckInvariants();
    CrossCheckReads();

    if (api_.get_set) {
      EternalSonataItemSet set{};
      if (api_.get_set(&set) == ETERNALSONATA_ITEM_OK) {
        set_ = set;
      }
    }

    PollScorePieces();
    ObserveTransitions();
  }

  void PollScorePieces() {
    pieces_.clear();
    piece_violations_.clear();
    collected_pieces_ = 0;
    marked_piece_ = api_.marked_score_piece ? api_.marked_score_piece() : 0;
    if (!api_.all_score_pieces) {
      return;
    }

    const int sizing = api_.all_score_pieces(nullptr, 0, pieces_collected_order_ ? 1 : 0);
    if (sizing > 0) {
      pieces_.resize(static_cast<size_t>(sizing));
      const int filled =
          api_.all_score_pieces(pieces_.data(), sizing, pieces_collected_order_ ? 1 : 0);
      pieces_.resize(filled < 0 ? 0 : static_cast<size_t>(filled));
    }

    // Counted off the pieces themselves rather than trusting the count call, so
    // the two can be compared.
    collected_pieces_ = api_.all_score_pieces(nullptr, 0, 1);
    if (collected_pieces_ < 0) {
      collected_pieces_ = 0;
    }

    piece_violations_.assign(pieces_.size(), 0u);
    for (size_t i = 0; i < pieces_.size(); ++i) {
      const EternalSonataScorePiece& piece = pieces_[i];
      unsigned v = 0;
      if (piece.item_id != piece.number + 349) v |= 1u << 0;
      if ((piece.order >= 0) != (piece.unlocked != 0)) v |= 1u << 1;
      if (api_.item_count && api_.item_count(piece.item_id) != (piece.unlocked ? 1 : 0)) {
        v |= 1u << 2;
      }
      EternalSonataItem as_item{};
      if (api_.get_item(piece.item_id, &as_item) == ETERNALSONATA_ITEM_OK) {
        if (as_item.slot != -1 || !as_item.is_score_piece ||
            as_item.score_piece_number != piece.number) {
          v |= 1u << 3;
        }
        if (as_item.set_entry_count != 0) v |= 1u << 4;
      }
      EternalSonataScorePiece directly{};
      if (api_.get_score_piece &&
          (api_.get_score_piece(piece.number, &directly) != ETERNALSONATA_ITEM_OK ||
           std::memcmp(&directly, &piece, sizeof(EternalSonataScorePiece)) != 0)) {
        v |= 1u << 5;
      }
      piece_violations_[i] = v;
    }
  }

  // A piece the player has not collected, for the mark-an-uncollected-piece
  // error path, or 0 if there is none.
  int UncollectedPieceNumber() const {
    if (!api_.score_piece_unlocked) {
      return 0;
    }
    for (int number = 1; number <= ETERNALSONATA_SCORE_PIECE_COUNT; ++number) {
      if (api_.score_piece_unlocked(number) == 0) {
        return number;
      }
    }
    return 0;
  }

  void CheckInvariants() {
    violations_.assign(items_.size(), 0u);
    for (size_t i = 0; i < items_.size(); ++i) {
      const EternalSonataItem& item = items_[i];
      unsigned v = 0;
      if (item.id < ETERNALSONATA_ITEM_ID_MIN || item.id > ETERNALSONATA_ITEM_ID_MAX) v |= 1u << 0;
      if (item.free_count != item.count - item.reserved_count) v |= 1u << 1;
      if (item.set_entry_count != item.reserved_count) v |= 1u << 2;
      if (item.category < ETERNALSONATA_ITEM_CATEGORY_CONSUMABLE ||
          item.category > ETERNALSONATA_ITEM_CATEGORY_ACCESSORY) {
        v |= 1u << 3;
      }
      for (size_t r = 0; r < sizeof(item.reserved) / sizeof(item.reserved[0]); ++r) {
        if (item.reserved[r] != 0) {
          v |= 1u << 4;
          break;
        }
      }
      violations_[i] = v;
    }
  }

  void CrossCheckReads() {
    cross_check_ok_ = true;
    if (owned_count_ >= 0 && fill_count_ >= 0 && owned_count_ != fill_count_) {
      cross_check_ok_ = false;
    }
    for (size_t i = 0; i < items_.size(); ++i) {
      EternalSonataItem directly{};
      if (api_.get_item(items_[i].id, &directly) != ETERNALSONATA_ITEM_OK) {
        cross_check_ok_ = false;
        continue;
      }
      if (std::memcmp(&directly, &items_[i], sizeof(EternalSonataItem)) != 0) {
        cross_check_ok_ = false;
      }
      if (api_.item_count) {
        const int count = api_.item_count(items_[i].id);
        if (count != items_[i].count) {
          cross_check_ok_ = false;
        }
      }
    }
  }

  // Diffs the current inventory + set against the previous poll to build the
  // probe's own view of what changed, independent of the bus.
  void ObserveTransitions() {
    const bool available = api_.available ? api_.available() != 0 : true;
    if (!available) {
      have_baseline_ = false;
      return;
    }

    std::array<int, ETERNALSONATA_ITEM_ID_MAX + 1> now_counts{};
    for (const EternalSonataItem& item : items_) {
      if (item.id >= 0 && item.id <= ETERNALSONATA_ITEM_ID_MAX) {
        now_counts[static_cast<size_t>(item.id)] = item.count;
      }
    }
    // Score pieces are not in the inventory, so GetOwnedItems never lists them,
    // but the host still publishes item.gained/lost for them. Fold them in or
    // the two counts could not agree.
    for (const EternalSonataScorePiece& piece : pieces_) {
      if (piece.item_id >= 0 && piece.item_id <= ETERNALSONATA_ITEM_ID_MAX) {
        now_counts[static_cast<size_t>(piece.item_id)] = piece.unlocked ? 1 : 0;
      }
    }
    std::array<bool, ETERNALSONATA_ITEM_ID_MAX + 1> now_in_set{};
    for (int i = 0; i < set_.count && i < ETERNALSONATA_ITEM_SET_CAPACITY; ++i) {
      const int id = set_.entries[i];
      if (id >= 0 && id <= ETERNALSONATA_ITEM_ID_MAX) {
        now_in_set[static_cast<size_t>(id)] = true;
      }
    }

    if (!have_baseline_) {
      have_baseline_ = true;
      last_counts_ = now_counts;
      last_in_set_ = now_in_set;
      last_marked_ = marked_piece_;
      return;
    }

    if (marked_piece_ != last_marked_) {
      ++observed_marked_;
      char line[128];
      std::snprintf(line, sizeof(line), "observed  mark      piece %d -> %d", last_marked_,
                    marked_piece_);
      PushEvent(line);
    }
    last_marked_ = marked_piece_;

    for (int id = ETERNALSONATA_ITEM_ID_MIN; id <= ETERNALSONATA_ITEM_ID_MAX; ++id) {
      const int before = last_counts_[static_cast<size_t>(id)];
      const int after = now_counts[static_cast<size_t>(id)];
      if (after > before) {
        ++observed_gained_;
        char line[128];
        std::snprintf(line, sizeof(line), "observed  gained    item %d: %d -> %d", id, before,
                      after);
        PushEvent(line);
      } else if (after < before) {
        ++observed_lost_;
        char line[128];
        std::snprintf(line, sizeof(line), "observed  lost      item %d: %d -> %d", id, before,
                      after);
        PushEvent(line);
      }
      const bool in_set_before = last_in_set_[static_cast<size_t>(id)];
      const bool in_set_after = now_in_set[static_cast<size_t>(id)];
      if (in_set_after && !in_set_before) {
        ++observed_set_added_;
        char line[128];
        std::snprintf(line, sizeof(line), "observed  set-added item %d", id);
        PushEvent(line);
      } else if (!in_set_after && in_set_before) {
        ++observed_set_removed_;
        char line[128];
        std::snprintf(line, sizeof(line), "observed  set-remvd item %d", id);
        PushEvent(line);
      }
    }
    last_counts_ = now_counts;
    last_in_set_ = now_in_set;
  }

  std::string WriteResult(const char* name, int result) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%s(%d%s) returned %s (%d)", name, set_item_id_, "",
                  ErrorName(result), result);
    return buffer;
  }

  std::string ErrorResult(const char* name, int result) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%s -> %s (%d)", name, ErrorName(result), result);
    return buffer;
  }

  void SubscribeToItemEvents() {
    if (!runtime_) {
      return;
    }
    auto* registry = runtime_->mod_registry();
    if (!registry) {
      return;
    }

    registry->Subscribe(ETERNALSONATA_ITEM_EVENT_GAINED,
                        [this](const rex::system::ModRegistry::EventPayload& payload) {
                          event_gained_.fetch_add(1);
                          char line[160];
                          std::snprintf(line, sizeof(line), "event     gained    item %llu, now %d",
                                        static_cast<unsigned long long>(payload.u64),
                                        static_cast<int>(payload.f64));
                          PushEvent(line);
                        });
    registry->Subscribe(ETERNALSONATA_ITEM_EVENT_LOST,
                        [this](const rex::system::ModRegistry::EventPayload& payload) {
                          event_lost_.fetch_add(1);
                          char line[160];
                          std::snprintf(line, sizeof(line), "event     lost      item %llu, now %d",
                                        static_cast<unsigned long long>(payload.u64),
                                        static_cast<int>(payload.f64));
                          PushEvent(line);
                        });
    registry->Subscribe(ETERNALSONATA_ITEM_EVENT_SET_ADDED,
                        [this](const rex::system::ModRegistry::EventPayload& payload) {
                          event_set_added_.fetch_add(1);
                          char line[160];
                          std::snprintf(line, sizeof(line),
                                        "event     set-added item %llu, entries %d",
                                        static_cast<unsigned long long>(payload.u64),
                                        static_cast<int>(payload.f64));
                          PushEvent(line);
                        });
    registry->Subscribe(ETERNALSONATA_ITEM_EVENT_SET_REMOVED,
                        [this](const rex::system::ModRegistry::EventPayload& payload) {
                          event_set_removed_.fetch_add(1);
                          char line[160];
                          std::snprintf(line, sizeof(line),
                                        "event     set-remvd item %llu, entries %d",
                                        static_cast<unsigned long long>(payload.u64),
                                        static_cast<int>(payload.f64));
                          PushEvent(line);
                        });
    registry->Subscribe(ETERNALSONATA_SCORE_PIECE_EVENT_MARKED,
                        [this](const rex::system::ModRegistry::EventPayload& payload) {
                          event_marked_.fetch_add(1);
                          char line[160];
                          std::snprintf(line, sizeof(line),
                                        "event     mark      piece %llu, item %d",
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
  ItemApi api_;
  bool visible_ = false;

  // Snapshot, owned by the draw thread.
  int owned_count_ = -1;
  int sizing_count_ = -1;
  int fill_count_ = -1;
  std::vector<EternalSonataItem> items_;
  std::vector<unsigned> violations_;
  bool cross_check_ok_ = false;
  EternalSonataItemSet set_{};

  std::vector<EternalSonataScorePiece> pieces_;
  std::vector<unsigned> piece_violations_;
  int collected_pieces_ = 0;
  int marked_piece_ = 0;
  bool pieces_collected_order_ = false;

  int set_item_id_ = 209;   // Floral Powder, the game's own starting set item
  int set_slot_index_ = 0;
  int give_item_id_ = 209;
  int give_count_ = 1;
  int piece_number_ = 1;
  std::string last_write_result_;
  std::string last_error_result_;
  std::string last_piece_result_;

  // The probe's own transition counters, draw-thread only.
  bool have_baseline_ = false;
  std::array<int, ETERNALSONATA_ITEM_ID_MAX + 1> last_counts_{};
  std::array<bool, ETERNALSONATA_ITEM_ID_MAX + 1> last_in_set_{};
  int observed_gained_ = 0;
  int observed_lost_ = 0;
  int observed_set_added_ = 0;
  int observed_set_removed_ = 0;
  int last_marked_ = 0;
  int observed_marked_ = 0;

  // Events, written by the registry tick.
  std::mutex events_mutex_;
  std::deque<std::string> events_;
  std::atomic<int> event_gained_{0};
  std::atomic<int> event_lost_{0};
  std::atomic<int> event_set_added_{0};
  std::atomic<int> event_set_removed_{0};
  std::atomic<int> event_marked_{0};
};

class ItemProbeMod : public rex::system::IModPlugin {
 public:
  explicit ItemProbeMod(rex::Runtime* runtime) : runtime_(runtime) {}

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    dialog_ = std::make_unique<ItemProbeDialog>(drawer, runtime_);
  }

 private:
  rex::Runtime* runtime_ = nullptr;
  std::unique_ptr<ItemProbeDialog> dialog_;
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
  return new ItemProbeMod(ctx->runtime);
}
