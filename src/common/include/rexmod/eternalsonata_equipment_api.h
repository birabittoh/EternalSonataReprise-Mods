// eternalsonata - ReXGlue Recompiled Project
//
// Public C ABI for the game's equipment: what each character is wearing in
// each of its four slots, what it could wear there, what a swap would do to
// its stats, and how to equip and unequip from a mod.
//
// This is the companion to eternalsonata_item_api.h and shares its id space:
// a piece of equipment is an item, with a record in the same master entity
// table and a name in the same text blocks. The reverse-engineering behind it
// is written up in docs/equipment.md.
//
// A mod does NOT link against this project. Copy this header into the mod and
// resolve the entry points at runtime out of the host executable, the same way
// the Item API is used (see eternalsonata_item_api.h):
//
//     auto equip = reinterpret_cast<EternalSonataEquipFn>(
//         GetProcAddress(GetModuleHandle(nullptr), "EternalSonataEquip"));
//     if (equip) { equip(1, 0, 1); }  // Allegretto, weapon slot, Hunting Knife
//
// Always null-check: a mod built against a newer host must still load on an
// older one. Check EternalSonataEquipmentAbiVersion() before using anything
// added after version 1.
//
// Events. Changes are published on the shared mod registry bus
// (rex::system::ModRegistry, reached via runtime->mod_registry()), so a mod
// subscribes by name and needs neither this header nor a linked symbol:
//
//     ETERNALSONATA_EQUIPMENT_EVENT_EQUIPPED   "eternalsonata.equipment.equipped"
//     ETERNALSONATA_EQUIPMENT_EVENT_UNEQUIPPED "eternalsonata.equipment.unequipped"
//
// In both the payload's `u64` is the character number and `f64` is the item id
// that went into or came out of the slot. `bytes` is empty. The slot is not in
// the payload; read it back with EternalSonataGetAllEquipment if it matters.
// They fire for the game's own changes as well as for a mod's, on the frame
// after the change. Loading a save republishes nothing: whatever the save
// restores is adopted silently.
//
// Threading. Every entry point here is safe to call from any thread, including
// the ImGui draw thread. Reads answer from guest memory immediately, and so
// does EternalSonataPreviewEquip, which is host-side arithmetic over the same
// tables the game reads. Writes have to run guest code, so they are queued
// onto the guest main thread and applied on its next frame, because guest
// calls need a live ThreadState that the draw thread does not have, and
// calling them from a draw hook crashes the game. Those functions therefore
// return ETERNALSONATA_EQUIPMENT_QUEUED rather than a final result, unless
// they are called from work already running on the guest main thread, in which
// case they run inline and return the real outcome. Everything that can be
// decided without running guest code (unknown character, unknown item, wrong
// slot for the item, the character cannot use it, the player does not hold
// one) is still reported immediately, before anything is queued; use
// EternalSonataCanEquip to ask that question on its own.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumped whenever anything below changes meaning. Additive changes bump the
// version; existing entry points keep their signature.
#define ETERNALSONATA_EQUIPMENT_ABI_VERSION 1u

// Event names on the mod registry bus. See the note at the top.
#define ETERNALSONATA_EQUIPMENT_EVENT_EQUIPPED "eternalsonata.equipment.equipped"
#define ETERNALSONATA_EQUIPMENT_EVENT_UNEQUIPPED "eternalsonata.equipment.unequipped"

// Characters are the same 1..10 the Party API uses.
#define ETERNALSONATA_EQUIPMENT_CHARACTER_MIN 1
#define ETERNALSONATA_EQUIPMENT_CHARACTER_MAX 10

// Item ids are the same 1..512 the Item API uses.
#define ETERNALSONATA_EQUIPMENT_ITEM_ID_MIN 1
#define ETERNALSONATA_EQUIPMENT_ITEM_ID_MAX 512

// Four slots per character, and the game offers no fifth. The stat struct has
// room for more halfwords after the four ids, but the recompute
// (sub_821E7898) and the commit (sub_821E8390) both only ever see these four,
// and what follows them is the Score Piece block, which is a different system.
#define ETERNALSONATA_EQUIPMENT_SLOT_COUNT 4

// The slots, in the order the game's own equipment screen lists them. Which
// items fit which slot is decided by the item's category (the same
// ETERNALSONATA_ITEM_CATEGORY_* value the Item API reports), so the two
// accessory slots take exactly the same items as each other.
enum {
  ETERNALSONATA_EQUIPMENT_SLOT_WEAPON = 0,     // category 1
  ETERNALSONATA_EQUIPMENT_SLOT_ARMOR = 1,      // category 2
  ETERNALSONATA_EQUIPMENT_SLOT_ACCESSORY1 = 2, // category 3
  ETERNALSONATA_EQUIPMENT_SLOT_ACCESSORY2 = 3  // category 3
};

