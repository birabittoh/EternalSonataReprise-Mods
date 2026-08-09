// party_overlay mod - overlay (F5) showing live party state read straight
// from guest memory, mirroring what the in-game status screen paints, plus
// buttons to add owned-but-benched characters back into the party. Read-only
// display; every address is hardcoded (the title has no symbol registry).
//
// Party struct base = 0x8243F3E8 (sub_821E6740 add / sub_821E5D68 reset):
//   +0x004  party level 1..6, low byte @ 0x8243F3EF
//   +0x8DC/+0x8DD u8 remaining/used level-cap budget; cap table is
//           word_8202CA70[level-1]
//
// +0x856 (word_8243FC3E, u16[32]) is NOT shown here. It looks like a "menu
// roster" of recruited char ids, and sub_821E6740 (the join routine, called
// by the party-menu "add" handler sub_82225FE0) does append to it - but it's
// only ever written by a live join event during the current process's
// execution, never restored from save data (the reset paths sub_821E5D68/
// sub_821E5A38 just zero it). So after loading a save where everyone was
// already recruited in a past session, it sits on whatever the heap left
// there (0xCD fill) until a new character actually joins - it does not
// reflect your save's real party and isn't worth displaying.
//
// Status screen (sub_82230658 driver, sub_822352F8 painter):
//   dword_8243FC08[10] is NOT "char ids in screen order". It is keyed by
//   CHARACTER NUMBER and its value is that character's 1-based display
//   POSITION on the status screen (0 = not in the party):
//
//       dword_8243FC08[c - 1] = display position of character c
//
//   Evidence:
//   * sub_820E78B8 (party-add) writes FC08[c-1] = (number of nonzero
//     entries) + 1, i.e. it appends the new member at the next free
//     *position* while leaving its slot fixed.
//   * sub_821E6428 and sub_821E6240 both locate a member with the idiom
//     "walk FC08 until *p == P, take the 1-based index" - a reverse lookup
//     of "which character is at position P", which only type-checks if the
//     stored value is a position.
//   * dword_8243FC04 (IDA's name for party base +0x81C) is simply a 1-based
//     alias of this same array: FC04[c] and FC08[c-1] are the same 4 bytes.
//     There is no second table. An earlier revision of this file read the
//     values as char ids and could not reconcile the two accessors; that
//     apparent contradiction was purely the 1-based/0-based aliasing.
//   * Both of the user's fully-reordered 10-character saves match this
//     model on all 10 entries, and the in-game "swap two reserves" test
//     comes out as an exact two-element exchange of position values.
//
//   Active party = the three characters whose position value is 1, 2 or 3
//   (this is exactly what sub_821E6428's roster build searches for). It is
//   not a fixed set of memory slots: a previous revision hardcoded slots
//   {0, 2, 3}, which only holds for saves whose first six characters happen
//   to sit at the position equal to their character number.
//
//   Stats live at 0x8243FD08 + 48*(c - 1), keyed by CHARACTER NUMBER, not by
//   display position: sub_822352F8 calls
//   sub_821E7898(c, &unk_8243FD08 + 48*c - 48) with the same index on both
//   sides, and sub_821E7898 bounds it to 1..10 and sources the character's
//   saved stats from 0x8243FD18 + 48*(c - 1). Layout: +0x00 level,
//   +0x0C/+0x10 HP cur/max, +0x14.. u16 ATK/MGC/DEF/SPD.
//
// "Add missing party member" buttons call the game's own join sequence
// through the runtime function dispatcher (no exports, so runtime dispatch
// by guest address is the only callable path):
//   1. sub_821FBFC0(&dword_8255EED8, id, 1, 0) - marks the character eligible
//      in the party-member DB (a u16 search table at 0x8255EF08, stride 2,
//      up to 0x8255F708; eligibility = (byte[+2]-byte[+0]) of the u32 record).
//      Without this, chars not in the starting party fail sub_821E6740's
//      eligibility gate (sub_821FBF20) with code 3.
//   2. sub_821E6740(id) - the real roster-add routine (same one
//      sub_821E5D68's reset and the party-menu handler use): validates
//      against the master char table, checks the budget, writes into the
//      roster (word_8243FC3E), updates owned/used counters. Returns
//      0 added / 1 roster full / 2 budget insufficient / 3 not eligible.
//      Pure guest-memory ops, safe to call with a zeroed PPCContext.
//   3. sub_821E6740 does not touch the status screen's FC08 list, so on
//      success the overlay assigns the next free display position itself
//      (mirroring sub_820E78B8),
//      then calls sub_821E6428() to rebuild the battle-party objects from
//      FC08 - the same sync the game's party-menu add/remove path runs.
//      Unlike step 1/2 this needs a real guest stack frame, so it's
//      dispatched via Execute() on a scratch stack + ThreadState (see
//      SyncBattleParty). Its file-I/O legs are gated on the battle-data
//      magic dword_8244B9A0 == 0x4E535953, so outside battle it degenerates
//      to memory-only ops.

