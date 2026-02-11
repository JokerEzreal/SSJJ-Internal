#include "unity_classes.h"
#include "../mono/mono_api.h"

#include <cstring>
#include <vector>

namespace unity {

// ---------------------------------------------------------------------------
// Static storage
// ---------------------------------------------------------------------------
static CachedImages  s_images;
static CachedClasses s_classes;
static CachedMethods s_methods;

CachedImages&  images()  { return s_images; }
CachedClasses& classes() { return s_classes; }
CachedMethods& methods() { return s_methods; }

// ---------------------------------------------------------------------------
// Assembly enumeration callback
// ---------------------------------------------------------------------------

// Temporary structure used during assembly iteration.
struct AssemblyCollector {
    std::vector<MonoAssembly*> assemblies;
};

static void assembly_iter_callback(MonoAssembly* assembly, void* user_data) {
    auto* collector = static_cast<AssemblyCollector*>(user_data);
    collector->assemblies.push_back(assembly);
}

// ---------------------------------------------------------------------------
// Helper: try to find a class in multiple images (returns first hit)
// ---------------------------------------------------------------------------
static MonoClass* find_class(const char* ns, const char* name,
                             MonoImage* primary, MonoImage* fallback = nullptr)
{
    MonoClass* cls = nullptr;
    if (primary) {
        cls = mono::class_from_name(primary, ns, name);
    }
    if (!cls && fallback && fallback != primary) {
        cls = mono::class_from_name(fallback, ns, name);
    }
    return cls;
}

// ---------------------------------------------------------------------------
// cache_initialize
// ---------------------------------------------------------------------------
bool cache_initialize() {
    // Step 1 -- Collect all loaded assemblies
    AssemblyCollector collector;
    mono::assembly_foreach(assembly_iter_callback, &collector);

    // Step 2 -- Identify the images we need
    for (MonoAssembly* asm_ptr : collector.assemblies) {
        MonoImage* img = mono::assembly_get_image(asm_ptr);
        if (!img) continue;

        const char* name = mono::image_get_name(img);
        if (!name) continue;

        if (strcmp(name, "UnityEngine") == 0) {
            // Older Unity: single UnityEngine assembly holds everything
            s_images.unity_engine = img;
            // In older builds, IMGUI lives in the same image
            if (!s_images.imgui_module)
                s_images.imgui_module = img;
        }
        else if (strcmp(name, "UnityEngine.CoreModule") == 0) {
            s_images.unity_engine = img;
        }
        else if (strcmp(name, "UnityEngine.IMGUIModule") == 0) {
            s_images.imgui_module = img;
        }
        else if (strcmp(name, "UnityEngine.InputLegacyModule") == 0) {
            s_images.input_module = img;
        }
        else if (strcmp(name, "Assembly-CSharp") == 0) {
            s_images.assembly_csharp = img;
        }
        else if (strcmp(name, "SSJJEntitas_Library") == 0) {
            s_images.entitas_lib = img;
        }
        else if (strcmp(name, "SSJJ.Contract") == 0) {
            s_images.contract_lib = img;
        }
    }

    // If we never found dedicated modules, fall back to main engine image
    if (!s_images.imgui_module)
        s_images.imgui_module = s_images.unity_engine;
    if (!s_images.input_module)
        s_images.input_module = s_images.unity_engine;

    if (!s_images.unity_engine) {
        // Cannot proceed without at least the core engine image
        return false;
    }

    // Step 3 -- Resolve classes
    // GUI-related classes may live in the IMGUI module or the main engine image.
    MonoImage* gui_img  = s_images.imgui_module;
    MonoImage* core_img = s_images.unity_engine;

    s_classes.GUI           = find_class("UnityEngine", "GUI",           gui_img, core_img);
    s_classes.GUILayout     = find_class("UnityEngine", "GUILayout",     gui_img, core_img);
    s_classes.GUIStyle      = find_class("UnityEngine", "GUIStyle",      gui_img, core_img);
    s_classes.GUISkin       = find_class("UnityEngine", "GUISkin",       gui_img, core_img);
    s_classes.GUIContent    = find_class("UnityEngine", "GUIContent",    gui_img, core_img);
    s_classes.GUIUtility    = find_class("UnityEngine", "GUIUtility",    gui_img, core_img);
    s_classes.Rect          = find_class("UnityEngine", "Rect",          core_img, gui_img);
    s_classes.Screen        = find_class("UnityEngine", "Screen",        core_img, gui_img);
    s_classes.GameObject    = find_class("UnityEngine", "GameObject",    core_img);
    s_classes.Object        = find_class("UnityEngine", "Object",        core_img);
    s_classes.MonoBehaviour = find_class("UnityEngine", "MonoBehaviour", core_img);
    MonoImage* input_img = s_images.input_module;
    s_classes.Input         = find_class("UnityEngine", "Input",         input_img, core_img);
    s_classes.KeyCode       = find_class("UnityEngine", "KeyCode",       input_img, core_img);
    s_classes.Color         = find_class("UnityEngine", "Color",         core_img);
    s_classes.Camera        = find_class("UnityEngine", "Camera",        core_img);
    s_classes.Texture2D     = find_class("UnityEngine", "Texture2D",     core_img);
    s_classes.Transform     = find_class("UnityEngine", "Transform",     core_img);

    // Critical check -- we absolutely need GUI and Screen at minimum
    if (!s_classes.GUI || !s_classes.Screen) {
        return false;
    }

    // Step 4 -- Resolve methods
    // The number argument to class_get_method_from_name is the C# parameter count.

    // ---- GUI ----
    if (s_classes.GUI) {
        s_methods.GUI_Label_2            = mono::class_get_method_from_name(s_classes.GUI, "Label", 2);
        s_methods.GUI_Box_2              = mono::class_get_method_from_name(s_classes.GUI, "Box", 2);
        s_methods.GUI_Button_2           = mono::class_get_method_from_name(s_classes.GUI, "Button", 2);
        s_methods.GUI_Toggle_3           = mono::class_get_method_from_name(s_classes.GUI, "Toggle", 3);
        s_methods.GUI_Window_4           = mono::class_get_method_from_name(s_classes.GUI, "Window", 4);
        s_methods.GUI_DragWindow_0       = mono::class_get_method_from_name(s_classes.GUI, "DragWindow", 0);
        s_methods.GUI_HorizontalSlider_5 = mono::class_get_method_from_name(s_classes.GUI, "HorizontalSlider", 5);
        s_methods.GUI_TextField_2        = mono::class_get_method_from_name(s_classes.GUI, "TextField", 2);
    }

    // ---- GUILayout ----
    if (s_classes.GUILayout) {
        s_methods.GUILayout_Label_1           = mono::class_get_method_from_name(s_classes.GUILayout, "Label", 1);
        s_methods.GUILayout_Button_1          = mono::class_get_method_from_name(s_classes.GUILayout, "Button", 1);
        s_methods.GUILayout_Toggle_2          = mono::class_get_method_from_name(s_classes.GUILayout, "Toggle", 2);
        s_methods.GUILayout_BeginVertical_0   = mono::class_get_method_from_name(s_classes.GUILayout, "BeginVertical", 0);
        s_methods.GUILayout_EndVertical_0     = mono::class_get_method_from_name(s_classes.GUILayout, "EndVertical", 0);
        s_methods.GUILayout_BeginHorizontal_0 = mono::class_get_method_from_name(s_classes.GUILayout, "BeginHorizontal", 0);
        s_methods.GUILayout_EndHorizontal_0   = mono::class_get_method_from_name(s_classes.GUILayout, "EndHorizontal", 0);
        s_methods.GUILayout_BeginArea_1       = mono::class_get_method_from_name(s_classes.GUILayout, "BeginArea", 1);
        s_methods.GUILayout_EndArea_0         = mono::class_get_method_from_name(s_classes.GUILayout, "EndArea", 0);
        s_methods.GUILayout_Space_1           = mono::class_get_method_from_name(s_classes.GUILayout, "Space", 1);
        s_methods.GUILayout_FlexibleSpace_0   = mono::class_get_method_from_name(s_classes.GUILayout, "FlexibleSpace", 0);
        s_methods.GUILayout_Width_1           = mono::class_get_method_from_name(s_classes.GUILayout, "Width", 1);
        s_methods.GUILayout_Height_1          = mono::class_get_method_from_name(s_classes.GUILayout, "Height", 1);
    }

    // ---- GUIStyle ----
    if (s_classes.GUIStyle) {
        s_methods.GUIStyle_set_fontSize  = mono::class_get_method_from_name(s_classes.GUIStyle, "set_fontSize", 1);
        s_methods.GUIStyle_set_richText  = mono::class_get_method_from_name(s_classes.GUIStyle, "set_richText", 1);
        s_methods.GUIStyle_set_alignment = mono::class_get_method_from_name(s_classes.GUIStyle, "set_alignment", 1);
        s_methods.GUIStyle_get_fontSize  = mono::class_get_method_from_name(s_classes.GUIStyle, "get_fontSize", 0);
        s_methods.GUIStyle_get_alignment = mono::class_get_method_from_name(s_classes.GUIStyle, "get_alignment", 0);
    }

    // ---- Screen ----
    if (s_classes.Screen) {
        s_methods.Screen_get_width  = mono::class_get_method_from_name(s_classes.Screen, "get_width", 0);
        s_methods.Screen_get_height = mono::class_get_method_from_name(s_classes.Screen, "get_height", 0);
    }

    // ---- Input ----
    if (s_classes.Input) {
        s_methods.Input_GetKeyDown          = mono::class_get_method_from_name(s_classes.Input, "GetKeyDown", 1);
        s_methods.Input_GetKey              = mono::class_get_method_from_name(s_classes.Input, "GetKey", 1);
        s_methods.Input_GetMouseButton      = mono::class_get_method_from_name(s_classes.Input, "GetMouseButton", 1);
        s_methods.Input_GetMouseButtonDown  = mono::class_get_method_from_name(s_classes.Input, "GetMouseButtonDown", 1);
        s_methods.Input_get_mousePosition   = mono::class_get_method_from_name(s_classes.Input, "get_mousePosition", 0);
    }

    // ---- GUIContent ----
    if (s_classes.GUIContent) {
        s_methods.GUIContent_ctor_1 = mono::class_get_method_from_name(s_classes.GUIContent, ".ctor", 1);
    }

    // ---- GUIUtility ----
    if (s_classes.GUIUtility) {
        s_methods.GUIUtility_GetBuiltinSkin = mono::class_get_method_from_name(s_classes.GUIUtility, "GetBuiltinSkin", 1);
    }

    // ---- GUI drawing / color / skin ----
    if (s_classes.GUI) {
        s_methods.GUI_DrawTexture_2 = mono::class_get_method_from_name(s_classes.GUI, "DrawTexture", 2);
        s_methods.GUI_set_color     = mono::class_get_method_from_name(s_classes.GUI, "set_color", 1);
        s_methods.GUI_get_color     = mono::class_get_method_from_name(s_classes.GUI, "get_color", 0);
        s_methods.GUI_get_skin      = mono::class_get_method_from_name(s_classes.GUI, "get_skin", 0);
    }

    // ---- GUISkin ----
    if (s_classes.GUISkin) {
        s_methods.GUISkin_get_label = mono::class_get_method_from_name(s_classes.GUISkin, "get_label", 0);
    }

    // ---- Camera ----
    if (s_classes.Camera) {
        s_methods.Camera_get_main           = mono::class_get_method_from_name(s_classes.Camera, "get_main", 0);
        s_methods.Camera_WorldToScreenPoint = mono::class_get_method_from_name(s_classes.Camera, "WorldToScreenPoint", 1);
        s_methods.Camera_get_pixelWidth     = mono::class_get_method_from_name(s_classes.Camera, "get_pixelWidth", 0);
        s_methods.Camera_get_pixelHeight    = mono::class_get_method_from_name(s_classes.Camera, "get_pixelHeight", 0);
    }

    // ---- Texture2D ----
    if (s_classes.Texture2D) {
        s_methods.Texture2D_ctor_2   = mono::class_get_method_from_name(s_classes.Texture2D, ".ctor", 2);
        s_methods.Texture2D_SetPixel = mono::class_get_method_from_name(s_classes.Texture2D, "SetPixel", 3);
        s_methods.Texture2D_Apply_0  = mono::class_get_method_from_name(s_classes.Texture2D, "Apply", 0);
    }

    // ---- Transform ----
    if (s_classes.Transform) {
        s_methods.Transform_get_position   = mono::class_get_method_from_name(s_classes.Transform, "get_position", 0);
        s_methods.Transform_get_childCount = mono::class_get_method_from_name(s_classes.Transform, "get_childCount", 0);
        s_methods.Transform_GetChild       = mono::class_get_method_from_name(s_classes.Transform, "GetChild", 1);
        s_methods.Transform_get_parent     = mono::class_get_method_from_name(s_classes.Transform, "get_parent", 0);
    }

    // ---- GameObject ----
    if (s_classes.GameObject) {
        s_methods.GameObject_get_transform = mono::class_get_method_from_name(s_classes.GameObject, "get_transform", 0);
    }

    // ---- GUIUtility (additional) ----
    if (s_classes.GUIUtility) {
        s_methods.GUIUtility_RotateAroundPivot = mono::class_get_method_from_name(s_classes.GUIUtility, "RotateAroundPivot", 2);
    }

    // ---- Object ----
    if (s_classes.Object) {
        s_methods.Object_get_name = mono::class_get_method_from_name(s_classes.Object, "get_name", 0);
        s_methods.Object_DontDestroyOnLoad = mono::class_get_method_from_name(s_classes.Object, "DontDestroyOnLoad", 1);
        s_methods.Object_set_hideFlags = mono::class_get_method_from_name(s_classes.Object, "set_hideFlags", 1);
    }

    return true;
}

// ---------------------------------------------------------------------------
// cache_shutdown
// ---------------------------------------------------------------------------
void cache_shutdown() {
    s_images  = {};
    s_classes = {};
    s_methods = {};
}

} // namespace unity
