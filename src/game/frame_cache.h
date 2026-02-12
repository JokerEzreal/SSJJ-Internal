#pragma once

#include "player_info.h"
#include "esp.h"
#include "../unity/unity_types.h"
#include <vector>

namespace frame_cache {

// Call once at the start of the first callback each frame.
// Uses a monotonic frame counter to detect new frames.
// Returns true if this is a new frame (cache was refreshed).
bool begin_frame();

// ---- Cached player data ----
// Returns the cached player list (computed once per frame).
const std::vector<player_info::PlayerData>& get_players();

// Returns cached local player data (extracted from the player list).
const player_info::PlayerData& get_local_player();

// ---- Cached camera/screen ----
float screen_width();
float screen_height();

// ---- Cached bone data per entity ----
struct CachedBones {
    esp::BoneTarget bones[20];  // BONE_COUNT slots
    int count;                   // number of valid bones filled in
    bool collected;              // true if collection was attempted this frame
};

// Get bone data for a player entity. Collects on first call per entity per
// frame, returns cached data on subsequent calls within the same frame.
// eye_unity = local player eye position in Unity world coords (for visibility).
const CachedBones& get_bone_data(MonoObject* entity, const unity::Vector3& eye_unity);

// ---- Cleanup ----

// Clear per-frame caches. Call at the end of the last callback each frame.
// (Entity pointers may change between frames, so the bone map must be cleared.)
void end_frame();

// Full teardown.
void shutdown();

} // namespace frame_cache
