#include "menu.h"
#include "../gui/gui.h"
#include "../unity/unity_types.h"
#include "../game/player_info.h"
#include "../game/esp.h"
#include "../game/aimbot.h"
#include "../game/visibility.h"
#include "../game/anticheat.h"
#include "../core/globals.h"

#include <cstdio>
#include <cstdarg>
#include <vector>
#include <string>
#include <fstream>
#include <ctime>
#include <cstdlib>
#include <algorithm>

namespace menu {

// ---------------------------------------------------------------------------
// Tab identifiers
// ---------------------------------------------------------------------------
enum Tab { TAB_MAIN = 0, TAB_DEBUG = 1 };

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------
static bool  s_visible    = true;
static Tab   s_current_tab = TAB_MAIN;

// Window position - default top-left
static float s_win_x = 10.0f;
static float s_win_y = 10.0f;
static float s_win_w = 320.0f;
static float s_prev_height = 250.0f;  // measured from previous frame
static bool  s_pos_initialized = false;

// Drag state
static bool  s_dragging   = false;
static float s_drag_off_x = 0.0f;
static float s_drag_off_y = 0.0f;
static constexpr float TITLE_BAR_H = 25.0f;

// Cached player data (refreshed periodically)
static player_info::PlayerData s_local_player;
static std::vector<player_info::PlayerData> s_all_players;
static int s_update_counter = 0;

// Dump state
static std::string s_last_dump_path;
static int  s_dump_status = 0;  // 0=none, 1=success, 2=fail
static int  s_dump_fade   = 0;  // frames left to show status

// ---------------------------------------------------------------------------
// Helpers
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
// Data dump to file for AI analysis
// ---------------------------------------------------------------------------
static void dump_data_to_file() {
    char timestamp[64];
    time_t now = time(nullptr);
    struct tm t_buf;
    localtime_s(&t_buf, &now);
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &t_buf);

    // Write to Desktop
    const char* userprofile = getenv("USERPROFILE");
    std::string filepath;
    if (userprofile) {
        filepath = std::string(userprofile) + "\\Desktop\\ssjj_dump_" + timestamp + ".txt";
    } else {
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        filepath = std::string(tmp) + "ssjj_dump_" + timestamp + ".txt";
    }

    std::ofstream f(filepath);
    if (!f.is_open()) {
        s_dump_status = 2;
        s_last_dump_path = filepath;
        s_dump_fade = 300;
        return;
    }

    f << "========================================\n";
    f << "SSJJ Internal - Data Dump for AI Analysis\n";
    f << "Timestamp: " << timestamp << "\n";
    f << "========================================\n\n";

    // --- Feature state ---
    f << "=== FEATURE STATE ===\n";
    f << "ESP Enabled: " << (esp::is_enabled() ? "YES" : "NO") << "\n";
    f << "Skeleton ESP: " << (esp::is_skeleton_enabled() ? "YES" : "NO") << "\n";
    f << "Aimbot: " << (aimbot::is_enabled() ? "YES" : "NO") << "\n\n";

    // --- Local player ---
    f << "=== LOCAL PLAYER ===\n";
    if (s_local_player.valid) {
        f << "Name: " << s_local_player.name << "\n";
        f << "TeamName: " << s_local_player.team_name << "\n";
        f << "TeamID: " << s_local_player.team_id << "\n";
        f << "HP: " << s_local_player.hp << "/" << s_local_player.max_hp << "\n";
        f << "Dead: " << (s_local_player.is_dead ? "YES" : "NO") << "\n";
        f << "EntityID: " << s_local_player.entity_id << "\n";
        f << "CID: " << s_local_player.cid << "\n";
        f << "Position: (" << s_local_player.position.x << ", "
          << s_local_player.position.y << ", " << s_local_player.position.z << ")\n";
        f << "Yaw: " << s_local_player.yaw << " Pitch: " << s_local_player.pitch << "\n";
        f << "Weapon: " << s_local_player.weapon_name
          << " (ID:" << s_local_player.weapon_id
          << " Lv:" << s_local_player.weapon_level << ")\n";
        f << "Kills: " << s_local_player.kill_count << "\n";
        f << "Velocity: (" << s_local_player.velocity.x << ", "
          << s_local_player.velocity.y << ", " << s_local_player.velocity.z << ")\n";
        f << "OnGround: " << (s_local_player.on_ground ? "YES" : "NO") << "\n";
        f << "Moving: " << (s_local_player.moving ? "YES" : "NO") << "\n";
        f << "Stamina: " << s_local_player.stamina << "\n";
    } else {
        f << "Not in game\n";
    }
    f << "\n";

