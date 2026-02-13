#include "aimbot.h"
#include "frame_cache.h"
#include "player_info.h"
#include "esp.h"
#include "visibility.h"
#include "../mono/mono_api.h"
#include "../mono/mono_types.h"
#include "../unity/unity_classes.h"
#include "../unity/unity_types.h"
#include "../gui/gui.h"

#include <Windows.h>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

namespace aimbot {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static constexpr float RAD2DEG = 57.2957795131f;
static constexpr float DEG2RAD = 0.0174532925f;

// Height offsets in SSJJ coordinate space (Z is up)
static constexpr float HEAD_HEIGHT = 160.0f;   // target head above feet
static constexpr float EYE_HEIGHT  = 150.0f;   // local eye/camera above feet

// Maximum angular distance (degrees) from crosshair to consider a target
static constexpr float AIM_FOV = 90.0f;

// Unity KeyCode values for mouse buttons
static constexpr int KEYCODE_MOUSE3 = 326;
static constexpr int KEYCODE_MOUSE4 = 327;

// ButtonConstant.Attack bitmask (key in InputComponent.KeyStates dictionary)
static constexpr int BUTTON_ATTACK = 64;

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static bool        s_enabled    = true;
static bool        s_silent     = true;     // silent mode on by default
static bool        s_auto_fire  = true;     // auto-fire in silent mode
static std::string s_debug;

// Auto-fire state: track whether WE forced the attack key so we can clear it
static bool        s_forced_fire = false;

// Silent-mode state
static bool   s_tracking           = false; // actively tracking real mouse
static float  s_real_yaw           = 0.0f;  // accumulated real mouse yaw (SSJJ)
static float  s_real_pitch         = 0.0f;  // accumulated real mouse pitch (SSJJ)
static float  s_prev_aim_yaw      = 0.0f;  // last SSJJ yaw written to InputComponent
static float  s_prev_aim_pitch    = 0.0f;  // last SSJJ pitch written to InputComponent

// Camera euler calibration (SSJJ angles → Unity camera euler mapping)
// Mapping: euler.y = -ssjj_yaw + offset_y,  euler.x = -ssjj_pitch + offset_x
static bool   s_cam_calibrated     = false;
static float  s_euler_offset_y     = 0.0f;
static float  s_euler_offset_x     = 0.0f;

// ---------------------------------------------------------------------------
// Cached handles
// ---------------------------------------------------------------------------
static struct {
    MonoClass* PunchOrientationComponent = nullptr;
    MonoClass* OrientationComponent      = nullptr;
    MonoClass* InputComponent            = nullptr;
} s_classes;

static struct {
    // Player entity access
    MonoMethod* Contexts_get_sharedInstance      = nullptr;
    MonoMethod* Contexts_get_player              = nullptr;
    MonoMethod* PlayerContext_get_myPlayerEntity  = nullptr;
    MonoMethod* PlayerEntity_get_punchOrientation = nullptr;
    MonoMethod* PlayerEntity_get_orientation      = nullptr;

