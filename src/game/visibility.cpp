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
static constexpr int CONTENTS_SOLID = 1;

// ---------------------------------------------------------------------------
// Cached handles
// ---------------------------------------------------------------------------
static struct {
    MonoMethod* Contexts_get_sharedInstance    = nullptr;
    MonoMethod* Contexts_get_battleRoom       = nullptr;
    MonoMethod* BattleRoomCtx_get_hasPyEngine = nullptr;
    MonoMethod* BattleRoomCtx_get_pyEngine    = nullptr;
    MonoMethod* BspEngine_GetTrace            = nullptr;
    MonoMethod* BspTrace_WorldTrace           = nullptr;
    MonoMethod* BspTrace_ctor                 = nullptr; // BspTrace(BspClipMapEx)
    MonoMethod* Trace_ctor                    = nullptr;
    MonoMethod* Trace_Reset                   = nullptr;
} s_methods;

static struct {
    MonoClassField* PyEngine_field   = nullptr; // PyEngineComponent.PyEngine
    MonoClassField* Trace_fraction   = nullptr; // Trace.fraction
    MonoClassField* Engine_clipMap   = nullptr; // BspEngine.clipMap
    MonoClassField* BspTrace_cm      = nullptr; // BspTrace.cm
} s_fields;

static struct {
    MonoClass* Trace      = nullptr;
    MonoClass* BspTrace   = nullptr;
    MonoClass* BspEngine  = nullptr;
} s_classes;

// ---------------------------------------------------------------------------
// Cached managed objects
// ---------------------------------------------------------------------------
static MonoObject* s_trace_result   = nullptr; // physics.trace.Trace instance
static uint32_t    s_trace_gc       = 0;
static MonoObject* s_own_bsp_trace  = nullptr; // Our own BspTrace instance
static uint32_t    s_bsp_trace_gc   = 0;

static MonoArray*  s_arr_start      = nullptr;
static MonoArray*  s_arr_end        = nullptr;
static MonoArray*  s_arr_mins       = nullptr;
static MonoArray*  s_arr_maxs       = nullptr;
static uint32_t    s_arr_gc[4]      = {};

static bool        s_initialized    = false;
static int         s_fail_count     = 0;
static constexpr int MAX_RETRIES    = 60;
static std::string s_debug;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
// MonoArray layout on 64-bit: MonoObject(16) + bounds*(8) + max_length(4) + pad(4) = 32
static constexpr int MONO_ARRAY_DATA_OFFSET = 32;

static float* array_data(MonoArray* arr) {
    if (!arr) return nullptr;
    return reinterpret_cast<float*>(reinterpret_cast<char*>(arr) + MONO_ARRAY_DATA_OFFSET);
}

static MonoObject* invoke_safe(MonoMethod* method, MonoObject* obj, MonoObject** exc) {
    if (!method) return nullptr;
    return mono::runtime_invoke(method, obj, nullptr, exc);
}

// ---------------------------------------------------------------------------
// Get BspClipMapEx from the game's BspEngine
// ---------------------------------------------------------------------------
static MonoObject* get_clip_map() {
    MonoObject* exc = nullptr;

    MonoObject* contexts = invoke_safe(s_methods.Contexts_get_sharedInstance, nullptr, &exc);
    if (!contexts || exc) return nullptr;

    MonoObject* battle_room = invoke_safe(s_methods.Contexts_get_battleRoom, contexts, &exc);
    if (!battle_room || exc) return nullptr;

    if (s_methods.BattleRoomCtx_get_hasPyEngine) {
        MonoObject* has_res = invoke_safe(s_methods.BattleRoomCtx_get_hasPyEngine, battle_room, &exc);
        if (exc || !has_res || !*static_cast<bool*>(mono::object_unbox(has_res)))
            return nullptr;
    }

    MonoObject* py_comp = invoke_safe(s_methods.BattleRoomCtx_get_pyEngine, battle_room, &exc);
    if (!py_comp || exc) return nullptr;

    MonoObject* engine = nullptr;
    if (s_fields.PyEngine_field)
        mono::field_get_value(py_comp, s_fields.PyEngine_field, &engine);
    if (!engine) return nullptr;

    // Read engine.clipMap field
    MonoObject* clip_map = nullptr;
    if (s_fields.Engine_clipMap)
        mono::field_get_value(engine, s_fields.Engine_clipMap, &clip_map);
    return clip_map;
}

