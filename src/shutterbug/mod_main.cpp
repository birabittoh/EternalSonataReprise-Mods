// shutterbug - adds one custom achievement, "Shutterbug", earned by taking
// five photographs during a single battle.
//
// It is the smallest example of the achievement API: the host assigns the id,
// so nothing here picks one, and the achievement shows up in the unlock toast,
// the F7 overlay and the status menu's Achievements screen (in its Custom tab,
// which exists precisely because a mod registered something) without this mod
// drawing anything itself.
//
// The counting is two events and a flag:
//
//   * the mod registry's frame tick polls EternalSonataIsBattleActive, and
//     resets the counter whenever a battle starts or ends, so photographs never
//     accumulate across encounters or from the field;
//   * "eternalsonata.photo.added" fires once per photograph the game commits to
//     the album, and the ones that land while a battle is running are counted.
//
// Both run on the registry's tick thread, so the counter needs no lock; the id
// is written from OnModuleLaunched on another thread and is atomic for that
// reason alone.
//
// One caveat worth knowing while testing: the album holds twelve photographs
// and the game stops committing new ones when it is full, so no event fires and
// nothing counts. Sell a few first.

#include <rex/system/mod_plugin.h>

#include <atomic>
#include <cstdint>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/mod_registry.h>

#include <rexmod/eternalsonata_achievement_api.h>
#include <rexmod/eternalsonata_battle_api.h>
#include <rexmod/eternalsonata_photo_api.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

constexpr int kPhotosRequired = 5;

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

// Every entry point is resolved at load time and null-checked before use, so
// this mod still loads on a host too old to have the achievement API; it just
// does nothing.
struct Api {
  EternalSonataRegisterCustomAchievementFn register_achievement = nullptr;
  EternalSonataUnlockAchievementFn unlock = nullptr;
  EternalSonataIsAchievementUnlockedFn is_unlocked = nullptr;
  EternalSonataIsBattleActiveFn battle_active = nullptr;

  bool ok() const { return register_achievement && unlock && battle_active; }
};

class ShutterbugMod : public rex::system::IModPlugin {
 public:
  explicit ShutterbugMod(rex::Runtime* runtime) : runtime_(runtime) {}

  void OnModuleLaunched() override {
    Resolve(api_.register_achievement, "EternalSonataRegisterCustomAchievement");
    Resolve(api_.unlock, "EternalSonataUnlockAchievement");
    Resolve(api_.is_unlocked, "EternalSonataIsAchievementUnlocked");
    Resolve(api_.battle_active, "EternalSonataIsBattleActive");
    if (!api_.ok()) {
      REXLOG_WARN("[shutterbug] host has no achievement API; mod idle");
      return;
    }

    // The host keeps whichever entry matches the language the game booted in
    // and falls back to the fields below for the ones nobody translated.
    static const EternalSonataAchievementTranslation kTranslations[] = {
        {ETERNALSONATA_LANGUAGE_ITALIAN, "Fotoreporter",
         "Scatta cinque fotografie in una sola battaglia.", nullptr},
        {ETERNALSONATA_LANGUAGE_FRENCH, "Paparazzi",
         "Prenez cinq photographies au cours d'un seul combat.", nullptr},
        {ETERNALSONATA_LANGUAGE_GERMAN, "Schnappschuss",
         "Mache fünf Fotos in einem einzigen Kampf.", nullptr},
        {ETERNALSONATA_LANGUAGE_SPANISH, "Fotógrafo",
         "Haz cinco fotografías en un solo combate.", nullptr},
    };

    EternalSonataCustomAchievementData data = {};
    data.name = "Shutterbug";
    data.description = "Take five photographs in a single battle.";
    data.gamerscore = 50;
    data.translations = kTranslations;
    data.translation_count =
        static_cast<int32_t>(sizeof(kTranslations) / sizeof(kTranslations[0]));
    const int id = api_.register_achievement(&data);
    if (id <= 0) {
      REXLOG_ERROR("[shutterbug] registration failed ({})", id);
      return;
    }
    achievement_id_.store(id);
    REXLOG_INFO("[shutterbug] registered as achievement {}", id);

    auto* registry = runtime_ ? runtime_->mod_registry() : nullptr;
    if (!registry) {
      REXLOG_WARN("[shutterbug] no mod registry; nothing will count");
      return;
    }
    registry->RegisterTick([this] { Tick(); });
    registry->Subscribe(ETERNALSONATA_PHOTO_EVENT_ADDED,
                        [this](const rex::system::ModRegistry::EventPayload&) {
                          OnPhotoAdded();
                        });
  }

 private:
  void Tick() {
    const bool active = api_.battle_active && api_.battle_active() != 0;
    if (active == in_battle_) {
      return;
    }
    in_battle_ = active;
    photos_ = 0;
  }

  void OnPhotoAdded() {
    const int id = achievement_id_.load();
    if (!in_battle_ || id <= 0) {
      return;
    }
    if (++photos_ < kPhotosRequired) {
      return;
    }
    // Unlocking an already unlocked achievement is a no-op that returns 0, so
    // the sixth photograph of a battle costs nothing.
    api_.unlock(id, /*show_toast=*/1);
  }

  rex::Runtime* runtime_ = nullptr;
  Api api_;
  std::atomic<int> achievement_id_{0};

  // Tick-thread only.
  bool in_battle_ = false;
  int photos_ = 0;
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
  return new ShutterbugMod(ctx->runtime);
}
