// Character names for mods, from the host's roster, so no mod keeps its own
// list of the cast: modded characters in slots 11 and 12 and renames show up
// as the game draws them.
#pragma once

#include <cstdio>

#include <rexmod/eternalsonata_party_api.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace rexmod {

// `character`'s display name, "" for a vacant slot, or "#N" on a host
// without the party API.
inline const char* CharacterName(int character) {
  static const auto get = [] {
#if defined(_WIN32)
    return reinterpret_cast<EternalSonataGetCharacterNameFn>(
        GetProcAddress(GetModuleHandleA(nullptr), "EternalSonataGetCharacterName"));
#else
    return reinterpret_cast<EternalSonataGetCharacterNameFn>(
        dlsym(RTLD_DEFAULT, "EternalSonataGetCharacterName"));
#endif
  }();
  if (get)
    return get(character);
  thread_local char fallback[8];
  std::snprintf(fallback, sizeof(fallback), "#%d", character);
  return fallback;
}

}  // namespace rexmod
