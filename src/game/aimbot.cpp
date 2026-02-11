#include "aimbot.h"
#include "player_info.h"
#include "esp.h"
#include "visibility.h"
#include "../mono/mono_api.h"
#include "../mono/mono_types.h"
#include "../unity/unity_classes.h"
#include "../unity/unity_types.h"
#include "../gui/gui.h"

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

// Unity KeyCode values for mouse side buttons
static constexpr int KEYCODE_MOUSE3 = 326;
static constexpr int KEYCODE_MOUSE4 = 327;

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static bool        s_enabled = true;
static std::string s_debug;

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
static MonoObject* invoke(MonoMethod* method, MonoObject* obj) {
    if (!method) return nullptr;
    return mono::runtime_invoke(method, obj, nullptr, nullptr);
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

    // Read local player data for position and team
    player_info::PlayerData local_data = player_info::read_local_player();
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

    // Enumerate all enemies
    auto players = player_info::read_all_players();

    // Local eye position in SSJJ coords
    unity::Vector3 eye = local_data.position;
    eye.z += EYE_HEIGHT;

    // Local eye in Unity world coords (for bone visibility checks)
    unity::Vector3 eye_unity;
    eye_unity.x = -local_data.position.y;
    eye_unity.y =  local_data.position.z + EYE_HEIGHT; // SSJJ Z → Unity Y
    eye_unity.z =  local_data.position.x;

    // Find best visible target with bone priority:
    // 1. Head (if visible) - highest priority
    // 2. Other visible bones - pick closest to crosshair
    // 3. Head (even if occluded) - fallback if no visible bones
    float best_dist = AIM_FOV;
    float best_yaw  = 0.0f;
    float best_pitch = 0.0f;
    bool  found = false;
    bool  used_bone = false;
    bool  target_visible = false;
    std::string target_name;
    std::string target_bone_name;

    // Bone priority for aiming (head first, then upper body, then limbs)
    static const int BONE_PRIORITY[] = {
        0,   // BONE_HEAD (highest)
        1,   // BONE_NECK
        2,   // BONE_SPINE2
        3,   // BONE_SPINE1
        4,   // BONE_SPINE
        5,   // BONE_PELVIS
        8,   // BONE_L_UPPER_ARM
        9,   // BONE_R_UPPER_ARM
        14,  // BONE_L_THIGH
        15,  // BONE_R_THIGH
    };
    static const char* BONE_NAMES[] = {
        "head", "neck", "spine2", "spine1", "spine", "pelvis",
        "l_clav", "r_clav", "l_arm", "r_arm", "l_fore", "r_fore",
        "l_hand", "r_hand", "l_thigh", "r_thigh", "l_calf", "r_calf",
        "l_foot", "r_foot"
    };

    for (const auto& p : players) {
        if (!p.valid || p.is_local || p.is_dead) continue;
        if (p.max_hp <= 0.0f) continue;

        // Skip teammates (same in-game team index)
        if (local_data.team_id >= 0 && p.team_id == local_data.team_id)
            continue;

        // Try to get per-bone visibility data
        esp::BoneTarget bone_targets[20] = {};
        int bone_count = 0;
        if (p._raw_entity) {
            bone_count = esp::get_bone_targets(p._raw_entity, eye_unity,
                                                bone_targets, 20);
        }

        // Strategy: find the best visible bone for this player
        // Priority: visible head > visible body bone > any head (no vis check)
        float player_best_dist = AIM_FOV;
        float player_best_yaw = 0, player_best_pitch = 0;
        bool  player_found = false;
        bool  player_bone_ok = false;
        bool  player_vis = false;
        int   player_best_bone = -1;

        if (bone_count > 0) {
            // First pass: look for visible bones (prioritize head)
            for (int pri_idx = 0; pri_idx < 10; pri_idx++) {
                int bi = BONE_PRIORITY[pri_idx];
                if (bi >= 20 || !bone_targets[bi].valid) continue;
                if (!bone_targets[bi].visible) continue;

                // Convert bone Unity pos → SSJJ
                unity::Vector3 bone_ssjj;
                bone_ssjj.x =  bone_targets[bi].world_pos.z;
                bone_ssjj.y = -bone_targets[bi].world_pos.x;
                bone_ssjj.z =  bone_targets[bi].world_pos.y;

                float ty, tp;
                calc_angle(eye, bone_ssjj, ty, tp);
                float dist = angular_distance(cur_yaw, cur_pitch, ty, tp);

                // Head gets a bonus (accept even if not closest)
                if (bi == 0) { // BONE_HEAD
                    player_best_dist = dist;
                    player_best_yaw = ty;
                    player_best_pitch = tp;
                    player_found = true;
                    player_bone_ok = true;
                    player_vis = true;
                    player_best_bone = bi;
                    break; // Head visible = best possible target
                }

                if (dist < player_best_dist) {
                    player_best_dist = dist;
                    player_best_yaw = ty;
                    player_best_pitch = tp;
                    player_found = true;
                    player_bone_ok = true;
                    player_vis = true;
                    player_best_bone = bi;
                }
            }

            // Second pass: if no visible bone found, use head anyway (fallback)
            if (!player_found && bone_targets[0].valid) {
                unity::Vector3 head_ssjj;
                head_ssjj.x =  bone_targets[0].world_pos.z;
                head_ssjj.y = -bone_targets[0].world_pos.x;
                head_ssjj.z =  bone_targets[0].world_pos.y;

                float ty, tp;
                calc_angle(eye, head_ssjj, ty, tp);
                player_best_dist = angular_distance(cur_yaw, cur_pitch, ty, tp);
                player_best_yaw = ty;
                player_best_pitch = tp;
                player_found = true;
                player_bone_ok = true;
                player_vis = false;
                player_best_bone = 0;
            }
        }

        // Fallback: no bone data at all, use estimated head position
        if (!player_found) {
            unity::Vector3 head = p.position;
            head.z += HEAD_HEIGHT;

            // Try to get head bone from esp
            unity::Vector3 head_unity_pos;
            if (p._raw_entity && esp::get_head_bone_world_pos(p._raw_entity, head_unity_pos)) {
                head.x = head_unity_pos.z;
                head.y = -head_unity_pos.x;
                head.z = head_unity_pos.y;
                player_bone_ok = true;
            }

            float ty, tp;
            calc_angle(eye, head, ty, tp);
            player_best_dist = angular_distance(cur_yaw, cur_pitch, ty, tp);
            player_best_yaw = ty;
            player_best_pitch = tp;
            player_found = true;
            player_best_bone = 0;
        }

        if (!player_found) continue;

        // Compare against global best: prefer visible targets over occluded
        bool better = false;
        if (player_vis && !target_visible) {
            better = true; // visible target always beats occluded
        } else if (player_vis == target_visible) {
            better = (player_best_dist < best_dist); // same visibility: closest wins
        }

        if (better && player_best_dist < AIM_FOV) {
            best_dist   = player_best_dist;
            best_yaw    = player_best_yaw;
            best_pitch  = player_best_pitch;
            found       = true;
            used_bone   = player_bone_ok;
            target_visible = player_vis;
            target_name = p.name;
            target_bone_name = (player_best_bone >= 0 && player_best_bone < 20)
                ? BONE_NAMES[player_best_bone] : "?";
        }
    }

    if (!found) { s_debug = "AIM: no target"; return; }

    // ---- Recoil compensation (single-point convergence) ----
    //
    // The game computes bullet direction as:
    //   shotDir = ViewAngle + 2 * PunchAngle
    //
    // To make shotDir point at the target:
    //   ViewAngle = targetAngle - 2 * PunchAngle
    //
    // This ensures every bullet hits the same point regardless of recoil.
    float aim_yaw   = normalize_angle(best_yaw   - 2.0f * punch_yaw);
    float aim_pitch = normalize_angle(best_pitch  - 2.0f * punch_pitch);

    // Write to InputComponent (upstream source) so the game naturally
    // propagates to UserCmd -> OrientationComponent -> Camera.
    if (input && s_fields.Input_Yaw) {
        write_input(input, aim_yaw, aim_pitch);
    }
    // Also write to OrientationComponent as fallback / immediate effect
    write_orientation(pe, aim_yaw, aim_pitch);

    char buf[256];
    snprintf(buf, sizeof(buf), "AIM: -> %s [%s] (%.1f deg) vis=%s punch=(%.1f,%.1f) src=%s",
        target_name.c_str(), target_bone_name.c_str(), best_dist,
        target_visible ? "Y" : "N",
        punch_yaw, punch_pitch,
        (input && s_fields.Input_Yaw) ? "input" : "orient");
    s_debug = buf;
}

// ---------------------------------------------------------------------------
// Per-frame update
// ---------------------------------------------------------------------------
void update() {
    if (!s_enabled) { s_debug = ""; return; }

    // Activate aimbot while mouse side button is held
    // Mouse3 (KeyCode 326) = thumb back button
    bool active = gui::get_key(KEYCODE_MOUSE3);
    if (!active) {
        s_debug = "AIM: ready [Mouse3]";
        return;
    }

    apply_aimbot();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void set_enabled(bool e) { s_enabled = e; }
bool is_enabled()        { return s_enabled; }
const char* get_debug_info() { return s_debug.c_str(); }

void shutdown() {
    s_classes = {};
    s_methods = {};
    s_fields  = {};
    s_enabled = false;
}

} // namespace aimbot
