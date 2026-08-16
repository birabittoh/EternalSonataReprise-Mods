// demo_characters - claims the game's vacant character slots and puts two new
// party members in them.
//
// The port widens every per-character table from the retail ten to twelve, but
// it deliberately puts no content in the two new slots: they start vacant, with
// no name, no stat template, and every id gate in the game shut, so a build
// with no mod loaded is exactly the ten-character game. A mod defines a slot to
// fill it in. This is the worked example of that, and it is meant to be copied.
//
// What a definition supplies:
//
//   name             what every screen draws. Single-byte (CP1252), because the
//                    game's font draws one glyph per byte.
//   template_source  which retail character's stat template the new one starts
//                    from. This is the table the game's own init and level-up
//                    paths read, so it decides starting stats and growth.
//   model_id         the pcNNN.bop battle model. Left at 0 the character asks
//                    for its own (pc011.bop, pc012.bop), which is what an asset
//                    mod supplying game/btldata/player/pc011.bop wants. This
//                    mod ships no assets, so it borrows two existing models
//                    rather than crashing the battle loader on a missing file.
//   stats            optional starting stats, applied the first time the
//                    character joins a party.
//
// Nothing here knows a guest address. Everything is the party API the host
// exports; see rexmod/eternalsonata_party_api.h.

#include <rex/system/mod_plugin.h>

#include <cstdint>
#include <cstring>

#include <rex/logging.h>

#include <rexmod/eternalsonata_party_api.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

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

// One character this mod wants to exist.
struct DemoCharacter {
  const char* name;
  int template_source;  // whose stats and growth to start from
  int model_id;         // pcNNN.bop to load; 0 = the slot's own id
};

// Two of them, because two vacant slots is what the host has. Jazz and Falsetto
// are borrowed for the models: this mod ships no .bop files, and the battle
// model loader has to find *something* when one of these walks into a fight.
// A mod with its own assets sets model_id to 0 and ships pc011.bop instead.
constexpr DemoCharacter kDemoCharacters[] = {
    {"Cadenza", ETERNALSONATA_CHAR_JAZZ, ETERNALSONATA_CHAR_JAZZ},
    {"Rondo", ETERNALSONATA_CHAR_FALSETTO, ETERNALSONATA_CHAR_FALSETTO},
};

struct SlotApi {
  EternalSonataGetAddedSlotCountFn slot_count = nullptr;
  EternalSonataGetAddedSlotFn slot_at = nullptr;
  EternalSonataIsCharacterDefinedFn is_defined = nullptr;
  EternalSonataDefineNextCharacterFn define_next = nullptr;

  bool ok() const { return slot_count && slot_at && is_defined && define_next; }
};

SlotApi LoadSlotApi() {
  SlotApi api;
  Resolve(api.slot_count, "EternalSonataGetAddedSlotCount");
  Resolve(api.slot_at, "EternalSonataGetAddedSlot");
  Resolve(api.is_defined, "EternalSonataIsCharacterDefined");
  Resolve(api.define_next, "EternalSonataDefineNextCharacter");
  return api;
}

class DemoCharactersMod : public rex::system::IModPlugin {
 public:
  // The slots are host state, not save state, so they are claimed once per run
  // and as early as there is a host to claim them from. Defining does not need
  // a save to be loaded: it writes the stat template and opens the id gates,
  // and the character is only actually built when something adds it to a party.
  void OnModuleLaunched() override {
    SlotApi api = LoadSlotApi();
    if (!api.ok()) {
      REXLOG_WARN("demo_characters: host has no vacant-slot API; nothing to do");
      return;
    }

    const int slots = api.slot_count();
    int defined = 0;
    for (const DemoCharacter& demo : kDemoCharacters) {
      if (defined >= slots) {
        REXLOG_WARN("demo_characters: no slot left for \"{}\" ({} in this build)", demo.name,
                    slots);
        break;
      }

      EternalSonataCharacterDefinition definition{};
      definition.struct_size = sizeof(definition);
      definition.name = demo.name;
      definition.template_source = demo.template_source;
      definition.model_id = demo.model_id;

      // Claim whichever slot is free rather than naming one, so this mod and
      // another that also wants a character can both load.
      const int character = api.define_next(&definition);
      if (character < 0) {
        REXLOG_WARN("demo_characters: could not define \"{}\" (error {})", demo.name, character);
        continue;
      }
      ++defined;
      REXLOG_INFO("demo_characters: \"{}\" is character {}", demo.name, character);
    }

    if (defined) {
      REXLOG_INFO("demo_characters: {} character(s) defined; add them from the party overlay",
                  defined);
    }
  }
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
  return new DemoCharactersMod();
}
