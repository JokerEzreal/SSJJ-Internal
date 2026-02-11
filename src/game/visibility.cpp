#include "visibility.h"
#include "../mono/mono_api.h"
#include "../mono/mono_types.h"
#include "../unity/unity_classes.h"
#include "../unity/unity_types.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace visibility {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
// Quake-style BSP content flag for solid geometry
static constexpr int CONTENTS_SOLID = 1;

// ---------------------------------------------------------------------------
// Cached handles for BspTrace access path
// ---------------------------------------------------------------------------
static struct {
    // Contexts → BattleRoomContext → PyEngineComponent → BspEngine → BspTrace
    MonoMethod* Contexts_get_sharedInstance = nullptr;
    MonoMethod* Contexts_get_battleRoom    = nullptr;
    MonoMethod* BattleRoomCtx_get_hasPyEngine = nullptr;
    MonoMethod* BattleRoomCtx_get_pyEngine    = nullptr;
    MonoMethod* BspEngine_GetTrace         = nullptr;
    MonoMethod* BspTrace_WorldTrace        = nullptr;

    // physics.trace.Trace
    MonoMethod* Trace_ctor                 = nullptr;
    MonoMethod* Trace_Reset                = nullptr;
} s_methods;

static struct {
    MonoClassField* PyEngine_field         = nullptr;  // PyEngineComponent.PyEngine
    MonoClassField* Trace_fraction         = nullptr;  // Trace.fraction
} s_fields;

static struct {
    MonoClass* Trace                       = nullptr;
    MonoClass* BspTrace                    = nullptr;
    MonoClass* BspEngine                   = nullptr;
} s_classes;

// ---------------------------------------------------------------------------
// Cached managed objects (created once, reused every frame)
// ---------------------------------------------------------------------------
static MonoObject* s_trace_result = nullptr;   // physics.trace.Trace instance
static uint32_t    s_trace_gc     = 0;

static MonoArray*  s_arr_start    = nullptr;   // float[3]
static MonoArray*  s_arr_end      = nullptr;   // float[3]
static MonoArray*  s_arr_mins     = nullptr;   // float[3] = {0,0,0}
static MonoArray*  s_arr_maxs     = nullptr;   // float[3] = {0,0,0}
static uint32_t    s_arr_gc[4]    = {};

static bool        s_initialized  = false;
static std::string s_debug;

// ---------------------------------------------------------------------------
// Helper: get raw data pointer for a Mono single-dimensional array
// On 64-bit Mono: MonoObject header (16 bytes) + length (8 bytes) = 24
// ---------------------------------------------------------------------------
static float* array_data(MonoArray* arr) {
    if (!arr) return nullptr;
    return reinterpret_cast<float*>(reinterpret_cast<char*>(arr) + 24);
}

