// save_anywhere - makes the pause menu's Save entry (its last one) selectable
// anywhere, instead of only while the player is standing on a save point.
//
// The gate is a guest flag the field scripts rewrite on their own schedule, so
// forcing it belongs in the host, which can set it immediately before the menu
// build reads it; a mod writing guest memory from a per-frame tick cannot do
// that reliably, and patching the branch is not an option in a recompilation.
// This mod is therefore just the switch: EternalSonataSetSaveAlwaysAllowed,
// out of rexmod/eternalsonata_save_api.h.

#include <rex/system/mod_plugin.h>

#include <cstdint>

#include <rex/logging.h>

#include <rexmod/eternalsonata_save_api.h>

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

class SaveAnywhereMod : public rex::system::IModPlugin {
 public:
  // OnModuleLaunched rather than OnCreateDialogs: the toggle refuses until the
  // save system has been bound to the runtime, which happens with the rest of
  // the guest module coming up.
  void OnModuleLaunched() override {
    EternalSonataSetSaveAlwaysAllowedFn set_allowed = nullptr;
    Resolve(set_allowed, "EternalSonataSetSaveAlwaysAllowed");
    if (!set_allowed) {
      REXLOG_WARN("save_anywhere: this host has no save API; nothing to do");
      return;
    }

    const int result = set_allowed(1);
    if (result < 0) {
      REXLOG_WARN("save_anywhere: could not force the save gate ({})", result);
      return;
    }
    REXLOG_INFO("save_anywhere: Save is now selectable away from save points");
  }

  void OnShutdown() override {
    EternalSonataSetSaveAlwaysAllowedFn set_allowed = nullptr;
    Resolve(set_allowed, "EternalSonataSetSaveAlwaysAllowed");
    if (set_allowed) {
      set_allowed(0);
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
  return new SaveAnywhereMod();
}
