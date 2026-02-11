#pragma once

#include "../unity/unity_types.h"

// Forward declaration
struct _MonoObject;
typedef struct _MonoObject MonoObject;

namespace esp {

// Initialize: cache classes/methods, create drawing texture.
// Call after unity::cache_initialize() and player_info::initialize().
bool initialize();

// Draw ESP overlay. Must be called during OnGUI context.
void draw();

// Toggle ESP on/off.
void set_enabled(bool enabled);
bool is_enabled();

// Toggle skeleton ESP on/off.
void set_skeleton_enabled(bool enabled);
bool is_skeleton_enabled();

// Debug info string
const char* get_esp_debug();

// Bone hierarchy dump for data export
const char* get_bone_dump();
void request_bone_dump();

// Get head bone world position (Unity coords) for a player entity.
// Returns false if skeleton data unavailable (falls back to estimated).
bool get_head_bone_world_pos(MonoObject* player_entity, unity::Vector3& out);

// Bone visibility data for aimbot
struct BoneTarget {
    unity::Vector3 world_pos;  // Unity world coords
    bool valid;                // bone was found
    bool visible;              // not occluded by walls
    int bone_id;               // BoneId enum value
};

// Get all bone positions and visibility for a player entity.
// eye_unity = local player eye in Unity world coords.
// Returns number of valid bones found.
int get_bone_targets(MonoObject* player_entity,
                     const unity::Vector3& eye_unity,
                     BoneTarget* out, int max_bones);

void shutdown();

} // namespace esp
