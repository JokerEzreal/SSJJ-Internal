#include "esp.h"
#include "player_info.h"
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

static ScreenPos world_to_screen(const unity::Vector3& world_pos, float screen_w, float screen_h) {
    ScreenPos r = { 0, 0, false };

    MonoMethod* get_main = unity::methods().Camera_get_main;
    MonoMethod* w2s      = unity::methods().Camera_WorldToScreenPoint;
    if (!get_main || !w2s) return r;

    MonoObject* camera = mono::runtime_invoke(get_main, nullptr, nullptr, nullptr);
    if (!camera) return r;

    unity::Vector3 pos = world_pos;
    void* args[1] = { &pos };
    MonoObject* result = mono::runtime_invoke(w2s, camera, args, nullptr);
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
    mono::runtime_invoke(tex_ctor, s_white_tex, ctor_args, nullptr);

    int px = 0, py = 0;
    unity::Color white = unity::Color::white();
    void* sp_args[3] = { &px, &py, &white };
    mono::runtime_invoke(tex_sp, s_white_tex, sp_args, nullptr);
    mono::runtime_invoke(tex_ap, s_white_tex, nullptr, nullptr);

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
    mono::runtime_invoke(rotate, nullptr, rot_args, nullptr);

    // Draw thin rect
    gui::set_color(col);
    gui::draw_texture(unity::Rect(x1, y1 - thickness * 0.5f, length, thickness),
                      s_white_tex);

    // Restore GUI matrix
    float neg = -angle;
    void* restore_args[2] = { &neg, &pivot };
    mono::runtime_invoke(rotate, nullptr, restore_args, nullptr);
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

    MonoObject* result = mono::runtime_invoke(get_pos, transform, nullptr, nullptr);
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
    MonoObject* result = mono::runtime_invoke(m, transform, nullptr, nullptr);
    if (!result) return 0;
    return *static_cast<int*>(mono::object_unbox(result));
}

// Get child Transform by index
static MonoObject* get_child(MonoObject* transform, int index) {
    if (!transform) return nullptr;
    MonoMethod* m = unity::methods().Transform_GetChild;
    if (!m) return nullptr;
    void* args[1] = { &index };
    return mono::runtime_invoke(m, transform, args, nullptr);
}

