#pragma once

#include "../unity/unity_types.h"
#include <string>

// Forward declaration to avoid pulling in mono_types.h
struct _MonoObject;
typedef struct _MonoObject MonoObject;

namespace gui {

// Must be called after unity::cache_initialize().
bool initialize();

// ---------------------------------------------------------------------------
// Immediate-mode GUI wrappers (UnityEngine.GUI)
// These MUST only be called during an OnGUI callback context.
// ---------------------------------------------------------------------------

void        label(const unity::Rect& pos, const char* text);
void        box(const unity::Rect& pos, const char* text);
bool        button(const unity::Rect& pos, const char* text);
bool        toggle(const unity::Rect& pos, bool value, const char* text);
float       horizontal_slider(const unity::Rect& pos, float value, float left, float right);
std::string text_field(const unity::Rect& pos, const char* text);
void        drag_window();
void        set_color(const unity::Color& color);
unity::Color get_color();
void        draw_texture(const unity::Rect& pos, MonoObject* texture);

// Font / style helpers for the default Label style
void        set_label_font_size(int size);
int         get_label_font_size();
void        set_label_alignment(int alignment);  // TextAnchor enum
int         get_label_alignment();

// TextAnchor constants
namespace text_anchor {
    constexpr int UpperLeft   = 0;
    constexpr int UpperCenter = 1;
    constexpr int UpperRight  = 2;
    constexpr int MiddleLeft  = 3;
    constexpr int MiddleCenter= 4;
    constexpr int MiddleRight = 5;
    constexpr int LowerLeft   = 6;
    constexpr int LowerCenter = 7;
    constexpr int LowerRight  = 8;
}

// ---------------------------------------------------------------------------
// GUILayout wrappers (UnityEngine.GUILayout)
// ---------------------------------------------------------------------------
namespace layout {
    void label(const char* text);
    bool button(const char* text);
    bool toggle(bool value, const char* text);
    void begin_vertical();
    void end_vertical();
    void begin_horizontal();
    void end_horizontal();
    void begin_area(const unity::Rect& rect);
    void end_area();
    void space(float pixels);
    void flexible_space();
}

// ---------------------------------------------------------------------------
// Screen info
// ---------------------------------------------------------------------------
int screen_width();
int screen_height();

// ---------------------------------------------------------------------------
// Input helpers
// ---------------------------------------------------------------------------
bool get_key_down(int keycode);
bool get_key(int keycode);
bool get_mouse_button(int button);
bool get_mouse_button_down(int button);
// Returns mouse position in GUI coordinates (y=0 at top).
unity::Vector2 get_mouse_position_gui();

// ---------------------------------------------------------------------------
// Unity KeyCode constants (subset)
// ---------------------------------------------------------------------------
namespace keycode {
    constexpr int Insert = 277;
    constexpr int Delete = 127;
    constexpr int Home   = 278;
    constexpr int End    = 279;
    constexpr int F1     = 282;
    constexpr int F2     = 283;
    constexpr int F3     = 284;
    constexpr int F4     = 285;
    constexpr int F5     = 286;
    constexpr int F6     = 287;
    constexpr int F7     = 288;
    constexpr int F8     = 289;
    constexpr int F9     = 290;
    constexpr int F10    = 291;
    constexpr int F11    = 292;
    constexpr int F12    = 293;
}

} // namespace gui