    // --- All players ---
    f << "=== ALL PLAYERS (" << s_all_players.size() << ") ===\n";
    for (int i = 0; i < (int)s_all_players.size(); i++) {
        const auto& p = s_all_players[i];
        f << "\n--- Player #" << (i + 1)
          << (p.is_local ? " [LOCAL]" : "")
          << (p.is_dead ? " [DEAD]" : "") << " ---\n";
        f << "Name: " << p.name << "\n";
        f << "TeamName: " << p.team_name << "\n";
        f << "TeamID: " << p.team_id << "\n";
        f << "HP: " << p.hp << "/" << p.max_hp << "\n";
        f << "EntityID: " << p.entity_id << "\n";
        f << "CID: " << p.cid << "\n";
        f << "Position: (" << p.position.x << ", "
          << p.position.y << ", " << p.position.z << ")\n";
        f << "Yaw: " << p.yaw << " Pitch: " << p.pitch << "\n";
        f << "Weapon: " << p.weapon_name
          << " (ID:" << p.weapon_id << " Lv:" << p.weapon_level << ")\n";
        f << "Kills: " << p.kill_count << "\n";
        f << "OnGround: " << (p.on_ground ? "YES" : "NO") << "\n";
        f << "Moving: " << (p.moving ? "YES" : "NO") << "\n";
        f << "Stamina: " << p.stamina << "\n";
    }
    f << "\n";

    // --- Bone hierarchy ---
    f << "=== BONE HIERARCHY ===\n";
    const char* bones = esp::get_bone_dump();
    if (bones && bones[0]) {
        f << bones << "\n";
    } else {
        f << "(No bone data - skeleton ESP off or no players visible)\n";
    }
    f << "\n";

    // --- Debug strings ---
    f << "=== DEBUG INFO ===\n";
    f << "PlayerInfo: " << player_info::get_debug_info() << "\n";
    const char* esp_dbg = esp::get_esp_debug();
    if (esp_dbg) f << "ESP: " << esp_dbg << "\n";
    const char* aim_dbg = aimbot::get_debug_info();
    if (aim_dbg) f << "Aimbot: " << aim_dbg << "\n";
    f << "\n";

    f << "=== VISIBILITY DIAGNOSTICS ===\n";
    f << visibility::dump_diagnostics();
    f << "\n";

    f << "=== ANTI-CHEAT BYPASS DIAGNOSTICS ===\n";
    f << anticheat::dump_diagnostics();
    f << "\n";

    // --- Reference: ECS component indices ---
    f << "=== COMPONENT INDICES (for reference) ===\n";
    f << "BasicInfo=17, Life=4, Orientation=6, Fpos=51\n";
    f << "CurrentWeapon=23, Move=59, EntityId=2, TeamName=81\n";
    f << "CurrKill=24, MyPlayer=43\n\n";

    // --- Reference: ECS access path ---
    f << "=== ECS ACCESS PATH ===\n";
    f << "Contexts.sharedInstance -> Contexts.player -> PlayerContext.myPlayerEntity\n";
    f << "PlayerEntity.GetComponent(index) -> Component fields\n";
    f << "Position: FposComponent.Change.GetPosIndex() -> PlayerEntity.GetCompenstatePos(idx)\n";
    f << "Skeleton: PlayerEntity.thirdPersonUnityObjects.ThirdTran.RootContainer -> Transform hierarchy\n";
    f << "Coordinate: SSJJ(x,y,z) -> Unity(-y, z, x)\n\n";

    f << "========================================\n";
    f << "End of dump - feed this file to AI for analysis\n";
    f << "========================================\n";

