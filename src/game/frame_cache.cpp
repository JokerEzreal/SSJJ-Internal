#include "frame_cache.h"
#include "player_info.h"
#include "esp.h"
#include "../gui/gui.h"
#include "../unity/unity_types.h"

#include <unordered_map>
#include <vector>
#include <cstring>
#include <Windows.h>

namespace frame_cache {

// ---------------------------------------------------------------------------
// Frame detection
// ---------------------------------------------------------------------------
static DWORD    s_last_tick     = 0;   // GetTickCount() of the previous begin_frame
static uint64_t s_frame_counter = 0;   // monotonically increasing, bumped each new frame
static bool     s_frame_active  = false; // true between begin_frame / end_frame

// ---------------------------------------------------------------------------
// Cached player data (populated once per frame)
// ---------------------------------------------------------------------------
static std::vector<player_info::PlayerData> s_players;
static bool s_players_valid = false;

// Cached local player (points into s_players or a static empty sentinel)
static player_info::PlayerData s_empty_local;
static const player_info::PlayerData* s_local_ptr = &s_empty_local;

// ---------------------------------------------------------------------------
// Cached screen dimensions
// ---------------------------------------------------------------------------
static float s_screen_w = 0.0f;
static float s_screen_h = 0.0f;
static bool  s_screen_valid = false;

// ---------------------------------------------------------------------------
// Cached bone data keyed by raw entity pointer (valid within one frame only)
// ---------------------------------------------------------------------------
static std::unordered_map<MonoObject*, CachedBones> s_bone_map;
static CachedBones s_empty_bones;  // returned when no data available

// ---------------------------------------------------------------------------
// begin_frame
// ---------------------------------------------------------------------------
bool begin_frame() {
    // Detect a new frame by tick change.  All callbacks within a single Unity
    // frame share the same GetTickCount() value (or very close).  If the tick
    // is identical to the previous begin_frame call, treat it as the same
    // frame and return false (caches already warm).
    DWORD now = GetTickCount();
    if (s_frame_active && now == s_last_tick)
        return false;

    // --- New frame ---
    s_last_tick = now;
    s_frame_counter++;
    s_frame_active = true;

    // Invalidate per-frame caches so they are lazily rebuilt on first access.
    s_players_valid = false;
    s_screen_valid  = false;
    s_bone_map.clear();

    return true;
}

// ---------------------------------------------------------------------------
// Internal: ensure player data is populated for this frame.
// ---------------------------------------------------------------------------
static void ensure_players() {
    if (s_players_valid) return;

    s_players = player_info::read_all_players(player_info::ReadMode::ESP);
    s_players_valid = true;

    // Find local player inside the list
    s_local_ptr = &s_empty_local;
    for (const auto& p : s_players) {
        if (p.is_local && p.valid) {
            s_local_ptr = &p;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Internal: ensure screen dimensions are cached.
// ---------------------------------------------------------------------------
static void ensure_screen() {
    if (s_screen_valid) return;
    s_screen_w = static_cast<float>(gui::screen_width());
    s_screen_h = static_cast<float>(gui::screen_height());
    s_screen_valid = true;
}

// ---------------------------------------------------------------------------
// get_players
// ---------------------------------------------------------------------------
const std::vector<player_info::PlayerData>& get_players() {
    ensure_players();
    return s_players;
}

// ---------------------------------------------------------------------------
// get_local_player
// ---------------------------------------------------------------------------
const player_info::PlayerData& get_local_player() {
    ensure_players();
    return *s_local_ptr;
}

// ---------------------------------------------------------------------------
// screen_width / screen_height
// ---------------------------------------------------------------------------
float screen_width() {
    ensure_screen();
    return s_screen_w;
}

float screen_height() {
    ensure_screen();
    return s_screen_h;
}

// ---------------------------------------------------------------------------
// get_bone_data
// ---------------------------------------------------------------------------
const CachedBones& get_bone_data(MonoObject* entity, const unity::Vector3& eye_unity) {
    if (!entity) return s_empty_bones;

    // Look up existing entry (or insert a default-constructed one)
    auto it = s_bone_map.find(entity);
    if (it != s_bone_map.end()) {
        return it->second;
    }

    // First access this frame -- collect bones via esp::get_bone_targets
    CachedBones cb;
    memset(&cb, 0, sizeof(cb));
    cb.collected = true;
    cb.count = esp::get_bone_targets(entity, eye_unity, cb.bones, 20);

    auto [ins, _] = s_bone_map.emplace(entity, cb);
    return ins->second;
}

// ---------------------------------------------------------------------------
// end_frame
// ---------------------------------------------------------------------------
void end_frame() {
    // Clear the bone map -- entity pointers are only valid within a single
    // frame and must not be carried over.
    s_bone_map.clear();
    s_frame_active = false;
}

// ---------------------------------------------------------------------------
// shutdown
// ---------------------------------------------------------------------------
void shutdown() {
    s_players.clear();
    s_players.shrink_to_fit();
    s_bone_map.clear();
    s_players_valid = false;
    s_screen_valid  = false;
    s_local_ptr = &s_empty_local;
    s_frame_counter = 0;
    s_last_tick = 0;
    s_frame_active = false;
}

} // namespace frame_cache
