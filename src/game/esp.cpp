#include "esp.h"
#include "player_info.h"
#include "../gui/gui.h"
#include "../mono/mono_api.h"
#include "../mono/mono_types.h"
#include "../unity/unity_classes.h"
#include "../unity/unity_types.h"
#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>

namespace esp {

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static bool       s_enabled       = true;
static MonoObject* s_white_tex    = nullptr;
static uint32_t    s_tex_gc_handle = 0;

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
// Draw ESP for all players
// ---------------------------------------------------------------------------
// Debug info for ESP
static std::string s_esp_debug;
const char* get_esp_debug() { return s_esp_debug.c_str(); }

void draw() {
    if (!s_enabled) return;

    // Create texture on first draw (main thread context)
    ensure_texture();

    float screen_w = static_cast<float>(gui::screen_width());
    float screen_h = static_cast<float>(gui::screen_height());

    // Debug: draw a small test rectangle at top-left to verify drawing works
    if (s_white_tex) {
        gui::set_color(unity::Color(1, 0, 0, 1));
        gui::draw_texture(unity::Rect(10, 10, 30, 30), s_white_tex);
        gui::set_color(unity::Color(1, 1, 1, 1));
    }

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

    snprintf(dbg, sizeof(dbg), "ESP: ok=%d fail=%d drawn=%d scr=(%.0f,%.0f)",
        w2s_ok, w2s_fail, drawn, first_sx, first_sy);
    s_esp_debug = dbg;
    if (!coord_diag.empty()) {
        s_esp_debug += "\n";
        s_esp_debug += coord_diag;
    }

    // Restore GUI color
    gui::set_color(orig_color);
}

// ---------------------------------------------------------------------------
// Enable / disable
// ---------------------------------------------------------------------------
void set_enabled(bool enabled) { s_enabled = enabled; }
bool is_enabled() { return s_enabled; }

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
