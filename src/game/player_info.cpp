#include "player_info.h"
#include "../mono/mono_api.h"
#include "../mono/mono_types.h"
#include "../unity/unity_classes.h"

#include <cstdio>

namespace player_info {

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

    // PlayerEntity component getters
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
static MonoObject* invoke(MonoMethod* method, MonoObject* obj) {
    if (!method) return nullptr;
    return mono::runtime_invoke(method, obj, nullptr, nullptr);
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

    // --- Resolve methods ---
    s_methods.Contexts_get_sharedInstance = mono::class_get_method_from_name(s_classes.Contexts, "get_sharedInstance", 0);
    s_methods.Contexts_get_player        = mono::class_get_method_from_name(s_classes.Contexts, "get_player", 0);

    s_methods.PlayerContext_get_myPlayerEntity = mono::class_get_method_from_name(s_classes.PlayerContext, "get_myPlayerEntity", 0);

    if (s_classes.PlayerEntity) {
        s_methods.PlayerEntity_HasComponent = mono::class_get_method_from_name(s_classes.PlayerEntity, "HasComponent", 1);
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
// ---------------------------------------------------------------------------
static PlayerData read_entity(MonoObject* player_entity) {
    PlayerData data;
    if (!player_entity) return data;
    data.valid = true;

    // --- BasicInfoComponent ---
    MonoObject* basic_info = invoke(s_methods.PlayerEntity_get_basicInfo, player_entity);
    if (basic_info) {
        MonoObject* name_obj = invoke(s_methods.BasicInfo_get_PlayerName, basic_info);
        if (name_obj) {
            data.name = read_mono_string(reinterpret_cast<MonoString*>(name_obj));
        }
        data.is_dead = unbox_bool(invoke(s_methods.BasicInfo_get_IsDead, basic_info));
        data.cid = unbox_int64(invoke(s_methods.BasicInfo_get_Cid, basic_info));

        MonoObject* entity_data = nullptr;
        read_field_object(basic_info, s_fields.BasicInfo_Current, entity_data);
        if (entity_data) {
            data.hp     = unbox_float(invoke(s_methods.EntityData_get_Hp, entity_data));
            data.max_hp = unbox_float(invoke(s_methods.EntityData_get_MaxHp, entity_data));
        }
    }

    // --- Orientation ---
    MonoObject* orientation = invoke(s_methods.PlayerEntity_get_orientation, player_entity);
    if (orientation) {
        read_field_float(orientation, s_fields.Orientation_Yaw, data.yaw);
        read_field_float(orientation, s_fields.Orientation_Pitch, data.pitch);
    }

    // --- Current Weapon ---
    MonoObject* weapon = invoke(s_methods.PlayerEntity_get_currentWeapon, player_entity);
    if (weapon) {
        data.weapon_name = read_field_string(weapon, s_fields.Weapon_Name);
        read_field_int(weapon, s_fields.Weapon_Weapon, data.weapon_id);
        read_field_int(weapon, s_fields.Weapon_Level, data.weapon_level);
    }

    // --- Move ---
    MonoObject* move = invoke(s_methods.PlayerEntity_get_move, player_entity);
    if (move) {
        read_field_vector3(move, s_fields.Move_Velocity, data.velocity);
        read_field_bool(move, s_fields.Move_OnGround, data.on_ground);
        read_field_bool(move, s_fields.Move_Moving, data.moving);
        read_field_float(move, s_fields.Move_Stamina, data.stamina);
    }

    // --- Life ---
    MonoObject* life = invoke(s_methods.PlayerEntity_get_life, player_entity);
    if (life) {
        read_field_bool(life, s_fields.Life_IsDead, data.is_dead);
    }

    // --- EntityId ---
    MonoObject* entity_id = invoke(s_methods.PlayerEntity_get_entityId, player_entity);
    if (entity_id) {
        read_field_int(entity_id, s_fields.EntityId_Value, data.entity_id);
    }

    // --- TeamName ---
    MonoObject* team_name = invoke(s_methods.PlayerEntity_get_teamName, player_entity);
    if (team_name) {
        data.team_name = read_field_string(team_name, s_fields.TeamName_TeamName);
    }

    // --- CurrKill ---
    MonoObject* curr_kill = invoke(s_methods.PlayerEntity_get_currKill, player_entity);
    if (curr_kill) {
        read_field_int(curr_kill, s_fields.CurrKill_Count, data.kill_count);
    }

    // --- Position ---
    // Prefer GetCompenstatePos which properly decrypts (undoes Seed scaling)
    // Falls back to raw Gp() if GetCompenstatePos is not available
    MonoObject* fpos = invoke(s_methods.PlayerEntity_get_fpos, player_entity);
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
                MonoObject* pos_result = mono::runtime_invoke(
                    s_methods.PlayerEntity_GetCompenstatePos, player_entity, args, nullptr);
                if (pos_result) {
                    auto* v3 = static_cast<unity::Vector3*>(mono::object_unbox(pos_result));
                    if (v3) { data.position = *v3; got_pos = true; }
                }
            }
        }
    }

