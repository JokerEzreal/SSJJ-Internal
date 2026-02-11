#include "esp.h"
#include "player_info.h"
#include "visibility.h"
#include "../gui/gui.h"
#include "../mono/mono_api.h"
#include "../mono/mono_types.h"
#include "../unity/unity_classes.h"
#include "../unity/unity_types.h"
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <cctype>
#include <string>
#include <vector>
#include <Windows.h>

namespace esp {

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static bool       s_enabled       = true;
static bool       s_skeleton_enabled = true;
static MonoObject* s_white_tex    = nullptr;
static uint32_t    s_tex_gc_handle = 0;

// ---------------------------------------------------------------------------
// Skeleton ESP cached handles
// ---------------------------------------------------------------------------
static struct {
    MonoClass*  ThirdPersonUnityObjectsComponent = nullptr;
    MonoMethod* PE_get_thirdPersonUnityObjects    = nullptr;
    MonoMethod* PE_get_hasThirdPersonUnityObjects = nullptr;
    MonoClassField* TPU_ThirdTran = nullptr;
    // ThirdTran fields (resolved dynamically on first use)
    MonoClassField* TT_HeadTransform  = nullptr;
    MonoClassField* TT_BodyTransform  = nullptr;
    MonoClassField* TT_RootContainer  = nullptr;
    bool fields_resolved = false;
} s_skel;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static const char* fmt(const char* format, ...) {
    static char buf[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    return buf;
}

// Exception-safe mono invoke.  Catches managed exceptions locally instead of
// letting them propagate through the C++ stack (which would abort the entire
// OnGUI callback and kill all drawing for the frame).
static MonoObject* invoke_safe(MonoMethod* method, MonoObject* obj,
                               void** params = nullptr)
{
    if (!method) return nullptr;
    MonoObject* exc = nullptr;
    MonoObject* ret = mono::runtime_invoke(method, obj, params, &exc);
    return exc ? nullptr : ret;
}

// ---------------------------------------------------------------------------
// Drawing primitives (require s_white_tex)
// ---------------------------------------------------------------------------
static void draw_filled_rect(float x, float y, float w, float h, const unity::Color& col) {
    if (!s_white_tex) return;
    gui::set_color(col);
    gui::draw_texture(unity::Rect(x, y, w, h), s_white_tex);
}

static void draw_box_outline(float x, float y, float w, float h, float t, const unity::Color& col) {
    draw_filled_rect(x, y, w, t, col);             // top
    draw_filled_rect(x, y + h - t, w, t, col);     // bottom
    draw_filled_rect(x, y, t, h, col);              // left
    draw_filled_rect(x + w - t, y, t, h, col);      // right
}

static void draw_text_shadow(float x, float y, float w, float h, const char* text) {
    gui::set_color(unity::Color(0, 0, 0, 1));
    gui::label(unity::Rect(x + 1, y + 1, w, h), text);
    gui::set_color(unity::Color(1, 1, 1, 1));
    gui::label(unity::Rect(x, y, w, h), text);
}

// ---------------------------------------------------------------------------
// World-to-screen projection
// ---------------------------------------------------------------------------
struct ScreenPos {
    float x, y;
    bool visible;
};

// Per-frame cached camera to avoid calling Camera.get_main hundreds of times
static MonoObject* s_frame_camera = nullptr;
static bool        s_frame_camera_valid = false;

// Stall diagnostics -- track last successful draw tick
static DWORD s_last_draw_tick = 0;
static DWORD s_max_gap_ms     = 0;  // longest gap between successful draws

static void refresh_frame_camera() {
    s_frame_camera = nullptr;
    s_frame_camera_valid = false;

    MonoMethod* get_main = unity::methods().Camera_get_main;
    if (!get_main) return;

    s_frame_camera = invoke_safe(get_main, nullptr);
    s_frame_camera_valid = true;  // mark as "checked this frame" even if null
}

static ScreenPos world_to_screen(const unity::Vector3& world_pos, float screen_w, float screen_h) {
    ScreenPos r = { 0, 0, false };

    MonoMethod* w2s = unity::methods().Camera_WorldToScreenPoint;
    if (!w2s || !s_frame_camera) return r;

    unity::Vector3 pos = world_pos;
    void* args[1] = { &pos };
    MonoObject* result = invoke_safe(w2s, s_frame_camera, args);
    if (!result) return r;

    auto* sp = static_cast<unity::Vector3*>(mono::object_unbox(result));
    if (!sp) return r;

    // Behind camera
    if (sp->z <= 0.0f) return r;

    // Unity screen coords: Y=0 at bottom.  GUI coords: Y=0 at top.
    r.x = sp->x;
    r.y = screen_h - sp->y;
    r.visible = (r.x >= -200.0f && r.x <= screen_w + 200.0f &&
                 r.y >= -200.0f && r.y <= screen_h + 200.0f);
    return r;
}

// ---------------------------------------------------------------------------
// Initialize -- just mark as ready; texture created lazily in draw()
// (Unity objects MUST be created on the main thread / OnGUI context,
//  not from our background init_thread)
// ---------------------------------------------------------------------------
bool initialize() {
    return true;
}

// ---------------------------------------------------------------------------
// Lazy texture creation -- called from draw() on the main thread.
// Also refreshes the pointer if the object was destroyed (scene change etc).
// ---------------------------------------------------------------------------
static void create_texture() {
    // Free old handle if any
    if (s_tex_gc_handle) {
        mono::gchandle_free(s_tex_gc_handle);
        s_tex_gc_handle = 0;
    }
    s_white_tex = nullptr;

    MonoClass*  tex_cls  = unity::classes().Texture2D;
    MonoMethod* tex_ctor = unity::methods().Texture2D_ctor_2;
    MonoMethod* tex_sp   = unity::methods().Texture2D_SetPixel;
    MonoMethod* tex_ap   = unity::methods().Texture2D_Apply_0;

    if (!tex_cls || !tex_ctor || !tex_sp || !tex_ap) return;

    MonoDomain* domain = mono::domain_get();
    if (!domain) return;

    s_white_tex = mono::object_new(domain, tex_cls);
    if (!s_white_tex) return;

    int tw = 1, th = 1;
    void* ctor_args[2] = { &tw, &th };
    invoke_safe(tex_ctor, s_white_tex, ctor_args);

    int px = 0, py = 0;
    unity::Color white = unity::Color::white();
    void* sp_args[3] = { &px, &py, &white };
    invoke_safe(tex_sp, s_white_tex, sp_args);
    invoke_safe(tex_ap, s_white_tex);

    // Protect texture from Unity's resource cleanup:
    // 1. Set hideFlags = HideAndDontSave (61) to survive scene loads
    MonoMethod* set_hide = unity::methods().Object_set_hideFlags;
    if (set_hide) {
        int flags = 61; // HideFlags.HideAndDontSave
        void* hide_args[1] = { &flags };
        invoke_safe(set_hide, s_white_tex, hide_args);
    }
    // 2. DontDestroyOnLoad as extra safeguard
    MonoMethod* ddol = unity::methods().Object_DontDestroyOnLoad;
    if (ddol) {
        void* ddol_args[1] = { s_white_tex };
        invoke_safe(ddol, nullptr, ddol_args);
    }

    // Pin the texture so the GC does not collect it
    s_tex_gc_handle = mono::gchandle_new(s_white_tex, 1);
}

static void ensure_texture() {
    // Refresh pointer from GC handle each call to detect if Unity destroyed it
    if (s_tex_gc_handle && mono::gchandle_get_target) {
        MonoObject* current = mono::gchandle_get_target(s_tex_gc_handle);
        if (!current) {
            // Object was collected/destroyed, recreate
            s_white_tex = nullptr;
            mono::gchandle_free(s_tex_gc_handle);
            s_tex_gc_handle = 0;
        } else {
            s_white_tex = current;
        }
    }

    // Create if we don't have a valid texture
    if (!s_white_tex) {
        create_texture();
    }
}

// ---------------------------------------------------------------------------
// Draw a line between two screen points using rotated rect
// ---------------------------------------------------------------------------
static void draw_line(float x1, float y1, float x2, float y2,
                      float thickness, const unity::Color& col)
{
    if (!s_white_tex) return;

    MonoMethod* rotate = unity::methods().GUIUtility_RotateAroundPivot;
    if (!rotate) return;

    float dx = x2 - x1;
    float dy = y2 - y1;
    float length = sqrtf(dx * dx + dy * dy);
    if (length < 0.5f) return;

    float angle = atan2f(dy, dx) * 57.2957795f;
    unity::Vector2 pivot = { x1, y1 };

    // Rotate GUI matrix
    void* rot_args[2] = { &angle, &pivot };
    invoke_safe(rotate, nullptr, rot_args);

    // Draw thin rect
    gui::set_color(col);
    gui::draw_texture(unity::Rect(x1, y1 - thickness * 0.5f, length, thickness),
                      s_white_tex);

    // Restore GUI matrix (MUST always run, even if draw failed)
    float neg = -angle;
    void* restore_args[2] = { &neg, &pivot };
    invoke_safe(rotate, nullptr, restore_args);
}

// ---------------------------------------------------------------------------
// Skeleton ESP helpers
// ---------------------------------------------------------------------------
static void init_skeleton_cache() {
    if (s_skel.PE_get_thirdPersonUnityObjects) return; // already done

    MonoImage* ent_img = unity::images().entitas_lib;
    if (!ent_img) return;

    MonoClass* PlayerEntity = mono::class_from_name(ent_img, "", "PlayerEntity");
    if (!PlayerEntity) return;

    s_skel.PE_get_thirdPersonUnityObjects =
        mono::class_get_method_from_name(PlayerEntity, "get_thirdPersonUnityObjects", 0);
    s_skel.PE_get_hasThirdPersonUnityObjects =
        mono::class_get_method_from_name(PlayerEntity, "get_hasThirdPersonUnityObjects", 0);

    s_skel.ThirdPersonUnityObjectsComponent = mono::class_from_name(
        ent_img, "Assets.Sources.Components.Player.UnityObjects",
        "ThirdPersonUnityObjectsComponent");

    if (s_skel.ThirdPersonUnityObjectsComponent) {
        s_skel.TPU_ThirdTran = mono::class_get_field_from_name(
            s_skel.ThirdPersonUnityObjectsComponent, "ThirdTran");
    }
}

// Read Transform.position -> Unity world Vector3
static bool get_transform_position(MonoObject* transform, unity::Vector3& out) {
    if (!transform) return false;
    MonoMethod* get_pos = unity::methods().Transform_get_position;
    if (!get_pos) return false;

    MonoObject* result = invoke_safe(get_pos, transform);
    if (!result) return false;

    auto* v = static_cast<unity::Vector3*>(mono::object_unbox(result));
    if (!v) return false;
    out = *v;
    return true;
}

// Get child count of a Transform
static int get_child_count(MonoObject* transform) {
    if (!transform) return 0;
    MonoMethod* m = unity::methods().Transform_get_childCount;
    if (!m) return 0;
    MonoObject* result = invoke_safe(m, transform);
    if (!result) return 0;
    return *static_cast<int*>(mono::object_unbox(result));
}

// Get child Transform by index
static MonoObject* get_child(MonoObject* transform, int index) {
    if (!transform) return nullptr;
    MonoMethod* m = unity::methods().Transform_GetChild;
    if (!m) return nullptr;
    void* args[1] = { &index };
    return invoke_safe(m, transform, args);
}

// Get parent Transform
static MonoObject* get_parent(MonoObject* transform) {
    if (!transform) return nullptr;
    MonoMethod* m = unity::methods().Transform_get_parent;
    if (!m) return nullptr;
    return invoke_safe(m, transform);
}

// ---------------------------------------------------------------------------
// Skeleton ESP: Name-based bone classification
// ---------------------------------------------------------------------------

// Bone identifiers for humanoid skeleton
enum BoneId {
    BONE_HEAD = 0, BONE_NECK,
    BONE_SPINE2, BONE_SPINE1, BONE_SPINE,
    BONE_PELVIS,
    BONE_L_CLAVICLE, BONE_R_CLAVICLE,
    BONE_L_UPPER_ARM, BONE_R_UPPER_ARM,
    BONE_L_FOREARM, BONE_R_FOREARM,
    BONE_L_HAND, BONE_R_HAND,
    BONE_L_THIGH, BONE_R_THIGH,
    BONE_L_CALF, BONE_R_CALF,
    BONE_L_FOOT, BONE_R_FOOT,
    BONE_COUNT,
    BONE_UNKNOWN = BONE_COUNT
};

// Get transform name as std::string
static std::string get_transform_name(MonoObject* transform) {
    if (!transform) return "";
    MonoMethod* get_name = unity::methods().Object_get_name;
    if (!get_name) return "";
    MonoObject* name_obj = invoke_safe(get_name, transform);
    if (!name_obj) return "";
    char* utf8 = mono::string_to_utf8(reinterpret_cast<MonoString*>(name_obj));
    if (!utf8) return "";
    std::string result(utf8);
    if (mono::free_mem) mono::free_mem(utf8);
    return result;
}

// Case-insensitive substring check on pre-lowered string
static bool name_has(const std::string& lower, const char* sub) {
    return lower.find(sub) != std::string::npos;
}

// Classify a bone transform name into a BoneId
static BoneId classify_bone(const std::string& name) {
    // Convert to lowercase
    std::string lw;
    lw.reserve(name.size());
    for (char c : name) lw += (char)tolower((unsigned char)c);

    // Skip non-skeleton transforms (weapons, effects, helpers)
    if (name_has(lw, "weapon") || name_has(lw, "gun") || name_has(lw, "knife") ||
        name_has(lw, "effect") || name_has(lw, "particle") || name_has(lw, "mesh") ||
        name_has(lw, "render") || name_has(lw, "shadow") || name_has(lw, "muzzle") ||
        name_has(lw, "fire") || name_has(lw, "bullet") || name_has(lw, "trail") ||
        name_has(lw, "dummy") || name_has(lw, "nub") || name_has(lw, "prop") ||
        name_has(lw, "twist") || name_has(lw, "roll") || name_has(lw, "slot") ||
        name_has(lw, "attach") || name_has(lw, "point") || name_has(lw, "ik ") ||
        name_has(lw, "handle") || name_has(lw, "shell") || name_has(lw, "mag") ||
        name_has(lw, "scope") || name_has(lw, "barrel") || name_has(lw, "trigger") ||
        name_has(lw, "finger") || name_has(lw, "toe") || name_has(lw, "ponytail") ||
        name_has(lw, "footstep"))
        return BONE_UNKNOWN;

    // Detect left/right side
    bool is_left  = name_has(lw, " l ") || name_has(lw, "left") || name_has(lw, "_l_") || name_has(lw, " l_");
    bool is_right = name_has(lw, " r ") || name_has(lw, "right") || name_has(lw, "_r_") || name_has(lw, " r_");
    // Also check end-of-string: "... L" or "... R"
    if (!is_left && !is_right && lw.size() > 2) {
        if (lw.back() == 'l' && lw[lw.size() - 2] == ' ') is_left = true;
        if (lw.back() == 'r' && lw[lw.size() - 2] == ' ') is_right = true;
    }

    // Match bone type (more specific patterns first)
    if (name_has(lw, "head"))       return BONE_HEAD;
    if (name_has(lw, "neck"))       return BONE_NECK;
    if (name_has(lw, "spine2") || name_has(lw, "spine 2") || name_has(lw, "chest"))
        return BONE_SPINE2;
    if (name_has(lw, "spine1") || name_has(lw, "spine 1"))
        return BONE_SPINE1;
    if (name_has(lw, "spine"))      return BONE_SPINE;
    if (name_has(lw, "pelvis") || name_has(lw, "hips"))
        return BONE_PELVIS;

    if (name_has(lw, "clavicle") || name_has(lw, "shoulder"))
        return is_right ? BONE_R_CLAVICLE : BONE_L_CLAVICLE;
    if (name_has(lw, "upperarm") || name_has(lw, "upper arm") || name_has(lw, "upper_arm"))
        return is_right ? BONE_R_UPPER_ARM : BONE_L_UPPER_ARM;
    if (name_has(lw, "forearm") || name_has(lw, "lowerarm") || name_has(lw, "lower arm") || name_has(lw, "fore arm"))
        return is_right ? BONE_R_FOREARM : BONE_L_FOREARM;
    if (name_has(lw, "hand") || name_has(lw, "wrist"))
        return is_right ? BONE_R_HAND : BONE_L_HAND;
    if (name_has(lw, "thigh") || name_has(lw, "upperleg") || name_has(lw, "upleg"))
        return is_right ? BONE_R_THIGH : BONE_L_THIGH;
    if (name_has(lw, "calf") || name_has(lw, "shin") || name_has(lw, "lowerleg") || name_has(lw, "lowleg"))
        return is_right ? BONE_R_CALF : BONE_L_CALF;
    if (name_has(lw, "foot"))
        return is_right ? BONE_R_FOOT : BONE_L_FOOT;

    return BONE_UNKNOWN;
}

// Recursively collect bone transforms from the hierarchy by name
static void collect_bones(MonoObject* transform, MonoObject* bones[BONE_COUNT],
                          int depth, int max_depth, int& found)
{
    if (!transform || depth > max_depth || found >= BONE_COUNT) return;

    std::string name = get_transform_name(transform);
    if (!name.empty()) {
        BoneId id = classify_bone(name);
        if (id != BONE_UNKNOWN && !bones[id]) {
            bones[id] = transform;
            found++;
        }
    }

    int child_count = get_child_count(transform);
    for (int i = 0; i < child_count && i < 30; i++) {
        MonoObject* child = get_child(transform, i);
        if (child) collect_bones(child, bones, depth + 1, max_depth, found);
    }
}

// Bone name debug dump (populated for first drawn skeleton)
static std::string s_bone_debug;

// Debug: collect all transform names from hierarchy
static void dump_bone_names(MonoObject* transform, std::string& out,
                            int depth, int max_depth)
{
    if (!transform || depth > max_depth) return;
    std::string name = get_transform_name(transform);
    if (!name.empty()) {
        BoneId id = classify_bone(name);
        char buf[128];
        snprintf(buf, sizeof(buf), "%*s%s [%d]\n", depth * 2, "", name.c_str(), (int)id);
        out += buf;
    }
    int cc = get_child_count(transform);
    for (int i = 0; i < cc && i < 30; i++) {
        MonoObject* child = get_child(transform, i);
        if (child) dump_bone_names(child, out, depth + 1, max_depth);
    }
}

// Draw skeleton for a single player entity
// vis_col = color for visible bones, occ_col = color for occluded bones
// eye_unity = local player eye in Unity world coords (for visibility checks)
// vis_enabled = whether to do per-bone visibility checks
static bool draw_player_skeleton(MonoObject* player_entity,
                                 float screen_w, float screen_h,
                                 const unity::Color& vis_col,
                                 const unity::Color& occ_col,
                                 const unity::Vector3* eye_unity,
                                 bool vis_enabled,
                                 bool do_debug_dump = false)
{
    if (!player_entity) return false;
    if (!s_skel.PE_get_hasThirdPersonUnityObjects ||
        !s_skel.PE_get_thirdPersonUnityObjects) return false;

    // Check hasThirdPersonUnityObjects
    MonoObject* has_res = invoke_safe(
        s_skel.PE_get_hasThirdPersonUnityObjects, player_entity);
    if (!has_res || !*static_cast<bool*>(mono::object_unbox(has_res)))
        return false;

    // Get ThirdPersonUnityObjectsComponent
    MonoObject* tpu = invoke_safe(
        s_skel.PE_get_thirdPersonUnityObjects, player_entity);
    if (!tpu) return false;

    // Read ThirdTran field
    if (!s_skel.TPU_ThirdTran) return false;
    MonoObject* third_tran = nullptr;
    mono::field_get_value(tpu, s_skel.TPU_ThirdTran, &third_tran);
    if (!third_tran) return false;

    // Resolve ThirdTran fields dynamically on first use
    if (!s_skel.fields_resolved && mono::object_get_class) {
        MonoClass* tt_cls = mono::object_get_class(third_tran);
        if (tt_cls) {
            s_skel.TT_HeadTransform = mono::class_get_field_from_name(tt_cls, "HeadTransform");
            s_skel.TT_BodyTransform = mono::class_get_field_from_name(tt_cls, "BodyTransform");
            s_skel.TT_RootContainer = mono::class_get_field_from_name(tt_cls, "RootContainer");
            s_skel.fields_resolved = true;
        }
    }

    // Get RootContainer (GameObject) -> transform
    MonoObject* root_go = nullptr;
    if (s_skel.TT_RootContainer)
        mono::field_get_value(third_tran, s_skel.TT_RootContainer, &root_go);
    if (!root_go) return false;

    MonoMethod* get_tf = unity::methods().GameObject_get_transform;
    if (!get_tf) return false;
    MonoObject* root_transform = invoke_safe(get_tf, root_go);
    if (!root_transform) return false;

    // Debug dump: collect all bone names (only for first player, once)
    if (do_debug_dump && s_bone_debug.empty()) {
        dump_bone_names(root_transform, s_bone_debug, 0, 15);
    }

    // Collect bone transforms by name classification
    MonoObject* bones[BONE_COUNT] = {};
    int found = 0;
    collect_bones(root_transform, bones, 0, 15, found);

    if (found < 3) {
        // Fallback: draw head + body from ThirdTran fields
        MonoObject* head_tf = nullptr;
        MonoObject* body_tf = nullptr;
        if (s_skel.TT_HeadTransform)
            mono::field_get_value(third_tran, s_skel.TT_HeadTransform, &head_tf);
        if (s_skel.TT_BodyTransform)
            mono::field_get_value(third_tran, s_skel.TT_BodyTransform, &body_tf);

        unity::Vector3 head_pos, body_pos;
        bool has_head = get_transform_position(head_tf, head_pos);
        bool has_body = get_transform_position(body_tf, body_pos);
        if (has_head && has_body) {
            ScreenPos sp_head = world_to_screen(head_pos, screen_w, screen_h);
            ScreenPos sp_body = world_to_screen(body_pos, screen_w, screen_h);
            if (sp_head.visible && sp_body.visible) {
                bool head_vis = !vis_enabled || !eye_unity ||
                    visibility::is_visible_unity(*eye_unity, head_pos);
                unity::Color c = head_vis ? vis_col : occ_col;
                draw_line(sp_body.x, sp_body.y, sp_head.x, sp_head.y, 2.0f, c);
                draw_filled_rect(sp_head.x - 3, sp_head.y - 3, 6, 6, c);
                draw_filled_rect(sp_body.x - 3, sp_body.y - 3, 6, 6, c);
            }
            return true;
        }
        return false;
    }

    // Get world and screen positions for all found bones, check visibility
    ScreenPos scr[BONE_COUNT] = {};
    unity::Vector3 world_pos[BONE_COUNT] = {};
    bool bone_visible[BONE_COUNT] = {};  // true = not occluded by walls
    for (int i = 0; i < BONE_COUNT; i++) {
        if (!bones[i]) continue;
        if (get_transform_position(bones[i], world_pos[i])) {
            scr[i] = world_to_screen(world_pos[i], screen_w, screen_h);
            // Visibility check (only for on-screen bones to save perf)
            if (scr[i].visible && vis_enabled && eye_unity) {
                bone_visible[i] = visibility::is_visible_unity(*eye_unity, world_pos[i]);
            } else {
                bone_visible[i] = true; // no check = assume visible
            }
        }
    }

    // Draw chain: connect consecutive found bones, color by visibility
    auto draw_chain = [&](const BoneId* chain, int len) {
        int prev = -1;
        for (int i = 0; i < len; i++) {
            int idx = (int)chain[i];
            if (!bones[idx] || !scr[idx].visible) continue;
            if (prev >= 0 && scr[prev].visible) {
                // Use visible color if either endpoint is visible
                bool seg_vis = bone_visible[prev] || bone_visible[idx];
                unity::Color c = seg_vis ? vis_col : occ_col;
                draw_line(scr[prev].x, scr[prev].y, scr[idx].x, scr[idx].y, 2.0f, c);
            }
            prev = idx;
        }
    };

    // Spine chain: head → neck → chest → mid → lower spine → pelvis
    static const BoneId SPINE_CHAIN[] = {
        BONE_HEAD, BONE_NECK, BONE_SPINE2, BONE_SPINE1, BONE_SPINE, BONE_PELVIS
    };
    draw_chain(SPINE_CHAIN, 6);

    // Arm attachment point: Neck (clavicles are children of Neck in this skeleton)
    // Fallback to Spine2 → Spine1 if Neck not found
    BoneId arm_root = BONE_UNKNOWN;
    if (bones[BONE_NECK])        arm_root = BONE_NECK;
    else if (bones[BONE_SPINE2]) arm_root = BONE_SPINE2;
    else if (bones[BONE_SPINE1]) arm_root = BONE_SPINE1;

    // Arm chains: neck → clavicle → upper arm → forearm → hand
    if (arm_root != BONE_UNKNOWN) {
        BoneId l_arm[] = { arm_root, BONE_L_CLAVICLE, BONE_L_UPPER_ARM, BONE_L_FOREARM, BONE_L_HAND };
        draw_chain(l_arm, 5);
        BoneId r_arm[] = { arm_root, BONE_R_CLAVICLE, BONE_R_UPPER_ARM, BONE_R_FOREARM, BONE_R_HAND };
        draw_chain(r_arm, 5);
    }

    // Leg chains
    static const BoneId L_LEG[] = { BONE_PELVIS, BONE_L_THIGH, BONE_L_CALF, BONE_L_FOOT };
    static const BoneId R_LEG[] = { BONE_PELVIS, BONE_R_THIGH, BONE_R_CALF, BONE_R_FOOT };
    draw_chain(L_LEG, 4);
    draw_chain(R_LEG, 4);

    // Draw joint dots (colored by visibility)
    for (int i = 0; i < BONE_COUNT; i++) {
        if (bones[i] && scr[i].visible) {
            unity::Color c = bone_visible[i] ? vis_col : occ_col;
            draw_filled_rect(scr[i].x - 2, scr[i].y - 2, 4, 4, c);
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Draw ESP for all players
// ---------------------------------------------------------------------------
// Debug info for ESP
static std::string s_esp_debug;
const char* get_esp_debug() { return s_esp_debug.c_str(); }
const char* get_bone_dump() { return s_bone_debug.c_str(); }
void request_bone_dump() { s_bone_debug.clear(); }

// ---------------------------------------------------------------------------
// Get head bone world position for a player entity (usable from any context)
// ---------------------------------------------------------------------------
bool get_head_bone_world_pos(MonoObject* player_entity, unity::Vector3& out) {
    init_skeleton_cache();

    if (!player_entity) return false;
    if (!s_skel.PE_get_hasThirdPersonUnityObjects ||
        !s_skel.PE_get_thirdPersonUnityObjects) return false;

    // Check hasThirdPersonUnityObjects
    MonoObject* has_res = invoke_safe(
        s_skel.PE_get_hasThirdPersonUnityObjects, player_entity);
    if (!has_res || !*static_cast<bool*>(mono::object_unbox(has_res)))
        return false;

    // Get ThirdPersonUnityObjectsComponent
    MonoObject* tpu = invoke_safe(
        s_skel.PE_get_thirdPersonUnityObjects, player_entity);
    if (!tpu || !s_skel.TPU_ThirdTran) return false;

    // Get ThirdTran
    MonoObject* third_tran = nullptr;
    mono::field_get_value(tpu, s_skel.TPU_ThirdTran, &third_tran);
    if (!third_tran) return false;

    // Resolve ThirdTran fields if not done yet
    if (!s_skel.fields_resolved && mono::object_get_class) {
        MonoClass* tt_cls = mono::object_get_class(third_tran);
        if (tt_cls) {
            s_skel.TT_HeadTransform = mono::class_get_field_from_name(tt_cls, "HeadTransform");
            s_skel.TT_BodyTransform = mono::class_get_field_from_name(tt_cls, "BodyTransform");
            s_skel.TT_RootContainer = mono::class_get_field_from_name(tt_cls, "RootContainer");
            s_skel.fields_resolved = true;
        }
    }

    // Read HeadTransform (direct bone reference, no hierarchy traversal)
    MonoObject* head_tf = nullptr;
    if (s_skel.TT_HeadTransform)
        mono::field_get_value(third_tran, s_skel.TT_HeadTransform, &head_tf);
    if (!head_tf) return false;

    return get_transform_position(head_tf, out);
}

// ---------------------------------------------------------------------------
// Get bone positions + visibility for aimbot (reuses skeleton cache)
// ---------------------------------------------------------------------------
int get_bone_targets(MonoObject* player_entity,
                     const unity::Vector3& eye_unity,
                     BoneTarget* out, int max_bones)
{
    init_skeleton_cache();

    if (!player_entity || !out || max_bones <= 0) return 0;
    if (!s_skel.PE_get_hasThirdPersonUnityObjects ||
        !s_skel.PE_get_thirdPersonUnityObjects) return 0;

    // Check hasThirdPersonUnityObjects
    MonoObject* has_res = invoke_safe(
        s_skel.PE_get_hasThirdPersonUnityObjects, player_entity);
    if (!has_res || !*static_cast<bool*>(mono::object_unbox(has_res)))
        return 0;

    MonoObject* tpu = invoke_safe(
        s_skel.PE_get_thirdPersonUnityObjects, player_entity);
    if (!tpu || !s_skel.TPU_ThirdTran) return 0;

    MonoObject* third_tran = nullptr;
    mono::field_get_value(tpu, s_skel.TPU_ThirdTran, &third_tran);
    if (!third_tran) return 0;

    // Resolve fields if needed
    if (!s_skel.fields_resolved && mono::object_get_class) {
        MonoClass* tt_cls = mono::object_get_class(third_tran);
        if (tt_cls) {
            s_skel.TT_HeadTransform = mono::class_get_field_from_name(tt_cls, "HeadTransform");
            s_skel.TT_BodyTransform = mono::class_get_field_from_name(tt_cls, "BodyTransform");
            s_skel.TT_RootContainer = mono::class_get_field_from_name(tt_cls, "RootContainer");
            s_skel.fields_resolved = true;
        }
    }

    MonoObject* root_go = nullptr;
    if (s_skel.TT_RootContainer)
        mono::field_get_value(third_tran, s_skel.TT_RootContainer, &root_go);
    if (!root_go) return 0;

    MonoMethod* get_tf = unity::methods().GameObject_get_transform;
    if (!get_tf) return 0;
    MonoObject* root_transform = invoke_safe(get_tf, root_go);
    if (!root_transform) return 0;

    // Collect bones
    MonoObject* bones[BONE_COUNT] = {};
    int found = 0;
    collect_bones(root_transform, bones, 0, 15, found);

    // Fill output with position + visibility
    int count = 0;
    int limit = (max_bones < BONE_COUNT) ? max_bones : BONE_COUNT;
    for (int i = 0; i < limit; i++) {
        out[i].bone_id = i;
        out[i].valid = false;
        out[i].visible = false;
        if (!bones[i]) continue;

        unity::Vector3 pos;
        if (!get_transform_position(bones[i], pos)) continue;

        out[i].world_pos = pos;
        out[i].valid = true;
        out[i].visible = visibility::is_visible_unity(eye_unity, pos);
        count++;
    }

    return count;
}

// Frame counter for stale detection in debug display
static uint32_t s_esp_frame = 0;

void draw() {
    if (!s_enabled) return;

    // Increment frame counter and mark debug as "in progress" so that if we
    // throw an exception, the stale string will be obviously different.
    s_esp_frame++;

    // Create texture on first draw (main thread context)
    ensure_texture();

    // Initialize skeleton cache on first draw
    init_skeleton_cache();

    // Cache Camera.main once per frame (avoids hundreds of Camera.get_main invokes)
    refresh_frame_camera();

    float screen_w = static_cast<float>(gui::screen_width());
    float screen_h = static_cast<float>(gui::screen_height());

    char dbg[1024];

    // If camera is not available (scene transition), skip rendering but don't abort
    if (!s_frame_camera) {
        s_esp_debug = "ESP: cam=NULL (scene loading?)";
        return;
    }

    // If texture is missing, try again but don't abort — text labels still work
    if (!s_white_tex) {
        s_esp_debug = "ESP: tex=NULL (recreating)";
        return;
    }

    // Save original GUI color so we can restore it when we are done
    unity::Color orig_color = gui::get_color();

    // ------ Fetch all player data from player_info module (single pass) ------
    std::vector<player_info::PlayerData> players = player_info::read_all_players();

    // Find local player's team_id from the already-fetched list (no extra Mono calls)
    int local_team = -1;
    for (const auto& p : players) {
        if (p.is_local && p.valid) { local_team = p.team_id; break; }
    }

    int w2s_ok = 0, w2s_fail = 0, drawn = 0;
    float first_sx = 0, first_sy = 0;

    for (const auto& p : players) {
        // Skip invalid, local, or dead players
        if (!p.valid || p.is_local || p.is_dead) continue;

        // Skip teammates (same team_id as local player)
        if (local_team >= 0 && p.team_id == local_team) continue;

        // --- SSJJ -> Unity coordinate conversion ---
        // Position is now properly decrypted (Seed scaling removed) via GetCompenstatePos
        // SSJJ(x,y,z) -> Unity(-y, z, x)
        unity::Vector3 unity_pos;
        unity_pos.x = -p.position.y;
        unity_pos.y =  p.position.z;
        unity_pos.z =  p.position.x;

        // --- W2S projection ---
        ScreenPos feet = world_to_screen(unity_pos, screen_w, screen_h);
        if (!feet.visible) { w2s_fail++; continue; }

        // Head = feet + ~175 units up in Unity Y axis (game uses centimeter-scale coords)
        unity::Vector3 head_unity = unity_pos;
        head_unity.y += 175.0f;
        ScreenPos head = world_to_screen(head_unity, screen_w, screen_h);
        w2s_ok++;
        if (w2s_ok == 1) { first_sx = feet.x; first_sy = feet.y; }
        if (!head.visible) continue;

        // --- Box dimensions ---
        float box_h = feet.y - head.y; // head is higher on screen (lower GUI Y)
        if (box_h < 4.0f) continue;    // too far away / too small
        float box_w = box_h * 0.45f;
        float box_x = head.x - box_w * 0.5f;
        float box_y = head.y;

        // --- (a) Red box outline, 2px thick ---
        unity::Color box_color(1.0f, 0.2f, 0.2f, 1.0f);
        draw_box_outline(box_x, box_y, box_w, box_h, 2.0f, box_color);

        // --- (b) White corner accents ---
        float corner_len = box_h * 0.15f;
        if (corner_len < 4.0f) corner_len = 4.0f;
        unity::Color accent(1, 1, 1, 0.8f);
        // Top-left
        draw_filled_rect(box_x - 1, box_y - 1, corner_len, 3, accent);
        draw_filled_rect(box_x - 1, box_y - 1, 3, corner_len, accent);
        // Top-right
        draw_filled_rect(box_x + box_w - corner_len + 1, box_y - 1, corner_len, 3, accent);
        draw_filled_rect(box_x + box_w - 1, box_y - 1, 3, corner_len, accent);
        // Bottom-left
        draw_filled_rect(box_x - 1, box_y + box_h - 2, corner_len, 3, accent);
        draw_filled_rect(box_x - 1, box_y + box_h - corner_len + 1, 3, corner_len, accent);
        // Bottom-right
        draw_filled_rect(box_x + box_w - corner_len + 1, box_y + box_h - 2, corner_len, 3, accent);
        draw_filled_rect(box_x + box_w - 1, box_y + box_h - corner_len + 1, 3, corner_len, accent);

        // --- (c) Health bar on left side (vertical, fills bottom-up) ---
        float hb_w = 4.0f;
        float hb_x = box_x - hb_w - 4.0f;
        float hb_y = box_y;
        float hb_h = box_h;
        float hp_pct = (p.max_hp > 0.0f) ? (p.hp / p.max_hp) : 0.0f;
        if (hp_pct < 0.0f) hp_pct = 0.0f;
        if (hp_pct > 1.0f) hp_pct = 1.0f;

        // Background
        draw_filled_rect(hb_x - 1, hb_y - 1, hb_w + 2, hb_h + 2, unity::Color(0, 0, 0, 0.7f));

        // Fill color: red -> yellow -> green gradient based on HP pct
        unity::Color hp_color;
        if (hp_pct > 0.5f) {
            float t = (hp_pct - 0.5f) * 2.0f;
            hp_color = unity::Color(1.0f - t, 1.0f, 0.0f, 1.0f); // yellow -> green
        } else {
            float t = hp_pct * 2.0f;
            hp_color = unity::Color(1.0f, t, 0.0f, 1.0f); // red -> yellow
        }
        float fill_h = hb_h * hp_pct;
        if (fill_h > 0.5f) {
            draw_filled_rect(hb_x, hb_y + (hb_h - fill_h), hb_w, fill_h, hp_color);
        }

        // --- Text positioning ---
        // Center text on the box's horizontal center
        float box_cx = box_x + box_w * 0.5f;
        float text_w = 300.0f;
        float text_x = box_cx - text_w * 0.5f;
        float text_h = 22.0f;  // enough height for larger font

        // Set larger font and center alignment for ESP text
        int orig_font_size = gui::get_label_font_size();
        int orig_alignment = gui::get_label_alignment();
        gui::set_label_font_size(14);
        gui::set_label_alignment(gui::text_anchor::UpperCenter);

        // --- (d) Player name above box (shadow text) ---
        draw_text_shadow(text_x, box_y - text_h - 2.0f, text_w, text_h, p.name.c_str());

        // --- (e) HP text below box ---
        float info_y = box_y + box_h + 2.0f;
        draw_text_shadow(text_x, info_y, text_w, text_h,
            fmt("HP: %.0f/%.0f", p.hp, p.max_hp));

        // --- (f) Weapon name below HP ---
        if (!p.weapon_name.empty()) {
            draw_text_shadow(text_x, info_y + text_h, text_w, text_h,
                p.weapon_name.c_str());
        }

        // Restore original font settings
        gui::set_label_font_size(orig_font_size);
        gui::set_label_alignment(orig_alignment);

        drawn++;
    }

    // --------------- Skeleton ESP pass ---------------
    // Reuses the same player data from box ESP (no second entity enumeration).
    // Filtering (local/dead/teammate) already done by the same criteria.

    // Compute local eye position in SSJJ coords for visibility checks
    // (We need it in SSJJ for the BspTrace, but is_visible_unity will
    //  receive it as "eye_ssjj" and convert target bones from Unity coords)
    // Actually: we pass eye as Unity coords since is_visible_unity expects both Unity.
    // Let's compute eye in Unity world coords from the local player data.
    unity::Vector3 eye_unity;
    bool have_eye = false;
    for (const auto& p : players) {
        if (p.is_local && p.valid) {
            // SSJJ pos → Unity: SSJJ(x,y,z) → Unity(-y, z, x)
            eye_unity.x = -p.position.y;
            eye_unity.y =  p.position.z + 150.0f; // eye height above feet (SSJJ Z → Unity Y)
            eye_unity.z =  p.position.x;
            have_eye = true;
            break;
        }
    }

    int skel_drawn = 0;
    if (s_skeleton_enabled && s_skel.PE_get_thirdPersonUnityObjects) {
        unity::Color vis_col(0.0f, 1.0f, 0.5f, 1.0f); // green = visible
        unity::Color occ_col(1.0f, 0.3f, 0.0f, 0.8f);  // orange = occluded

        for (const auto& p : players) {
            if (!p.valid || p.is_local || p.is_dead) continue;
            if (local_team >= 0 && p.team_id == local_team) continue;
            if (!p._raw_entity) continue;

            bool debug_first = (skel_drawn == 0);
            if (draw_player_skeleton(p._raw_entity, screen_w, screen_h,
                                     vis_col, occ_col,
                                     have_eye ? &eye_unity : nullptr,
                                     have_eye,
                                     debug_first))
                skel_drawn++;
        }
    }

    // Track draw gaps for stall diagnosis
    DWORD now_tick = GetTickCount();
    if (s_last_draw_tick > 0) {
        DWORD gap = now_tick - s_last_draw_tick;
        if (gap > s_max_gap_ms) s_max_gap_ms = gap;
    }
    s_last_draw_tick = now_tick;

    snprintf(dbg, sizeof(dbg), "ESP: ok=%d fail=%d drawn=%d skel=%d gap=%lums #%u",
        w2s_ok, w2s_fail, drawn, skel_drawn, s_max_gap_ms, s_esp_frame);
    s_esp_debug = dbg;
    if (!s_bone_debug.empty()) {
        s_esp_debug += "\nBones:\n";
        s_esp_debug += s_bone_debug;
    }

    // Restore GUI color
    gui::set_color(orig_color);
}

// ---------------------------------------------------------------------------
// Enable / disable
// ---------------------------------------------------------------------------
void set_enabled(bool enabled) { s_enabled = enabled; }
bool is_enabled() { return s_enabled; }
void set_skeleton_enabled(bool enabled) { s_skeleton_enabled = enabled; }
bool is_skeleton_enabled() { return s_skeleton_enabled; }

// ---------------------------------------------------------------------------
// Shutdown -- free GC handle and null texture
// ---------------------------------------------------------------------------
void shutdown() {
    if (s_tex_gc_handle) {
        mono::gchandle_free(s_tex_gc_handle);
        s_tex_gc_handle = 0;
    }
    s_white_tex = nullptr;
}

} // namespace esp
