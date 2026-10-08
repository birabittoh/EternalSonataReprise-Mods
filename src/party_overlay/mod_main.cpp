// party_overlay - an overlay (F5) over the game's party: who is in it, in what
// order, with what stats, plus adding, removing, reordering and healing.
//
// The same keybind also opens a second window over the battle in progress: the
// battle state machine, both sides' units, whose turn it is, and a button that
// ends the battle in the player's favour.
//
// There is no reverse engineering left in this mod. Every guest address, the
// join sequence, the battle-party resync, the battle-safety gate and the
// battle layout that used to live here (or in the game's own debug overlay)
// now live in the game itself and are reached through the two APIs it exports:
// rexmod/eternalsonata_party_api.h and rexmod/eternalsonata_battle_api.h, both
// copied verbatim from the game repo's src/. See the game's
// docs/party-system.md and src/battle_layout.h for the structures underneath.
// This file is UI.

#include <rex/system/mod_plugin.h>

#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/runtime.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

#include <rexmod/eternalsonata_battle_api.h>
#include <rexmod/eternalsonata_party_api.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

// Every entry point is resolved out of the host executable at load time and
// null-checked before use, so this mod still loads (and simply says so) on a
// host too old to have the party API.
struct PartyApi {
  EternalSonataPartyAbiVersionFn abi_version = nullptr;
  EternalSonataIsPartyAvailableFn is_available = nullptr;
  EternalSonataIsPartyEditableFn is_editable = nullptr;
  EternalSonataGetCharacterNameFn name = nullptr;
  EternalSonataGetPartyMembersFn members = nullptr;
  EternalSonataIsCharacterInPartyFn in_party = nullptr;
  EternalSonataGetCharacterPositionFn position = nullptr;
  EternalSonataGetPartyLevelFn level = nullptr;
  EternalSonataGetPartyLevelBudgetFn budget = nullptr;
  EternalSonataGetPartyLevelBudgetUsedFn budget_used = nullptr;
  EternalSonataGetPartyLevelBudgetFreeFn budget_free = nullptr;
  EternalSonataGetCharacterStatsFn stats = nullptr;
  EternalSonataGetCharacterBaseStatsFn base_stats = nullptr;
  EternalSonataSetCharacterStatsFn set_stats = nullptr;
  EternalSonataHealCharacterFn heal = nullptr;
  EternalSonataHealPartyFn heal_party = nullptr;
  EternalSonataAddCharacterToPartyFn add = nullptr;
  EternalSonataRemoveCharacterFromPartyFn remove = nullptr;
  EternalSonataSetCharacterPositionFn set_position = nullptr;
  EternalSonataSetPartyLevelFn set_level = nullptr;
  bool ok() const { return abi_version && is_available && name && members && add && remove; }
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

// The battle API is a separate ABI from the party one and can be missing on
// its own, so it is resolved and reported separately: a host with the party
// API but no battle API still gets the party window, and just says the battle
// window is unavailable.
struct BattleApi {
  EternalSonataBattleAbiVersionFn abi_version = nullptr;
  EternalSonataIsBattleActiveFn is_active = nullptr;
  EternalSonataGetBattleStateFn state = nullptr;
  EternalSonataGetBattleStateNameFn state_name = nullptr;
  EternalSonataGetBattlePartyUnitFn party_unit = nullptr;
  EternalSonataGetBattleEnemyFn enemy = nullptr;
  EternalSonataGetBattleEnemyNameFn enemy_name = nullptr;
  EternalSonataWinBattleFn win = nullptr;
  EternalSonataSkipBattleIntroFn skip_intro = nullptr;
  EternalSonataSetBattlePartyHpFn set_party_hp = nullptr;
  EternalSonataSetBattleEnemyHpFn set_enemy_hp = nullptr;
  EternalSonataSkipBattleTurnFn skip_turn = nullptr;

