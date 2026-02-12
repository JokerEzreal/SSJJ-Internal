#include "player_info.h"
#include "../mono/mono_api.h"
#include "../mono/mono_types.h"
#include "../unity/unity_classes.h"

#include <cstdio>

namespace player_info {

// ---------------------------------------------------------------------------
// Entitas component indices (from PlayerEntity generated code)
// ---------------------------------------------------------------------------
namespace comp_idx {
    constexpr int ENTITY_ID       = 2;
    constexpr int LIFE            = 4;
    constexpr int ORIENTATION     = 6;
    constexpr int BASIC_INFO      = 17;
    constexpr int CURRENT_WEAPON  = 23;
    constexpr int CURR_KILL       = 24;
    constexpr int MY_PLAYER       = 43;
    constexpr int FPOS            = 51;
    constexpr int MOVE            = 59;
    constexpr int TEAM_NAME       = 81;
}

// ---------------------------------------------------------------------------
// mono_array_get macro -- direct memory access into MonoArray data.
//
// MonoArray layout (64-bit Mono):
//   MonoObject header  : 16 bytes (vtable* + sync*)
//   MonoArrayBounds*   :  8 bytes (bounds pointer, null for 1D arrays)
//   uintptr_t max_len  :  8 bytes (max_length)
//   T data[0]          : array elements start at offset 32
//
// This is equivalent to the Mono C API's mono_array_get macro but we define
// it locally to avoid a header dependency.  For reference arrays (MonoObject*)
// each element is a pointer.
// ---------------------------------------------------------------------------
#define MONO_ARRAY_DATA_OFFSET 32

#define mono_array_get(arr, type, index) \
    (*(type*)(((char*)(arr)) + MONO_ARRAY_DATA_OFFSET + sizeof(type) * (index)))

#define mono_array_length(arr) \
    (*(uintptr_t*)(((char*)(arr)) + 24))

// ---------------------------------------------------------------------------
// Cached class handles
// ---------------------------------------------------------------------------
static struct {
    MonoClass* Contexts            = nullptr;  // "" namespace, SSJJEntitas_Library
    MonoClass* PlayerContext       = nullptr;  // "" namespace
    MonoClass* PlayerEntity        = nullptr;  // "" namespace
    MonoClass* BasicInfoComponent  = nullptr;  // Assets.Sources.Components.Player
    MonoClass* OrientationComponent = nullptr; // Assets.Sources.Components.Common
    MonoClass* CurrentWeaponComponent = nullptr; // Assets.Sources.Components.Player
    MonoClass* MoveComponent       = nullptr;  // Assets.Sources.Components.Player
    MonoClass* LifeComponent       = nullptr;  // Assets.Sources.Components.Common
    MonoClass* EntityIdComponent   = nullptr;  // Assets.Sources.Components.Common
    MonoClass* TeamNameComponent   = nullptr;  // Assets.Sources.Components.Player
    MonoClass* CurrKillComponent   = nullptr;  // Assets.Sources.Components.Player
    MonoClass* FposComponent       = nullptr;  // Assets.Sources.Components.Player
    MonoClass* PlayerEntityData    = nullptr;  // NetData (SSJJ.Contract)
    MonoClass* Entity              = nullptr;  // Entitas.Entity (base class)
} s_classes;

// ---------------------------------------------------------------------------
// Cached method handles (property getters / methods)
// ---------------------------------------------------------------------------
static struct {
    // Contexts
    MonoMethod* Contexts_get_sharedInstance  = nullptr;  // static
    MonoMethod* Contexts_get_player         = nullptr;  // instance

    // PlayerContext
    MonoMethod* PlayerContext_get_myPlayerEntity = nullptr;

    // PlayerEntity component getters (kept as fallback; primary path uses
    // direct component array access via s_fields.Entity_components)
    MonoMethod* PlayerEntity_HasComponent        = nullptr;
    MonoMethod* PlayerEntity_get_basicInfo       = nullptr;
    MonoMethod* PlayerEntity_get_orientation      = nullptr;
    MonoMethod* PlayerEntity_get_currentWeapon    = nullptr;
    MonoMethod* PlayerEntity_get_move             = nullptr;
    MonoMethod* PlayerEntity_get_life             = nullptr;
    MonoMethod* PlayerEntity_get_entityId         = nullptr;
    MonoMethod* PlayerEntity_get_teamName         = nullptr;
    MonoMethod* PlayerEntity_get_currKill         = nullptr;
    MonoMethod* PlayerEntity_get_fpos             = nullptr;

