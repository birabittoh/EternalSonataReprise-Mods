// save_anywhere - makes the pause menu's Save entry selectable anywhere,
// instead of only while the party stands at a save point.
//
// Android host resolution (verified against the v1.3.4 arm64 release):
// upstream's dlsym(RTLD_DEFAULT) lookup cannot work on Android - no
// EternalSonata* symbol is exported into .dynsym, and linker-namespace
// isolation (mod dlopen()ed by librexruntime.so vs the app's class
// namespace) hides the host library's symbols from the mod even when they
// are exported. Instead, scan /proc/self/maps for libeternalsonata.so's
// load bias, add the .symtab offset of EternalSonataSetSaveAlwaysAllowed,
// and verify a 16-byte prologue fingerprint at the target before ever
// calling. A fingerprint mismatch (host release changed and the offset
// moved) logs a warning and leaves the mod inert rather than calling a
// wrong address. See the companion issue for the symbol-visibility side.
//
// Gate control: the gate is cleared at launch/shutdown and only enabled
// per-frame once the field scripts have been observed rewriting the
// save-point flag (upstream save_system.cpp documents it is rewritten "on
// their own schedule") - direct evidence the world is live, which keeps a
// forced gate from disturbing the save-load sequence (v1.3.4 builds menu
// roots during load; the menu hook latches kMenuSaveEnabledAddr whenever
// the gate is forced, which stalls the load if the gate is on too early).
//
// Offsets: 0x29ac77c is the v1.3.4 value and will drift between host
// releases (it moved -0x14 from v1.3.3); the fingerprint check turns that
// drift into a log line instead of a crash.

#include <rex/system/mod_plugin.h>
#include <rex/system/mod_registry.h>

#include <rex/runtime.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <rex/logging.h>

#include <rexmod/eternalsonata_save_api.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

#if defined(__ANDROID__)
// Host v1.3.4 (arm64) .symtab offsets, verified by fingerprint.
constexpr uintptr_t kSetSaveAlwaysAllowedOffset = 0x29ac77c;
constexpr uintptr_t kGetSaveLocationOffset = 0x2ab9d54;
constexpr uintptr_t kSetSaveLocationOffset = 0x2ab9e3c;
// Guest flags observed by the tick (same addresses the host's systems use).
constexpr uint32_t kAtSavePointAddr = 0x8243C360u;
constexpr const char* kHostLibSuffix = "/lib/arm64/libeternalsonata.so";
constexpr unsigned char kExpectedBytes[16] = {
    0xFD, 0x7B, 0xBD, 0xA9, 0xF5, 0x0B, 0x00, 0xF9,
    0xF4, 0x4F, 0x02, 0xA9, 0xFD, 0x03, 0x00, 0x91,
};
#endif

using SetAllowedFn = int (*)(int);
using GetLocFn = int (*)(void);
using SetLocFn = int (*)(int);

struct HostApi {
  SetAllowedFn set_allowed = nullptr;
  GetLocFn get_location = nullptr;
  SetLocFn set_location = nullptr;
};

#if defined(__ANDROID__)
bool ResolveHost(HostApi& api) {
  FILE* f = std::fopen("/proc/self/maps", "r");
  if (!f) {
    REXLOG_WARN("save_anywhere: cannot open /proc/self/maps");
    return false;
  }
  char line[2048];
  uintptr_t base = 0;
  const size_t suffix_len = std::strlen(kHostLibSuffix);
  while (std::fgets(line, sizeof(line), f)) {
    char* sp = std::strchr(line, '/');
    if (!sp) {
      continue;
    }
    char* nl = std::strchr(sp, '\n');
    if (nl) {
      *nl = '\0';
    }
    const size_t len = std::strlen(sp);
    if (len >= suffix_len && std::strcmp(sp + len - suffix_len, kHostLibSuffix) == 0) {
      base = std::strtoull(line, nullptr, 16);
      break;
    }
  }
  std::fclose(f);
  if (base == 0) {
    REXLOG_WARN("save_anywhere: libeternalsonata.so not found in maps");
    return false;
  }
  auto* target = reinterpret_cast<unsigned char*>(base + kSetSaveAlwaysAllowedOffset);
  unsigned char got[16];
  std::memcpy(got, target, 16);
  char gothex[33];
  for (int i = 0; i < 16; ++i) {
    std::sprintf(gothex + i * 2, "%02X", got[i]);
  }
  REXLOG_INFO("save_anywhere: host base = 0x{:016x}, target 0x{:016x}, bytes {}",
              base, (uintptr_t)target, gothex);
  if (std::memcmp(got, kExpectedBytes, 16) != 0) {
    REXLOG_WARN("save_anywhere: fingerprint mismatch; refusing to call");
    return false;
  }
  api.set_allowed = reinterpret_cast<SetAllowedFn>(base + kSetSaveAlwaysAllowedOffset);
  api.get_location = reinterpret_cast<GetLocFn>(base + kGetSaveLocationOffset);
  api.set_location = reinterpret_cast<SetLocFn>(base + kSetSaveLocationOffset);
  return true;
}
#else
bool ResolveHost(HostApi& api) {
  api.set_allowed = reinterpret_cast<SetAllowedFn>(dlsym(RTLD_DEFAULT, "EternalSonataSetSaveAlwaysAllowed"));
  api.get_location = reinterpret_cast<GetLocFn>(dlsym(RTLD_DEFAULT, "EternalSonataGetSaveLocation"));
  api.set_location = reinterpret_cast<SetLocFn>(dlsym(RTLD_DEFAULT, "EternalSonataSetSaveLocation"));
  return api.set_allowed != nullptr;
}
#endif

