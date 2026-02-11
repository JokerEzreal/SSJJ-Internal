#pragma once

#include "../mono/mono_types.h"

namespace unity {

// Cached Mono image handles for the assemblies we care about
struct CachedImages {
    MonoImage* unity_engine    = nullptr;  // UnityEngine or UnityEngine.CoreModule
    MonoImage* imgui_module    = nullptr;  // UnityEngine.IMGUIModule (may be same as unity_engine)
    MonoImage* input_module    = nullptr;  // UnityEngine.InputLegacyModule (may be same as unity_engine)
    MonoImage* assembly_csharp = nullptr;  // Assembly-CSharp
    MonoImage* entitas_lib     = nullptr;  // SSJJEntitas_Library (game ECS)
    MonoImage* contract_lib    = nullptr;  // SSJJ.Contract (network data)
    MonoImage* physics_lib     = nullptr;  // SSJJPhysics_Library (BSP physics)
    MonoImage* physics_net     = nullptr;  // physics.net (trace types)
};

// Cached MonoClass* handles for Unity engine classes
struct CachedClasses {
    MonoClass* GUI            = nullptr;
    MonoClass* GUILayout      = nullptr;
    MonoClass* GUIStyle       = nullptr;
    MonoClass* GUISkin        = nullptr;
    MonoClass* GUIContent     = nullptr;
    MonoClass* GUIUtility     = nullptr;
    MonoClass* Rect           = nullptr;
    MonoClass* Screen         = nullptr;
    MonoClass* GameObject     = nullptr;
    MonoClass* Object         = nullptr;  // UnityEngine.Object
    MonoClass* MonoBehaviour  = nullptr;
    MonoClass* Input          = nullptr;
    MonoClass* KeyCode        = nullptr;
    MonoClass* Color          = nullptr;
    MonoClass* Camera         = nullptr;
    MonoClass* Texture2D      = nullptr;
    MonoClass* Transform      = nullptr;
};

// Cached MonoMethod* handles for frequently-used Unity methods
struct CachedMethods {
    // ---- GUI static methods ----
    MonoMethod* GUI_Label_2               = nullptr;  // GUI.Label(Rect, string)
    MonoMethod* GUI_Box_2                 = nullptr;  // GUI.Box(Rect, string)
    MonoMethod* GUI_Button_2              = nullptr;  // GUI.Button(Rect, string) -> bool
    MonoMethod* GUI_Toggle_3              = nullptr;  // GUI.Toggle(Rect, bool, string) -> bool
    MonoMethod* GUI_Window_4              = nullptr;  // GUI.Window(int, Rect, WindowFunction, string) -> Rect
    MonoMethod* GUI_DragWindow_0          = nullptr;  // GUI.DragWindow()
    MonoMethod* GUI_HorizontalSlider_5    = nullptr;  // GUI.HorizontalSlider(Rect, float, float, float, ...) -> float
    MonoMethod* GUI_TextField_2           = nullptr;  // GUI.TextField(Rect, string) -> string

    // ---- GUILayout static methods ----
    MonoMethod* GUILayout_Label_1         = nullptr;  // GUILayout.Label(string)
    MonoMethod* GUILayout_Button_1        = nullptr;  // GUILayout.Button(string) -> bool
    MonoMethod* GUILayout_Toggle_2        = nullptr;  // GUILayout.Toggle(bool, string) -> bool
    MonoMethod* GUILayout_BeginVertical_0   = nullptr;
    MonoMethod* GUILayout_EndVertical_0     = nullptr;
    MonoMethod* GUILayout_BeginHorizontal_0 = nullptr;
    MonoMethod* GUILayout_EndHorizontal_0   = nullptr;
    MonoMethod* GUILayout_BeginArea_1     = nullptr;  // GUILayout.BeginArea(Rect)
    MonoMethod* GUILayout_EndArea_0       = nullptr;
    MonoMethod* GUILayout_Space_1         = nullptr;  // GUILayout.Space(float)
    MonoMethod* GUILayout_FlexibleSpace_0 = nullptr;
    MonoMethod* GUILayout_Width_1         = nullptr;  // GUILayout.Width(float)
    MonoMethod* GUILayout_Height_1        = nullptr;  // GUILayout.Height(float)