    f.close();
    s_dump_status = 1;
    s_last_dump_path = filepath;
    s_dump_fade = 300;  // show status for ~5 seconds
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void initialize() {
    s_visible = true;
    s_current_tab = TAB_MAIN;
    s_pos_initialized = false;
    s_dragging = false;
    s_update_counter = 0;
    s_local_player = {};
    s_all_players.clear();
    s_dump_status = 0;
    s_last_dump_path.clear();
    s_dump_fade = 0;
}

void on_update() {
    // Toggle menu visibility
    if (gui::get_key_down(gui::keycode::Insert)) {
        s_visible = !s_visible;
    }

    // Toggle aimbot
    if (gui::get_key_down(gui::keycode::F2)) {
        aimbot::set_enabled(!aimbot::is_enabled());
    }

    // Toggle skeleton ESP
    if (gui::get_key_down(gui::keycode::F3)) {
        esp::set_skeleton_enabled(!esp::is_skeleton_enabled());
    }

    // Refresh player data every ~15 frames
    if (++s_update_counter >= 15) {
        s_update_counter = 0;
        s_local_player = player_info::read_local_player();
        s_all_players  = player_info::read_all_players();
    }
}

void on_gui() {
    // Always draw ESP regardless of menu visibility
    esp::draw();

    if (!s_visible) return;

    // Layout constants
    constexpr float PAD     = 10.0f;
    constexpr float LINE_H  = 20.0f;
    constexpr float GAP     = 2.0f;
    constexpr float BTN_H   = 24.0f;

    // Handle dragging
    unity::Vector2 mouse = gui::get_mouse_position_gui();

    if (gui::get_mouse_button_down(0)) {
        if (mouse.x >= s_win_x && mouse.x <= s_win_x + s_win_w &&
            mouse.y >= s_win_y && mouse.y <= s_win_y + TITLE_BAR_H) {
            s_dragging   = true;
            s_drag_off_x = mouse.x - s_win_x;
            s_drag_off_y = mouse.y - s_win_y;
        }
    }
    if (!gui::get_mouse_button(0)) s_dragging = false;
    if (s_dragging) {
        s_win_x = mouse.x - s_drag_off_x;
        s_win_y = mouse.y - s_drag_off_y;
    }

    // Draw window background using previous frame's measured height
    float win_h = s_prev_height;
    gui::box(unity::Rect(s_win_x, s_win_y, s_win_w, win_h), "SSJJ Internal");

    float x = s_win_x + PAD;
    float w = s_win_w - PAD * 2.0f;
    float y = s_win_y + 30.0f;  // below title bar

    // ================================================================
    // Tab bar
    // ================================================================
    float tab_w = (w - 5.0f) * 0.5f;
    bool main_active = (s_current_tab == TAB_MAIN);
    bool dbg_active  = (s_current_tab == TAB_DEBUG);

    if (gui::button({x, y, tab_w, BTN_H}, main_active ? "[ Main ]" : "Main"))
        s_current_tab = TAB_MAIN;
    if (gui::button({x + tab_w + 5.0f, y, tab_w, BTN_H}, dbg_active ? "[ Debug ]" : "Debug"))
        s_current_tab = TAB_DEBUG;
    y += BTN_H + 4.0f;

    // ================================================================
    // TAB: Main - Feature toggles & tools
    // ================================================================
    if (s_current_tab == TAB_MAIN) {
        gui::label({x, y, w, LINE_H}, "--- Features ---");
        y += LINE_H + GAP;

        // ESP toggle
        bool esp_on = gui::toggle({x, y, w, LINE_H}, esp::is_enabled(), "ESP Overlay");
        esp::set_enabled(esp_on);
        y += LINE_H + GAP;

        // Skeleton ESP toggle
        bool skel_on = gui::toggle({x, y, w, LINE_H}, esp::is_skeleton_enabled(), "Skeleton ESP  [F3]");
        esp::set_skeleton_enabled(skel_on);
        y += LINE_H + GAP;

        // Aimbot toggle
        bool aim_on = gui::toggle({x, y, w, LINE_H}, aimbot::is_enabled(), "Aimbot  [F2]");
        aimbot::set_enabled(aim_on);
        y += LINE_H + GAP;

        // Aimbot status
        const char* aim_dbg = aimbot::get_debug_info();
        if (aim_on && aim_dbg && aim_dbg[0]) {
            gui::label({x + 20, y, w - 20, LINE_H}, aim_dbg);
            y += LINE_H + GAP;
        }

        // Tools section
        y += 6.0f;
        gui::label({x, y, w, LINE_H}, "--- Tools ---");
        y += LINE_H + GAP;

        // Dump data button
        if (gui::button({x, y, w, BTN_H}, "Dump Data to File (AI Analysis)")) {
            // Request fresh bone dump then write file
            esp::request_bone_dump();
            // Force immediate data refresh
            s_local_player = player_info::read_local_player();
            s_all_players  = player_info::read_all_players();
            dump_data_to_file();
        }
        y += BTN_H + GAP;

        // Dump status feedback
        if (s_dump_fade > 0) {
            s_dump_fade--;
            if (s_dump_status == 1) {
                gui::label({x, y, w, LINE_H}, "Dumped OK!");
                y += LINE_H + GAP;
                // Show truncated path
                std::string display_path = s_last_dump_path;
                if (display_path.size() > 42)
                    display_path = "..." + display_path.substr(display_path.size() - 39);
                gui::label({x, y, w, LINE_H}, display_path.c_str());
                y += LINE_H + GAP;
            } else if (s_dump_status == 2) {
                gui::label({x, y, w, LINE_H}, "Dump FAILED - check permissions");
                y += LINE_H + GAP;
            }
        }

        // Footer
        y += 4.0f;
        gui::label({x, y, w, LINE_H}, "[Insert] Menu  [End] Unload");
        y += LINE_H + GAP;
    }

    // ================================================================
    // TAB: Debug - Player info & diagnostics
    // ================================================================
    else if (s_current_tab == TAB_DEBUG) {
        // --- Local player info ---
        gui::label({x, y, w, LINE_H}, "--- Local Player ---");
        y += LINE_H + GAP;

        if (!s_local_player.valid) {
            gui::label({x, y, w, LINE_H}, "Not in game");
            y += LINE_H + GAP;
        } else {
            gui::label({x, y, w, LINE_H},
                fmt("Name: %s  |  TeamID: %d",
                    s_local_player.name.c_str(), s_local_player.team_id));
            y += LINE_H + GAP;

            gui::label({x, y, w, LINE_H},
                fmt("HP: %.0f/%.0f  |  Dead: %s  |  ID: %d",
                    s_local_player.hp, s_local_player.max_hp,
                    s_local_player.is_dead ? "Y" : "N", s_local_player.entity_id));
            y += LINE_H + GAP;

            gui::label({x, y, w, LINE_H},
                fmt("Pos: (%.1f, %.1f, %.1f)  Yaw: %.1f",
                    s_local_player.position.x, s_local_player.position.y,
                    s_local_player.position.z, s_local_player.yaw));
            y += LINE_H + GAP;

            gui::label({x, y, w, LINE_H},
                fmt("Weapon: %s  |  Kills: %d",
                    s_local_player.weapon_name.c_str(), s_local_player.kill_count));
            y += LINE_H + GAP;
        }

        // --- All players ---
        y += 4.0f;
        gui::label({x, y, w, LINE_H},
            fmt("--- All Players (%d) ---", (int)s_all_players.size()));
        y += LINE_H + GAP;

        for (int i = 0; i < (int)s_all_players.size(); i++) {
            const auto& p = s_all_players[i];
            if (p.is_local) continue;

            // Cap display to avoid massive window
            if (y - s_win_y > 600.0f) {
                gui::label({x, y, w, LINE_H},
                    fmt("... +%d more", (int)s_all_players.size() - i));
                y += LINE_H + GAP;
                break;
            }

            gui::label({x, y, w, LINE_H},
                fmt("#%d %s%s T:%d HP:%.0f/%.0f",
                    i + 1, p.name.c_str(),
                    p.is_dead ? "[D]" : "",
                    p.team_id, p.hp, p.max_hp));
            y += LINE_H + GAP;
        }

        // --- Debug diagnostics ---
        y += 4.0f;
        gui::label({x, y, w, LINE_H}, "--- Diagnostics ---");
        y += LINE_H + GAP;

        const auto& pi_dbg = player_info::get_debug_info();
        if (!pi_dbg.empty()) {
            gui::label({x, y, w, LINE_H}, fmt("PI: %s", pi_dbg.c_str()));
            y += LINE_H + GAP;
        }

        const char* vis_dbg = visibility::get_debug_info();
        if (vis_dbg && vis_dbg[0]) {
            gui::label({x, y, w, LINE_H}, fmt("VIS: %s", vis_dbg));
            y += LINE_H + GAP;
        }

        const char* ac_dbg = anticheat::get_debug_info();
        if (ac_dbg && ac_dbg[0]) {
            gui::label({x, y, w, LINE_H}, fmt("AC: %s", ac_dbg));
            y += LINE_H + GAP;
        }

        const char* esp_dbg = esp::get_esp_debug();
        if (esp_dbg && esp_dbg[0]) {
            // Render multi-line ESP debug, limited to 15 lines
            std::string esp_str(esp_dbg);
            size_t pos = 0;
            int lines = 0;
            while (pos < esp_str.size() && lines < 15) {
                size_t nl = esp_str.find('\n', pos);
                std::string line = (nl == std::string::npos) ?
                    esp_str.substr(pos) : esp_str.substr(pos, nl - pos);
                if (!line.empty()) {
                    gui::label({x, y, w, LINE_H}, line.c_str());
                    y += LINE_H + GAP;
                    lines++;
                }
                if (nl == std::string::npos) break;
                pos = nl + 1;
            }
            if (lines >= 15) {
                gui::label({x, y, w, LINE_H}, "... (dump to file for full data)");
                y += LINE_H + GAP;
            }
        }

        // Footer
        y += 4.0f;
        gui::label({x, y, w, LINE_H}, "[Insert] Menu  [End] Unload");
        y += LINE_H + GAP;
    }

    // Measure content height for next frame's box
    s_prev_height = (y - s_win_y) + PAD;
}

bool is_visible() {
    return s_visible;
}

} // namespace menu
