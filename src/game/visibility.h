#pragma once

#include "../unity/unity_types.h"
#include <string>

namespace visibility {

// Initialize: resolve BspTrace classes/methods and create cached objects.
// Call after unity::cache_initialize().
bool initialize();

// Check if a line from 'from' to 'to' is clear (no solid walls).
// Both positions in SSJJ coordinate space (game physics coords).
// Returns true if the line is unobstructed (fraction == 1.0).
bool is_visible_ssjj(const unity::Vector3& from_ssjj, const unity::Vector3& to_ssjj);

// Check if a line from 'from' to 'to' is clear.
// Both positions in Unity world space. Automatically converts to SSJJ coords.
bool is_visible_unity(const unity::Vector3& from_unity, const unity::Vector3& to_unity);

// Debug info
const char* get_debug_info();

// Diagnostic dump (call from dump button context)
std::string dump_diagnostics();

// Cleanup
void shutdown();

} // namespace visibility