// ---------------------------------------------------------------------------
// Create our own BspTrace + Trace result + arrays
// ---------------------------------------------------------------------------
static bool create_cached_objects() {
    MonoDomain* domain = mono::domain_get();
    if (!domain) { s_debug = "VIS: no domain"; return false; }

    // --- Create float arrays ---
    MonoClass* float_cls = mono::get_single_class ? mono::get_single_class() : nullptr;
    if (!float_cls || !mono::array_new) { s_debug = "VIS: no float class"; return false; }

    s_arr_start = mono::array_new(domain, float_cls, 3);
    s_arr_end   = mono::array_new(domain, float_cls, 3);
    s_arr_mins  = mono::array_new(domain, float_cls, 3);
    s_arr_maxs  = mono::array_new(domain, float_cls, 3);
    if (!s_arr_start || !s_arr_end || !s_arr_mins || !s_arr_maxs) {
        s_debug = "VIS: array_new failed"; return false;
    }

    memset(array_data(s_arr_mins), 0, 3 * sizeof(float));
    memset(array_data(s_arr_maxs), 0, 3 * sizeof(float));

    s_arr_gc[0] = mono::gchandle_new(reinterpret_cast<MonoObject*>(s_arr_start), 1);
    s_arr_gc[1] = mono::gchandle_new(reinterpret_cast<MonoObject*>(s_arr_end),   1);
    s_arr_gc[2] = mono::gchandle_new(reinterpret_cast<MonoObject*>(s_arr_mins),  1);
    s_arr_gc[3] = mono::gchandle_new(reinterpret_cast<MonoObject*>(s_arr_maxs),  1);

    // --- Create Trace result object ---
    if (!s_classes.Trace) { s_debug = "VIS: Trace class null"; return false; }
    s_trace_result = mono::object_new(domain, s_classes.Trace);
    if (!s_trace_result) { s_debug = "VIS: Trace alloc failed"; return false; }

    if (mono::runtime_object_init) {
        mono::runtime_object_init(s_trace_result);
    } else if (s_methods.Trace_ctor) {
        MonoObject* exc = nullptr;
        mono::runtime_invoke(s_methods.Trace_ctor, s_trace_result, nullptr, &exc);
        if (exc) { s_debug = "VIS: Trace ctor exc"; return false; }
    } else {
        s_debug = "VIS: no way to init Trace";
        return false;
    }
    s_trace_gc = mono::gchandle_new(s_trace_result, 1);

    // --- Create our own BspTrace(clipMap) ---
    MonoObject* clip_map = get_clip_map();
    if (!clip_map) { s_debug = "VIS: no clipMap (not in match?)"; return false; }

    if (!s_classes.BspTrace || !s_methods.BspTrace_ctor) {
        s_debug = "VIS: BspTrace class/ctor null";
        return false;
    }

    s_own_bsp_trace = mono::object_new(domain, s_classes.BspTrace);
    if (!s_own_bsp_trace) { s_debug = "VIS: BspTrace alloc failed"; return false; }

    // Call BspTrace(BspClipMapEx map)
    MonoObject* exc = nullptr;
    void* ctor_args[1] = { clip_map };
    mono::runtime_invoke(s_methods.BspTrace_ctor, s_own_bsp_trace, ctor_args, &exc);
    if (exc) {
        s_debug = "VIS: BspTrace ctor exception";
        s_own_bsp_trace = nullptr;
        return false;
    }

    s_bsp_trace_gc = mono::gchandle_new(s_own_bsp_trace, 1);

    // Verify our BspTrace has cm set
    MonoObject* cm_check = nullptr;
    if (s_fields.BspTrace_cm)
        mono::field_get_value(s_own_bsp_trace, s_fields.BspTrace_cm, &cm_check);

    char buf[256];
    snprintf(buf, sizeof(buf), "VIS: ready (own BspTrace=%p cm=%p)",
        s_own_bsp_trace, cm_check);
    s_debug = buf;
    return true;
}

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------
bool initialize() {
    MonoImage* ent_img  = unity::images().entitas_lib;
    MonoImage* phy_img  = unity::images().physics_lib;
    MonoImage* pnet_img = unity::images().physics_net;

    if (!ent_img) { s_debug = "VIS: no entitas_lib"; return false; }

    // --- Classes ---
    MonoClass* Contexts = mono::class_from_name(ent_img, "", "Contexts");
    MonoClass* BattleRoomContext = mono::class_from_name(ent_img, "", "BattleRoomContext");
    MonoClass* PyEngineComponent = mono::class_from_name(
        ent_img, "Assets.Sources.Components.BattleRoom", "PyEngineComponent");

    if (!Contexts || !BattleRoomContext) {
        s_debug = "VIS: no Contexts/BattleRoomContext";
        return false;
    }

    if (phy_img) {
        s_classes.BspEngine = mono::class_from_name(phy_img, "SSJJPhysics", "BspEngine");
        s_classes.BspTrace  = mono::class_from_name(phy_img, "SSJJPhysics", "BspTrace");
    }
    if (pnet_img) {
        s_classes.Trace = mono::class_from_name(pnet_img, "physics.trace", "Trace");
    }

    // --- Methods ---
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
        s_fields.Engine_clipMap =
            mono::class_get_field_from_name(s_classes.BspEngine, "clipMap");
    }

    if (s_classes.BspTrace) {
        s_methods.BspTrace_WorldTrace =
            mono::class_get_method_from_name(s_classes.BspTrace, "WorldTrace", 9);
        s_methods.BspTrace_ctor =
            mono::class_get_method_from_name(s_classes.BspTrace, ".ctor", 1);
        s_fields.BspTrace_cm =
            mono::class_get_field_from_name(s_classes.BspTrace, "cm");
    }

    if (s_classes.Trace) {
        s_methods.Trace_ctor    = mono::class_get_method_from_name(s_classes.Trace, ".ctor", 0);
        s_methods.Trace_Reset   = mono::class_get_method_from_name(s_classes.Trace, "Reset", 0);
        s_fields.Trace_fraction = mono::class_get_field_from_name(s_classes.Trace, "fraction");
    }

    if (PyEngineComponent) {
        s_fields.PyEngine_field = mono::class_get_field_from_name(PyEngineComponent, "PyEngine");
    }

    // Validate
    if (!s_methods.Contexts_get_sharedInstance ||
        !s_methods.Contexts_get_battleRoom ||
        !s_methods.BattleRoomCtx_get_pyEngine ||
        !s_classes.Trace || !s_fields.Trace_fraction) {
        s_debug = "VIS: missing critical methods";
        return false;
    }

    if (!s_methods.BspTrace_ctor) {
        s_debug = "VIS: BspTrace ctor not found (will use game's)";
        // Not fatal - we can fall back to game's BspTrace
    }

    s_debug = "VIS: init OK (objects deferred)";
    s_initialized = true;
    return true;
}

