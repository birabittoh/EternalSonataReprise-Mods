#include <rex/system/mod_plugin.h>

#include <algorithm>
#include <cstdint>
#include <memory>

#include <imgui.h>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

#include <rexmod/eternalsonata_overworld_api.h>

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

struct PositionApi {
  EternalSonataOverworldAbiVersionFn version = nullptr;
  EternalSonataGetFieldPositionFn get = nullptr;
  EternalSonataSetFieldPositionFn set = nullptr;
  EternalSonataGetFieldCameraFn get_camera = nullptr;
  EternalSonataSetFieldCameraControlFn camera_control = nullptr;
  EternalSonataSetFieldCameraFn set_camera = nullptr;
  EternalSonataSetFieldCollisionEnabledFn set_collision = nullptr;
  EternalSonataIsFieldCollisionEnabledFn get_collision = nullptr;
  EternalSonataGetFieldFacingFn get_facing = nullptr;
  EternalSonataSetFieldFacingFn set_facing = nullptr;

  PositionApi() {
    Resolve(version, "EternalSonataOverworldAbiVersion");
    Resolve(get, "EternalSonataGetFieldPosition");
    Resolve(set, "EternalSonataSetFieldPosition");
    Resolve(get_camera, "EternalSonataGetFieldCamera");
    Resolve(camera_control, "EternalSonataSetFieldCameraControl");
    Resolve(set_camera, "EternalSonataSetFieldCamera");
    Resolve(set_collision, "EternalSonataSetFieldCollisionEnabled");
    Resolve(get_collision, "EternalSonataIsFieldCollisionEnabled");
    Resolve(get_facing, "EternalSonataGetFieldFacing");
    Resolve(set_facing, "EternalSonataSetFieldFacing");
  }

  bool facing_ready() const { return get_facing && set_facing; }

  bool collision_ready() const {
    return set_collision && get_collision;
  }

  bool ready() const {
    return get && set;
  }

  bool camera_ready() const {
    return get_camera && camera_control && set_camera;
  }
};

class FieldPositionDialog : public rex::ui::ImGuiDialog {
 public:
  explicit FieldPositionDialog(rex::ui::ImGuiDrawer* drawer)
      : ImGuiDialog(drawer) {
    rex::ui::RegisterBind(
        "bind_field_position_probe", "F9", "Toggle field position probe",
        [this] { visible_ = !visible_; }, [this] { return visible_; },
        "Field Position Probe##rex");
  }

