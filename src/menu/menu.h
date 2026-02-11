#pragma once

namespace menu {

// One-time setup. Call after gui::initialize().
void initialize();

// Called every frame during the OnGUI callback context (menu window only).
// ESP is drawn in a separate callback to isolate exceptions.
void on_gui_menu();

// Called every frame during the Update callback context.
void on_update();

// Returns true when the menu overlay is currently shown.
bool is_visible();

} // namespace menu
