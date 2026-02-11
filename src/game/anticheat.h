#pragma once

#include <string>

namespace anticheat {

// Initialize: resolve anti-cheat classes/fields. Call after unity::cache_initialize().
bool initialize();

// Apply patches every frame. Call from on_update or on_late_update.
void update();

// Debug info
const char* get_debug_info();

// Diagnostics for dump
std::string dump_diagnostics();

// Cleanup
void shutdown();

} // namespace anticheat