  ~FieldPositionDialog() override {
    if (camera_enabled_ && api_.camera_control) {
      api_.camera_control(0);
    }
    if (collisions_off_ && api_.set_collision) {
      api_.set_collision(1);
    }
    rex::ui::UnregisterBind("bind_field_position_probe");
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!visible_) {
      if (camera_enabled_) {
        api_.camera_control(0);
        camera_enabled_ = false;
      }
      if (collisions_off_ && api_.set_collision) {
        api_.set_collision(1);
        collisions_off_ = false;
      }
      return;
    }
    ImGui::SetNextWindowSize(ImVec2(430, 650), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Field Position Probe##rex", &visible_,
                      ImGuiWindowFlags_NoCollapse)) {
      ImGui::End();
      return;
    }
    if (!api_.ready()) {
      ImGui::TextUnformatted("Position API unavailable in this game build.");
      ImGui::Text("ABI version: %u  Expected: %u",
                  api_.version ? api_.version() : 0u,
                  ETERNALSONATA_OVERWORLD_ABI_VERSION);
      ImGui::Text("Getter: %s  Setter: %s", api_.get ? "found" : "missing",
                  api_.set ? "found" : "missing");
      ImGui::End();
      return;
    }
    ImGui::Text("Host ABI version: %u", api_.version ? api_.version() : 0u);

    EternalSonataFieldPosition live{};
    const int read_result = api_.get(&live);
    if (read_result == ETERNALSONATA_OVERWORLD_OK) {
      ImGui::Text("Live  X %.3f  Y %.3f  Z %.3f", live.x, live.y, live.z);
      if (!have_edit_) {
        edit_ = live;
        have_edit_ = true;
      }
    } else if (read_result == ETERNALSONATA_OVERWORLD_ERR_IN_BATTLE) {
      ImGui::TextUnformatted("Position unavailable during battle.");
    } else {
      ImGui::TextUnformatted("Waiting for a field leader.");
      have_edit_ = false;
      have_target_ = false;
    }

    ImGui::BeginDisabled(read_result != ETERNALSONATA_OVERWORLD_OK);
    if (ImGui::Button("Capture live")) {
      edit_ = live;
      target_ = live;
      have_target_ = true;
    }
    ImGui::InputFloat("X", &edit_.x, 0.0f, 0.0f, "%.3f");
    ImGui::InputFloat("Y", &edit_.y, 0.0f, 0.0f, "%.3f");
    ImGui::InputFloat("Z", &edit_.z, 0.0f, 0.0f, "%.3f");
    if (ImGui::Button("Set position")) {
      set_result_ = api_.set(&edit_);
      if (set_result_ == ETERNALSONATA_OVERWORLD_QUEUED) {
        target_ = edit_;
        have_target_ = true;
      }
    }
    ImGui::Text("Last set result: %d", set_result_);

    ImGui::Separator();
    DrawCollisionRow();
    if (ImGui::Checkbox("Free movement", &free_movement_)) {
      target_ = live;
      have_target_ = free_movement_;
    }
    ImGui::SliderFloat("Speed", &speed_, 0.1f, 30.0f, "%.1f units per second");
    ImGui::TextUnformatted("Hold a movement button to test repeated writes.");
    if (free_movement_ && !have_target_) {
      target_ = live;
      have_target_ = true;
    }
    EternalSonataFieldPosition moved = target_;
    const float step = speed_ * std::min(io.DeltaTime, 0.05f);
    auto button = [](const char* label) {
      ImGui::Button(label, ImVec2(105, 0));
      return ImGui::IsItemActive();
    };
    if (button("X left")) moved.x -= step;
    ImGui::SameLine();
    if (button("X right")) moved.x += step;
    if (button("Y down")) moved.y -= step;
    ImGui::SameLine();
    if (button("Y up")) moved.y += step;
    if (button("Z back")) moved.z -= step;
    ImGui::SameLine();
    if (button("Z forward")) moved.z += step;
    if (free_movement_) {
      target_ = moved;
      set_result_ = api_.set(&target_);
      ImGui::Text("Target  X %.3f  Y %.3f  Z %.3f", target_.x,
                  target_.y, target_.z);
    }
    ImGui::EndDisabled();
    DrawFacingPanel(io);
    DrawCameraPanel(io);
    ImGui::End();
  }

 private:
  void DrawFacingPanel(ImGuiIO& io) {
    ImGui::Separator();
    ImGui::TextUnformatted("Facing");
    if (!api_.facing_ready()) {
      ImGui::TextUnformatted("Facing API unavailable in this game build.");
      return;
    }
    EternalSonataFieldFacing live{};
    if (api_.get_facing(&live) != ETERNALSONATA_OVERWORLD_OK) {
      ImGui::TextUnformatted("Waiting for a field leader.");
      have_facing_edit_ = false;
      pinned_facing_ = false;
      return;
    }
    constexpr float kToDegrees = 57.2957795f;
    ImGui::Text("Yaw       %.2f rad  (%.1f deg)", live.yaw, live.yaw * kToDegrees);
    ImGui::Text("Forward   %.3f  %.3f  %.3f", live.forward.x, live.forward.y,
                live.forward.z);
    ImGui::Text("Rotation  %.3f  %.3f  %.3f rad", live.rotation.x,
                live.rotation.y, live.rotation.z);
    if (!have_facing_edit_) {
      facing_edit_ = live.rotation;
      have_facing_edit_ = true;
    }
    if (ImGui::Button("Capture live facing")) {
      facing_edit_ = live.rotation;
    }
    ImGui::InputFloat("Face pitch", &facing_edit_.x, 0.0f, 0.0f, "%.3f");
    ImGui::InputFloat("Face yaw", &facing_edit_.y, 0.0f, 0.0f, "%.3f");
    ImGui::InputFloat("Face roll", &facing_edit_.z, 0.0f, 0.0f, "%.3f");
    if (ImGui::Button("Set facing")) {
      facing_result_ = api_.set_facing(&facing_edit_);
    }
    ImGui::SameLine();
    ImGui::Text("Last facing result: %d", facing_result_);
    ImGui::Checkbox("Pin facing", &pinned_facing_);
    ImGui::SameLine();
    ImGui::TextDisabled("(walking turns the character back)");
    ImGui::SliderFloat("Face turn speed", &face_turn_speed_, 0.05f, 6.0f,
                       "%.2f rad per second");

    const float step = face_turn_speed_ * std::min(io.DeltaTime, 0.05f);
    auto hold = [](const char* label) {
      ImGui::Button(label, ImVec2(105, 0));
      return ImGui::IsItemActive();
    };
    bool turned = false;
    if (hold("Face left")) { facing_edit_.y -= step; turned = true; }
    ImGui::SameLine();
    if (hold("Face right")) { facing_edit_.y += step; turned = true; }
    if (pinned_facing_ || turned) {
      facing_result_ = api_.set_facing(&facing_edit_);
    }
  }

  void DrawCollisionRow() {
    if (!api_.collision_ready()) {
      ImGui::TextUnformatted("Collision API unavailable in this game build.");
      return;
    }
    collisions_off_ = api_.get_collision() == 0;
    if (ImGui::Checkbox("Disable collisions", &collisions_off_)) {
      collision_result_ = api_.set_collision(collisions_off_ ? 0 : 1);
      if (collision_result_ != ETERNALSONATA_OVERWORLD_OK) {
        collisions_off_ = api_.get_collision() == 0;
      }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(walls and floor snapping off, party and NPCs)");
    ImGui::Text("Last collision result: %d", collision_result_);
  }

  void DrawCameraPanel(ImGuiIO& io) {
    ImGui::Separator();
    ImGui::TextUnformatted("Camera");
    if (!api_.camera_ready()) {
      ImGui::TextUnformatted("Camera API unavailable in this game build.");
      return;
    }
    EternalSonataFieldCamera live{};
    const int read_result = api_.get_camera(&live);
    if (read_result == ETERNALSONATA_OVERWORLD_OK) {
      ImGui::Text("Live position  %.3f  %.3f  %.3f", live.position.x,
                  live.position.y, live.position.z);
      ImGui::Text("Live rotation  %.3f  %.3f  %.3f rad", live.rotation.x,
                  live.rotation.y, live.rotation.z);
      if (!have_camera_edit_) {
        camera_edit_ = live;
        have_camera_edit_ = true;
      }
    } else {
      ImGui::TextUnformatted("Waiting for an active field camera.");
      have_camera_edit_ = false;
      have_camera_target_ = false;
      camera_enabled_ = false;
    }

    ImGui::BeginDisabled(read_result != ETERNALSONATA_OVERWORLD_OK);
    if (ImGui::Button("Capture live camera")) {
      camera_edit_ = live;
      camera_target_ = live;
      have_camera_target_ = true;
    }
    ImGui::InputFloat("Camera X", &camera_edit_.position.x, 0, 0, "%.3f");
    ImGui::InputFloat("Camera Y", &camera_edit_.position.y, 0, 0, "%.3f");
    ImGui::InputFloat("Camera Z", &camera_edit_.position.z, 0, 0, "%.3f");
    ImGui::InputFloat("Pitch", &camera_edit_.rotation.x, 0, 0, "%.3f");
    ImGui::InputFloat("Yaw", &camera_edit_.rotation.y, 0, 0, "%.3f");
    ImGui::InputFloat("Roll", &camera_edit_.rotation.z, 0, 0, "%.3f");

    if (ImGui::Checkbox("Camera control", &camera_enabled_)) {
      camera_control_result_ = api_.camera_control(camera_enabled_ ? 1 : 0);
      if (camera_control_result_ != ETERNALSONATA_OVERWORLD_QUEUED) {
        camera_enabled_ = false;
      } else if (camera_enabled_) {
        camera_target_ = live;
        have_camera_target_ = true;
      }
    }
    ImGui::Text("Camera control result: %d", camera_control_result_);
    ImGui::BeginDisabled(!camera_enabled_);
    if (ImGui::Button("Set camera")) {
      camera_set_result_ = api_.set_camera(&camera_edit_);
      if (camera_set_result_ == ETERNALSONATA_OVERWORLD_QUEUED) {
        camera_target_ = camera_edit_;
        have_camera_target_ = true;
      }
    }
    ImGui::Text("Camera set result: %d", camera_set_result_);
    ImGui::SliderFloat("Camera speed", &camera_speed_, 0.1f, 30.0f,
                       "%.1f units per second");
    ImGui::SliderFloat("Turn speed", &turn_speed_, 0.05f, 3.0f,
                       "%.2f rad per second");
    if (camera_enabled_ && !have_camera_target_) {
      camera_target_ = live;
      have_camera_target_ = true;
    }
    auto hold = [](const char* label) {
      ImGui::Button(label, ImVec2(105, 0));
      return ImGui::IsItemActive();
    };
    const float dt = std::min(io.DeltaTime, 0.05f);
    if (hold("Cam X left")) camera_target_.position.x -= camera_speed_ * dt;
    ImGui::SameLine();
    if (hold("Cam X right")) camera_target_.position.x += camera_speed_ * dt;
    if (hold("Cam Y down")) camera_target_.position.y -= camera_speed_ * dt;
    ImGui::SameLine();
    if (hold("Cam Y up")) camera_target_.position.y += camera_speed_ * dt;
    if (hold("Cam Z back")) camera_target_.position.z -= camera_speed_ * dt;
    ImGui::SameLine();
    if (hold("Cam Z forward")) camera_target_.position.z += camera_speed_ * dt;
    if (hold("Pitch down")) camera_target_.rotation.x -= turn_speed_ * dt;
    ImGui::SameLine();
    if (hold("Pitch up")) camera_target_.rotation.x += turn_speed_ * dt;
    if (hold("Yaw left")) camera_target_.rotation.y -= turn_speed_ * dt;
    ImGui::SameLine();
    if (hold("Yaw right")) camera_target_.rotation.y += turn_speed_ * dt;
    if (camera_enabled_ && have_camera_target_) {
      camera_set_result_ = api_.set_camera(&camera_target_);
      ImGui::Text("Camera target  %.3f  %.3f  %.3f", camera_target_.position.x,
                  camera_target_.position.y, camera_target_.position.z);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
  }

  PositionApi api_;
  EternalSonataFieldPosition edit_{};
  bool visible_ = false;
  bool have_edit_ = false;
  bool free_movement_ = false;
  bool have_target_ = false;
  EternalSonataFieldPosition target_{};
  float speed_ = 5.0f;
  int set_result_ = ETERNALSONATA_OVERWORLD_ERR_UNAVAILABLE;
  EternalSonataFieldCamera camera_edit_{};
  EternalSonataFieldCamera camera_target_{};
  bool have_camera_edit_ = false;
  bool have_camera_target_ = false;
  bool camera_enabled_ = false;
  float camera_speed_ = 5.0f;
  float turn_speed_ = 0.5f;
  bool collisions_off_ = false;
  int collision_result_ = ETERNALSONATA_OVERWORLD_OK;
  int camera_control_result_ = ETERNALSONATA_OVERWORLD_ERR_UNAVAILABLE;
  int camera_set_result_ = ETERNALSONATA_OVERWORLD_ERR_UNAVAILABLE;
  EternalSonataFieldPosition facing_edit_{};
  bool have_facing_edit_ = false;
  bool pinned_facing_ = false;
  float face_turn_speed_ = 1.5f;
  int facing_result_ = ETERNALSONATA_OVERWORLD_ERR_UNAVAILABLE;
};

class FieldPositionProbe : public rex::system::IModPlugin {
 public:
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    dialog_ = std::make_unique<FieldPositionDialog>(drawer);
  }

 private:
  std::unique_ptr<FieldPositionDialog> dialog_;
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
  return new FieldPositionProbe();
}
