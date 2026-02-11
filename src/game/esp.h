#pragma once

namespace esp {

// Initialize: cache classes/methods, create drawing texture.
// Call after unity::cache_initialize() and player_info::initialize().
bool initialize();

// Draw ESP overlay. Must be called during OnGUI context.
void draw();

// Toggle ESP on/off.
void set_enabled(bool enabled);
bool is_enabled();

// Debug info string
const char* get_esp_debug();

void shutdown();

} // namespace esp