// ---------------------------------------------------------------------------
// Helper: invoke a method on an object
// ---------------------------------------------------------------------------
static MonoObject* invoke(MonoMethod* method, MonoObject* obj) {
    if (!method) return nullptr;
    return mono::runtime_invoke(method, obj, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// Coordinate conversion
// ---------------------------------------------------------------------------
// Unity(ux, uy, uz) → SSJJ(uz, -ux, uy)
static void unity_to_ssjj(const unity::Vector3& u, float out[3]) {
    out[0] =  u.z;
    out[1] = -u.x;
    out[2] =  u.y;
}

// ---------------------------------------------------------------------------
// Create cached managed arrays and trace object
// ---------------------------------------------------------------------------
static bool create_cached_objects() {
    MonoDomain* domain = mono::domain_get();
    if (!domain) return false;

    MonoClass* float_cls = mono::get_single_class ? mono::get_single_class() : nullptr;
    if (!float_cls || !mono::array_new) return false;

    // Create 4 float[3] arrays
    s_arr_start = mono::array_new(domain, float_cls, 3);
    s_arr_end   = mono::array_new(domain, float_cls, 3);
    s_arr_mins  = mono::array_new(domain, float_cls, 3);
    s_arr_maxs  = mono::array_new(domain, float_cls, 3);

    if (!s_arr_start || !s_arr_end || !s_arr_mins || !s_arr_maxs)
        return false;

    // Zero out mins/maxs (point trace, no hull width)
    memset(array_data(s_arr_mins), 0, 3 * sizeof(float));
    memset(array_data(s_arr_maxs), 0, 3 * sizeof(float));

    // Pin arrays to prevent GC collection
    s_arr_gc[0] = mono::gchandle_new(reinterpret_cast<MonoObject*>(s_arr_start), 1);
    s_arr_gc[1] = mono::gchandle_new(reinterpret_cast<MonoObject*>(s_arr_end),   1);
    s_arr_gc[2] = mono::gchandle_new(reinterpret_cast<MonoObject*>(s_arr_mins),  1);
    s_arr_gc[3] = mono::gchandle_new(reinterpret_cast<MonoObject*>(s_arr_maxs),  1);

    // Create Trace result object
    if (!s_classes.Trace) return false;
    s_trace_result = mono::object_new(domain, s_classes.Trace);
    if (!s_trace_result) return false;

    // Call Trace constructor
    if (s_methods.Trace_ctor) {
        mono::runtime_invoke(s_methods.Trace_ctor, s_trace_result, nullptr, nullptr);
    }

    // Pin trace result
    s_trace_gc = mono::gchandle_new(s_trace_result, 1);

    return true;
}

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------
bool initialize() {
    MonoImage* ent_img = unity::images().entitas_lib;
    MonoImage* phy_img = unity::images().physics_lib;
    MonoImage* pnet_img = unity::images().physics_net;

    if (!ent_img) { s_debug = "VIS: no entitas_lib"; return false; }

    // --- Resolve classes ---
    MonoClass* Contexts = mono::class_from_name(ent_img, "", "Contexts");
    MonoClass* BattleRoomContext = mono::class_from_name(ent_img, "", "BattleRoomContext");
    MonoClass* PyEngineComponent = mono::class_from_name(
        ent_img, "Assets.Sources.Components.BattleRoom", "PyEngineComponent");

    if (!Contexts || !BattleRoomContext) {
        s_debug = "VIS: no Contexts/BattleRoomContext";
        return false;
    }

    // BspEngine and BspTrace from SSJJPhysics_Library
    if (phy_img) {
        s_classes.BspEngine = mono::class_from_name(phy_img, "SSJJPhysics", "BspEngine");
        s_classes.BspTrace  = mono::class_from_name(phy_img, "SSJJPhysics", "BspTrace");
    }

    // physics.trace.Trace from physics.net
    if (pnet_img) {
        s_classes.Trace = mono::class_from_name(pnet_img, "physics.trace", "Trace");
    }

    // --- Resolve methods ---
    s_methods.Contexts_get_sharedInstance =
        mono::class_get_method_from_name(Contexts, "get_sharedInstance", 0);
    s_methods.Contexts_get_battleRoom =
        mono::class_get_method_from_name(Contexts, "get_battleRoom", 0);

    if (BattleRoomContext) {
        s_methods.BattleRoomCtx_get_hasPyEngine =
            mono::class_get_method_from_name(BattleRoomContext, "get_hasPyEngine", 0);
        s_methods.BattleRoomCtx_get_pyEngine =
            mono::class_get_method_from_name(BattleRoomContext, "get_pyEngine", 0);
    }

    if (s_classes.BspEngine) {
        s_methods.BspEngine_GetTrace =
            mono::class_get_method_from_name(s_classes.BspEngine, "GetTrace", 0);
    }

    if (s_classes.BspTrace) {
        s_methods.BspTrace_WorldTrace =
            mono::class_get_method_from_name(s_classes.BspTrace, "WorldTrace", 9);
    }

    if (s_classes.Trace) {
        s_methods.Trace_ctor  = mono::class_get_method_from_name(s_classes.Trace, ".ctor", 0);
        s_methods.Trace_Reset = mono::class_get_method_from_name(s_classes.Trace, "Reset", 0);
        s_fields.Trace_fraction = mono::class_get_field_from_name(s_classes.Trace, "fraction");
    }

    // --- Resolve fields ---
    if (PyEngineComponent) {
        s_fields.PyEngine_field = mono::class_get_field_from_name(PyEngineComponent, "PyEngine");
    }

    // Validate critical path
    if (!s_methods.Contexts_get_sharedInstance ||
        !s_methods.Contexts_get_battleRoom ||
        !s_methods.BattleRoomCtx_get_pyEngine ||
        !s_classes.Trace ||
        !s_fields.Trace_fraction) {
        char buf[256];
        snprintf(buf, sizeof(buf), "VIS: missing methods ctx=%p br=%p pe=%p trace=%p frac=%p",
            s_methods.Contexts_get_sharedInstance,
            s_methods.Contexts_get_battleRoom,
            s_methods.BattleRoomCtx_get_pyEngine,
            s_classes.Trace,
            s_fields.Trace_fraction);
        s_debug = buf;
        return false;
    }

    s_debug = "VIS: init OK (objects deferred)";
    s_initialized = true;
    return true;
}

// ---------------------------------------------------------------------------
// Get BspTrace instance from ECS
// ---------------------------------------------------------------------------
static MonoObject* get_bsp_trace() {
    // Contexts.sharedInstance
    MonoObject* contexts = invoke(s_methods.Contexts_get_sharedInstance, nullptr);
    if (!contexts) return nullptr;

    // Contexts.battleRoom
    MonoObject* battle_room = invoke(s_methods.Contexts_get_battleRoom, contexts);
    if (!battle_room) return nullptr;

    // Check hasPyEngine
    if (s_methods.BattleRoomCtx_get_hasPyEngine) {
        MonoObject* has_res = invoke(s_methods.BattleRoomCtx_get_hasPyEngine, battle_room);
        if (!has_res || !*static_cast<bool*>(mono::object_unbox(has_res)))
            return nullptr;
    }

    // BattleRoomContext.pyEngine -> PyEngineComponent
    MonoObject* py_comp = invoke(s_methods.BattleRoomCtx_get_pyEngine, battle_room);
    if (!py_comp) return nullptr;

    // PyEngineComponent.PyEngine (field) -> IPyEngine (BspEngine)
    MonoObject* engine = nullptr;
    if (s_fields.PyEngine_field)
        mono::field_get_value(py_comp, s_fields.PyEngine_field, &engine);
    if (!engine) return nullptr;

    // BspEngine.GetTrace() -> IPyTrace (BspTrace)
    if (!s_methods.BspEngine_GetTrace) return nullptr;
    return invoke(s_methods.BspEngine_GetTrace, engine);
}

// ---------------------------------------------------------------------------
// Core visibility check (SSJJ coordinates)
// ---------------------------------------------------------------------------
bool is_visible_ssjj(const unity::Vector3& from_ssjj, const unity::Vector3& to_ssjj) {
    if (!s_initialized) return true; // fail open

    // Lazy-create managed objects on first call (must be on main thread)
    if (!s_trace_result) {
        if (!create_cached_objects()) {
            s_debug = "VIS: failed to create objects";
            return true; // fail open
        }
    }

    // Get BspTrace
    MonoObject* bsp_trace = get_bsp_trace();
    if (!bsp_trace) {
        s_debug = "VIS: no BspTrace";
        return true; // fail open (not in match)
    }

    if (!s_methods.BspTrace_WorldTrace) {
        s_debug = "VIS: no WorldTrace method";
        return true;
    }

    // Fill start/end arrays with SSJJ coordinates
    float* start_data = array_data(s_arr_start);
    float* end_data   = array_data(s_arr_end);
    if (!start_data || !end_data) return true;

    start_data[0] = from_ssjj.x;
    start_data[1] = from_ssjj.y;
    start_data[2] = from_ssjj.z;
    end_data[0]   = to_ssjj.x;
    end_data[1]   = to_ssjj.y;
    end_data[2]   = to_ssjj.z;

    // Reset trace result
    if (s_methods.Trace_Reset)
        mono::runtime_invoke(s_methods.Trace_Reset, s_trace_result, nullptr, nullptr);

    // Call WorldTrace(results, start, end, mins, maxs, brushmask, entity, flag, filter)
    int brushmask = CONTENTS_SOLID;
    int flag = 0;
    void* args[9] = {
        s_trace_result,                              // Trace results
        s_arr_start,                                 // float[] start
        s_arr_end,                                   // float[] end
        s_arr_mins,                                  // float[] mins (zeros)
        s_arr_maxs,                                  // float[] maxs (zeros)
        &brushmask,                                  // int brushmask
        nullptr,                                     // IPyEntity entity (null)
        &flag,                                       // int flag
        nullptr                                      // IEntityCollisionFilter filter (null)
    };

    mono::runtime_invoke(s_methods.BspTrace_WorldTrace, bsp_trace, args, nullptr);

    // Read fraction
    float fraction = 0.0f;
    if (s_fields.Trace_fraction)
        mono::field_get_value(s_trace_result, s_fields.Trace_fraction, &fraction);

    return fraction >= 1.0f;
}

// ---------------------------------------------------------------------------
// Visibility check (Unity world coordinates)
// ---------------------------------------------------------------------------
bool is_visible_unity(const unity::Vector3& from_unity, const unity::Vector3& to_unity) {
    // Convert Unity → SSJJ coords
    unity::Vector3 from_ssjj, to_ssjj;
    from_ssjj.x =  from_unity.z;
    from_ssjj.y = -from_unity.x;
    from_ssjj.z =  from_unity.y;
    to_ssjj.x   =  to_unity.z;
    to_ssjj.y   = -to_unity.x;
    to_ssjj.z   =  to_unity.y;

    return is_visible_ssjj(from_ssjj, to_ssjj);
}

// ---------------------------------------------------------------------------
// Debug / Shutdown
// ---------------------------------------------------------------------------
const char* get_debug_info() { return s_debug.c_str(); }

void shutdown() {
    if (s_trace_gc) {
        mono::gchandle_free(s_trace_gc);
        s_trace_gc = 0;
    }
    for (auto& gc : s_arr_gc) {
        if (gc) { mono::gchandle_free(gc); gc = 0; }
    }
    s_trace_result = nullptr;
    s_arr_start = s_arr_end = s_arr_mins = s_arr_maxs = nullptr;
    s_methods = {};
    s_fields = {};
    s_classes = {};
    s_initialized = false;
}

} // namespace visibility