  bool ok() const { return abi_version && is_active && state && party_unit && enemy; }
};

PartyApi LoadPartyApi() {
  PartyApi api;
  Resolve(api.abi_version, "EternalSonataPartyAbiVersion");
  Resolve(api.is_available, "EternalSonataIsPartyAvailable");
  Resolve(api.is_editable, "EternalSonataIsPartyEditable");
  Resolve(api.name, "EternalSonataGetCharacterName");
  Resolve(api.members, "EternalSonataGetPartyMembers");
  Resolve(api.in_party, "EternalSonataIsCharacterInParty");
  Resolve(api.position, "EternalSonataGetCharacterPosition");
  Resolve(api.level, "EternalSonataGetPartyLevel");
  Resolve(api.budget, "EternalSonataGetPartyLevelBudget");
  Resolve(api.budget_used, "EternalSonataGetPartyLevelBudgetUsed");
  Resolve(api.budget_free, "EternalSonataGetPartyLevelBudgetFree");
  Resolve(api.stats, "EternalSonataGetCharacterStats");
  Resolve(api.base_stats, "EternalSonataGetCharacterBaseStats");
  Resolve(api.set_stats, "EternalSonataSetCharacterStats");
  Resolve(api.heal, "EternalSonataHealCharacter");
  Resolve(api.heal_party, "EternalSonataHealParty");
  Resolve(api.add, "EternalSonataAddCharacterToParty");
  Resolve(api.remove, "EternalSonataRemoveCharacterFromParty");
  Resolve(api.set_position, "EternalSonataSetCharacterPosition");
  Resolve(api.set_level, "EternalSonataSetPartyLevel");
  return api;
}

BattleApi LoadBattleApi() {
  BattleApi api;
  Resolve(api.abi_version, "EternalSonataBattleAbiVersion");
  Resolve(api.is_active, "EternalSonataIsBattleActive");
  Resolve(api.state, "EternalSonataGetBattleState");
  Resolve(api.state_name, "EternalSonataGetBattleStateName");
  Resolve(api.party_unit, "EternalSonataGetBattlePartyUnit");
  Resolve(api.enemy, "EternalSonataGetBattleEnemy");
  Resolve(api.enemy_name, "EternalSonataGetBattleEnemyName");
  Resolve(api.win, "EternalSonataWinBattle");
  Resolve(api.skip_intro, "EternalSonataSkipBattleIntro");
  Resolve(api.set_party_hp, "EternalSonataSetBattlePartyHp");
  Resolve(api.set_enemy_hp, "EternalSonataSetBattleEnemyHp");
  Resolve(api.skip_turn, "EternalSonataSkipBattleTurn");
  return api;
}

const char* BattleActorKindName(int kind) {
  switch (kind) {
    case ETERNALSONATA_BATTLE_ACTOR_PARTY:
      return "party";
    case ETERNALSONATA_BATTLE_ACTOR_ENEMY:
      return "enemy";
    default:
      return "nobody";
  }
}

class PartyOverlayDialog : public rex::ui::ImGuiDialog {
 public:
  explicit PartyOverlayDialog(rex::ui::ImGuiDrawer* drawer)
      : ImGuiDialog(drawer), api_(LoadPartyApi()), battle_(LoadBattleApi()) {
    rex::ui::RegisterBind(
        "bind_party_overlay", "F5", "Toggle party and battle overlays",
        [this] { visible_ = !visible_; }, [this] { return visible_; }, "Party Overlay");
  }

  ~PartyOverlayDialog() override { rex::ui::UnregisterBind("bind_party_overlay"); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    (void)io;
    if (!visible_) {
      return;
    }

    DrawOverlayWindow();

    for (int character = 1; character <= ETERNALSONATA_CHARACTER_COUNT; ++character) {
      if (detail_open_[character]) {
        DrawDetailWindow(character);
      }
    }
  }