uint8_t ReadGuestByte(const rex::Runtime* runtime, uint32_t address) {
  if (!runtime) {
    return 0;
  }
  auto* memory = const_cast<rex::Runtime*>(runtime)->memory();
  if (!memory) {
    return 0;
  }
  const auto* host = memory->TranslateVirtual<const uint8_t*>(address);
  return host ? *host : 0;
}

class SaveAnywhereMod : public rex::system::IModPlugin {
 public:
  void OnModuleLaunched() override {
    if (!ResolveHost(api_)) {
      REXLOG_WARN("save_anywhere: this host has no save API; nothing to do");
      return;
    }
    // Gate starts cleared; enabled per-frame only once scripts are live.
    api_.set_allowed(0);
    prev_at_save_point_ = ReadGuestByte(runtime_, kAtSavePointAddr);
    REXLOG_INFO("save_anywhere: armed, initial at-save-point = {}", prev_at_save_point_);
    auto* registry = runtime_->mod_registry();
    registry->RegisterTick([this] { Tick(); });
    registry->Subscribe("eternalsonata.save.started",
                        [this](const rex::system::ModRegistry::EventPayload& p) {
                          OnSaveStarted(static_cast<int>(p.u64));
                        });
  }

  void Tick() {
    const uint8_t cur = ReadGuestByte(runtime_, kAtSavePointAddr);
    if (cur != prev_at_save_point_) {
      if (!scripts_alive_) {
        REXLOG_INFO("save_anywhere: field scripts live (flag {} -> {}), enabling save anywhere",
                    prev_at_save_point_, cur);
      }
      scripts_alive_ = true;
      prev_at_save_point_ = cur;
    }
    api_.set_allowed(scripts_alive_ ? 1 : 0);

    // Track the last non-zero map location, for naming away-from-point saves.
    if (api_.get_location) {
      const int loc = api_.get_location();
      if (loc > 0) {
        last_location_ = loc;
      }
    }
  }

  void OnSaveStarted(int slot) {
    if (!api_.get_location || !api_.set_location) {
      return;
    }
    const int cur = api_.get_location();
    if (cur == 0 && last_location_ > 0) {
      api_.set_location(last_location_);
      REXLOG_INFO("save_anywhere: save {} tagged with location {} (last known map)",
                  slot, last_location_);
    }
  }

  void OnShutdown() override {
    if (api_.set_allowed) {
      api_.set_allowed(0);
    }
  }

  void AttachContext(const rex::system::ModHostContext* ctx) {
    runtime_ = ctx ? ctx->runtime : nullptr;
  }

 private:
  HostApi api_{};
  rex::Runtime* runtime_ = nullptr;
  uint8_t prev_at_save_point_ = 0;
  bool scripts_alive_ = false;
  int last_location_ = 0;
};

}  // namespace

static SaveAnywhereMod g_mod;

extern "C" REX_MOD_PLUGIN_EXPORT uint32_t rex_mod_abi_version(void) {
  return rex::system::kModPluginAbiVersion;
}

extern "C" REX_MOD_PLUGIN_EXPORT rex::system::IModPlugin* rex_mod_create(
    uint32_t abi_version, const rex::system::ModHostContext* ctx) {
  if (abi_version != rex::system::kModPluginAbiVersion || !ctx) {
    return nullptr;
  }
  g_mod.AttachContext(ctx);
  return &g_mod;
}
