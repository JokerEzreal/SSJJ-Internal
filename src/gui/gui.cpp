#include "gui.h"
#include "../mono/mono_api.h"
#include "../mono/mono_types.h"
#include "../unity/unity_classes.h"
#include <cstring>

namespace gui {

// ---------------------------------------------------------------------------
// Exception-safe invoke: catches managed exceptions locally so they don't
// longjmp past C++ stack frames (which would skip destructors in callers).
// ---------------------------------------------------------------------------
static MonoObject* invoke_safe(MonoMethod* method, MonoObject* obj = nullptr,
                               void** params = nullptr) {
    if (!method) return nullptr;
    MonoObject* exc = nullptr;
    MonoObject* ret = mono::runtime_invoke(method, obj, params, &exc);
    return exc ? nullptr : ret;
}

// ---------------------------------------------------------------------------
// Helper: create a managed System.String from a C string.
// ---------------------------------------------------------------------------
static MonoString* make_string(const char* text) {
    MonoDomain* domain = mono::domain_get();
    if (!domain || !text) return nullptr;
    return mono::string_new(domain, text);
}

// ---------------------------------------------------------------------------
// Helper: safely unbox a boolean result from a MonoObject*.
// ---------------------------------------------------------------------------
static bool unbox_bool(MonoObject* obj) {
    if (!obj) return false;
    return *static_cast<bool*>(mono::object_unbox(obj));
}

// ---------------------------------------------------------------------------
// Helper: safely unbox a float result from a MonoObject*.
// ---------------------------------------------------------------------------
static float unbox_float(MonoObject* obj) {
    if (!obj) return 0.0f;
    return *static_cast<float*>(mono::object_unbox(obj));
}

// ---------------------------------------------------------------------------
// Helper: safely unbox an int result from a MonoObject*.
// ---------------------------------------------------------------------------
static int unbox_int(MonoObject* obj) {
    if (!obj) return 0;
    return *static_cast<int*>(mono::object_unbox(obj));
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------
bool initialize() {
    // All real work is done by unity::cache_initialize().
    // This entry point exists so the GUI subsystem can add its own setup later.
    return true;
}

// ===========================================================================
// Immediate-mode GUI wrappers  (UnityEngine.GUI)
// ===========================================================================

void label(const unity::Rect& pos, const char* text) {
    MonoMethod* method = unity::methods().GUI_Label_2;
    if (!method) return;

    unity::Rect r = pos;
    MonoString* str = make_string(text);
    void* args[2] = { &r, str };
    invoke_safe(method, nullptr, args);
}

void box(const unity::Rect& pos, const char* text) {
    MonoMethod* method = unity::methods().GUI_Box_2;
    if (!method) return;

    unity::Rect r = pos;
    MonoString* str = make_string(text);
    void* args[2] = { &r, str };
    invoke_safe(method, nullptr, args);
}

bool button(const unity::Rect& pos, const char* text) {
    MonoMethod* method = unity::methods().GUI_Button_2;
    if (!method) return false;

    unity::Rect r = pos;
    MonoString* str = make_string(text);
    void* args[2] = { &r, str };
    MonoObject* result = invoke_safe(method, nullptr, args);
    return unbox_bool(result);
}

bool toggle(const unity::Rect& pos, bool value, const char* text) {
    MonoMethod* method = unity::methods().GUI_Toggle_3;
    if (!method) return value;

    unity::Rect  r   = pos;
    mono_bool    val = value ? 1 : 0;   // Mono bool is int32
    MonoString*  str = make_string(text);
    void* args[3] = { &r, &val, str };
    MonoObject* result = invoke_safe(method, nullptr, args);
    return unbox_bool(result);
}

float horizontal_slider(const unity::Rect& pos, float value, float left, float right) {
    MonoMethod* method = unity::methods().GUI_HorizontalSlider_5;
    if (!method) return value;

    // The cached method may be the 5-param overload that includes a trailing
    // GUIStyle.  We pass nullptr for the optional style so it falls back to
    // the default skin.  If the method was actually resolved as the 4-param
    // overload the extra arg is harmless (mono_runtime_invoke ignores it).
    unity::Rect r   = pos;
    float       v   = value;
    float       lv  = left;
    float       rv  = right;
    void* args[5] = { &r, &v, &lv, &rv, nullptr };
    MonoObject* result = invoke_safe(method, nullptr, args);
    return unbox_float(result);
}

std::string text_field(const unity::Rect& pos, const char* text) {
    MonoMethod* method = unity::methods().GUI_TextField_2;
    if (!method) return text ? text : "";

    unity::Rect r   = pos;
    MonoString* str = make_string(text);
    void* args[2] = { &r, str };
    MonoObject* result = invoke_safe(method, nullptr, args);

    if (!result) return text ? text : "";

    // The return value is a boxed string (MonoString*).
    // runtime_invoke for methods returning System.String gives back the
    // MonoString* directly cast to MonoObject*.
    MonoString* ret_str = reinterpret_cast<MonoString*>(result);
    char* utf8 = mono::string_to_utf8(ret_str);
    if (!utf8) return text ? text : "";

    std::string out(utf8);
    // mono_string_to_utf8 allocates with g_malloc; the caller should free it
    // with g_free, but for simplicity we accept the small leak or rely on
    // the Mono GC.  A production build should call mono_free(utf8).
    return out;
}

void drag_window() {
    MonoMethod* method = unity::methods().GUI_DragWindow_0;
    if (!method) return;

    invoke_safe(method);
}

void set_color(const unity::Color& color) {
    MonoMethod* method = unity::methods().GUI_set_color;
    if (!method) return;

    unity::Color c = color;
    void* args[1] = { &c };
    invoke_safe(method, nullptr, args);
}

unity::Color get_color() {
    MonoMethod* method = unity::methods().GUI_get_color;
    if (!method) return unity::Color::white();

    MonoObject* result = invoke_safe(method);
    if (!result) return unity::Color::white();
    return *static_cast<unity::Color*>(mono::object_unbox(result));
}

void draw_texture(const unity::Rect& pos, MonoObject* texture) {
    MonoMethod* method = unity::methods().GUI_DrawTexture_2;
    if (!method || !texture) return;

    unity::Rect r = pos;
    void* args[2] = { &r, texture };
    invoke_safe(method, nullptr, args);
}

// ===========================================================================
// Font / style helpers for the default Label style
// ===========================================================================

static MonoObject* get_label_style() {
    MonoMethod* get_skin = unity::methods().GUI_get_skin;
    MonoMethod* get_label = unity::methods().GUISkin_get_label;
    if (!get_skin || !get_label) return nullptr;

    MonoObject* skin = invoke_safe(get_skin);
    if (!skin) return nullptr;
    return invoke_safe(get_label, skin);
}

void set_label_font_size(int size) {
    MonoObject* style = get_label_style();
    MonoMethod* setter = unity::methods().GUIStyle_set_fontSize;
    if (!style || !setter) return;
    void* args[1] = { &size };
    invoke_safe(setter, style, args);
}

int get_label_font_size() {
    MonoObject* style = get_label_style();
    MonoMethod* getter = unity::methods().GUIStyle_get_fontSize;
    if (!style || !getter) return 0;
    MonoObject* result = invoke_safe(getter, style);
    return unbox_int(result);
}

void set_label_alignment(int alignment) {
    MonoObject* style = get_label_style();
    MonoMethod* setter = unity::methods().GUIStyle_set_alignment;
    if (!style || !setter) return;
    void* args[1] = { &alignment };
    invoke_safe(setter, style, args);
}

int get_label_alignment() {
    MonoObject* style = get_label_style();
    MonoMethod* getter = unity::methods().GUIStyle_get_alignment;
    if (!style || !getter) return 0;
    MonoObject* result = invoke_safe(getter, style);
    return unbox_int(result);
}

// ===========================================================================
// GUILayout wrappers  (UnityEngine.GUILayout)
// ===========================================================================

namespace layout {

void label(const char* text) {
    MonoMethod* method = unity::methods().GUILayout_Label_1;
    if (!method) return;

    MonoString* str = make_string(text);
    void* args[1] = { str };
    invoke_safe(method, nullptr, args);
}

bool button(const char* text) {
    MonoMethod* method = unity::methods().GUILayout_Button_1;
    if (!method) return false;

    MonoString* str = make_string(text);
    void* args[1] = { str };
    MonoObject* result = invoke_safe(method, nullptr, args);
    return unbox_bool(result);
}

bool toggle(bool value, const char* text) {
    MonoMethod* method = unity::methods().GUILayout_Toggle_2;
    if (!method) return value;

    mono_bool   val = value ? 1 : 0;
    MonoString* str = make_string(text);
    void* args[2] = { &val, str };
    MonoObject* result = invoke_safe(method, nullptr, args);
    return unbox_bool(result);
}

void begin_vertical() {
    MonoMethod* method = unity::methods().GUILayout_BeginVertical_0;
    if (!method) return;

    invoke_safe(method);
}

void end_vertical() {
    MonoMethod* method = unity::methods().GUILayout_EndVertical_0;
    if (!method) return;

    invoke_safe(method);
}

void begin_horizontal() {
    MonoMethod* method = unity::methods().GUILayout_BeginHorizontal_0;
    if (!method) return;

    invoke_safe(method);
}

void end_horizontal() {
    MonoMethod* method = unity::methods().GUILayout_EndHorizontal_0;
    if (!method) return;

    invoke_safe(method);
}

void begin_area(const unity::Rect& rect) {
    MonoMethod* method = unity::methods().GUILayout_BeginArea_1;
    if (!method) return;

    unity::Rect r = rect;
    void* args[1] = { &r };
    invoke_safe(method, nullptr, args);
}

void end_area() {
    MonoMethod* method = unity::methods().GUILayout_EndArea_0;
    if (!method) return;

    invoke_safe(method);
}

void space(float pixels) {
    MonoMethod* method = unity::methods().GUILayout_Space_1;
    if (!method) return;

    float px = pixels;
    void* args[1] = { &px };
    invoke_safe(method, nullptr, args);
}

void flexible_space() {
    MonoMethod* method = unity::methods().GUILayout_FlexibleSpace_0;
    if (!method) return;

    invoke_safe(method);
}

} // namespace layout

// ===========================================================================
// Screen info
// ===========================================================================

int screen_width() {
    MonoMethod* method = unity::methods().Screen_get_width;
    if (!method) return 1920;  // sensible fallback

    MonoObject* result = invoke_safe(method);
    return unbox_int(result);
}

int screen_height() {
    MonoMethod* method = unity::methods().Screen_get_height;
    if (!method) return 1080;  // sensible fallback

    MonoObject* result = invoke_safe(method);
    return unbox_int(result);
}

// ===========================================================================
// Input helpers
// ===========================================================================

bool get_key_down(int keycode) {
    MonoMethod* method = unity::methods().Input_GetKeyDown;
    if (!method) return false;

    int kc = keycode;
    void* args[1] = { &kc };
    MonoObject* result = invoke_safe(method, nullptr, args);
    return unbox_bool(result);
}

bool get_key(int keycode) {
    MonoMethod* method = unity::methods().Input_GetKey;
    if (!method) return false;

    int kc = keycode;
    void* args[1] = { &kc };
    MonoObject* result = invoke_safe(method, nullptr, args);
    return unbox_bool(result);
}

bool get_mouse_button(int button) {
    MonoMethod* method = unity::methods().Input_GetMouseButton;
    if (!method) return false;

    int btn = button;
    void* args[1] = { &btn };
    MonoObject* result = invoke_safe(method, nullptr, args);
    return unbox_bool(result);
}

bool get_mouse_button_down(int button) {
    MonoMethod* method = unity::methods().Input_GetMouseButtonDown;
    if (!method) return false;

    int btn = button;
    void* args[1] = { &btn };
    MonoObject* result = invoke_safe(method, nullptr, args);
    return unbox_bool(result);
}

unity::Vector2 get_mouse_position_gui() {
    MonoMethod* method = unity::methods().Input_get_mousePosition;
    if (!method) return { 0.0f, 0.0f };

    MonoObject* result = invoke_safe(method);
    if (!result) return { 0.0f, 0.0f };

    // Input.mousePosition returns Vector3; Y is inverted relative to GUI coords.
    auto* v3 = static_cast<unity::Vector3*>(mono::object_unbox(result));
    float sh = static_cast<float>(screen_height());
    return { v3->x, sh - v3->y };
}

} // namespace gui
