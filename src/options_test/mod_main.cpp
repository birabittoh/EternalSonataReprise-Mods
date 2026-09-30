// options_test: registers enough rows on the native Options screen to fill the
// first mods page and spill onto the next, so the paging can be checked without
// a real mod behind it. Values live only in this process; changes are logged.

#include <rex/system/mod_plugin.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

#include <rex/logging.h>

#include <rexmod/eternalsonata_options_api.h>

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
Fn Resolve(const char* symbol) {
  return reinterpret_cast<Fn>(HostSymbol(symbol));
}

// Enough rows to overflow the first mods page onto the next one.
constexpr int kRowCount = 12;
const char* const kValues[] = {"Off", "Low", "Mid", "High", "Max"};
constexpr int kMaxValues = 5;

std::atomic<int> g_value[kRowCount] = {};
char g_label[kRowCount][24];

// Row i has 2 + i % 4 values, so rows of every width show up.
int ValueCount(int row) { return 2 + row % (kMaxValues - 1); }

int GetValue(void* user) {
  return g_value[reinterpret_cast<intptr_t>(user)].load();
}

void SetValue(int index, void* user) {
  const intptr_t row = reinterpret_cast<intptr_t>(user);
  g_value[row].store(index);
  REXLOG_INFO("[options_test] {} -> {}", g_label[row], kValues[index]);
}

class OptionsTestMod : public rex::system::IModPlugin {
 public:
  void OnModuleLaunched() override {
    const auto version = Resolve<EternalSonataOptionsAbiVersionFn>(
        "EternalSonataOptionsAbiVersion");
    const auto reg = Resolve<EternalSonataRegisterOptionRowFn>(
        "EternalSonataRegisterOptionRow");
    if (!version || !reg) {
      REXLOG_WARN("[options_test] host has no Options API");
      return;
    }
    for (intptr_t i = 0; i < kRowCount; ++i) {
      std::snprintf(g_label[i], sizeof(g_label[i]), "Test Row %d", static_cast<int>(i + 1));
      const int row = reg(g_label[i], kValues, ValueCount(static_cast<int>(i)), &GetValue,
                          &SetValue, reinterpret_cast<void*>(i));
      if (row < 0) {
        REXLOG_WARN("[options_test] {} rejected", g_label[i]);
        continue;
      }
      REXLOG_INFO("[options_test] registered {} as row {} (host ABI {})", g_label[i], row,
                  version());
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
  return new OptionsTestMod();
}