    // Fallback: raw Gp() (still has Seed scaling, but better than nothing)
    if (!got_pos && fpos && s_methods.Fpos_Gp) {
        MonoObject* pos_result = mono::runtime_invoke(s_methods.Fpos_Gp, fpos, nullptr, nullptr);
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
    PlayerData data = read_entity(player_entity);
    data.is_local = true;
    return data;
}

// ---------------------------------------------------------------------------
// Read all players in the match
// ---------------------------------------------------------------------------
std::vector<PlayerData> read_all_players() {
    std::vector<PlayerData> result;
    char dbg[512];

    MonoObject* contexts = invoke(s_methods.Contexts_get_sharedInstance, nullptr);
    if (!contexts) { s_debug_info = "ERR: contexts=null"; return result; }

    MonoObject* player_ctx = invoke(s_methods.Contexts_get_player, contexts);
    if (!player_ctx) { s_debug_info = "ERR: player_ctx=null"; return result; }

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
    MonoObject* entity_list = mono::runtime_invoke(s_ctx_get_entities, player_ctx, nullptr, nullptr);
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

    // Component indices for HasComponent
    constexpr int IDX_BASIC_INFO = 17;
    constexpr int IDX_MY_PLAYER  = 43;
    int processed = 0, skipped = 0;

    for (int i = 0; i < count; i++) {
        void* idx_args[1] = { &i };
        MonoObject* entity = mono::runtime_invoke(s_list_get_item, entity_list, idx_args, nullptr);
        if (!entity) continue;

        // Resolve HasComponent dynamically from the entity's runtime class
        if (!s_entity_has_comp && mono::object_get_class) {
            MonoClass* ent_cls = mono::object_get_class(entity);
            if (ent_cls) {
                s_entity_has_comp = mono::class_get_method_from_name(ent_cls, "HasComponent", 1);
            }
        }

        // Check if entity has BasicInfo component
        if (s_entity_has_comp) {
            int bi = IDX_BASIC_INFO;
            void* bi_args[1] = { &bi };
            MonoObject* has_res = mono::runtime_invoke(s_entity_has_comp, entity, bi_args, nullptr);
            if (!unbox_bool(has_res)) { skipped++; continue; }
        }

        PlayerData data = read_entity(entity);

        // Check if this is local player
        if (s_entity_has_comp) {
            int mp = IDX_MY_PLAYER;
            void* mp_args[1] = { &mp };
            MonoObject* is_local_res = mono::runtime_invoke(s_entity_has_comp, entity, mp_args, nullptr);
            data.is_local = unbox_bool(is_local_res);
        }

        result.push_back(std::move(data));
        processed++;
    }

    snprintf(dbg, sizeof(dbg), "OK ctx=%d list=%d proc=%d skip=%d hasComp=%p",
        ctx_count, count, processed, skipped, (void*)s_entity_has_comp);
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