    // InputComponent access (via UserCommandContext)
    MonoMethod* Contexts_get_userCommand          = nullptr;
    MonoMethod* UserCommandContext_get_input       = nullptr;
} s_methods;

static struct {
    MonoClassField* PunchYaw         = nullptr;
    MonoClassField* PunchPitch       = nullptr;
    MonoClassField* Orientation_Yaw  = nullptr;
    MonoClassField* Orientation_Pitch = nullptr;
    // InputComponent fields (source of truth for aim direction)
    MonoClassField* Input_Yaw        = nullptr;
    MonoClassField* Input_Pitch      = nullptr;
    MonoClassField* Input_TempYaw    = nullptr;   // double - high precision accumulator
    MonoClassField* Input_TempPitch  = nullptr;   // double - high precision accumulator
} s_fields;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
// Exception-safe invoke: catches managed exceptions locally so they don't
// longjmp past C++ stack frames (which would skip destructors of std::string,
// std::vector etc. and corrupt the heap).
static MonoObject* invoke(MonoMethod* method, MonoObject* obj,
                          void** params = nullptr) {
    if (!method) return nullptr;
    MonoObject* exc = nullptr;
    MonoObject* ret = mono::runtime_invoke(method, obj, params, &exc);
    return exc ? nullptr : ret;
}

static float normalize_angle(float angle) {
    while (angle > 180.0f)  angle -= 360.0f;
    while (angle < -180.0f) angle += 360.0f;
    return angle;
}

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------
bool initialize() {
    MonoImage* ent_img = unity::images().entitas_lib;
    if (!ent_img) return false;

    MonoClass* Contexts            = mono::class_from_name(ent_img, "", "Contexts");
    MonoClass* PlayerContext       = mono::class_from_name(ent_img, "", "PlayerContext");
    MonoClass* PlayerEntity        = mono::class_from_name(ent_img, "", "PlayerEntity");
    MonoClass* UserCommandContext  = mono::class_from_name(ent_img, "", "UserCommandContext");

    s_classes.PunchOrientationComponent = mono::class_from_name(
        ent_img, "Assets.Sources.Components.Player", "PunchOrientationComponent");
    s_classes.OrientationComponent = mono::class_from_name(
        ent_img, "Assets.Sources.Components.Common", "OrientationComponent");
    s_classes.InputComponent = mono::class_from_name(
        ent_img, "Assets.Sources.Components.UserComand", "InputComponent");

    if (!Contexts || !PlayerContext || !PlayerEntity) return false;

    // Methods -- player entity
    s_methods.Contexts_get_sharedInstance =
        mono::class_get_method_from_name(Contexts, "get_sharedInstance", 0);
    s_methods.Contexts_get_player =
        mono::class_get_method_from_name(Contexts, "get_player", 0);
    s_methods.PlayerContext_get_myPlayerEntity =
        mono::class_get_method_from_name(PlayerContext, "get_myPlayerEntity", 0);
    s_methods.PlayerEntity_get_punchOrientation =
        mono::class_get_method_from_name(PlayerEntity, "get_punchOrientation", 0);
    s_methods.PlayerEntity_get_orientation =
        mono::class_get_method_from_name(PlayerEntity, "get_orientation", 0);

    // Methods -- InputComponent via UserCommandContext
    s_methods.Contexts_get_userCommand =
        mono::class_get_method_from_name(Contexts, "get_userCommand", 0);
    if (UserCommandContext) {
        s_methods.UserCommandContext_get_input =
            mono::class_get_method_from_name(UserCommandContext, "get_input", 0);
    }

    // Fields -- PunchOrientationComponent
    if (s_classes.PunchOrientationComponent && mono::class_get_field_from_name) {
        s_fields.PunchYaw   = mono::class_get_field_from_name(
            s_classes.PunchOrientationComponent, "PunchYaw");
        s_fields.PunchPitch = mono::class_get_field_from_name(
            s_classes.PunchOrientationComponent, "PunchPitch");
    }

    // Fields -- OrientationComponent (for reading current view angles)
    if (s_classes.OrientationComponent && mono::class_get_field_from_name) {
        s_fields.Orientation_Yaw   = mono::class_get_field_from_name(
            s_classes.OrientationComponent, "Yaw");
        s_fields.Orientation_Pitch = mono::class_get_field_from_name(
            s_classes.OrientationComponent, "Pitch");
    }

    // Fields -- InputComponent (source of truth for aim direction)
    if (s_classes.InputComponent && mono::class_get_field_from_name) {
        s_fields.Input_Yaw       = mono::class_get_field_from_name(
            s_classes.InputComponent, "Yaw");
        s_fields.Input_Pitch     = mono::class_get_field_from_name(
            s_classes.InputComponent, "Pitch");
        s_fields.Input_TempYaw   = mono::class_get_field_from_name(
            s_classes.InputComponent, "TempYaw");
        s_fields.Input_TempPitch = mono::class_get_field_from_name(
            s_classes.InputComponent, "TempPitch");
    }

    return true;
}

// ---------------------------------------------------------------------------
// Access local player entity
// ---------------------------------------------------------------------------
static MonoObject* get_local_player_entity() {
    MonoObject* contexts = invoke(s_methods.Contexts_get_sharedInstance, nullptr);
    if (!contexts) return nullptr;
    MonoObject* player_ctx = invoke(s_methods.Contexts_get_player, contexts);
    if (!player_ctx) return nullptr;
    return invoke(s_methods.PlayerContext_get_myPlayerEntity, player_ctx);
}

// ---------------------------------------------------------------------------
// Access InputComponent
// ---------------------------------------------------------------------------
static MonoObject* get_input_component() {
    MonoObject* contexts = invoke(s_methods.Contexts_get_sharedInstance, nullptr);
    if (!contexts) return nullptr;
    MonoObject* uc_ctx = invoke(s_methods.Contexts_get_userCommand, contexts);
    if (!uc_ctx) return nullptr;
    return invoke(s_methods.UserCommandContext_get_input, uc_ctx);
}

// ---------------------------------------------------------------------------
// Read / write component fields
// ---------------------------------------------------------------------------
static void read_punch(MonoObject* pe, float& py, float& pp) {
    py = pp = 0.0f;
    MonoObject* punch = invoke(s_methods.PlayerEntity_get_punchOrientation, pe);
    if (!punch) return;
    if (s_fields.PunchYaw)   mono::field_get_value(punch, s_fields.PunchYaw, &py);
    if (s_fields.PunchPitch) mono::field_get_value(punch, s_fields.PunchPitch, &pp);
}

static void read_orientation(MonoObject* pe, float& yaw, float& pitch) {
    yaw = pitch = 0.0f;
    MonoObject* ori = invoke(s_methods.PlayerEntity_get_orientation, pe);
    if (!ori) return;
    if (s_fields.Orientation_Yaw)   mono::field_get_value(ori, s_fields.Orientation_Yaw, &yaw);
    if (s_fields.Orientation_Pitch) mono::field_get_value(ori, s_fields.Orientation_Pitch, &pitch);
}

static void read_input(MonoObject* input, float& yaw, float& pitch) {
    yaw = pitch = 0.0f;
    if (!input) return;
    if (s_fields.Input_Yaw)   mono::field_get_value(input, s_fields.Input_Yaw, &yaw);
    if (s_fields.Input_Pitch) mono::field_get_value(input, s_fields.Input_Pitch, &pitch);
}

static void read_input_temp(MonoObject* input, double& temp_yaw, double& temp_pitch) {
    temp_yaw = temp_pitch = 0.0;
    if (!input) return;
    if (s_fields.Input_TempYaw)   mono::field_get_value(input, s_fields.Input_TempYaw, &temp_yaw);
    if (s_fields.Input_TempPitch) mono::field_get_value(input, s_fields.Input_TempPitch, &temp_pitch);
}

static void write_input(MonoObject* input, float yaw, float pitch) {
    if (!input) return;
    if (s_fields.Input_Yaw)   mono::field_set_value(input, s_fields.Input_Yaw, &yaw);
    if (s_fields.Input_Pitch) mono::field_set_value(input, s_fields.Input_Pitch, &pitch);
    // Must also set TempYaw/TempPitch (double) -- these are high-precision
    // accumulators. If we only set Yaw/Pitch, the game's next frame will
    // compute Yaw from TempYaw + mouse_delta and overwrite our value.
    double temp_yaw   = static_cast<double>(yaw);
    double temp_pitch = static_cast<double>(pitch);
    if (s_fields.Input_TempYaw)   mono::field_set_value(input, s_fields.Input_TempYaw, &temp_yaw);
    if (s_fields.Input_TempPitch) mono::field_set_value(input, s_fields.Input_TempPitch, &temp_pitch);
}

static void write_orientation(MonoObject* pe, float yaw, float pitch) {
    MonoObject* ori = invoke(s_methods.PlayerEntity_get_orientation, pe);
    if (!ori) return;
    if (s_fields.Orientation_Yaw)   mono::field_set_value(ori, s_fields.Orientation_Yaw, &yaw);
    if (s_fields.Orientation_Pitch) mono::field_set_value(ori, s_fields.Orientation_Pitch, &pitch);
}

// ---------------------------------------------------------------------------
// Auto-fire: simulate left mouse button via Windows API
// This injects a real OS-level mouse event that Unity reads on next frame.
// ---------------------------------------------------------------------------
static void set_auto_fire_state(bool fire) {
    if (fire && !s_forced_fire) {
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
        s_forced_fire = true;
    } else if (!fire && s_forced_fire) {
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
        s_forced_fire = false;
    }
}

// ---------------------------------------------------------------------------
// Camera Transform helpers (for silent aim camera override)
// ---------------------------------------------------------------------------
static bool get_camera_euler(unity::Vector3& out) {
    auto& m = unity::methods();
    if (!m.Camera_get_main || !m.Component_get_transform ||
        !m.Transform_get_eulerAngles) return false;

    MonoObject* cam = invoke(m.Camera_get_main, nullptr);
    if (!cam) return false;
    MonoObject* tf = invoke(m.Component_get_transform, cam);
    if (!tf) return false;
    MonoObject* result = invoke(m.Transform_get_eulerAngles, tf);
    if (!result) return false;
    auto* p = reinterpret_cast<unity::Vector3*>(mono::object_unbox(result));
    if (!p) return false;
    out = *p;
    return true;
}

static void set_camera_euler(const unity::Vector3& euler) {
    auto& m = unity::methods();
    if (!m.Camera_get_main || !m.Component_get_transform ||
        !m.Transform_set_eulerAngles) return;

    MonoObject* cam = invoke(m.Camera_get_main, nullptr);
    if (!cam) return;
    MonoObject* tf = invoke(m.Component_get_transform, cam);
    if (!tf) return;
    unity::Vector3 e = euler;
    void* args[1] = { &e };
    invoke(m.Transform_set_eulerAngles, tf, args);
}

// Convert Unity euler angle [0,360) to signed [-180,180)
static float euler_to_signed(float e) {
    return e > 180.0f ? e - 360.0f : e;
}

// ---------------------------------------------------------------------------
// Angle math (SSJJ coordinate space)
//
// SSJJ forward direction vector from (yaw, pitch):
//   forward.x = cos(pitch) * cos(yaw)
//   forward.y = cos(pitch) * sin(yaw)
//   forward.z = sin(pitch)
// ---------------------------------------------------------------------------
static void calc_angle(const unity::Vector3& from, const unity::Vector3& to,
                       float& yaw, float& pitch)
{
    float dx = to.x - from.x;
    float dy = to.y - from.y;
    float dz = to.z - from.z;

    float dist_h = sqrtf(dx * dx + dy * dy);
    yaw   = atan2f(dy, dx) * RAD2DEG;
    pitch = atan2f(dz, dist_h) * RAD2DEG;
}

static float angular_distance(float y1, float p1, float y2, float p2) {
    float dy = normalize_angle(y2 - y1);
    float dp = normalize_angle(p2 - p1);
    return sqrtf(dy * dy + dp * dp);
}

// ---------------------------------------------------------------------------
// Core aimbot logic
// ---------------------------------------------------------------------------
static void apply_aimbot() {
    MonoObject* pe = get_local_player_entity();
    if (!pe) { s_debug = "AIM: no entity"; return; }

    // Get InputComponent (source of truth for aim direction)
    MonoObject* input = get_input_component();

    // Read local player data from frame cache (shared with ESP, no duplicate read)
    const auto& local_data = frame_cache::get_local_player();
    if (!local_data.valid || local_data.is_dead) { s_debug = "AIM: dead"; return; }

    // Current view angles -- read from InputComponent if available, else OrientationComponent
    float cur_yaw, cur_pitch;
    if (input && s_fields.Input_Yaw) {
        read_input(input, cur_yaw, cur_pitch);
    } else {
        read_orientation(pe, cur_yaw, cur_pitch);
    }

    // Current recoil
    float punch_yaw, punch_pitch;
    read_punch(pe, punch_yaw, punch_pitch);

    // Enumerate all enemies from frame cache (shared with ESP)
    const auto& players = frame_cache::get_players();

    // Local eye position in SSJJ coords
    unity::Vector3 eye = local_data.position;
    eye.z += EYE_HEIGHT;

    // Local eye in Unity world coords (for bone visibility checks)
    unity::Vector3 eye_unity;
    eye_unity.x = -local_data.position.y;
    eye_unity.y =  local_data.position.z + EYE_HEIGHT; // SSJJ Z → Unity Y
    eye_unity.z =  local_data.position.x;

    // ---- Visible-bone-only aimbot ----
    // Rule: only aim at bones we can actually SEE.
    //   - Head visible  → always aim head (highest priority)
    //   - Head occluded  → aim at the visible bone closest to torso center
    //   - No visible bones on anyone → don't aim at all
    float best_dist = AIM_FOV;
    float best_yaw  = 0.0f;
    float best_pitch = 0.0f;
    bool  found = false;
    std::string target_name;
    std::string target_bone_name;

    // Bone names for debug display
    static const char* BONE_NAMES[] = {
        "head", "neck", "spine2", "spine1", "spine", "pelvis",
        "l_clav", "r_clav", "l_arm", "r_arm", "l_fore", "r_fore",
        "l_hand", "r_hand", "l_thigh", "r_thigh", "l_calf", "r_calf",
        "l_foot", "r_foot"
    };

    // Torso proximity priority (lower = closer to torso center = better)
    // Used when head is NOT visible to pick the most central visible bone.
    static const int TORSO_PRIORITY[20] = {
        0,   // 0:  head        (special: always best if visible)
        2,   // 1:  neck
        1,   // 2:  spine2      (upper chest - very central)
        1,   // 3:  spine1
        1,   // 4:  spine
        1,   // 5:  pelvis
        3,   // 6:  l_clavicle  (shoulder)
        3,   // 7:  r_clavicle
        4,   // 8:  l_upper_arm
        4,   // 9:  r_upper_arm
        5,   // 10: l_forearm
        5,   // 11: r_forearm
        6,   // 12: l_hand
        6,   // 13: r_hand
        4,   // 14: l_thigh
        4,   // 15: r_thigh
        5,   // 16: l_calf
        5,   // 17: r_calf
        6,   // 18: l_foot
        6,   // 19: r_foot
    };

    for (const auto& p : players) {
        if (!p.valid || p.is_local || p.is_dead) continue;
        if (p.max_hp <= 0.0f) continue;

        // Skip teammates
        if (local_data.team_id >= 0 && p.team_id == local_data.team_id)
            continue;

        // Get per-bone visibility data from frame cache (shared with ESP)
        if (!p._raw_entity) continue;
        const auto& cached = frame_cache::get_bone_data(p._raw_entity, eye_unity);
        if (cached.count <= 0) continue; // no bone data -> skip
        const esp::BoneTarget* bone_targets = cached.bones;
        int bone_count = cached.count;

        // Find the best VISIBLE bone for this player:
        //   Head visible → pick head immediately
        //   Else → pick the visible bone with lowest TORSO_PRIORITY
        int   chosen_bone = -1;
        int   chosen_priority = 999;

        // Check head first (bone 0) - if visible, it always wins
        if (bone_targets[0].valid && bone_targets[0].visible) {
            chosen_bone = 0;
        } else {
            // Scan all bones for the visible one closest to torso
            for (int bi = 0; bi < 20 && bi < bone_count; bi++) {
                if (!bone_targets[bi].valid || !bone_targets[bi].visible) continue;
                if (TORSO_PRIORITY[bi] < chosen_priority) {
                    chosen_priority = TORSO_PRIORITY[bi];
                    chosen_bone = bi;
                }
            }
        }

        if (chosen_bone < 0) continue; // no visible bone on this enemy

        // Convert bone Unity pos → SSJJ
        // For head bone (index 0), raise aim point by +10 units in height
        unity::Vector3 bone_unity = bone_targets[chosen_bone].world_pos;
        if (chosen_bone == 0)
            bone_unity.y += 10.0f;

        unity::Vector3 bone_ssjj;
        bone_ssjj.x =  bone_unity.z;
        bone_ssjj.y = -bone_unity.x;
        bone_ssjj.z =  bone_unity.y;

        float ty, tp;
        calc_angle(eye, bone_ssjj, ty, tp);
        float dist = angular_distance(cur_yaw, cur_pitch, ty, tp);

        // Compare: closest to crosshair wins
        if (dist < best_dist) {
            best_dist   = dist;
            best_yaw    = ty;
            best_pitch  = tp;
            found       = true;
            target_name = p.name;
            target_bone_name = (chosen_bone >= 0 && chosen_bone < 20)
                ? BONE_NAMES[chosen_bone] : "?";
        }
    }

    // In normal mode or not-yet-tracking silent mode, return early if no target.
    // In silent+tracking mode, we must keep running to maintain delta tracking
    // and camera override — otherwise the camera snaps when a target dies.
    if (!found && !(s_silent && s_tracking)) {
        s_debug = "AIM: no visible target";
        return;
    }

    // ---- Recoil compensation (single-point convergence) ----
    //
    // The game computes bullet direction as:
    //   shotDir = ViewAngle + 2 * PunchAngle
    //
    // To make shotDir point at the target:
    //   ViewAngle = targetAngle - 2 * PunchAngle
    //
    // This ensures every bullet hits the same point regardless of recoil.
    float aim_yaw   = found ? normalize_angle(best_yaw   - 2.0f * punch_yaw)   : 0.0f;
    float aim_pitch = found ? normalize_angle(best_pitch  - 2.0f * punch_pitch) : 0.0f;

    if (s_silent && input) {
        // ---- Silent mode ----
        // Write target angles to InputComponent (affects next UserCmd → server),
        // but override Camera.main.transform.eulerAngles so the player sees their
        // real mouse direction.
        //
        // Mouse delta tracking: use InputComponent.Yaw which contains
        //   (what we wrote last frame) + mouse_delta_this_frame
        // This is recoil-free — PunchOrientation only affects the camera/shot
        // direction, NOT InputComponent.Yaw.  So the delta is pure mouse input.

        if (!s_tracking) {
            // First frame: initialize real direction from current InputComponent
            // (aimbot hasn't written anything yet, so Yaw = real mouse direction).
            s_real_yaw   = cur_yaw;
            s_real_pitch = cur_pitch;
            s_prev_aim_yaw   = cur_yaw;
            s_prev_aim_pitch = cur_pitch;

            // Calibrate camera euler mapping for the override
            unity::Vector3 cam_euler = {0, 0, 0};
            if (get_camera_euler(cam_euler)) {
                s_euler_offset_y = euler_to_signed(cam_euler.y) + cur_yaw;
                s_euler_offset_x = euler_to_signed(cam_euler.x) + cur_pitch;
                s_cam_calibrated = true;
            }

            s_tracking = true;
        } else {
            // Extract mouse delta from InputComponent.Yaw (set by game's Update):
            //   cur_yaw = s_prev_aim_yaw + mouse_delta
            // No recoil contamination — InputComponent.Yaw is pure aim input.
            float delta_yaw   = normalize_angle(cur_yaw   - s_prev_aim_yaw);
            float delta_pitch = normalize_angle(cur_pitch  - s_prev_aim_pitch);
            s_real_yaw   = normalize_angle(s_real_yaw   + delta_yaw);
            s_real_pitch = normalize_angle(s_real_pitch  + delta_pitch);
            if (s_real_pitch >  89.0f) s_real_pitch =  89.0f;
            if (s_real_pitch < -89.0f) s_real_pitch = -89.0f;
        }

        // 1. Write angles to InputComponent
        //    Always write target angles when a target exists — bullets always
        //    hit the closest-to-crosshair enemy. Real-time target switching
        //    every frame (no locking).
        if (found) {
            write_input(input, aim_yaw, aim_pitch);
            s_prev_aim_yaw   = aim_yaw;
            s_prev_aim_pitch = aim_pitch;
        } else {
            // No target: write real angles → natural behavior
            write_input(input, s_real_yaw, s_real_pitch);
            s_prev_aim_yaw   = s_real_yaw;
            s_prev_aim_pitch = s_real_pitch;
        }

        // 2. Auto-fire: simulate mouse click when target found
        if (s_auto_fire) {
            set_auto_fire_state(found);
        }

        // 3. Write real to OrientationComponent (player model / first-person weapon)
        write_orientation(pe, s_real_yaw, s_real_pitch);

        // 4. Override camera Transform to show the real mouse direction.
        //    This runs in LateUpdate, right before rendering — final word.
        if (s_cam_calibrated) {
            unity::Vector3 cam_euler = {0, 0, 0};
            bool cam_ok = get_camera_euler(cam_euler);
            unity::Vector3 desired;
            desired.x = -s_real_pitch + s_euler_offset_x;
            desired.y = -s_real_yaw   + s_euler_offset_y;
            desired.z = cam_ok ? cam_euler.z : 0.0f;
            set_camera_euler(desired);
        }
    } else {
        // ---- Normal mode (existing behavior) ----
        if (!found) { s_debug = "AIM: no visible target"; return; }
        // Write to InputComponent (upstream source) so the game naturally
        // propagates to UserCmd -> OrientationComponent -> Camera.
        if (input && s_fields.Input_Yaw) {
            write_input(input, aim_yaw, aim_pitch);
        }
        // Also write to OrientationComponent as fallback / immediate effect
        write_orientation(pe, aim_yaw, aim_pitch);
        s_tracking = false;
    }

    if (found) {
        char buf[256];
        snprintf(buf, sizeof(buf), "AIM%s%s: -> %s [%s] (%.1f deg)",
            s_silent ? " [S]" : "",
            (s_silent && s_auto_fire) ? " FIRE" : "",
            target_name.c_str(), target_bone_name.c_str(), best_dist);
        s_debug = buf;
    } else {
        s_debug = s_silent ? "AIM [S]: no target" : "AIM: no visible target";
    }
}

// ---------------------------------------------------------------------------
// Per-frame update
// ---------------------------------------------------------------------------
void update() {
    if (!s_enabled) {
        // If we were tracking, hand back control cleanly
        if (s_tracking) {
            MonoObject* input = get_input_component();
            if (input) write_input(input, s_real_yaw, s_real_pitch);
            MonoObject* pe = get_local_player_entity();
            if (pe) write_orientation(pe, s_real_yaw, s_real_pitch);
            s_tracking = false;
            s_cam_calibrated = false;
        }
        set_auto_fire_state(false);  // release mouse if forced
        s_debug = "";
        return;
    }

    if (s_silent) {
        // Silent mode: always active, no hotkey required.
        // Tracking runs continuously; target angles only written on left-click.
        apply_aimbot();
    } else {
        // Normal mode: activate aimbot while Mouse3 is held
        bool active = gui::get_key(KEYCODE_MOUSE3);
        if (!active) {
            // Key released — hand back control to real mouse direction
            if (s_tracking) {
                MonoObject* input = get_input_component();
                if (input) write_input(input, s_real_yaw, s_real_pitch);
                MonoObject* pe = get_local_player_entity();
                if (pe) write_orientation(pe, s_real_yaw, s_real_pitch);
                s_tracking = false;
                s_cam_calibrated = false;
            }
            s_debug = "AIM: ready [Mouse3]";
            return;
        }
        apply_aimbot();
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void set_enabled(bool e) { s_enabled = e; }
bool is_enabled()        { return s_enabled; }
void set_silent(bool s)  { s_silent = s; }
bool is_silent()         { return s_silent; }
void set_auto_fire(bool a) { s_auto_fire = a; }
bool is_auto_fire()      { return s_auto_fire; }
const char* get_debug_info() { return s_debug.c_str(); }

void shutdown() {
    s_classes = {};
    s_methods = {};
    s_fields  = {};
    s_enabled = false;
    s_silent  = true;
    s_auto_fire = true;
    s_tracking = false;
    s_forced_fire = false;
    s_cam_calibrated = false;
}

} // namespace aimbot