 private:
  // A single window: the read-only party roster and edit controls when there
  // is no battle, or the battle state and its own controls when there is.
  // They never have anything useful to say at the same time (the roster is
  // frozen for the duration anyway, see is_editable), so showing both would
  // just be two panes competing for attention.
  void DrawOverlayWindow() {
    EternalSonataBattleState state{};
    const bool battle_active = battle_.ok() && battle_.state(&state) == ETERNALSONATA_BATTLE_OK &&
                               state.active;

    if (battle_active) {
      // Fixed width, auto height. Everything in here changes from frame to
      // frame (names arriving, "acting" appearing, HP digits gaining a
      // column), and a plain auto-resize would have the window twitch wider
      // and narrower the whole battle. Constraining both bounds to the same
      // width pins it while still letting the height follow the unit count.
      ImGui::SetNextWindowSizeConstraints(ImVec2(kBattleWindowWidth, 0.0f),
                                          ImVec2(kBattleWindowWidth, FLT_MAX));
    } else {
      ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin("Party Overlay", &visible_, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::End();
      return;
    }

    if (battle_active) {
      DrawBattleHeader(state);
      ImGui::Separator();
      DrawBattleSide("Party", state.party_count, battle_.party_unit, true);
      ImGui::Separator();
      DrawBattleSide("Enemies", state.enemy_count, battle_.enemy, false);
      ImGui::End();
      return;
    }

    if (!api_.ok()) {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                         "This build of the game has no party API.");
      ImGui::TextDisabled("Eternal Sonata: Reprise 1.2.0 or newer is required.");
      ImGui::End();
      return;
    }

    if (!api_.is_available()) {
      ImGui::TextDisabled("Start or load a game to view party state.");
      ImGui::End();
      return;
    }

    DrawHeader();
    ImGui::Separator();
    DrawMembers();
    DrawBench();

    ImGui::End();
  }

  void DrawBattleHeader(const EternalSonataBattleState& state) {
    if (state.actor_kind == ETERNALSONATA_BATTLE_ACTOR_NONE) {
      ImGui::TextDisabled("Turn: nobody");
    } else if (state.actor_kind == ETERNALSONATA_BATTLE_ACTOR_PARTY) {
      // Look the acting slot's character up through the two battle-side
      // getters rather than printing the raw kind/slot pair, so this reads
      // "Turn: Polka" instead of "Turn: party #0".
      EternalSonataBattleUnit unit{};
      const char* name = (battle_.party_unit && api_.ok() &&
                          battle_.party_unit(state.actor_slot, &unit) == ETERNALSONATA_BATTLE_OK)
                             ? api_.name(unit.character)
                             : "";
      if (name && name[0] != '\0') {
        ImGui::Text("Turn: %s", name);
      } else {
        ImGui::Text("Turn: party #%d", state.actor_slot);
      }
    } else {
      const char* name = battle_.enemy_name ? battle_.enemy_name(state.actor_slot) : "";
      if (name && name[0] != '\0') {
        ImGui::Text("Turn: %s", name);
      } else {
        ImGui::Text("Turn: enemy #%d", state.actor_slot);
      }
    }

    ImGui::SameLine();
    const char* fsm_name = battle_.state_name ? battle_.state_name(state.fsm_state) : "";
    if (fsm_name[0] != '\0') {
      ImGui::Text("State %d (%s)", state.fsm_state, fsm_name);
    } else {
      ImGui::Text("State %d", state.fsm_state);
    }

    // Two independent per-turn timers, both hosted on the same per-actor
    // object the game refreshes each turn (see battle_layout.h "Turn
    // timers" in the game repo). Neither is meaningful outside a turn, so
    // both report a negative tick count rather than 0 when the object is
    // not currently resolvable. On their own line rather than the button
    // row below, so the buttons' own SameLine chain stays uninterrupted.
    bool prior_timer = false;
    if (state.command_timer_ticks >= 0) {
      // battle_layout.h's per-level duration table uses byte 255 as a
      // "disabled" sentinel, shipped for party levels 1-2. At those levels
      // the raw tick count can read back as anything (including 0: the
      // completion check at level 1 ignores the timer altogether and the
      // field is never even armed), so check the party level directly
      // instead of trusting a live tick value to prove "disabled" on its own.
      const int level = api_.level ? api_.level() : -1;
      const bool disabled = (level >= 1 && level <= 2) || state.command_timer_ticks >= 255 * 300;
      if (disabled) {
        ImGui::Text("Turn Timer: infinite%s", state.command_timer_active ? "" : " (idle)");
      } else {
        ImGui::Text("Turn Timer: %.2fs%s", state.command_timer_ticks / 300.0f,
                   state.command_timer_active ? "" : " (idle)");
      }
      prior_timer = true;
    }
    if (state.turn_end_ticks >= 0) {
      if (prior_timer) {
        ImGui::SameLine();
      }
      ImGui::Text("Action Gauge: %.2fs (mode %d)", state.turn_end_ticks / 300.0f,
                 state.turn_end_mode);
    }

    // Only useful during the intro, and it is the intro's own tail that gets
    // cut, so it stays visible (disabled) outside one rather than appearing
    // and vanishing as the battle starts.
    bool prior_button = false;
    if (battle_.skip_intro) {
      const bool in_intro = state.fsm_state == ETERNALSONATA_BATTLE_FSM_INTRO;
      ImGui::BeginDisabled(!in_intro);
      if (ImGui::Button("Skip Intro")) {
        battle_.skip_intro();
      }
      ImGui::EndDisabled();
      prior_button = true;
    }

    if (battle_.set_party_hp && battle_.party_unit) {
      if (prior_button) {
        ImGui::SameLine();
      }
      if (ImGui::Button("Heal Party")) {
        for (int slot = 0; slot < state.party_count; ++slot) {
          EternalSonataBattleUnit unit{};
          if (battle_.party_unit(slot, &unit) == ETERNALSONATA_BATTLE_OK) {
            battle_.set_party_hp(slot, unit.hp_max);
          }
        }
      }
      prior_button = true;
    }

    if (battle_.skip_turn) {
      if (prior_button) {
        ImGui::SameLine();
      }
      // Grey out when the preconditions are not met: nobody is acting or
      // FSM not in state 12. The API itself also checks and returns
      // ERR_NOT_IN_TURN, but greying the button out communicates the
      // restriction visually.
      const bool can_skip = state.actor_kind != ETERNALSONATA_BATTLE_ACTOR_NONE &&
                            state.fsm_state == ETERNALSONATA_BATTLE_FSM_TURN;
      ImGui::BeginDisabled(!can_skip);
      if (ImGui::Button("Skip Turn")) {
        battle_.skip_turn();
      }
      ImGui::EndDisabled();
      prior_button = true;
    }

    if (!battle_.win) {
      return;
    }

    // Past this point the battle is already decided and playing out its
    // wrap-up chain, so there is nothing left to force and can_win_now will
    // never come back true.
    const bool ending = state.fsm_state >= ETERNALSONATA_BATTLE_FSM_ENDING;

    if (prior_button) {
      ImGui::SameLine();
    }
    ImGui::BeginDisabled(ending);
    if (ImGui::Button("Win Battle")) {
      battle_.win();
    }
    ImGui::EndDisabled();
  }

