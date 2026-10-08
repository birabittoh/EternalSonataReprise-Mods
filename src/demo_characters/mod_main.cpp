// demo_characters - puts two new party members in the game's vacant character
// slots, as the worked example of the character definition API.
//
// On Xbox 360 data the port's per character tables are twelve wide, but slots
// 11 and 12 start vacant: a build with no mod loaded is the ten character
// game. A mod defines a slot to fill it. On PS3 data Crescendo and Serenade
// hold both, so the host reports no slots and this mod does nothing.
//
// A definition names a base character and supplies whatever it has of its
// own; everything else is the base's:
//
//   name, id     what every screen draws, and the stable key saves use
//   base         stat template and growth, and the base's case wherever the
//                game switches on the character
//   model_path   an Xbox 360 NOBJ body for the field, battle and events
//   battle_file  pcNNN.bop: motions, camera scripts, effects and specials.
//                0 loads pc011.bop for slot 11 when a mod ships it
//   voice_file   pcNNN.csf, the same way
//   portraits    status, panel and the two camp face sets
//
// This mod ships no assets, so it borrows the bases' models and files. The
// same two characters could be declared with no code at all, as [[character]]
// tables in assets.toml (docs/modded-characters.md in the main repository).

#include <rex/system/mod_plugin.h>

#include <cstdint>

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

struct DemoCharacter {
  const char* id;
  const char* name;
  int base;
};

constexpr DemoCharacter kDemoCharacters[] = {
    {"demo_characters/cadenza", "Cadenza", ETERNALSONATA_CHAR_JAZZ},
    {"demo_characters/rondo", "Rondo", ETERNALSONATA_CHAR_FALSETTO},
};

class DemoCharactersMod : public rex::system::IModPlugin {
 public:
  // Definitions are host state, not save state, so they are made on every run
  // and before a save loads.
  void OnModuleLaunched() override {
    EternalSonataPartyAbiVersionFn version = nullptr;
    EternalSonataDefineNextCharacterFn define_next = nullptr;
    Resolve(version, "EternalSonataPartyAbiVersion");
    Resolve(define_next, "EternalSonataDefineNextCharacter");
    if (!version || version() < 3 || !define_next) {
      REXLOG_WARN("demo_characters: host has no character definition API; nothing to do");
      return;
    }

    for (const DemoCharacter& demo : kDemoCharacters) {
      EternalSonataCharacterDefinition definition{};
      definition.struct_size = sizeof(definition);
      definition.id = demo.id;
      definition.name = demo.name;
      definition.base = demo.base;
      // Borrow the base's battle file and voices rather than load pc011.bop,
      // which this mod does not ship.
      definition.battle_file = demo.base;
      definition.voice_file = demo.base;

      // Whichever slot is free, so another character mod can load too.
      const int character = define_next(&definition);
      if (character == ETERNALSONATA_PARTY_ERR_NO_SLOTS) {
        REXLOG_WARN("demo_characters: no slot left for \"{}\"", demo.name);
        break;
      }
      if (character < 0) {
        REXLOG_WARN("demo_characters: could not define \"{}\" (error {})", demo.name, character);
        continue;
      }
      REXLOG_INFO("demo_characters: \"{}\" is character {}; add it from the party overlay",
                  demo.name, character);
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
