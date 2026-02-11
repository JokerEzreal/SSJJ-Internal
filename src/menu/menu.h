#pragma once

namespace menu {

// One-time setup. Call after gui::initialize().
void initialize();

// Called every frame during the OnGUI callback context.
void on_gui();

// Called every frame during the Update callback context.
void on_update();

// Returns true when the menu overlay is currently shown.
bool is_visible();

} // namespace menu
