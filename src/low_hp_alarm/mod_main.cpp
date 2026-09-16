// low_hp_alarm - beeps in battle while any living party member is below 20%
// HP, using the battle API introduced in Battle API v2 (see the game repo's
// src/eternalsonata_battle_api.h, copied verbatim here as
// rexmod/eternalsonata_battle_api.h).
//
// The beep is a synthesized tone rather than a game asset: AppKeep.bmd packs
// its audio in an in-house "CSF "/"BOOK" chunk format that nothing in
// default.xex actually parses (likely a hardware XMA path), so there is no
// known decoder to pull a real clip from. RawPcmPlayer (rex/audio) plays the
// synthesized PCM directly, independent of the guest's own audio pipeline.

#include <rex/system/mod_plugin.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include <imgui.h>

#include <rex/audio/raw_pcm_player.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include <rexmod/eternalsonata_battle_api.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

constexpr float kLowHpThreshold = 0.2f;
constexpr float kBeepInterval = 0.5f;   // seconds between beeps while low
constexpr int kSampleRate = 44100;
constexpr float kBeepDuration = 0.12f;  // seconds
constexpr float kBeepFrequency = 1200.0f;

// Every entry point is resolved out of the host executable at load time and
// null-checked before use, so this mod still loads (and just stays silent)
// on a host too old to have the battle API.
struct BattleApi {
  EternalSonataBattleAbiVersionFn abi_version = nullptr;
  EternalSonataIsBattleActiveFn is_active = nullptr;
  EternalSonataGetBattleStateFn state = nullptr;
  EternalSonataGetBattlePartyUnitFn party_unit = nullptr;

  bool ok() const { return abi_version && is_active && state && party_unit; }
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

BattleApi LoadBattleApi() {
  BattleApi api;
  Resolve(api.abi_version, "EternalSonataBattleAbiVersion");
  Resolve(api.is_active, "EternalSonataIsBattleActive");
  Resolve(api.state, "EternalSonataGetBattleState");
  Resolve(api.party_unit, "EternalSonataGetBattlePartyUnit");
  return api;
}

// A short tone with a linear fade in/out, so repeated triggers click rather
// than pop.
std::vector<int16_t> MakeBeep() {
  const int sample_count = static_cast<int>(kSampleRate * kBeepDuration);
  const int fade_samples = sample_count / 8;
  std::vector<int16_t> pcm(sample_count);
  for (int i = 0; i < sample_count; ++i) {
    float envelope = 1.0f;
    if (i < fade_samples) {
      envelope = static_cast<float>(i) / fade_samples;
    } else if (i >= sample_count - fade_samples) {
      envelope = static_cast<float>(sample_count - i) / fade_samples;
    }
    const float t = static_cast<float>(i) / kSampleRate;
    const float sample = std::sin(2.0f * 3.14159265f * kBeepFrequency * t) * envelope;
    pcm[i] = static_cast<int16_t>(sample * 22000.0f);
  }
  return pcm;
}

// No visible UI: this dialog exists purely to get a per-frame OnDraw tick
// (ImGuiDialog::Draw runs every frame regardless of any window being open).
class LowHpAlarmDialog : public rex::ui::ImGuiDialog {
 public:
  explicit LowHpAlarmDialog(rex::ui::ImGuiDrawer* drawer)
      : ImGuiDialog(drawer), battle_(LoadBattleApi()), beep_(MakeBeep()) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!battle_.ok()) {
      return;
    }

    time_since_beep_ += io.DeltaTime;

    if (!IsAnyoneCriticallyLow()) {
      return;
    }

    if (time_since_beep_ >= kBeepInterval) {
      time_since_beep_ = 0.0f;
      player_.PlayInt16(beep_, kSampleRate, 1);
    }
  }

 private:
  bool IsAnyoneCriticallyLow() {
    EternalSonataBattleState state{};
    if (battle_.state(&state) != ETERNALSONATA_BATTLE_OK || !state.active) {
      return false;
    }
    for (int slot = 0; slot < state.party_count; ++slot) {
      EternalSonataBattleUnit unit{};
      if (battle_.party_unit(slot, &unit) != ETERNALSONATA_BATTLE_OK) {
        continue;
      }
      if (unit.alive && unit.hp_ratio >= 0.0f && unit.hp_ratio < kLowHpThreshold) {
        return true;
      }
    }
    return false;
  }

  BattleApi battle_;
  std::vector<int16_t> beep_;
  rex::audio::RawPcmPlayer player_;
  float time_since_beep_ = 0.0f;
};

class LowHpAlarmMod : public rex::system::IModPlugin {
 public:
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    dialog_ = std::make_unique<LowHpAlarmDialog>(drawer);
  }

 private:
  std::unique_ptr<LowHpAlarmDialog> dialog_;
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
  return new LowHpAlarmMod();
}