// ---------------------------------------------------------------------------
// Core visibility check
// ---------------------------------------------------------------------------
bool is_visible_ssjj(const unity::Vector3& from_ssjj, const unity::Vector3& to_ssjj) {
    if (!s_initialized) return true;
    if (s_fail_count >= MAX_RETRIES) return true;

    // Lazy-create on first call
    if (!s_trace_result || !s_own_bsp_trace) {
        if (!create_cached_objects()) {
            s_fail_count = MAX_RETRIES; // don't retry until next map
            return true;
        }
    }

    if (!s_methods.BspTrace_WorldTrace) return true;

    // Fill arrays
    float* start_data = array_data(s_arr_start);
    float* end_data   = array_data(s_arr_end);
    if (!start_data || !end_data) return true;

    start_data[0] = from_ssjj.x;
    start_data[1] = from_ssjj.y;
    start_data[2] = from_ssjj.z;
    end_data[0]   = to_ssjj.x;
    end_data[1]   = to_ssjj.y;
    end_data[2]   = to_ssjj.z;

    // Call WorldTrace on OUR BspTrace instance
    int brushmask = CONTENTS_SOLID;
    int flag = 0;
    void* args[9] = {
        s_trace_result, s_arr_start, s_arr_end,
        s_arr_mins, s_arr_maxs, &brushmask,
        nullptr, &flag, nullptr
    };

    MonoObject* exc = nullptr;
    mono::runtime_invoke(s_methods.BspTrace_WorldTrace, s_own_bsp_trace, args, &exc);
    if (exc) {
        const char* type_name = "?";
        if (mono::object_get_class && mono::class_get_name) {
            MonoClass* ec = mono::object_get_class(exc);
            if (ec) type_name = mono::class_get_name(ec);
        }
        char buf[256];
        snprintf(buf, sizeof(buf), "WorldTrace [%s] fail=%d", type_name, s_fail_count);
        s_debug = buf;
        s_fail_count++;
        return true;
    }

    s_fail_count = 0;

    float fraction = 0.0f;
    if (s_fields.Trace_fraction)
        mono::field_get_value(s_trace_result, s_fields.Trace_fraction, &fraction);

    return fraction >= 1.0f;
}