// Results. Everything >= 0 is success.
enum {
  ETERNALSONATA_EQUIPMENT_OK = 0,
  // The change was accepted and will be applied on the guest thread's next
  // frame. See the threading note at the top.
  ETERNALSONATA_EQUIPMENT_QUEUED = 1,

  // Nothing is loaded yet, or the tables are not mapped (e.g. at the title
  // screen before a save is loaded). Every mutation refuses in this state.
  ETERNALSONATA_EQUIPMENT_ERR_UNAVAILABLE = -1,
  // The character number is outside 1..10.
  ETERNALSONATA_EQUIPMENT_ERR_INVALID_CHARACTER = -2,
  // The slot is outside 0..3.
  ETERNALSONATA_EQUIPMENT_ERR_INVALID_SLOT = -3,
  // The item id is outside 1..512.
  ETERNALSONATA_EQUIPMENT_ERR_INVALID_ITEM = -4,
  // The item is not equipment at all, or is equipment this character is not
  // allowed to wear.
  ETERNALSONATA_EQUIPMENT_ERR_NOT_EQUIPPABLE = -5,
  // The item is equipment this character can wear, but not in this slot: its
  // category names a different one.
  ETERNALSONATA_EQUIPMENT_ERR_WRONG_SLOT = -6,
  // The player is not carrying one. Equipment is taken out of the inventory
  // while it is worn, so an item already on somebody cannot be equipped again
  // until it is removed.
  ETERNALSONATA_EQUIPMENT_ERR_NOT_OWNED = -7,
  // The slot is already empty (unequip only).
  ETERNALSONATA_EQUIPMENT_ERR_SLOT_EMPTY = -8,
  ETERNALSONATA_EQUIPMENT_ERR_INVALID_ARGUMENT = -10
};

// One equipment slot: what is in it, and what that item contributes.
typedef struct EternalSonataEquipment {
  // 1..10, the character this slot belongs to.
  int32_t character;
  // 0..3, one of the ETERNALSONATA_EQUIPMENT_SLOT_* values.
  int32_t slot;
  // The item in the slot, or 0 when the slot is empty. All the fields below
  // are 0 in that case.
  int32_t item_id;
  // Into the same BTX blocks the Item API reads, equal to item_id - 1. The
  // strings themselves are answered by EternalSonataGetEquipmentName and
  // EternalSonataGetEquipmentDescription.
  int32_t name_text_id;
  int32_t description_text_id;
  // The item's category, which is what decides the slot it fits: 1 weapon,
  // 2 armor, 3 accessory. Same values as ETERNALSONATA_ITEM_CATEGORY_*.
  int32_t category;

  // The flat bonus this item contributes, straight from its master record.
  // This is the additive half of the game's stat maths only: each item also
  // carries five multipliers that are pooled across everything the character
  // is wearing and applied to the totals afterwards, so these numbers do not
  // add up to the difference the status screen shows. For the number a screen
  // would actually draw, use EternalSonataPreviewEquip.
  int32_t hp, attack, magic, defense, speed;

  int32_t reserved[8];  // zero-filled; room for later additions
} EternalSonataEquipment;

// The six stats the equipment screen compares, in the order the game's own
// preview hands them back.
typedef struct EternalSonataEquipStats {
  int32_t hp_max;
  int32_t attack;
  int32_t magic;
  int32_t defense;
  int32_t speed;
  // Current HP, rescaled by however much maximum HP moved. Never below 1.
  int32_t hp;

  int32_t reserved[8];  // zero-filled; room for later additions
} EternalSonataEquipStats;

// ---------------------------------------------------------------------------
// Capability
// ---------------------------------------------------------------------------

// Host ABI version, so a mod can tell what it is talking to.
typedef uint32_t (*EternalSonataEquipmentAbiVersionFn)(void);

// True once the stat arrays, the master table and the inventory are all
// readable and a save has been loaded. False at the title screen.
typedef int (*EternalSonataIsEquipmentSystemAvailableFn)(void);

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

// Fills `out` with what `character` has in `slot`. An empty slot is not an
// error: it answers OK with item_id 0. Returns ETERNALSONATA_EQUIPMENT_OK or a
// negative error.
typedef int (*EternalSonataGetEquipmentFn)(int character, int slot,
                                           EternalSonataEquipment* out);