  // `count` units of one side, each fetched through `get`. `party` selects
  // where the names come from: the party API (which honours renames) for
  // party members, the battle API's own text lookup for enemies.
  void DrawBattleSide(const char* title, int count,
                      int (*get)(int, EternalSonataBattleUnit*), bool party) {
    ImGui::Text("%s (%d):", title, count);
    if (count <= 0) {
      ImGui::TextDisabled("  none");
      return;
    }
    for (int slot = 0; slot < count; ++slot) {
      EternalSonataBattleUnit unit{};
      if (get(slot, &unit) != ETERNALSONATA_BATTLE_OK) {
        continue;
      }
      DrawBattleUnit(unit, party);
    }
  }

  void DrawBattleUnit(const EternalSonataBattleUnit& unit, bool party) {
    // An enemy name is empty only on the very first frame it is asked for,
    // while the lookup is queued on the guest thread, so falling back to the
    // slot number keeps the row from changing width once it arrives.
    const char* name = party ? (api_.ok() ? api_.name(unit.character) : "")
                             : (battle_.enemy_name ? battle_.enemy_name(unit.slot) : "");
    std::string label = "  #" + std::to_string(unit.slot);
    if (name && name[0] != '\0') {
      label += " ";
      label += name;
    }

    // Fallen units are greyed rather than hidden: the slot still exists and
    // the game still counts it, so dropping it would make the list lie about
    // how many units are in the battle.
    if (!unit.alive) {
      ImGui::TextDisabled("%s", label.c_str());
    } else {
      ImGui::TextUnformatted(label.c_str());
    }

    if (unit.acting) {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "%s",
                         unit.resolving ? "acting" : "turn");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Lv%d  s%d", unit.level, unit.unit_state);

    // The API answers this for both sides; -1 means it could not be worked
    // out at all, which only leaves a zero-length bar.
    float fraction = unit.hp_ratio;
    fraction = fraction < 0.0f ? 0.0f : (fraction > 1.0f ? 1.0f : fraction);

    char overlay[64];
    if (unit.hp_max > 0) {
      snprintf(overlay, sizeof(overlay), "%d / %d", unit.hp, unit.hp_max);
    } else {
      // Only reachable for an enemy whose live part index read out of range,
      // where the cached ratio is all there is.
      snprintf(overlay, sizeof(overlay), "%.0f%%", fraction * 100.0f);
    }

    // Red as health runs out, so a nearly dead unit reads at a glance.
    const ImVec4 bar(1.0f - 0.6f * fraction, 0.25f + 0.45f * fraction, 0.3f * fraction, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, bar);
    ImGui::ProgressBar(fraction, ImVec2(-1.0f, ImGui::GetTextLineHeight()), overlay);
    ImGui::PopStyleColor();
  }