#include <rex/system/mod_plugin.h>

#include <cstdint>
#include <memory>
#include <string>

#include <imgui.h>

#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

// The game's own Discord-presence code (src/room_presence.h/cpp in the game
// repo) already tracks battle state reliably via guest-function hooks -- it
// is NOT derivable from a single guest memory poll (tried and confirmed
// wrong: dword_8244B9A0 is a live heap pointer, not a fixed magic, and reads
// nonzero outside battle too). The game exe exports
// EternalSonataIsBattleActive() specifically so mods loaded into the same
// process can query that same tracked state instead of re-deriving it.
bool IsBattleActive() {
  using IsBattleActiveFn = bool (*)();
#if defined(_WIN32)
  static IsBattleActiveFn fn = reinterpret_cast<IsBattleActiveFn>(
      GetProcAddress(GetModuleHandleA(nullptr), "EternalSonataIsBattleActive"));
#else
  static IsBattleActiveFn fn = reinterpret_cast<IsBattleActiveFn>(
      dlsym(RTLD_DEFAULT, "EternalSonataIsBattleActive"));
#endif
  return fn && fn();
}

constexpr uint32_t kPartyBase = 0x8243F3E8;

constexpr uint32_t kPartyLevelAddr = 0x8243F3EF;    // u8: party level 1..6 (low byte of +0x04)
constexpr uint32_t kCapRemainingAddr = 0x8243FCC4;  // u8: remaining level capacity (+0x8DC)
constexpr uint32_t kCapUsedAddr = 0x8243FCC5;       // u8: used level capacity (+0x8DD)
constexpr uint32_t kCapTableAddr = 0x8202CA70;      // u16[12]: caps per party level

// u32[10]: display position of character c at index c-1 (0 = not in party).
// See the long note at the top of this file - this is a position table, not
// a list of char ids in screen order.
constexpr uint32_t kStatusMemberListAddr = 0x8243FC08;
constexpr uint32_t kStatusMemberListCount = 10;
constexpr uint32_t kStatusMemberListEnd = 0x8243FC30;   // one past the last u32
constexpr uint32_t kMemberInListFlagAddr = 0x8243FC2F;  // byte_8243FC2F, 1-based by char number
// The status screen's active party is display positions 1..3; everyone with a
// higher position is a reserve. byte_8243FC3A[3] looked like the natural place
// to find the active set, but every xref to it is either a reset-to-zero or a
// read - nothing in the binary writes real values into it, which is why
// sub_821E6428 always takes its fallback path and rebuilds the roster by
// searching FC08 for position values 1, 2 and 3.
constexpr uint32_t kActivePartySize = 3;
constexpr uint32_t kStatusSlotStatsAddr = 0x8243FD08;   // per-character stat structs
constexpr uint32_t kStatusSlotStatsStride = 48;
constexpr uint32_t kStatLevelOffset = 0x00;  // u32 (provisional: level)
constexpr uint32_t kStatHpCurOffset = 0x0C;  // u32
constexpr uint32_t kStatHpMaxOffset = 0x10;  // u32
constexpr uint32_t kStat1Offset = 0x14;      // u16 ATK
constexpr uint32_t kStat2Offset = 0x16;      // u16 MGC
constexpr uint32_t kStat3Offset = 0x18;      // u16 DEF
constexpr uint32_t kStat4Offset = 0x1A;      // u16 SPD

constexpr uint32_t kMaxPartyMembers = 10;