    // ---- GUIStyle ----
    MonoMethod* GUIStyle_set_fontSize     = nullptr;
    MonoMethod* GUIStyle_set_richText     = nullptr;
    MonoMethod* GUIStyle_set_alignment    = nullptr;

    // ---- Screen ----
    MonoMethod* Screen_get_width          = nullptr;
    MonoMethod* Screen_get_height         = nullptr;

    // ---- Input ----
    MonoMethod* Input_GetKeyDown          = nullptr;  // Input.GetKeyDown(KeyCode) -> bool
    MonoMethod* Input_GetKey              = nullptr;  // Input.GetKey(KeyCode) -> bool
    MonoMethod* Input_GetMouseButton      = nullptr;  // Input.GetMouseButton(int) -> bool
    MonoMethod* Input_GetMouseButtonDown  = nullptr;  // Input.GetMouseButtonDown(int) -> bool
    MonoMethod* Input_get_mousePosition   = nullptr;  // Input.mousePosition -> Vector3

    // ---- GUIContent ----
    MonoMethod* GUIContent_ctor_1         = nullptr;  // new GUIContent(string)

    // ---- GUISkin / GUIUtility ----
    MonoMethod* GUIUtility_GetBuiltinSkin = nullptr;

    // ---- GUI drawing / color ----
    MonoMethod* GUI_DrawTexture_2         = nullptr;  // GUI.DrawTexture(Rect, Texture)
    MonoMethod* GUI_set_color             = nullptr;  // GUI.set_color(Color)
    MonoMethod* GUI_get_color             = nullptr;  // GUI.get_color() -> Color

    // ---- GUI skin / style ----
    MonoMethod* GUI_get_skin              = nullptr;  // GUI.get_skin() -> GUISkin
    MonoMethod* GUISkin_get_label         = nullptr;  // GUISkin.get_label() -> GUIStyle
    MonoMethod* GUIStyle_get_fontSize     = nullptr;  // GUIStyle.get_fontSize() -> int
    MonoMethod* GUIStyle_get_alignment    = nullptr;  // GUIStyle.get_alignment() -> int

    // ---- Camera ----
    MonoMethod* Camera_get_main           = nullptr;  // Camera.main (static)
    MonoMethod* Camera_WorldToScreenPoint = nullptr;  // Camera.WorldToScreenPoint(Vector3)
    MonoMethod* Camera_get_pixelWidth     = nullptr;
    MonoMethod* Camera_get_pixelHeight    = nullptr;

    // ---- Texture2D ----
    MonoMethod* Texture2D_ctor_2          = nullptr;  // new Texture2D(int, int)
    MonoMethod* Texture2D_SetPixel        = nullptr;  // SetPixel(int, int, Color)
    MonoMethod* Texture2D_Apply_0         = nullptr;  // Apply()

    // ---- Transform ----
    MonoMethod* Transform_get_position    = nullptr;  // Transform.position -> Vector3
    MonoMethod* Transform_get_childCount  = nullptr;  // Transform.childCount -> int
    MonoMethod* Transform_GetChild        = nullptr;  // Transform.GetChild(int) -> Transform
    MonoMethod* Transform_get_parent      = nullptr;  // Transform.parent -> Transform

    // ---- GameObject ----
    MonoMethod* GameObject_get_transform  = nullptr;  // GameObject.transform -> Transform

    // ---- GUIUtility ----
    MonoMethod* GUIUtility_RotateAroundPivot = nullptr;  // RotateAroundPivot(float, Vector2)

    // ---- Object ----
    MonoMethod* Object_get_name           = nullptr;  // UnityEngine.Object.name -> string
    MonoMethod* Object_DontDestroyOnLoad  = nullptr;  // static DontDestroyOnLoad(Object)
    MonoMethod* Object_set_hideFlags      = nullptr;  // Object.hideFlags = HideFlags
};

// Initialize the class/method cache. Returns false if critical classes are missing.
bool cache_initialize();

// Release cached references (safe to call multiple times).
void cache_shutdown();

// Accessors for the global cached data.
CachedImages&  images();
CachedClasses& classes();
CachedMethods& methods();

} // namespace unity
