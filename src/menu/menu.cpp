#include "menu.h"
#include "../gui/gui.h"
#include "../unity/unity_types.h"
#include "../game/player_info.h"
#include "../game/esp.h"
#include "../game/aimbot.h"

#include <cstdio>
#include <cstdarg>
#include <vector>

namespace menu {

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------
static bool  s_visible    = true;

// Persistent window position (initialized on first frame)
static unity::Rect s_menu_rect(0, 0, 620, 800);
static bool        s_rect_initialized = false;

// Drag state
static bool  s_dragging   = false;
static float s_drag_off_x = 0.0f;
static float s_drag_off_y = 0.0f;

// Title bar height used for drag hit-testing
static constexpr float TITLE_BAR_H = 25.0f;

// Cached player data (refreshed periodically)
static player_info::PlayerData s_local_player;
static std::vector<player_info::PlayerData> s_all_players;
static int s_update_counter = 0;

// ---------------------------------------------------------------------------
// Helper: format a line into a static buffer
// ---------------------------------------------------------------------------
static const char* fmt(const char* format, ...) {
    static char buf[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    return buf;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void initialize() {
    s_visible    = true;
    s_rect_initialized = false;
    s_dragging   = false;
    s_update_counter = 0;
    s_local_player = {};
    s_all_players.clear();
}

void on_update() {
    // Toggle menu visibility with the Insert key.
    if (gui::get_key_down(gui::keycode::Insert)) {
        s_visible = !s_visible;
    }

    // Toggle aimbot with F2 key
    if (gui::get_key_down(gui::keycode::F2)) {
        aimbot::set_enabled(!aimbot::is_enabled());
    }

    // Refresh player data every ~15 frames
    if (++s_update_counter >= 15) {
        s_update_counter = 0;
        s_local_player = player_info::read_local_player();
        s_all_players  = player_info::read_all_players();
    }
}

void on_gui() {
    esp::draw();

    if (!s_visible) return;

    // ---- First-frame: centre the window ----
    if (!s_rect_initialized) {
        float sw = static_cast<float>(gui::screen_width());
        float sh = static_cast<float>(gui::screen_height());
        s_menu_rect.x = sw * 0.5f - s_menu_rect.width  * 0.5f;
        s_menu_rect.y = sh * 0.5f - s_menu_rect.height * 0.5f;
        s_rect_initialized = true;
    }

    // ---- Handle dragging ----
    unity::Vector2 mouse = gui::get_mouse_position_gui();

    if (gui::get_mouse_button_down(0)) {
        if (mouse.x >= s_menu_rect.x &&
            mouse.x <= s_menu_rect.x + s_menu_rect.width &&
            mouse.y >= s_menu_rect.y &&
            mouse.y <= s_menu_rect.y + TITLE_BAR_H)
        {
            s_dragging   = true;
            s_drag_off_x = mouse.x - s_menu_rect.x;
            s_drag_off_y = mouse.y - s_menu_rect.y;
        }
    }

    if (!gui::get_mouse_button(0)) {
        s_dragging = false;
    }

    if (s_dragging) {
        s_menu_rect.x = mouse.x - s_drag_off_x;
        s_menu_rect.y = mouse.y - s_drag_off_y;
    }

    // ---- Draw UI ----
    gui::box(s_menu_rect, "SSJJ Internal");

    float x     = s_menu_rect.x + 10.0f;
    float y     = s_menu_rect.y + 30.0f;
    float w     = s_menu_rect.width - 20.0f;
    float lineH = 20.0f;
    float gap   = 2.0f;

    // ================================================================
    // Local Player Info Section
    // ================================================================
    gui::label({ x, y, w, lineH }, "=== Local Player ===");
    y += lineH + gap;

    if (!s_local_player.valid) {
        gui::label({ x, y, w, lineH }, "Not in game");
        y += lineH + gap;
    } else {
        gui::label({ x, y, w, lineH },
            fmt("Name: %s  |  Team: %s", s_local_player.name.c_str(), s_local_player.team_name.c_str()));
        y += lineH + gap;

        gui::label({ x, y, w, lineH },
            fmt("HP: %.0f/%.0f  |  Dead: %s  |  ID: %d",
                s_local_player.hp, s_local_player.max_hp,
                s_local_player.is_dead ? "Y" : "N", s_local_player.entity_id));
        y += lineH + gap;

        gui::label({ x, y, w, lineH },
            fmt("Pos: (%.1f, %.1f, %.1f)  Yaw: %.1f",
                s_local_player.position.x, s_local_player.position.y,
                s_local_player.position.z, s_local_player.yaw));
        y += lineH + gap;

        gui::label({ x, y, w, lineH },
            fmt("Weapon: %s  |  Kills: %d",
                s_local_player.weapon_name.c_str(), s_local_player.kill_count));
        y += lineH + gap;
    }

    // ================================================================
    // Features Section
    // ================================================================
    y += 5.0f;
    gui::label({ x, y, w, lineH }, "=== Features ===");
    y += lineH + gap;

    gui::label({ x, y, w, lineH },
        fmt("[F2] Aimbot: %s  |  [Mouse3] Activate", aimbot::is_enabled() ? "ON" : "OFF"));
    y += lineH + gap;

    const char* aim_dbg = aimbot::get_debug_info();
    if (aim_dbg && aim_dbg[0]) {
        gui::label({ x, y, w, lineH }, fmt("  %s", aim_dbg));
        y += lineH + gap;
    }

    // ================================================================
    // All Players Section
    // ================================================================
    y += 5.0f;
    gui::label({ x, y, w, lineH },
        fmt("=== All Players (%d) ===", (int)s_all_players.size()));
    y += lineH + gap;

    // Debug info
    const auto& dbg = player_info::get_debug_info();
    if (!dbg.empty()) {
        gui::label({ x, y, w, lineH }, fmt("DBG: %s", dbg.c_str()));
        y += lineH + gap;
    }
    const char* esp_dbg = esp::get_esp_debug();
    if (esp_dbg && esp_dbg[0]) {
        // ESP debug may contain multiple lines separated by \n
        std::string esp_str(esp_dbg);
        size_t pos = 0;
        while (pos < esp_str.size()) {
            size_t nl = esp_str.find('\n', pos);
            std::string line = (nl == std::string::npos) ?
                esp_str.substr(pos) : esp_str.substr(pos, nl - pos);
            if (!line.empty()) {
                gui::label({ x, y, w, lineH }, line.c_str());
                y += lineH + gap;
            }
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
    }

    for (int i = 0; i < (int)s_all_players.size(); i++) {
        const auto& p = s_all_players[i];
        if (p.is_local) continue;

        // Don't overflow the window
        if (y + lineH * 3 > s_menu_rect.y + s_menu_rect.height - 10.0f) {
            gui::label({ x, y, w, lineH }, fmt("... +%d more", (int)s_all_players.size() - i));
            break;
        }

        // Separator line
        const char* tag = p.is_local ? " [ME]" : "";
        const char* dead_tag = p.is_dead ? " [DEAD]" : "";

        gui::label({ x, y, w, lineH },
            fmt("#%d %s%s%s  Team:%s  ID:%d",
                i + 1, p.name.c_str(), tag, dead_tag,
                p.team_name.c_str(), p.entity_id));
        y += lineH + gap;

        gui::label({ x + 15, y, w - 15, lineH },
            fmt("HP:%.0f/%.0f  Pos:(%.1f,%.1f,%.1f)  Wpn:%s",
                p.hp, p.max_hp,
                p.position.x, p.position.y, p.position.z,
                p.weapon_name.c_str()));
        y += lineH + gap + 2.0f;
    }

    // Footer
    y = s_menu_rect.y + s_menu_rect.height - lineH - 5.0f;
    gui::label({ x, y, w, lineH }, "[Insert] Menu  [F2] Aimbot  [Mouse3] Aim  [End] Unload");
}

bool is_visible() {
    return s_visible;
}

} // namespace menu