// Character number -> name, taken verbatim from the game's own name table at
// 0x8203304B, which stores the cast in character-number order (it appears
// twice back to back, the second copy prefixed with the "<r>" ruby marker).
// Note the order: Polka is character 2, Beat 3, Frederic 4. An earlier
// revision had 2/3/4 as Beat/Frederic/Polka because it validated names by max
// HP while reading FC08 values as char ids; once FC08 is read as a position
// table (see the note at the top) the binary's own order is the correct one.
constexpr const char* kCharNames[kMaxPartyMembers + 1] = {
    nullptr, "Allegretto", "Polka", "Beat", "Frederic", "Viola", "Salsa",
    "Jazz",   "Falsetto",  "Claves", "March"};

template <typename T>
bool ReadGuest(rex::memory::Memory* memory, uint32_t guest_address, T* out) {
  auto* host = memory->TranslateVirtual<const uint8_t*>(guest_address);
  if (!host) {
    return false;
  }
  *out = rex::memory::load_and_swap<T>(host);
  return true;
}

template <typename T>
bool WriteGuest(rex::memory::Memory* memory, uint32_t guest_address, const T& value) {
  auto* host = memory->TranslateVirtual<uint8_t*>(guest_address);
  if (!host) {
    return false;
  }
  rex::memory::store_and_swap<T>(host, value);
  return true;
}

bool WriteGuestByte(rex::memory::Memory* memory, uint32_t guest_address, uint8_t value) {
  auto* host = memory->TranslateVirtual<uint8_t*>(guest_address);
  if (!host) {
    return false;
  }
  *host = value;
  return true;
}

bool IsReadable(rex::memory::Memory* memory, uint32_t guest_address, uint32_t span) {
  auto* heap = memory->LookupHeap(guest_address);
  return heap && heap->QueryRangeAccess(guest_address, guest_address + span - 1) !=
                     rex::memory::PageAccess::kNoAccess;
}

bool ReadGuestByte(rex::memory::Memory* memory, uint32_t guest_address, uint8_t* out) {
  auto* host = memory->TranslateVirtual<const uint8_t*>(guest_address);
  if (!host) {
    return false;
  }
  *out = *host;
  return true;
}

class PartyOverlayDialog : public rex::ui::ImGuiDialog {
 public:
  PartyOverlayDialog(rex::ui::ImGuiDrawer* drawer, rex::Runtime* runtime)
      : ImGuiDialog(drawer), runtime_(runtime) {
    rex::ui::RegisterBind(
        "bind_party_overlay", "F5", "Toggle party overlay", [this] { visible_ = !visible_; },
        [this] { return visible_; }, "Party Overlay");
  }

  ~PartyOverlayDialog() override { rex::ui::UnregisterBind("bind_party_overlay"); }

 protected:
  // Status members: FC08[c-1] is character c's 1-based display position (0 =
  // not in the party), and that character's stats live at
  // 0x8243FD08 + 48*(c-1) - both keyed by character number, so a member's
  // stats stay put while a reorder only rewrites position values. See the
  // note at the top of this file.
  struct StatusMember {
    bool readable = false;
    uint16_t char_id = 0;   // 1-based character number
    uint32_t position = 0;  // 1-based display position, 0 = not in party
    uint32_t level = 0;
    uint32_t hp_cur = 0;
    uint32_t hp_max = 0;
    uint16_t s1 = 0;
    uint16_t s2 = 0;
    uint16_t s3 = 0;
    uint16_t s4 = 0;
  };

  void OnDraw(ImGuiIO& io) override {
    (void)io;
    if (!visible_) {
      return;
    }

    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Party Overlay", &visible_, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::End();
      return;
    }

    auto* memory = runtime_ ? runtime_->memory() : nullptr;
    if (!memory || !IsReadable(memory, kPartyBase, 8)) {
      ImGui::TextDisabled("Start or load a game to view party state.");
      ImGui::End();
      return;
    }

    uint8_t level = 0;
    ReadGuestByte(memory, kPartyLevelAddr, &level);

    // Capacity budget.
    uint8_t cap_remaining = 0;
    uint8_t cap_used = 0;
    ReadGuestByte(memory, kCapRemainingAddr, &cap_remaining);
    ReadGuestByte(memory, kCapUsedAddr, &cap_used);
    uint16_t cap_table = 0;
    bool cap_table_readable = false;
    if (level >= 1 && level <= 6) {
      cap_table_readable = IsReadable(memory, kCapTableAddr + 2 * (level - 1), 2);
      cap_table_readable =
          cap_table_readable && ReadGuest(memory, kCapTableAddr + 2 * (level - 1), &cap_table);
    }
    uint16_t cap = static_cast<uint8_t>(cap_table);