// Get parent Transform
static MonoObject* get_parent(MonoObject* transform) {
    if (!transform) return nullptr;
    MonoMethod* m = unity::methods().Transform_get_parent;
    if (!m) return nullptr;
    return mono::runtime_invoke(m, transform, nullptr, nullptr);
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
    MonoObject* name_obj = mono::runtime_invoke(get_name, transform, nullptr, nullptr);
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
static bool draw_player_skeleton(MonoObject* player_entity,
                                 float screen_w, float screen_h,
                                 const unity::Color& col,
                                 bool do_debug_dump = false)
{
    if (!player_entity) return false;
    if (!s_skel.PE_get_hasThirdPersonUnityObjects ||
        !s_skel.PE_get_thirdPersonUnityObjects) return false;

    // Check hasThirdPersonUnityObjects
    MonoObject* has_res = mono::runtime_invoke(
        s_skel.PE_get_hasThirdPersonUnityObjects, player_entity, nullptr, nullptr);
    if (!has_res || !*static_cast<bool*>(mono::object_unbox(has_res)))
        return false;

    // Get ThirdPersonUnityObjectsComponent
    MonoObject* tpu = mono::runtime_invoke(
        s_skel.PE_get_thirdPersonUnityObjects, player_entity, nullptr, nullptr);
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
    MonoObject* root_transform = mono::runtime_invoke(get_tf, root_go, nullptr, nullptr);
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
                draw_line(sp_body.x, sp_body.y, sp_head.x, sp_head.y, 2.0f, col);
                draw_filled_rect(sp_head.x - 3, sp_head.y - 3, 6, 6, col);
                draw_filled_rect(sp_body.x - 3, sp_body.y - 3, 6, 6, col);
            }
            return true;
        }
        return false;
    }

    // Get screen positions for all found bones
    ScreenPos scr[BONE_COUNT] = {};
    for (int i = 0; i < BONE_COUNT; i++) {
        if (!bones[i]) continue;
        unity::Vector3 pos;
        if (get_transform_position(bones[i], pos)) {
            scr[i] = world_to_screen(pos, screen_w, screen_h);
        }
    }

    // Draw chain: connect consecutive found bones, skipping missing ones
    auto draw_chain = [&](const BoneId* chain, int len) {
        int prev = -1;
        for (int i = 0; i < len; i++) {
            int idx = (int)chain[i];
            if (!bones[idx] || !scr[idx].visible) continue;
            if (prev >= 0 && scr[prev].visible) {
                draw_line(scr[prev].x, scr[prev].y, scr[idx].x, scr[idx].y, 2.0f, col);
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

    // Draw joint dots
    for (int i = 0; i < BONE_COUNT; i++) {
        if (bones[i] && scr[i].visible) {
            draw_filled_rect(scr[i].x - 2, scr[i].y - 2, 4, 4, col);
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
    MonoObject* has_res = mono::runtime_invoke(
        s_skel.PE_get_hasThirdPersonUnityObjects, player_entity, nullptr, nullptr);
    if (!has_res || !*static_cast<bool*>(mono::object_unbox(has_res)))
        return false;

    // Get ThirdPersonUnityObjectsComponent
    MonoObject* tpu = mono::runtime_invoke(
        s_skel.PE_get_thirdPersonUnityObjects, player_entity, nullptr, nullptr);
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

void draw() {
    if (!s_enabled) return;

    // Create texture on first draw (main thread context)
    ensure_texture();

    // Initialize skeleton cache on first draw
    init_skeleton_cache();

    float screen_w = static_cast<float>(gui::screen_width());
    float screen_h = static_cast<float>(gui::screen_height());

    char dbg[1024];
    if (!s_white_tex) {
        s_esp_debug = "ESP: tex=NULL";
        return;
    }

    // Save original GUI color so we can restore it when we are done
    unity::Color orig_color = gui::get_color();

    // ------ Fetch all player data from player_info module ------
    std::vector<player_info::PlayerData> players = player_info::read_all_players();

    // --- Coordinate diagnostic: test local player W2S with different mappings ---
    // The correct mapping will project the local player to ~screen center
    std::string coord_diag;
    for (const auto& p : players) {
        if (!p.valid || !p.is_local) continue;

        // Test 1: Raw coords (no conversion)
        unity::Vector3 raw = p.position;
        ScreenPos sp_raw = world_to_screen(raw, screen_w, screen_h);

        // Test 2: SsjjToUnity(-y, z, x)
        unity::Vector3 conv;
        conv.x = -p.position.y;
        conv.y =  p.position.z;
        conv.z =  p.position.x;
        ScreenPos sp_conv = world_to_screen(conv, screen_w, screen_h);

        // Test 3: (x, z, y) - simple height swap
        unity::Vector3 swap;
        swap.x = p.position.x;
        swap.y = p.position.z;
        swap.z = p.position.y;
        ScreenPos sp_swap = world_to_screen(swap, screen_w, screen_h);

        snprintf(dbg, sizeof(dbg),
            "DIAG pos=(%.0f,%.0f,%.0f) raw_scr=(%.0f,%.0f,%d) conv_scr=(%.0f,%.0f,%d) swap_scr=(%.0f,%.0f,%d)",
            p.position.x, p.position.y, p.position.z,
            sp_raw.x, sp_raw.y, sp_raw.visible ? 1 : 0,
            sp_conv.x, sp_conv.y, sp_conv.visible ? 1 : 0,
            sp_swap.x, sp_swap.y, sp_swap.visible ? 1 : 0);
        coord_diag = dbg;
        break;
    }

    int w2s_ok = 0, w2s_fail = 0, drawn = 0;
    float first_sx = 0, first_sy = 0;

    for (const auto& p : players) {
        // Skip invalid, local, or dead players
        if (!p.valid || p.is_local || p.is_dead) continue;

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
    int skel_drawn = 0;
    if (s_skeleton_enabled && s_skel.PE_get_thirdPersonUnityObjects) {
        // Cached Contexts access methods (resolved once)
        static MonoMethod* s_ctx_shared = nullptr;
        static MonoMethod* s_ctx_player = nullptr;
        if (!s_ctx_shared) {
            MonoClass* ctx_cls = mono::class_from_name(unity::images().entitas_lib, "", "Contexts");
            if (ctx_cls) {
                s_ctx_shared = mono::class_get_method_from_name(ctx_cls, "get_sharedInstance", 0);
                s_ctx_player = mono::class_get_method_from_name(ctx_cls, "get_player", 0);
            }
        }
        MonoObject* contexts = s_ctx_shared ?
            mono::runtime_invoke(s_ctx_shared, nullptr, nullptr, nullptr) : nullptr;
        if (contexts) {
            MonoObject* player_ctx = s_ctx_player ?
                mono::runtime_invoke(s_ctx_player, contexts, nullptr, nullptr) : nullptr;
            if (player_ctx) {
                // Resolve GetEntities dynamically
                static MonoMethod* s_get_entities = nullptr;
                if (!s_get_entities && mono::object_get_class && mono::class_get_parent) {
                    MonoClass* cls = mono::object_get_class(player_ctx);
                    for (int d = 0; cls && d < 10; d++) {
                        s_get_entities = mono::class_get_method_from_name(cls, "GetEntities", 0);
                        if (s_get_entities) break;
                        cls = mono::class_get_parent(cls);
                    }
                }
                if (s_get_entities) {
                    MonoObject* entity_list = mono::runtime_invoke(
                        s_get_entities, player_ctx, nullptr, nullptr);
                    if (entity_list) {
                        static MonoMethod* s_get_count = nullptr;
                        static MonoMethod* s_get_item = nullptr;
                        if (!s_get_count && mono::object_get_class) {
                            MonoClass* lcls = mono::object_get_class(entity_list);
                            if (lcls) {
                                s_get_count = mono::class_get_method_from_name(lcls, "get_Count", 0);
                                s_get_item = mono::class_get_method_from_name(lcls, "get_Item", 1);
                            }
                        }
                        if (s_get_count && s_get_item) {
                            MonoObject* cnt_obj = mono::runtime_invoke(
                                s_get_count, entity_list, nullptr, nullptr);
                            int cnt = cnt_obj ? *static_cast<int*>(mono::object_unbox(cnt_obj)) : 0;

                            static MonoMethod* s_has_comp = nullptr;
                            static MonoMethod* s_get_basic_info = nullptr;
                            static MonoMethod* s_basic_get_is_dead = nullptr;

                            // Resolve death-check methods once
                            if (!s_get_basic_info) {
                                MonoClass* pe = mono::class_from_name(
                                    unity::images().entitas_lib, "", "PlayerEntity");
                                if (pe) s_get_basic_info =
                                    mono::class_get_method_from_name(pe, "get_basicInfo", 0);
                            }
                            if (!s_basic_get_is_dead) {
                                MonoClass* bi = mono::class_from_name(
                                    unity::images().entitas_lib,
                                    "Assets.Sources.Components.Player",
                                    "BasicInfoComponent");
                                if (bi) s_basic_get_is_dead =
                                    mono::class_get_method_from_name(bi, "get_IsDead", 0);
                            }

                            for (int i = 0; i < cnt && i < 64; i++) {
                                void* idx_args[1] = { &i };
                                MonoObject* entity = mono::runtime_invoke(
                                    s_get_item, entity_list, idx_args, nullptr);
                                if (!entity) continue;

                                // Resolve HasComponent
                                if (!s_has_comp && mono::object_get_class) {
                                    MonoClass* ecls = mono::object_get_class(entity);
                                    if (ecls) s_has_comp = mono::class_get_method_from_name(ecls, "HasComponent", 1);
                                }

                                // Skip local player (component index 43 = MyPlayer)
                                if (s_has_comp) {
                                    int mp_idx = 43;
                                    void* mp_args[1] = { &mp_idx };
                                    MonoObject* is_local = mono::runtime_invoke(s_has_comp, entity, mp_args, nullptr);
                                    if (is_local && *static_cast<bool*>(mono::object_unbox(is_local)))
                                        continue;
                                }

                                // Skip dead players via BasicInfoComponent.IsDead
                                if (s_get_basic_info && s_basic_get_is_dead) {
                                    MonoObject* basic = mono::runtime_invoke(
                                        s_get_basic_info, entity, nullptr, nullptr);
                                    if (basic) {
                                        MonoObject* dead_res = mono::runtime_invoke(
                                            s_basic_get_is_dead, basic, nullptr, nullptr);
                                        if (dead_res && *static_cast<bool*>(mono::object_unbox(dead_res)))
                                            continue;
                                    }
                                }

                                unity::Color skel_col(0.0f, 1.0f, 0.5f, 1.0f); // green
                                bool debug_first = (skel_drawn == 0); // dump bone names for first player
                                if (draw_player_skeleton(entity, screen_w, screen_h, skel_col, debug_first))
                                    skel_drawn++;
                            }
                        }
                    }
                }
            }
        }
    }

    snprintf(dbg, sizeof(dbg), "ESP: ok=%d fail=%d drawn=%d skel=%d scr=(%.0f,%.0f)",
        w2s_ok, w2s_fail, drawn, skel_drawn, first_sx, first_sy);
    s_esp_debug = dbg;
    if (!coord_diag.empty()) {
        s_esp_debug += "\n";
        s_esp_debug += coord_diag;
    }
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
