#pragma once

namespace aimbot {

// Initialize cached classes/methods. Call after unity::cache_initialize() and player_info::initialize().
bool initialize();

// Called every frame to apply aimbot logic.
// Call from LateUpdate (preferred) or Update.
void update();

// Aimbot on/off toggle
void set_enabled(bool enabled);
bool is_enabled();

// Silent mode: bullets go to target, camera stays at real mouse direction
void set_silent(bool silent);
bool is_silent();

// Auto-fire: automatically fire when silent aimbot has a target
void set_auto_fire(bool auto_fire);
bool is_auto_fire();

// Debug info
const char* get_debug_info();

// Cleanup
void shutdown();

} // namespace aimbot