    // BasicInfoComponent property getters
    MonoMethod* BasicInfo_get_PlayerName  = nullptr;
    MonoMethod* BasicInfo_get_IsDead      = nullptr;
    MonoMethod* BasicInfo_get_Cid         = nullptr;

    // PlayerEntityData property getters
    MonoMethod* EntityData_get_Hp     = nullptr;
    MonoMethod* EntityData_get_MaxHp  = nullptr;
    MonoMethod* EntityData_get_Team   = nullptr;

    // FposComponent
    MonoMethod* Fpos_Gp = nullptr;  // Gp() -> Vector3

    // PlayerEntity
    MonoMethod* PlayerEntity_GetCompenstatePos = nullptr;  // GetCompenstatePos(int) -> Vector3

    // FposComponent.Change -> Change.GetPosIndex()
    MonoMethod* Change_GetPosIndex = nullptr;
} s_methods;

// ---------------------------------------------------------------------------
// Cached field handles
// ---------------------------------------------------------------------------
static struct {
    // Entitas.Entity base class -- the key optimization field
    MonoClassField* Entity_components = nullptr;  // _components : IComponent[]

    // BasicInfoComponent
    MonoClassField* BasicInfo_Current = nullptr;  // -> PlayerEntityData

    // OrientationComponent
    MonoClassField* Orientation_Yaw   = nullptr;
    MonoClassField* Orientation_Pitch = nullptr;

    // CurrentWeaponComponent
    MonoClassField* Weapon_Name   = nullptr;  // string
    MonoClassField* Weapon_Weapon = nullptr;  // int
    MonoClassField* Weapon_Level  = nullptr;  // int

    // MoveComponent
    MonoClassField* Move_Velocity  = nullptr;  // Vector3
    MonoClassField* Move_OnGround  = nullptr;  // bool
    MonoClassField* Move_Moving    = nullptr;  // bool
    MonoClassField* Move_Stamina   = nullptr;  // float

    // LifeComponent
    MonoClassField* Life_IsDead = nullptr;  // bool

    // EntityIdComponent
    MonoClassField* EntityId_Value = nullptr;  // int

    // TeamNameComponent
    MonoClassField* TeamName_TeamName = nullptr;  // string

    // CurrKillComponent
    MonoClassField* CurrKill_Count = nullptr;  // int