  void DrawHeader() {
    const int level = api_.level ? api_.level() : -1;
    if (level > 0) {
      ImGui::Text("Party Lv. %d", level);
      if (api_.set_level) {
        ImGui::SameLine();
        if (ImGui::SmallButton("-##level") && level > 1) {
          api_.set_level(level - 1);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("+##level") && level < 6) {
          api_.set_level(level + 1);
        }
      }
    }
    if (api_.budget && api_.budget_used && api_.budget_free) {
      const int cap = api_.budget();
      if (cap >= 0) {
        ImGui::Text("Budget %d used, %d free, of %d", api_.budget_used(), api_.budget_free(), cap);
      }
    }
    const bool editable = !api_.is_editable || api_.is_editable();
    if (editable && api_.heal_party && ImGui::Button("Heal party")) {
      api_.heal_party();
    }
    if (!editable) {
      if (api_.heal_party) {
        ImGui::SameLine();
      }
      ImGui::TextDisabled("(battle in progress: no party edits)");
    }
  }

  void DrawMembers() {
    int ids[ETERNALSONATA_CHARACTER_COUNT] = {};
    const int count = api_.members(ids, ETERNALSONATA_CHARACTER_COUNT);
    if (count <= 0) {
      ImGui::TextDisabled("No party members.");
      return;
    }

    // One fixed column for the buttons so they line up whatever the names are.
    const float button_column =
        ImGui::CalcTextSize("  #12 Allegretto__").x + 3.0f * ImGui::GetStyle().ItemSpacing.x;
    const bool editable = api_.is_editable && api_.is_editable();

    ImGui::Text("Active party:");
    for (int i = 0; i < count; ++i) {
      if (i == ETERNALSONATA_ACTIVE_PARTY_SIZE) {
        ImGui::Text("Reserves:");
      }
      const int character = ids[i];
      ImGui::Text("  #%d %s", i + 1, api_.name(character));
      ImGui::SameLine(button_column);
      if (ImGui::Button(("Stats##" + std::to_string(character)).c_str())) {
        detail_open_[character] = !detail_open_[character];
      }
      if (!editable) {
        continue;
      }
      if (api_.set_position && i > 0) {
        ImGui::SameLine();
        if (ImGui::Button(("Up##" + std::to_string(character)).c_str())) {
          api_.set_position(character, i);
        }
      }
      if (api_.set_position && i + 1 < count) {
        ImGui::SameLine();
        if (ImGui::Button(("Down##" + std::to_string(character)).c_str())) {
          api_.set_position(character, i + 2);
        }
      }
      // The party has to keep somebody in it; the game has no concept of an
      // empty one and its own menu will not let you get there either.
      if (count > 1) {
        ImGui::SameLine();
        if (ImGui::Button(("Remove##" + std::to_string(character)).c_str())) {
          api_.remove(character);
        }
      }
    }
  }

