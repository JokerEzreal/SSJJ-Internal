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

// Debug info
const char* get_debug_info();

// Cleanup
void shutdown();

} // namespace aimbot