    // FposComponent
    MonoClassField* Fpos_Change = nullptr;  // Change object
} s_fields;

// Resolved dynamically at runtime from actual object classes
static MonoMethod* s_ctx_get_entities  = nullptr;  // Context<T>.GetEntities()
static MonoMethod* s_ctx_get_count     = nullptr;  // Context<T>.get_count
static MonoMethod* s_list_get_count    = nullptr;  // List<IEntity>.get_Count
static MonoMethod* s_list_get_item     = nullptr;  // List<IEntity>.get_Item
static MonoMethod* s_entity_has_comp   = nullptr;  // Entity.HasComponent(int)

// Debug info for menu display
static std::string s_debug_info;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
// Exception-safe invoke: catches managed exceptions locally so they don't
// longjmp past C++ stack frames (which would skip destructors of std::string,
// std::vector etc. and corrupt the heap).
static MonoObject* invoke(MonoMethod* method, MonoObject* obj,
                          void** params = nullptr) {
    if (!method) return nullptr;
    MonoObject* exc = nullptr;
    MonoObject* ret = mono::runtime_invoke(method, obj, params, &exc);
    return exc ? nullptr : ret;
}

static bool unbox_bool(MonoObject* obj) {
    if (!obj) return false;
    return *static_cast<bool*>(mono::object_unbox(obj));
}

static float unbox_float(MonoObject* obj) {
    if (!obj) return 0.0f;
    return *static_cast<float*>(mono::object_unbox(obj));
}

static int unbox_int(MonoObject* obj) {
    if (!obj) return 0;
    return *static_cast<int*>(mono::object_unbox(obj));
}

static int64_t unbox_int64(MonoObject* obj) {
    if (!obj) return 0;
    return *static_cast<int64_t*>(mono::object_unbox(obj));
}

static std::string read_mono_string(MonoString* str) {
    if (!str) return "";
    char* utf8 = mono::string_to_utf8(str);
    if (!utf8) return "";
    std::string result(utf8);
    // Note: ideally call mono_free(utf8), but minor leak is acceptable
    return result;
}

static void read_field_float(MonoObject* obj, MonoClassField* field, float& out) {
    if (obj && field) mono::field_get_value(obj, field, &out);
}

static void read_field_int(MonoObject* obj, MonoClassField* field, int& out) {
    if (obj && field) mono::field_get_value(obj, field, &out);
}

static void read_field_bool(MonoObject* obj, MonoClassField* field, bool& out) {
    if (!obj || !field) return;
    mono_bool val = 0;
    mono::field_get_value(obj, field, &val);
    out = val != 0;
}

static void read_field_vector3(MonoObject* obj, MonoClassField* field, unity::Vector3& out) {
    if (obj && field) mono::field_get_value(obj, field, &out);
}

static std::string read_field_string(MonoObject* obj, MonoClassField* field) {
    if (!obj || !field) return "";
    MonoString* str = nullptr;
    mono::field_get_value(obj, field, &str);
    return read_mono_string(str);
}

static void read_field_object(MonoObject* obj, MonoClassField* field, MonoObject*& out) {
    out = nullptr;
    if (obj && field) mono::field_get_value(obj, field, &out);
}

// ---------------------------------------------------------------------------
// Direct component array access
// ---------------------------------------------------------------------------
// Read the _components array from an entity object.  Returns nullptr if the
// field has not been resolved or the entity is null.
static MonoArray* get_components_array(MonoObject* entity) {
    if (!entity || !s_fields.Entity_components) return nullptr;
    MonoArray* arr = nullptr;
    mono::field_get_value(entity, s_fields.Entity_components, &arr);
    return arr;
}

// Read a component from the entity's _components array by index.
// Returns nullptr if the array is null, index is out of bounds, or the
// component slot is empty (entity doesn't have that component).
static MonoObject* get_component_direct(MonoArray* components, int index) {
    if (!components) return nullptr;
    uintptr_t len = mono_array_length(components);
    if (index < 0 || static_cast<uintptr_t>(index) >= len) return nullptr;
    return mono_array_get(components, MonoObject*, index);
}

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------
bool initialize() {
    MonoImage* ent_img = unity::images().entitas_lib;
    MonoImage* con_img = unity::images().contract_lib;

    if (!ent_img) return false;

    // --- Resolve classes ---
    s_classes.Contexts   = mono::class_from_name(ent_img, "", "Contexts");
    s_classes.PlayerContext = mono::class_from_name(ent_img, "", "PlayerContext");
    s_classes.PlayerEntity  = mono::class_from_name(ent_img, "", "PlayerEntity");

    s_classes.BasicInfoComponent    = mono::class_from_name(ent_img, "Assets.Sources.Components.Player", "BasicInfoComponent");
    s_classes.OrientationComponent  = mono::class_from_name(ent_img, "Assets.Sources.Components.Common", "OrientationComponent");
    s_classes.CurrentWeaponComponent = mono::class_from_name(ent_img, "Assets.Sources.Components.Player", "CurrentWeaponComponent");
    s_classes.MoveComponent         = mono::class_from_name(ent_img, "Assets.Sources.Components.Player", "MoveComponent");
    s_classes.LifeComponent         = mono::class_from_name(ent_img, "Assets.Sources.Components.Common", "LifeComponent");
    s_classes.EntityIdComponent     = mono::class_from_name(ent_img, "Assets.Sources.Components.Common", "EntityIdComponent");
    s_classes.TeamNameComponent     = mono::class_from_name(ent_img, "Assets.Sources.Components.Player", "TeamNameComponent");
    s_classes.CurrKillComponent     = mono::class_from_name(ent_img, "Assets.Sources.Components.Player", "CurrKillComponent");
    s_classes.FposComponent         = mono::class_from_name(ent_img, "Assets.Sources.Components.Player", "FposComponent");

    if (con_img) {
        s_classes.PlayerEntityData = mono::class_from_name(con_img, "NetData", "PlayerEntityData");
    }

    if (!s_classes.Contexts || !s_classes.PlayerContext || !s_classes.PlayerEntity) {
        return false;
    }

    // --- Resolve Entitas.Entity base class for _components field ---
    // Walk up from PlayerEntity to find the Entity base class that has _components.
    {
        MonoClass* cls = s_classes.PlayerEntity;
        while (cls && mono::class_get_parent) {
            const char* name = mono::class_get_name ? mono::class_get_name(cls) : nullptr;
            if (name && strcmp(name, "Entity") == 0) {
                s_classes.Entity = cls;
                break;
            }
            cls = mono::class_get_parent(cls);
        }
        // Resolve _components field from the Entity base class
        if (s_classes.Entity && mono::class_get_field_from_name) {
            s_fields.Entity_components = mono::class_get_field_from_name(s_classes.Entity, "_components");
        }
    }

    // --- Resolve methods ---
    s_methods.Contexts_get_sharedInstance = mono::class_get_method_from_name(s_classes.Contexts, "get_sharedInstance", 0);
    s_methods.Contexts_get_player        = mono::class_get_method_from_name(s_classes.Contexts, "get_player", 0);

    s_methods.PlayerContext_get_myPlayerEntity = mono::class_get_method_from_name(s_classes.PlayerContext, "get_myPlayerEntity", 0);

    if (s_classes.PlayerEntity) {
        s_methods.PlayerEntity_HasComponent = mono::class_get_method_from_name(s_classes.PlayerEntity, "HasComponent", 1);
        // Keep method handles as fallback in case direct array access fails
        s_methods.PlayerEntity_get_basicInfo    = mono::class_get_method_from_name(s_classes.PlayerEntity, "get_basicInfo", 0);
        s_methods.PlayerEntity_get_orientation   = mono::class_get_method_from_name(s_classes.PlayerEntity, "get_orientation", 0);
        s_methods.PlayerEntity_get_currentWeapon = mono::class_get_method_from_name(s_classes.PlayerEntity, "get_currentWeapon", 0);
        s_methods.PlayerEntity_get_move          = mono::class_get_method_from_name(s_classes.PlayerEntity, "get_move", 0);
        s_methods.PlayerEntity_get_life          = mono::class_get_method_from_name(s_classes.PlayerEntity, "get_life", 0);
        s_methods.PlayerEntity_get_entityId      = mono::class_get_method_from_name(s_classes.PlayerEntity, "get_entityId", 0);
        s_methods.PlayerEntity_get_teamName      = mono::class_get_method_from_name(s_classes.PlayerEntity, "get_teamName", 0);
        s_methods.PlayerEntity_get_currKill      = mono::class_get_method_from_name(s_classes.PlayerEntity, "get_currKill", 0);
        s_methods.PlayerEntity_get_fpos          = mono::class_get_method_from_name(s_classes.PlayerEntity, "get_fpos", 0);
    }

    if (s_classes.BasicInfoComponent) {
        s_methods.BasicInfo_get_PlayerName = mono::class_get_method_from_name(s_classes.BasicInfoComponent, "get_PlayerName", 0);
        s_methods.BasicInfo_get_IsDead     = mono::class_get_method_from_name(s_classes.BasicInfoComponent, "get_IsDead", 0);
        s_methods.BasicInfo_get_Cid        = mono::class_get_method_from_name(s_classes.BasicInfoComponent, "get_Cid", 0);
    }

    if (s_classes.PlayerEntityData) {
        s_methods.EntityData_get_Hp    = mono::class_get_method_from_name(s_classes.PlayerEntityData, "get_Hp", 0);
        s_methods.EntityData_get_MaxHp = mono::class_get_method_from_name(s_classes.PlayerEntityData, "get_MaxHp", 0);
        s_methods.EntityData_get_Team  = mono::class_get_method_from_name(s_classes.PlayerEntityData, "get_Team", 0);
    }

    if (s_classes.FposComponent) {
        s_methods.Fpos_Gp = mono::class_get_method_from_name(s_classes.FposComponent, "Gp", 0);
    }

    // GetCompenstatePos on PlayerEntity (takes int index, returns decrypted SSJJ position)
    if (s_classes.PlayerEntity) {
        s_methods.PlayerEntity_GetCompenstatePos =
            mono::class_get_method_from_name(s_classes.PlayerEntity, "GetCompenstatePos", 1);
    }

    // --- Resolve fields ---
    if (s_classes.BasicInfoComponent && mono::class_get_field_from_name) {
        s_fields.BasicInfo_Current = mono::class_get_field_from_name(s_classes.BasicInfoComponent, "Current");
    }

    if (s_classes.OrientationComponent && mono::class_get_field_from_name) {
        s_fields.Orientation_Yaw   = mono::class_get_field_from_name(s_classes.OrientationComponent, "Yaw");
        s_fields.Orientation_Pitch = mono::class_get_field_from_name(s_classes.OrientationComponent, "Pitch");
    }

    if (s_classes.CurrentWeaponComponent && mono::class_get_field_from_name) {
        s_fields.Weapon_Name   = mono::class_get_field_from_name(s_classes.CurrentWeaponComponent, "Name");
        s_fields.Weapon_Weapon = mono::class_get_field_from_name(s_classes.CurrentWeaponComponent, "Weapon");
        s_fields.Weapon_Level  = mono::class_get_field_from_name(s_classes.CurrentWeaponComponent, "Level");
    }

    if (s_classes.MoveComponent && mono::class_get_field_from_name) {
        s_fields.Move_Velocity = mono::class_get_field_from_name(s_classes.MoveComponent, "Velocity");
        s_fields.Move_OnGround = mono::class_get_field_from_name(s_classes.MoveComponent, "OnGround");
        s_fields.Move_Moving   = mono::class_get_field_from_name(s_classes.MoveComponent, "Moving");
        s_fields.Move_Stamina  = mono::class_get_field_from_name(s_classes.MoveComponent, "Stamina");
    }

    if (s_classes.LifeComponent && mono::class_get_field_from_name) {
        s_fields.Life_IsDead = mono::class_get_field_from_name(s_classes.LifeComponent, "IsDead");
    }

    if (s_classes.EntityIdComponent && mono::class_get_field_from_name) {
        s_fields.EntityId_Value = mono::class_get_field_from_name(s_classes.EntityIdComponent, "Value");
    }

    if (s_classes.TeamNameComponent && mono::class_get_field_from_name) {
        s_fields.TeamName_TeamName = mono::class_get_field_from_name(s_classes.TeamNameComponent, "TeamName");
    }

    if (s_classes.CurrKillComponent && mono::class_get_field_from_name) {
        s_fields.CurrKill_Count = mono::class_get_field_from_name(s_classes.CurrKillComponent, "Count");
    }

    if (s_classes.FposComponent && mono::class_get_field_from_name) {
        s_fields.Fpos_Change = mono::class_get_field_from_name(s_classes.FposComponent, "Change");
    }

    return true;
}

// ---------------------------------------------------------------------------
// Read a single player entity's data (shared logic)
//
// Optimization: instead of calling ~10 PlayerEntity.get_XYZ() property
// getters (each a mono::runtime_invoke), we read the entity's _components
// array directly via mono::field_get_value + pointer arithmetic.  This
// replaces ~10 invokes with ~1 field read + fast in-memory array accesses.
//
// The ReadMode parameter controls which components are read:
//   Full    - all components (17 invokes -> ~5 invokes with optimization)
//   ESP     - name, dead, team, hp, position, entity_id, weapon (~4 invokes)
//   Minimal - dead, team, position, entity_id (~3 invokes)
// ---------------------------------------------------------------------------
static PlayerData read_entity(MonoObject* player_entity, ReadMode mode) {
    PlayerData data;
    if (!player_entity) return data;
    data.valid = true;

    // ---- Try direct component array access (fast path) ----
    // Reads _components field from Entity base class, then indexes into it
    // directly.  Each component is a MonoObject* in a managed array.
    // This replaces ~10 mono::runtime_invoke calls with 1 field_get_value
    // + N pointer-arithmetic array reads (zero invoke overhead).
    MonoArray* components = get_components_array(player_entity);

    // --- BasicInfoComponent (invoke cost: 3-6 depending on mode) ---
    // Always needed: provides is_dead, team_id.  ESP/Full also reads name, hp.
    MonoObject* basic_info = components
        ? get_component_direct(components, comp_idx::BASIC_INFO)
        : invoke(s_methods.PlayerEntity_get_basicInfo, player_entity);

    if (basic_info) {
        // is_dead (1 invoke) -- always needed
        data.is_dead = unbox_bool(invoke(s_methods.BasicInfo_get_IsDead, basic_info));

        // EntityData -> team_id, hp, max_hp (field read + 1-3 invokes)
        MonoObject* entity_data = nullptr;
        read_field_object(basic_info, s_fields.BasicInfo_Current, entity_data);
        if (entity_data) {
            // team_id (1 invoke) -- always needed
            data.team_id = unbox_int(invoke(s_methods.EntityData_get_Team, entity_data));

            if (mode != ReadMode::Minimal) {
                // hp, max_hp (2 invokes) -- ESP and Full
                data.hp      = unbox_float(invoke(s_methods.EntityData_get_Hp, entity_data));
                data.max_hp  = unbox_float(invoke(s_methods.EntityData_get_MaxHp, entity_data));
            }
        }

        if (mode != ReadMode::Minimal) {
            // name (1 invoke) -- ESP and Full
            MonoObject* name_obj = invoke(s_methods.BasicInfo_get_PlayerName, basic_info);
            if (name_obj) {
                data.name = read_mono_string(reinterpret_cast<MonoString*>(name_obj));
            }
        }

        if (mode == ReadMode::Full) {
            // cid (1 invoke) -- Full only
            data.cid = unbox_int64(invoke(s_methods.BasicInfo_get_Cid, basic_info));
        }
    }

    // --- EntityId (0 invokes -- direct field read) ---
    // Needed by all modes (ESP for display, Minimal for aimbot entity tracking)
    MonoObject* entity_id_comp = components
        ? get_component_direct(components, comp_idx::ENTITY_ID)
        : invoke(s_methods.PlayerEntity_get_entityId, player_entity);
    if (entity_id_comp) {
        read_field_int(entity_id_comp, s_fields.EntityId_Value, data.entity_id);
    }

    // --- Life (0 invokes -- direct field read) ---
    // Double-check is_dead from LifeComponent (BasicInfo.IsDead may lag)
    MonoObject* life = components
        ? get_component_direct(components, comp_idx::LIFE)
        : invoke(s_methods.PlayerEntity_get_life, player_entity);
    if (life) {
        read_field_bool(life, s_fields.Life_IsDead, data.is_dead);
    }

    // --- Orientation (0 invokes -- direct field reads) ---
    // Full only -- aimbot reads orientation separately via its own cached handles
    if (mode == ReadMode::Full) {
        MonoObject* orientation = components
            ? get_component_direct(components, comp_idx::ORIENTATION)
            : invoke(s_methods.PlayerEntity_get_orientation, player_entity);
        if (orientation) {
            read_field_float(orientation, s_fields.Orientation_Yaw, data.yaw);
            read_field_float(orientation, s_fields.Orientation_Pitch, data.pitch);
        }
    }

    // --- Current Weapon (0 invokes -- direct field reads) ---
    // ESP and Full
    if (mode != ReadMode::Minimal) {
        MonoObject* weapon = components
            ? get_component_direct(components, comp_idx::CURRENT_WEAPON)
            : invoke(s_methods.PlayerEntity_get_currentWeapon, player_entity);
        if (weapon) {
            data.weapon_name = read_field_string(weapon, s_fields.Weapon_Name);
            read_field_int(weapon, s_fields.Weapon_Weapon, data.weapon_id);
            read_field_int(weapon, s_fields.Weapon_Level, data.weapon_level);
        }
    }

    // --- Move (0 invokes -- direct field reads) ---
    // Full only
    if (mode == ReadMode::Full) {
        MonoObject* move = components
            ? get_component_direct(components, comp_idx::MOVE)
            : invoke(s_methods.PlayerEntity_get_move, player_entity);
        if (move) {
            read_field_vector3(move, s_fields.Move_Velocity, data.velocity);
            read_field_bool(move, s_fields.Move_OnGround, data.on_ground);
            read_field_bool(move, s_fields.Move_Moving, data.moving);
            read_field_float(move, s_fields.Move_Stamina, data.stamina);
        }
    }

    // --- TeamName (0 invokes -- direct field read) ---
    // Full only -- team_id from EntityData is sufficient for ESP/Minimal
    if (mode == ReadMode::Full) {
        MonoObject* team_name = components
            ? get_component_direct(components, comp_idx::TEAM_NAME)
            : invoke(s_methods.PlayerEntity_get_teamName, player_entity);
        if (team_name) {
            data.team_name = read_field_string(team_name, s_fields.TeamName_TeamName);
        }
    }

    // --- CurrKill (0 invokes -- direct field read) ---
    // Full only
    if (mode == ReadMode::Full) {
        MonoObject* curr_kill = components
            ? get_component_direct(components, comp_idx::CURR_KILL)
            : invoke(s_methods.PlayerEntity_get_currKill, player_entity);
        if (curr_kill) {
            read_field_int(curr_kill, s_fields.CurrKill_Count, data.kill_count);
        }
    }

    // --- Position (2 invokes: Change.GetPosIndex + GetCompenstatePos) ---
    // Always needed by all modes
    MonoObject* fpos = components
        ? get_component_direct(components, comp_idx::FPOS)
        : invoke(s_methods.PlayerEntity_get_fpos, player_entity);
    bool got_pos = false;

    if (fpos && s_methods.PlayerEntity_GetCompenstatePos && s_fields.Fpos_Change) {
        // Read fpos.Change object
        MonoObject* change_obj = nullptr;
        read_field_object(fpos, s_fields.Fpos_Change, change_obj);
        if (change_obj) {
            // Resolve Change.GetPosIndex dynamically
            if (!s_methods.Change_GetPosIndex && mono::object_get_class) {
                MonoClass* change_cls = mono::object_get_class(change_obj);
                if (change_cls) {
                    s_methods.Change_GetPosIndex = mono::class_get_method_from_name(change_cls, "GetPosIndex", 0);
                }
            }
            if (s_methods.Change_GetPosIndex) {
                int pos_index = unbox_int(invoke(s_methods.Change_GetPosIndex, change_obj));
                void* args[1] = { &pos_index };
                MonoObject* pos_result = invoke(
                    s_methods.PlayerEntity_GetCompenstatePos, player_entity, args);
                if (pos_result) {
                    auto* v3 = static_cast<unity::Vector3*>(mono::object_unbox(pos_result));
                    if (v3) { data.position = *v3; got_pos = true; }
                }
            }
        }
    }

    // Fallback: raw Gp() (still has Seed scaling, but better than nothing)
    if (!got_pos && fpos && s_methods.Fpos_Gp) {
        MonoObject* pos_result = invoke(s_methods.Fpos_Gp, fpos);
        if (pos_result) {
            auto* v3 = static_cast<unity::Vector3*>(mono::object_unbox(pos_result));
            if (v3) data.position = *v3;
        }
    }

    return data;
}

// ---------------------------------------------------------------------------
// Read local player data
// ---------------------------------------------------------------------------
PlayerData read_local_player() {
    MonoObject* contexts = invoke(s_methods.Contexts_get_sharedInstance, nullptr);
    if (!contexts) return {};

    MonoObject* player_ctx = invoke(s_methods.Contexts_get_player, contexts);
    if (!player_ctx) return {};

    MonoObject* player_entity = invoke(s_methods.PlayerContext_get_myPlayerEntity, player_ctx);
    PlayerData data = read_entity(player_entity, ReadMode::Full);
    data.is_local = true;
    data._raw_entity = player_entity;
    return data;
}

// ---------------------------------------------------------------------------
// Read all players in the match
// ---------------------------------------------------------------------------
std::vector<PlayerData> read_all_players(ReadMode mode) {
    std::vector<PlayerData> result;
    char dbg[512];

    MonoObject* contexts = invoke(s_methods.Contexts_get_sharedInstance, nullptr);
    if (!contexts) { s_debug_info = "ERR: contexts=null"; return result; }

    MonoObject* player_ctx = invoke(s_methods.Contexts_get_player, contexts);
    if (!player_ctx) { s_debug_info = "ERR: player_ctx=null"; return result; }

    // Get myPlayerEntity pointer for is_local detection (pointer comparison)
    MonoObject* my_player_entity = invoke(s_methods.PlayerContext_get_myPlayerEntity, player_ctx);

    // Read local player name as fallback for is_local detection.
    // Use direct array access if available to avoid extra invokes.
    std::string local_name;
    if (my_player_entity) {
        MonoArray* local_comps = get_components_array(my_player_entity);
        MonoObject* local_bi = local_comps
            ? get_component_direct(local_comps, comp_idx::BASIC_INFO)
            : invoke(s_methods.PlayerEntity_get_basicInfo, my_player_entity);
        if (local_bi) {
            MonoObject* name_obj = invoke(s_methods.BasicInfo_get_PlayerName, local_bi);
            if (name_obj) local_name = read_mono_string(reinterpret_cast<MonoString*>(name_obj));
        }
    }

    // Resolve GetEntities & get_count by walking the class hierarchy
    // (GetEntities is on generic base class Context<T>, not on PlayerContext directly)
    if (!s_ctx_get_entities && mono::object_get_class && mono::class_get_parent) {
        MonoClass* cls = mono::object_get_class(player_ctx);
        // Walk up to 10 levels of inheritance
        for (int depth = 0; cls && depth < 10; depth++) {
            if (!s_ctx_get_entities)
                s_ctx_get_entities = mono::class_get_method_from_name(cls, "GetEntities", 0);
            if (!s_ctx_get_count)
                s_ctx_get_count = mono::class_get_method_from_name(cls, "get_count", 0);
            if (s_ctx_get_entities && s_ctx_get_count) break;
            cls = mono::class_get_parent(cls);
        }
    }

    // Show entity count from context itself (doesn't need GetEntities)
    int ctx_count = 0;
    if (s_ctx_get_count) {
        ctx_count = unbox_int(invoke(s_ctx_get_count, player_ctx));
    }

    // Debug: show class hierarchy if GetEntities still not found
    if (!s_ctx_get_entities) {
        std::string hierarchy;
        if (mono::object_get_class && mono::class_get_name && mono::class_get_parent) {
            MonoClass* cls = mono::object_get_class(player_ctx);
            for (int d = 0; cls && d < 5; d++) {
                if (!hierarchy.empty()) hierarchy += " -> ";
                const char* n = mono::class_get_name(cls);
                hierarchy += n ? n : "?";
                cls = mono::class_get_parent(cls);
            }
        }
        snprintf(dbg, sizeof(dbg), "ERR: GetEntities not found. ctx_count=%d chain=[%s]",
            ctx_count, hierarchy.c_str());
        s_debug_info = dbg;
        return result;
    }

    // Get all entities
    MonoObject* entity_list = invoke(s_ctx_get_entities, player_ctx);
    if (!entity_list) {
        snprintf(dbg, sizeof(dbg), "ERR: entity_list=null. ctx_count=%d", ctx_count);
        s_debug_info = dbg;
        return result;
    }

    // Resolve List methods dynamically on first call
    if (!s_list_get_count && mono::object_get_class) {
        MonoClass* list_cls = mono::object_get_class(entity_list);
        if (list_cls) {
            s_list_get_count = mono::class_get_method_from_name(list_cls, "get_Count", 0);
            s_list_get_item  = mono::class_get_method_from_name(list_cls, "get_Item", 1);
        }
    }
    if (!s_list_get_count || !s_list_get_item) {
        snprintf(dbg, sizeof(dbg), "ERR: List methods null. Count=%p Item=%p ctx_count=%d",
            (void*)s_list_get_count, (void*)s_list_get_item, ctx_count);
        s_debug_info = dbg;
        return result;
    }

    int count = unbox_int(invoke(s_list_get_count, entity_list));
    if (count <= 0 || count > 200) {
        snprintf(dbg, sizeof(dbg), "list_count=%d ctx_count=%d (skip)", count, ctx_count);
        s_debug_info = dbg;
        return result;
    }

    int processed = 0, skipped = 0;

    for (int i = 0; i < count; i++) {
        void* idx_args[1] = { &i };
        MonoObject* entity = invoke(s_list_get_item, entity_list, idx_args);
        if (!entity) continue;

        // --- Check if entity has BasicInfo component ---
        // Optimization: use direct component array access instead of
        // calling HasComponent(17) via runtime_invoke.  If _components
        // field is resolved, this is a field read + pointer check (0 invokes).
        // Falls back to HasComponent invoke if direct access unavailable.
        MonoArray* ent_components = get_components_array(entity);
        bool has_basic_info = false;

        if (ent_components) {
            // Fast path: direct null check on array element (0 invokes)
            has_basic_info = (get_component_direct(ent_components, comp_idx::BASIC_INFO) != nullptr);
        } else {
            // Fallback: invoke HasComponent (1 invoke)
            if (!s_entity_has_comp && mono::object_get_class) {
                MonoClass* ent_cls = mono::object_get_class(entity);
                if (ent_cls) {
                    s_entity_has_comp = mono::class_get_method_from_name(ent_cls, "HasComponent", 1);
                }
                if (!s_entity_has_comp) {
                    s_entity_has_comp = s_methods.PlayerEntity_HasComponent;
                }
            }
            if (s_entity_has_comp) {
                int bi = comp_idx::BASIC_INFO;
                void* bi_args[1] = { &bi };
                MonoObject* has_res = invoke(s_entity_has_comp, entity, bi_args);
                has_basic_info = unbox_bool(has_res);
            }
        }

        if (!has_basic_info) { skipped++; continue; }

        PlayerData data = read_entity(entity, mode);
        data._raw_entity = entity;

        // Check if this is local player:
        // Method 1: pointer comparison with myPlayerEntity (most reliable)
        if (my_player_entity && entity == my_player_entity) {
            data.is_local = true;
        }
        // Method 2: name matching fallback
        else if (!local_name.empty() && data.name == local_name) {
            data.is_local = true;
        }
        // Method 3: HasComponent(MyPlayer) check via direct array access
        else if (ent_components) {
            // Fast path: direct null check (0 invokes)
            data.is_local = (get_component_direct(ent_components, comp_idx::MY_PLAYER) != nullptr);
        }
        // Method 4: HasComponent(MyPlayer) invoke fallback
        else if (s_entity_has_comp) {
            int mp = comp_idx::MY_PLAYER;
            void* mp_args[1] = { &mp };
            MonoObject* is_local_res = invoke(s_entity_has_comp, entity, mp_args);
            data.is_local = unbox_bool(is_local_res);
        }

        result.push_back(std::move(data));
        processed++;
    }

    const char* mode_str = (mode == ReadMode::Full) ? "full" :
                           (mode == ReadMode::ESP)  ? "esp"  : "min";
    snprintf(dbg, sizeof(dbg), "OK ctx=%d list=%d proc=%d skip=%d mode=%s comp=%s",
        ctx_count, count, processed, skipped, mode_str,
        s_fields.Entity_components ? "direct" : "invoke");
    s_debug_info = dbg;
    return result;
}

// ---------------------------------------------------------------------------
// Debug info getter
// ---------------------------------------------------------------------------
const std::string& get_debug_info() {
    return s_debug_info;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------
void shutdown() {
    s_classes = {};
    s_methods = {};
    s_fields  = {};
}

} // namespace player_info