    StatusMember members[kStatusMemberListCount];
    uint32_t status_members = 0;
    for (uint32_t c = 0; c < kStatusMemberListCount; ++c) {
      StatusMember& s = members[c];
      uint32_t pos = 0;
      uint32_t pos_addr = kStatusMemberListAddr + 4 * c;
      if (!IsReadable(memory, pos_addr, 4) || !ReadGuest(memory, pos_addr, &pos)) {
        break;
      }
      s.readable = true;
      s.char_id = static_cast<uint16_t>(c + 1);
      s.position = pos;
      if (pos == 0) {
        continue;
      }
      ++status_members;
      uint32_t base = kStatusSlotStatsAddr + kStatusSlotStatsStride * c;
      bool stats_ok =
          IsReadable(memory, base, kStat4Offset + 2) &&
          ReadGuest(memory, base + kStatLevelOffset, &s.level) &&
          ReadGuest(memory, base + kStatHpCurOffset, &s.hp_cur) &&
          ReadGuest(memory, base + kStatHpMaxOffset, &s.hp_max) &&
          ReadGuest(memory, base + kStat1Offset, &s.s1) &&
          ReadGuest(memory, base + kStat2Offset, &s.s2) &&
          ReadGuest(memory, base + kStat3Offset, &s.s3) &&
          ReadGuest(memory, base + kStat4Offset, &s.s4);
      if (!stats_ok) {
        s.s1 = 0xFFFF;
      }
    }

    // Reverse the position table so rows can be walked in true screen order.
    // Positions the game never assigned (or duplicated, which shouldn't
    // happen) simply stay empty.
    const StatusMember* by_position[kStatusMemberListCount + 1] = {};
    for (const StatusMember& s : members) {
      if (s.position >= 1 && s.position <= kStatusMemberListCount &&
          by_position[s.position] == nullptr) {
        by_position[s.position] = &s;
      }
    }

    ImGui::Text("Party Lv. %u", level);
    ImGui::SameLine();
    ImGui::Text("Status members %u", status_members);