// Fills `out` with up to `max` of the character's slots, in slot order,
// empty ones included, and returns how many were written, or a negative error.
// Pass max = 0 to just count. There are always
// ETERNALSONATA_EQUIPMENT_SLOT_COUNT of them.
typedef int (*EternalSonataGetAllEquipmentFn)(int character, EternalSonataEquipment* out,
                                              int max);

// The item id in one slot, 0 when the slot is empty, or a negative error.
// Shorthand for reading EternalSonataEquipment::item_id.
typedef int (*EternalSonataGetEquippedItemFn)(int character, int slot);

// The equipment's name and description in the language the game is running
// in. Identical to the Item API's answers for the same id, and repeated here
// so a mod that only wants equipment does not have to resolve both APIs.
// Never null: an id the blocks have nothing for answers "". The returned
// pointer stays valid for the life of the process.
typedef const char* (*EternalSonataGetEquipmentNameFn)(int item_id);
typedef const char* (*EternalSonataGetEquipmentDescriptionFn)(int item_id);

// ---------------------------------------------------------------------------
// What fits where
// ---------------------------------------------------------------------------

// Whether `character` is allowed to wear `item_id` at all, ignoring slots and
// ignoring whether the player holds one: 1 yes, 0 no, or a negative error.
// This is the game's own eligibility test and it answers without running any
// guest code.
typedef int (*EternalSonataCanEquipFn)(int character, int item_id);

// Which slot `item_id` goes in, one of the ETERNALSONATA_EQUIPMENT_SLOT_*
// values, or a negative error. An accessory answers
// ETERNALSONATA_EQUIPMENT_SLOT_ACCESSORY1, since either accessory slot takes
// it. Anything that is not equipment answers
// ETERNALSONATA_EQUIPMENT_ERR_NOT_EQUIPPABLE.
typedef int (*EternalSonataGetEquipmentSlotForItemFn)(int item_id);

// Whether EternalSonataEquip would be accepted right now, without changing
// anything: ETERNALSONATA_EQUIPMENT_OK, or the error it would fail with
// (NOT_EQUIPPABLE, WRONG_SLOT, NOT_OWNED, ...).
typedef int (*EternalSonataCanEquipInSlotFn)(int character, int slot, int item_id);

// Fills `ids_out` with up to `max` item ids the player is carrying that
// `character` could put in `slot`, and returns how many were written, or a
// negative error. Pass max = 0 to just count. What is already worn is not in
// the list: the game takes equipment out of the inventory while it is on
// somebody.
typedef int (*EternalSonataGetEquippableItemsFn)(int character, int slot, int* ids_out,
                                                 int max);

// ---------------------------------------------------------------------------
// Previewing
// ---------------------------------------------------------------------------

// Fills `out` with the stats `character` would have with `item_id` in `slot`,
// changing nothing. Pass item_id 0 to preview emptying the slot. If the
// character cannot wear the item, `out` is filled with its current stats and
// the call returns ETERNALSONATA_EQUIPMENT_ERR_NOT_EQUIPPABLE, which is what
// the game's own comparison does. Returns ETERNALSONATA_EQUIPMENT_OK on a
// preview that would be allowed.
typedef int (*EternalSonataPreviewEquipFn)(int character, int slot, int item_id,
                                           EternalSonataEquipStats* out);

// The stats `character` has right now, the same six values a preview answers
// with. Returns ETERNALSONATA_EQUIPMENT_OK or a negative error.
typedef int (*EternalSonataGetEquipStatsFn)(int character, EternalSonataEquipStats* out);

// ---------------------------------------------------------------------------
// Equipping
// ---------------------------------------------------------------------------

// Puts `item_id` in `character`'s `slot` through the game's own commit, so the
// item leaves the inventory, whatever was in the slot goes back into it, and
// the character's stats are recomputed. Returns
// ETERNALSONATA_EQUIPMENT_QUEUED, or a negative error decided up front.
typedef int (*EternalSonataEquipFn)(int character, int slot, int item_id);

// Empties `character`'s `slot`, putting what was in it back in the inventory
// and recomputing the character's stats. Returns
// ETERNALSONATA_EQUIPMENT_QUEUED, or a negative error decided up front.
typedef int (*EternalSonataUnequipFn)(int character, int slot);

// Empties all four of `character`'s slots. Returns
// ETERNALSONATA_EQUIPMENT_QUEUED, or a negative error decided up front. A slot
// that is already empty is skipped rather than being an error.
typedef int (*EternalSonataUnequipAllFn)(int character);

#ifdef __cplusplus
}  // extern "C"
#endif
