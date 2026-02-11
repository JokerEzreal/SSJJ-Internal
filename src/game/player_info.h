#pragma once

#include "../unity/unity_types.h"
#include <string>
#include <vector>

// Forward declaration
struct _MonoObject;
typedef struct _MonoObject MonoObject;

namespace player_info {

struct PlayerData {
    bool valid = false;
    bool is_local = false;

    // Raw entity pointer (valid only within the current frame, do NOT cache)
    MonoObject* _raw_entity = nullptr;

    // Identity
    std::string name;
    int entity_id = 0;
    int64_t cid = 0;
    std::string team_name;
    int team_id = -1;   // in-game team index (from PlayerEntityData.Team)

    // Health
    float hp = 0.0f;
    float max_hp = 0.0f;
    bool is_dead = false;

    // Position (decrypted via FposComponent.Gp())
    unity::Vector3 position;

    // View angles
    float yaw = 0.0f;
    float pitch = 0.0f;

    // Movement
    unity::Vector3 velocity;
    bool on_ground = false;
    bool moving = false;
    float stamina = 0.0f;

    // Weapon
    std::string weapon_name;
    int weapon_id = 0;
    int weapon_level = 0;

    // Combat
    int kill_count = 0;
};

// Initialize cached classes/methods. Call after unity::cache_initialize().
bool initialize();

// Read current local player data. Safe to call even when not in a match.
PlayerData read_local_player();

// Read all players in the match. Returns empty vector if not in a match.
std::vector<PlayerData> read_all_players();

// Debug info string (for diagnosing issues)
const std::string& get_debug_info();

// Cleanup
void shutdown();

} // namespace player_info