bool is_visible_unity(const unity::Vector3& from_unity, const unity::Vector3& to_unity) {
    unity::Vector3 from_ssjj, to_ssjj;
    from_ssjj.x =  from_unity.z;  from_ssjj.y = -from_unity.x;  from_ssjj.z =  from_unity.y;
    to_ssjj.x   =  to_unity.z;    to_ssjj.y   = -to_unity.x;    to_ssjj.z   =  to_unity.y;
    return is_visible_ssjj(from_ssjj, to_ssjj);
}

// ---------------------------------------------------------------------------
// Diagnostic dump
// ---------------------------------------------------------------------------
std::string dump_diagnostics() {
    std::string out;
    char buf[512];

    out += "=== Visibility Diagnostics ===\n";
    snprintf(buf, sizeof(buf), "initialized=%d fail_count=%d/%d\n",
        s_initialized, s_fail_count, MAX_RETRIES);
    out += buf;

    snprintf(buf, sizeof(buf), "BspTrace class=%p ctor=%p WorldTrace=%p\n",
        s_classes.BspTrace, s_methods.BspTrace_ctor, s_methods.BspTrace_WorldTrace);
    out += buf;

    snprintf(buf, sizeof(buf), "Trace class=%p ctor=%p fraction_field=%p\n",
        s_classes.Trace, s_methods.Trace_ctor, s_fields.Trace_fraction);
    out += buf;

    snprintf(buf, sizeof(buf), "Engine clipMap_field=%p PyEngine_field=%p\n",
        s_fields.Engine_clipMap, s_fields.PyEngine_field);
    out += buf;

    snprintf(buf, sizeof(buf), "own_bsp_trace=%p trace_result=%p\n",
        s_own_bsp_trace, s_trace_result);
    out += buf;

    snprintf(buf, sizeof(buf), "arrays: start=%p end=%p mins=%p maxs=%p\n",
        s_arr_start, s_arr_end, s_arr_mins, s_arr_maxs);
    out += buf;

    // Try to read clipMap from game's engine
    MonoObject* clip_map = get_clip_map();
    snprintf(buf, sizeof(buf), "game clipMap=%p\n", clip_map);
    out += buf;

    // Check our BspTrace's cm field
    if (s_own_bsp_trace && s_fields.BspTrace_cm) {
        MonoObject* cm = nullptr;
        mono::field_get_value(s_own_bsp_trace, s_fields.BspTrace_cm, &cm);
        snprintf(buf, sizeof(buf), "own BspTrace.cm=%p\n", cm);
        out += buf;
    }

    // Check Trace fields
    if (s_trace_result && s_classes.Trace) {
        MonoClassField* f_ep = mono::class_get_field_from_name(s_classes.Trace, "endPos");
        MonoClassField* f_re = mono::class_get_field_from_name(s_classes.Trace, "resolvedEnd");
        MonoObject* ep = nullptr;
        MonoObject* re = nullptr;
        if (f_ep) mono::field_get_value(s_trace_result, f_ep, &ep);
        if (f_re) mono::field_get_value(s_trace_result, f_re, &re);
        snprintf(buf, sizeof(buf), "Trace.endPos=%p resolvedEnd=%p\n", ep, re);
        out += buf;
    }

    // Try a test trace (from player pos if available)
    if (s_own_bsp_trace && s_methods.BspTrace_WorldTrace && s_trace_result &&
        s_arr_start && s_arr_end && s_arr_mins && s_arr_maxs) {
        // Simple test: trace a short line straight up from (0,0,0) to (0,0,100)
        float* sd = array_data(s_arr_start);
        float* ed = array_data(s_arr_end);
        sd[0] = 1000.0f; sd[1] = 1000.0f; sd[2] = 100.0f;
        ed[0] = 1000.0f; ed[1] = 1000.0f; ed[2] = 200.0f;

        int bm = CONTENTS_SOLID;
        int fl = 0;
        void* args[9] = {
            s_trace_result, s_arr_start, s_arr_end,
            s_arr_mins, s_arr_maxs, &bm, nullptr, &fl, nullptr
        };

        MonoObject* exc = nullptr;
        mono::runtime_invoke(s_methods.BspTrace_WorldTrace, s_own_bsp_trace, args, &exc);
        if (exc) {
            const char* tn = "?";
            if (mono::object_get_class && mono::class_get_name) {
                MonoClass* ec = mono::object_get_class(exc);
                if (ec) tn = mono::class_get_name(ec);
            }
            snprintf(buf, sizeof(buf), "TEST TRACE: EXCEPTION [%s]\n", tn);
            out += buf;
        } else {
            float frac = 0.0f;
            if (s_fields.Trace_fraction)
                mono::field_get_value(s_trace_result, s_fields.Trace_fraction, &frac);
            snprintf(buf, sizeof(buf), "TEST TRACE: fraction=%.4f (%s)\n",
                frac, frac >= 1.0f ? "clear" : "blocked");
            out += buf;
        }
    } else {
        out += "TEST TRACE: objects not ready\n";
    }

    out += "debug: " + s_debug + "\n";
    return out;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------
const char* get_debug_info() { return s_debug.c_str(); }

void shutdown() {
    if (s_bsp_trace_gc) { mono::gchandle_free(s_bsp_trace_gc); s_bsp_trace_gc = 0; }
    if (s_trace_gc) { mono::gchandle_free(s_trace_gc); s_trace_gc = 0; }
    for (auto& gc : s_arr_gc) { if (gc) { mono::gchandle_free(gc); gc = 0; } }
    s_own_bsp_trace = nullptr;
    s_trace_result = nullptr;
    s_arr_start = s_arr_end = s_arr_mins = s_arr_maxs = nullptr;
    s_methods = {};
    s_fields = {};
    s_classes = {};
    s_initialized = false;
    s_fail_count = 0;
}

} // namespace visibility