    if (cap_table_readable) {
      bool budget_ok = (cap_used + cap_remaining) == cap;
      if (budget_ok) {
        ImGui::Text("Budget used %u / cap %u  (rem %u)", cap_used, cap, cap_remaining);
      } else {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "Budget MISMATCH used %u + rem %u != cap %u", cap_used, cap_remaining,
                           cap);
      }
    } else {
      ImGui::TextDisabled("cap table unreadable at level %u", level);
    }

    // Status members + add-member buttons need an actual loaded party. At
    // the main menu status_members is 0 and the add-member join sequence
    // (sub_821FBFC0/sub_821E6740/sub_821E6428) crashes the game when called
    // against unloaded party state, so skip this whole section (including
    // its separator) rather than just hiding the member list.
    if (status_members > 0) {
      ImGui::Separator();

      // Fixed column so every row's Stats button lines up regardless of name
      // length ("  #10 Allegretto" is the widest case).
      const float kStatsButtonColumnX =
          ImGui::CalcTextSize("  #10 Allegretto").x + 3.0f * ImGui::GetStyle().ItemSpacing.x;

      // Renders the member occupying display position p. Stats were read from
      // kStatusSlotStatsAddr + 48*(char-1), keyed by character number the same
      // way the game's own painter sub_822352F8 does, so this is exactly what
      // the status screen shows.
      auto render_member_row = [&](uint32_t p) {
        const StatusMember* m = by_position[p];
        if (m == nullptr) {
          ImGui::TextDisabled("  #%u empty", p);
          return;
        }
        const StatusMember& s = *m;
        const char* name =
            (s.char_id >= 1 && s.char_id <= kMaxPartyMembers) ? kCharNames[s.char_id] : nullptr;
        if (!s.readable) {
          ImGui::TextDisabled("  #%u not readable", p);
          return;
        }
        ImGui::Text("  #%u %s", p, name ? name : "??");
        ImGui::SameLine(kStatsButtonColumnX);
        std::string stats_btn = "Stats##stats" + std::to_string(s.char_id);
        if (ImGui::Button(stats_btn.c_str())) {
          detail_open_[s.char_id] = !detail_open_[s.char_id];
        }
      };

      // Positions 1..3 are the active party, the rest are reserves.
      const uint32_t active_count =
          status_members < kActivePartySize ? status_members : kActivePartySize;
      ImGui::Text("Active party:");
      for (uint32_t p = 1; p <= active_count; ++p) {
        render_member_row(p);
      }

      if (status_members > active_count) {
        ImGui::Text("Reserves:");
        for (uint32_t p = active_count + 1; p <= status_members; ++p) {
          render_member_row(p);
        }
      }

      // Detail/edit sub-overlays, one per character with an open "Stats"
      // button. Each is its own floating ImGui window (not nested inside
      // Party Overlay) so it can be moved/closed independently.
      for (uint16_t cid = 1; cid <= kMaxPartyMembers; ++cid) {
        if (detail_open_[cid]) {
          RenderDetailWindow(memory, cid, members[cid - 1]);
        }
      }

      // Add-missing-party-member buttons: one per char id (1..10, the game's
      // full cast) not currently in the status member list - this includes
      // characters not yet story-eligible, which is why the join sequence
      // below starts with an eligibility bump (sub_821FBFC0) before the real
      // roster-add (sub_821E6740). Then gives the character the next free
      // display position in FC08. A character is in the party iff its position
      // value is nonzero.
      auto in_party = [&](uint16_t cid) {
        return members[cid - 1].readable && members[cid - 1].position != 0;
      };
      bool any_missing = false;
      for (uint16_t cid = 1; cid <= kMaxPartyMembers; ++cid) {
        if (!in_party(cid)) {
          any_missing = true;
          break;
        }
      }
      // The whole join sequence (sub_821FBFC0/sub_821E6740/sub_821E6428)
      // crashes mid-battle (see the note on kGuestBattleSyncFuncAddr), so the
      // section is skipped outright while a battle is active rather than
      // shown disabled.
      if (any_missing && !IsBattleActive()) {
        ImGui::Separator();
        ImGui::Text("Add missing party member:");
        uint32_t button_i = 0;
        for (uint16_t cid = 1; cid <= kMaxPartyMembers; ++cid) {
          if (in_party(cid)) {
            continue;
          }
          const char* name = kCharNames[cid];
          std::string label = name ? std::string(name) : ("Char " + std::to_string(cid));
          label += "##add" + std::to_string(cid);
          // Two buttons per row.
          if ((button_i % 2) != 0) {
            ImGui::SameLine();
          }
          ++button_i;
          if (ImGui::Button(label.c_str())) {
            CallGuestAdd(cid);
          }
        }
      }
    }

    ImGui::End();
  }

  private:
  // Guest addresses of the game's roster-add and member-availability
  // routines. The add is gated on eligibility (sub_821FBF20); the availability
  // routine (sub_821FBFC0, a1=&dword_8255EED8, a2=char id, a3=amount,
  // a4=refresh-flag) inserts the char into the member DB / raises its count
  // byte and returns the new count. The reset path sub_821E5D68 seeds the
  // starting party as: sub_821FBFC0(&dword_8255EED8, id, 1, 0) then
  // sub_821E6740(id) - we mirror that exactly.
  static constexpr uint32_t kGuestAddFuncAddr = 0x821E6740;
  static constexpr uint32_t kGuestBumpFuncAddr = 0x821FBFC0;
  static constexpr uint32_t kMemberDbGuestAddr = 0x8255EED8;
  // Battle-party sync routine sub_821E6428() - repacks the battle-party
  // objects unk_824D0480/unk_824D13C0 from the active-party bytes
  // (byte_8243FC3A) + the FC08 status list. Called by the game's party-menu
  // add/remove path (sub_820E78B8/sub_820E7948) and by battle load
  // (sub_820FD998). Requires a real guest stack frame; dispatched via
  // Execute() on a scratch stack (see SyncBattleParty).
  static constexpr uint32_t kGuestBattleSyncFuncAddr = 0x821E6428;
  // The whole guest join sequence (sub_821FBFC0/sub_821E6740/sub_821E6428)
  // crashes mid-battle with STATUS_FLOAT_INVALID_OPERATION (0xC000008F) -
  // reproduced via lldb 2026-08-07, deep inside sub_821E6428's battle-model/
  // skinning math (bottoms out in a vmaddfp reading what looks like an
  // uninitialized transform for the newly-added character's battle object,
  // since it never went through the game's own .bop load/setup path). The
  // overlay disables its add-member buttons entirely while a battle is
  // active (see the file-scope IsBattleActive()) rather than work around it.

  // Invoke the game's own join sequence through the runtime function
  // dispatcher (the exe exports nothing, so runtime dispatch by guest address
  // is the only callable path). Returns the sub_821E6740 code (0 added /
  // 1 roster full / 2 budget / 3 ineligible) or a negative host-side error:
  //   -1 no runtime / dispatcher / function registered
  //   -2 no guest memory base
  // On success the char id is also appended to the status screen's FC08
  // list (the step sub_821E6740 does NOT do).
  int CallGuestAdd(uint16_t char_id) {
    if (!runtime_) {
      return -1;
    }
    auto* fd = runtime_->function_dispatcher();
    if (!fd) {
      return -1;
    }
    auto* bump = fd->GetFunction(kGuestBumpFuncAddr);
    auto* add = fd->GetFunction(kGuestAddFuncAddr);
    if (!bump || !add) {
      return -1;
    }
    uint8_t* base = runtime_->virtual_membase();
    if (!base) {
      return -2;
    }
    PPCContext ctx{};
    ctx.r3.u64 = kMemberDbGuestAddr;
    ctx.r4.u64 = char_id;
    ctx.r5.u64 = 1;
    ctx.r6.u64 = 0;
    bump(ctx, base);
    PPCContext ctx2{};
    ctx2.r3.u64 = char_id;
    add(ctx2, base);
    int result = static_cast<int>(ctx2.r3.u64);
    if (result == 0) {
      AppendToStatusList(base, char_id);
      // The party list changed, so rebuild the live battle-party objects the
      // way the game's party-menu add path (sub_820E78B8) does. The overlay
      // disables the add buttons entirely while a battle is active (see
      // kBattleMagicAddr note above), so this path is never reached mid-battle.
      SyncBattleParty();
    }
    return result;
  }

  // Invoke the game's battle-party sync routine sub_821E6428() through the
  // runtime dispatcher on a scratch guest stack + ThreadState. The routine
  // repacks the battle-party objects unk_824D0480/unk_824D13C0 from the
  // active-party bytes (byte_8243FC3A) + the FC08 status list - the same work
  // the game's party-menu add/remove path (sub_820E78B8/sub_820E7948) and the
  // battle-load path (sub_820FD998) do. Its only file-I/O legs
  // (sub_821A03D0, sub_821BD1C0, sub_821BD778) are gated on the battle-data
  // magic dword_8244B9A0 == 0x4E535953, so when battle data is not loaded the
  // call degenerates to pure memory ops + NULL-safe teardown.
  // Returns the routine's r3 (1 = battle party rebuilt, 0 = no battle data)
  // or a negative host-side error: -1 no runtime / dispatcher / function,
  // -2 scratch stack / PCR allocation failed.
  int SyncBattleParty() {
    if (!runtime_) {
      return -1;
    }
    auto* fd = runtime_->function_dispatcher();
    auto* sync = fd ? fd->GetFunction(kGuestBattleSyncFuncAddr) : nullptr;
    if (!fd || !sync) {
      return -1;
    }
    auto* memory = runtime_->memory();
    if (!memory) {
      return -1;
    }

    // Scratch guest stack. 64KB is far more than E6428 + its teardown callees
    // need, and the stack-heap (0x70000000) layout with guard pages that real
    // threads use is unnecessary for a one-shot host-driven call.
    constexpr uint32_t kScratchStackSize = 0x10000;
    uint32_t stack = memory->SystemHeapAlloc(kScratchStackSize);
    if (!stack) {
      return -2;
    }

    // Minimal PCR block (X_KPCR, 0x2D8 bytes, zeroed by SystemHeapAlloc) so
    // r13-relative reads inside the closure see valid fields. XThread sets
    // the same fields when it constructs a real guest thread.
    uint32_t pcr = memory->SystemHeapAlloc(0x2D8);
    if (!pcr) {
      memory->SystemHeapFree(stack);
      return -2;
    }
    auto* kpcr = memory->TranslateVirtual<uint8_t*>(pcr);
    // X_KPCR field offsets: tls_ptr@0x00, pcr_ptr@0x30, prcb@0x2A8,
    // stack_base_ptr@0x70 (high address), stack_end_ptr@0x74 (low address).
    rex::memory::store_and_swap<uint32_t>(kpcr + 0x00, 0);          // tls_ptr: none
    rex::memory::store_and_swap<uint64_t>(kpcr + 0x30, pcr);        // pcr_ptr
    rex::memory::store_and_swap<uint32_t>(kpcr + 0x70, stack + kScratchStackSize);
    rex::memory::store_and_swap<uint32_t>(kpcr + 0x74, stack);
    rex::memory::store_and_swap<uint32_t>(kpcr + 0x2A8, pcr + 0x100);  // prcb -> prcb_data

    rex::runtime::ThreadState ts(UINT_MAX, stack + kScratchStackSize, pcr, memory);
    uint64_t args[1] = {0};
    uint64_t result = fd->Execute(&ts, kGuestBattleSyncFuncAddr, args, 0);

    memory->SystemHeapFree(pcr);
    memory->SystemHeapFree(stack);
    return static_cast<int>(result);
  }

  // Give char_id the next free display position on the status screen and clear
  // its in-list flag byte, exactly mirroring sub_820E78B8 (the game's own
  // "member added to the party list" routine): FC08[id-1] = (number of members
  // already in the party) + 1, then byte_8243FC2F[id] = 0. FC08 is a position
  // table keyed by character number, so the slot to write is fixed by the id -
  // an earlier revision searched for the first zero entry and stored the id
  // there, which corrupted the table whenever id-1 wasn't that slot.
  void AppendToStatusList(uint8_t* base, uint16_t char_id) {
    if (char_id < 1 || char_id > kStatusMemberListCount) {
      return;
    }
    auto* self = base + kStatusMemberListAddr + 4 * (char_id - 1);
    if (rex::memory::load_and_swap<uint32_t>(self) != 0) {
      return;  // already in the party
    }
    uint32_t count = 0;
    for (uint32_t c = 0; c < kStatusMemberListCount; ++c) {
      if (rex::memory::load_and_swap<uint32_t>(base + kStatusMemberListAddr + 4 * c) != 0) {
        ++count;
      }
    }
    rex::memory::store_and_swap<uint32_t>(self, count + 1);
    base[kMemberInListFlagAddr + char_id] = 0;
  }

  // Scratch values for the edit form, keyed by character number. Only
  // populated when entering edit mode, so in-progress edits aren't clobbered
  // by the next frame's re-read of guest memory.
  struct EditBuffer {
    int level = 0;
    int hp_cur = 0;
    int hp_max = 0;
    int s1 = 0;
    int s2 = 0;
    int s3 = 0;
    int s4 = 0;
  };

  // Draws the "Stats" sub-overlay for one character as its own floating
  // ImGui window. Read-only until "Edit" is pressed, which snapshots the
  // current values into edit_buf_[char_id] and switches every field to an
  // InputInt; "Save" writes the edited values back to guest memory at
  // kStatusSlotStatsAddr + 48*(char_id-1) and returns to read-only, "Cancel"
  // discards the edits.
  void RenderDetailWindow(rex::memory::Memory* memory, uint16_t char_id, const StatusMember& s) {
    const char* name =
        (char_id >= 1 && char_id <= kMaxPartyMembers) ? kCharNames[char_id] : nullptr;
    std::string title = (name ? std::string(name) : ("Char " + std::to_string(char_id))) +
                         " Stats##detail" + std::to_string(char_id);

    bool open = detail_open_[char_id];
    ImGui::SetNextWindowSize(ImVec2(240, 0), ImGuiCond_FirstUseEver);
    // Auto-resize every frame so switching into/out of edit mode (which adds
    // wider InputInt fields) grows or shrinks the window instead of leaving
    // it clipped or oversized from whichever mode it was last sized for.
    if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize)) {
      if (s.s1 == 0xFFFF && !edit_mode_[char_id]) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "stats not readable");
      } else if (!edit_mode_[char_id]) {
        // Fixed label column so values line up regardless of label width
        // ("Level" is the widest of the six).
        const float label_col_x =
            ImGui::CalcTextSize("Level").x + 2.0f * ImGui::GetStyle().ItemSpacing.x;
        auto stat_row = [&](const char* label, const std::string& value) {
          ImGui::Text("%s", label);
          ImGui::SameLine(label_col_x);
          ImGui::Text("%s", value.c_str());
        };
        stat_row("Level", std::to_string(s.level));
        stat_row("HP", std::to_string(s.hp_cur) + " / " + std::to_string(s.hp_max));
        stat_row("ATK", std::to_string(s.s1));
        stat_row("MGC", std::to_string(s.s2));
        stat_row("DEF", std::to_string(s.s3));
        stat_row("SPD", std::to_string(s.s4));
        if (ImGui::Button("Edit")) {
          EditBuffer& b = edit_buf_[char_id];
          b.level = static_cast<int>(s.level);
          b.hp_cur = static_cast<int>(s.hp_cur);
          b.hp_max = static_cast<int>(s.hp_max);
          b.s1 = s.s1;
          b.s2 = s.s2;
          b.s3 = s.s3;
          b.s4 = s.s4;
          edit_mode_[char_id] = true;
        }
      } else {
        EditBuffer& b = edit_buf_[char_id];
        ImGui::SetNextItemWidth(100.0f);
        ImGui::InputInt("Level", &b.level);
        ImGui::SetNextItemWidth(100.0f);
        ImGui::InputInt("HP cur", &b.hp_cur);
        ImGui::SetNextItemWidth(100.0f);
        ImGui::InputInt("HP max", &b.hp_max);
        ImGui::SetNextItemWidth(100.0f);
        ImGui::InputInt("ATK", &b.s1);
        ImGui::SetNextItemWidth(100.0f);
        ImGui::InputInt("MGC", &b.s2);
        ImGui::SetNextItemWidth(100.0f);
        ImGui::InputInt("DEF", &b.s3);
        ImGui::SetNextItemWidth(100.0f);
        ImGui::InputInt("SPD", &b.s4);
        if (ImGui::Button("Save")) {
          uint32_t base = kStatusSlotStatsAddr + kStatusSlotStatsStride * (char_id - 1);
          WriteGuest(memory, base + kStatLevelOffset, ClampU32(b.level));
          WriteGuest(memory, base + kStatHpCurOffset, ClampU32(b.hp_cur));
          WriteGuest(memory, base + kStatHpMaxOffset, ClampU32(b.hp_max));
          WriteGuest(memory, base + kStat1Offset, ClampU16(b.s1));
          WriteGuest(memory, base + kStat2Offset, ClampU16(b.s2));
          WriteGuest(memory, base + kStat3Offset, ClampU16(b.s3));
          WriteGuest(memory, base + kStat4Offset, ClampU16(b.s4));
          edit_mode_[char_id] = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
          edit_mode_[char_id] = false;
        }
      }
    }
    ImGui::End();

    detail_open_[char_id] = open;
    if (!open) {
      edit_mode_[char_id] = false;
    }
  }

  static uint32_t ClampU32(int v) { return v < 0 ? 0 : static_cast<uint32_t>(v); }
  static uint16_t ClampU16(int v) {
    if (v < 0) {
      return 0;
    }
    return v > 0xFFFF ? uint16_t{0xFFFF} : static_cast<uint16_t>(v);
  }

  rex::Runtime* runtime_ = nullptr;
  bool visible_ = false;
  // Indexed by character number (1..kMaxPartyMembers); index 0 unused.
  bool detail_open_[kMaxPartyMembers + 1] = {};
  bool edit_mode_[kMaxPartyMembers + 1] = {};
  EditBuffer edit_buf_[kMaxPartyMembers + 1];
};

class PartyOverlayMod : public rex::system::IModPlugin {
 public:
  explicit PartyOverlayMod(rex::Runtime* runtime) : runtime_(runtime) {}

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    dialog_ = std::make_unique<PartyOverlayDialog>(drawer, runtime_);
  }

 private:
  rex::Runtime* runtime_ = nullptr;
  std::unique_ptr<PartyOverlayDialog> dialog_;
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
  return new PartyOverlayMod(ctx->runtime);
}