  void DrawBench() {
    if (!api_.is_editable || !api_.is_editable()) {
      return;
    }
    std::vector<int> missing;
    for (int character = 1; character <= ETERNALSONATA_CHARACTER_COUNT; ++character) {
      // A vacant slot (11 and 12 on 360 data, until a mod names them) has
      // no name; leave it out rather than show a button that adds nobody.
      const char* name = api_.name(character);
      if (!name || name[0] == '\0') {
        continue;
      }
      if (!api_.in_party(character)) {
        missing.push_back(character);
      }
    }
    if (missing.empty()) {
      return;
    }

    ImGui::Separator();
    ImGui::Text("Add to party:");
    for (size_t i = 0; i < missing.size(); ++i) {
      if ((i % 2) != 0) {
        ImGui::SameLine();
      }
      const int character = missing[i];
      const std::string label = std::string(api_.name(character)) + "##add" + std::to_string(character);
      if (ImGui::Button(label.c_str())) {
        api_.add(character);
      }
    }
  }

  // Scratch values for the edit form, filled when edit mode is entered so an
  // in-progress edit is not clobbered by the next frame's re-read.
  struct EditBuffer {
    EternalSonataCharacterStats stats{};
  };

  void DrawDetailWindow(int character) {
    const std::string title =
        std::string(api_.name(character)) + " Stats##detail" + std::to_string(character);

    bool open = detail_open_[character];
    ImGui::SetNextWindowSize(ImVec2(240, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize)) {
      EternalSonataCharacterStats stats{};
      // Read the character's own stats, not the equipment-adjusted ones: those
      // are what a write goes back into, so editing them round-trips cleanly.
      const bool have =
          api_.base_stats && api_.base_stats(character, &stats) == ETERNALSONATA_PARTY_OK;
      if (!have) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "stats unavailable");
      } else if (!edit_mode_[character]) {
        const float label_column =
            ImGui::CalcTextSize("Level").x + 2.0f * ImGui::GetStyle().ItemSpacing.x;
        auto row = [&](const char* label, const std::string& value) {
          ImGui::Text("%s", label);
          ImGui::SameLine(label_column);
          ImGui::Text("%s", value.c_str());
        };
        row("Level", std::to_string(stats.level));
        row("HP", std::to_string(stats.hp) + " / " + std::to_string(stats.hp_max));
        row("ATK", std::to_string(stats.attack));
        row("MGC", std::to_string(stats.magic));
        row("DEF", std::to_string(stats.defense));
        row("SPD", std::to_string(stats.speed));
        if (api_.set_stats && ImGui::Button("Edit")) {
          edit_[character].stats = stats;
          edit_mode_[character] = true;
        }
        if (api_.heal) {
          ImGui::SameLine();
          if (ImGui::Button("Heal")) {
            api_.heal(character);
          }
        }
      } else {
        EternalSonataCharacterStats& b = edit_[character].stats;
        auto field = [](const char* label, int32_t* value) {
          ImGui::SetNextItemWidth(100.0f);
          int scratch = static_cast<int>(*value);
          if (ImGui::InputInt(label, &scratch)) {
            *value = static_cast<int32_t>(scratch);
          }
        };
        field("Level", &b.level);
        field("HP", &b.hp);
        field("HP max", &b.hp_max);
        field("ATK", &b.attack);
        field("MGC", &b.magic);
        field("DEF", &b.defense);
        field("SPD", &b.speed);
        if (ImGui::Button("Save")) {
          api_.set_stats(character, &b);
          edit_mode_[character] = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
          edit_mode_[character] = false;
        }
      }
    }
    ImGui::End();

    detail_open_[character] = open;
    if (!open) {
      edit_mode_[character] = false;
    }
  }

  static constexpr float kBattleWindowWidth = 340.0f;

  PartyApi api_;
  BattleApi battle_;
  bool visible_ = false;
  // Indexed by character number (1..10); index 0 unused.
  bool detail_open_[ETERNALSONATA_CHARACTER_COUNT + 1] = {};
  bool edit_mode_[ETERNALSONATA_CHARACTER_COUNT + 1] = {};
  EditBuffer edit_[ETERNALSONATA_CHARACTER_COUNT + 1];
};

class PartyOverlayMod : public rex::system::IModPlugin {
 public:
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    dialog_ = std::make_unique<PartyOverlayDialog>(drawer);
  }

 private:
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
  return new PartyOverlayMod();
}
