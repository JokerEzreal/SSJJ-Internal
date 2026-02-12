#include "anticheat.h"
#include "../mono/mono_api.h"
#include "../mono/mono_types.h"
#include "../unity/unity_classes.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <Windows.h>

namespace anticheat {

// ---------------------------------------------------------------------------
// Cached handles
// ---------------------------------------------------------------------------
static struct {
    // LimitYawSpeedComponent (anti-aimbot yaw speed cap)
    MonoClass* LimitYawSpeedComponent  = nullptr;
    // AceComponent (Tencent ACE anti-cheat)
    MonoClass* AceComponent            = nullptr;
    // Md5Utility (file integrity check)
    MonoClass* Md5Utility              = nullptr;
    // UserCommandContext + InputComponent
    MonoClass* UserCommandContext       = nullptr;
} s_classes;

static struct {
    MonoClassField* LimitYaw_Yaw       = nullptr;   // float
    MonoClassField* Ace_IsRefresh      = nullptr;   // bool
    MonoClassField* Ace_AntiJObject    = nullptr;   // JObject
} s_fields;

static struct {
    MonoMethod* Contexts_get_sharedInstance    = nullptr;
    MonoMethod* Contexts_get_userCommand       = nullptr;
    // YawLimit is on UserCommandEntity, NOT UserCommandContext.
    // Path: UserCommandContext -> inputEntity -> UserCommandEntity -> limitYawSpeed
    MonoMethod* UCCtx_get_inputEntity          = nullptr;  // -> UserCommandEntity
    MonoMethod* UCEntity_get_hasLimitYaw       = nullptr;  // -> bool
    MonoMethod* UCEntity_get_limitYaw          = nullptr;  // -> LimitYawSpeedComponent
    MonoMethod* Contexts_get_battleRoom        = nullptr;
    MonoMethod* BattleRoomCtx_get_ace          = nullptr;
    MonoMethod* BattleRoomCtx_get_hasAce       = nullptr;

    // Md5Utility methods to patch
    MonoMethod* Md5_GetMD5HashFromFile         = nullptr;
    MonoMethod* Md5_CalculateMD5               = nullptr;
    MonoMethod* Md5_LoadUnityMD5               = nullptr;
} s_methods;

static bool        s_initialized = false;
static std::string s_debug;
static int         s_yaw_patch_count = 0;
static int         s_ace_patch_count = 0;
static bool        s_md5_patched = false;
static bool        s_gameguard_patched = false;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static MonoObject* invoke_safe(MonoMethod* method, MonoObject* obj) {
    if (!method) return nullptr;
    MonoObject* exc = nullptr;
    MonoObject* res = mono::runtime_invoke(method, obj, nullptr, &exc);
    return exc ? nullptr : res;
}

// ---------------------------------------------------------------------------
// Patch Md5Utility methods using JIT + byte patching
//
// We compile the method to native code, then overwrite the prologue
// so it returns an empty string immediately.
// ---------------------------------------------------------------------------
static bool patch_md5_method(MonoMethod* method, const char* name) {
    if (!method || !mono::compile_method) return false;

    void* native = mono::compile_method(method);
    if (!native) {
        printf("[anticheat] Failed to JIT-compile %s\n", name);
        return false;
    }

    // We need to make the method return "" (empty string).
    // Strategy: overwrite the prologue with code that:
    //   mov rax, <pointer_to_empty_mono_string>
    //   ret
    //
    // First, create a persistent empty Mono string.
    MonoDomain* domain = mono::domain_get();
    if (!domain) return false;

    static MonoString* s_empty_str = nullptr;
    static uint32_t s_empty_str_gc = 0;
    if (!s_empty_str) {
        s_empty_str = mono::string_new(domain, "");
        if (!s_empty_str) return false;
        s_empty_str_gc = mono::gchandle_new(reinterpret_cast<MonoObject*>(s_empty_str), 1);
    }

    // Make memory writable
    DWORD old_protect;
    if (!VirtualProtect(native, 16, PAGE_EXECUTE_READWRITE, &old_protect)) {
        printf("[anticheat] VirtualProtect failed for %s\n", name);
        return false;
    }

    // Write:  mov rax, imm64 ; ret
    unsigned char* code = static_cast<unsigned char*>(native);
    code[0] = 0x48;  // REX.W
    code[1] = 0xB8;  // MOV RAX, imm64
    memcpy(&code[2], &s_empty_str, sizeof(void*));  // 8 bytes pointer
    code[10] = 0xC3; // RET

    // Restore protection
    VirtualProtect(native, 16, old_protect, &old_protect);

    printf("[anticheat] Patched %s at %p\n", name, native);
    return true;
}

// ---------------------------------------------------------------------------
// Patch GameGuard detection callback in UnityPlayer.dll
// ---------------------------------------------------------------------------
// sub_14004E9F0 is the nProtect GameGuard callback that handles detection
// events.  Error codes 1013/1015 (hack detected), 1020 (banned), and 670
// (collision program) all trigger a MessageBoxTimeoutW dialog followed by
// a PostMessageW(WM_QUIT) to kill the game.
//
// We patch the function entry to: mov eax, 1; ret
// This prevents the dialog and the forced exit.
// ---------------------------------------------------------------------------
static constexpr uintptr_t GG_CALLBACK_RVA = 0x4E9F0;  // sub_14004E9F0

static bool patch_gameguard_callback() {
    HMODULE unity = GetModuleHandleA("UnityPlayer.dll");
    if (!unity) {
        printf("[anticheat] UnityPlayer.dll not found\n");
        return false;
    }

    BYTE* target = reinterpret_cast<BYTE*>(unity) + GG_CALLBACK_RVA;

    // Verify we're patching the right place: the function should start with
    // a recognizable prologue.  sub_14004E9F0 uses sub rsp / push patterns.
    // We do a loose check: if the first byte is 0xCC (int3 / padding) we're
    // probably at the wrong offset -- bail out.
    if (target[0] == 0xCC) {
        printf("[anticheat] GG callback at %p looks like padding, skipping\n", target);
        return false;
    }

    DWORD old_protect;
    if (!VirtualProtect(target, 16, PAGE_EXECUTE_READWRITE, &old_protect)) {
        printf("[anticheat] VirtualProtect failed for GG callback\n");
        return false;
    }

    // mov eax, 1  (B8 01 00 00 00)
    // ret         (C3)
    target[0] = 0xB8;
    target[1] = 0x01;
    target[2] = 0x00;
    target[3] = 0x00;
    target[4] = 0x00;
    target[5] = 0xC3;

    VirtualProtect(target, 16, old_protect, &old_protect);

    printf("[anticheat] Patched GameGuard callback at %p\n", target);
    return true;
}

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------
bool initialize() {
    MonoImage* ent_img  = unity::images().entitas_lib;
    MonoImage* base_img = unity::images().base_lib;

    if (!ent_img) { s_debug = "AC: no entitas_lib"; return false; }

    // --- Resolve classes ---
    MonoClass* Contexts = mono::class_from_name(ent_img, "", "Contexts");
    MonoClass* BattleRoomContext = mono::class_from_name(ent_img, "", "BattleRoomContext");
    MonoClass* UserCommandContext = mono::class_from_name(ent_img, "", "UserCommandContext");

    s_classes.LimitYawSpeedComponent = mono::class_from_name(
        ent_img, "Assets.Sources.Components.UserComand", "LimitYawSpeedComponent");
    s_classes.AceComponent = mono::class_from_name(
        ent_img, "Assets.Sources.Components.BattleRoom", "AceComponent");

    if (base_img) {
        s_classes.Md5Utility = mono::class_from_name(
            base_img, "SSJJBase.Utility", "Md5Utility");
    }

    // --- Resolve methods ---
    if (Contexts) {
        s_methods.Contexts_get_sharedInstance =
            mono::class_get_method_from_name(Contexts, "get_sharedInstance", 0);
        s_methods.Contexts_get_userCommand =
            mono::class_get_method_from_name(Contexts, "get_userCommand", 0);
        s_methods.Contexts_get_battleRoom =
            mono::class_get_method_from_name(Contexts, "get_battleRoom", 0);
    }

    if (UserCommandContext) {
        s_methods.UCCtx_get_inputEntity =
            mono::class_get_method_from_name(UserCommandContext, "get_inputEntity", 0);
    }

    // limitYawSpeed lives on UserCommandEntity, not UserCommandContext
    MonoClass* UserCommandEntity = mono::class_from_name(ent_img, "", "UserCommandEntity");
    if (UserCommandEntity) {
        s_methods.UCEntity_get_hasLimitYaw =
            mono::class_get_method_from_name(UserCommandEntity, "get_hasLimitYawSpeed", 0);
        s_methods.UCEntity_get_limitYaw =
            mono::class_get_method_from_name(UserCommandEntity, "get_limitYawSpeed", 0);
    }

    if (BattleRoomContext) {
        s_methods.BattleRoomCtx_get_ace =
            mono::class_get_method_from_name(BattleRoomContext, "get_ace", 0);
        s_methods.BattleRoomCtx_get_hasAce =
            mono::class_get_method_from_name(BattleRoomContext, "get_hasAce", 0);
    }

    // --- Resolve fields ---
    if (s_classes.LimitYawSpeedComponent) {
        s_fields.LimitYaw_Yaw = mono::class_get_field_from_name(
            s_classes.LimitYawSpeedComponent, "Yaw");
    }

    if (s_classes.AceComponent) {
        s_fields.Ace_IsRefresh = mono::class_get_field_from_name(
            s_classes.AceComponent, "IsRefresh");
        s_fields.Ace_AntiJObject = mono::class_get_field_from_name(
            s_classes.AceComponent, "AntiJObject");
    }

    // --- Resolve Md5Utility methods ---
    if (s_classes.Md5Utility) {
        s_methods.Md5_GetMD5HashFromFile =
            mono::class_get_method_from_name(s_classes.Md5Utility, "GetMD5HashFromFile", 1);
        s_methods.Md5_CalculateMD5 =
            mono::class_get_method_from_name(s_classes.Md5Utility, "CalculateMD5", 1);
        s_methods.Md5_LoadUnityMD5 =
            mono::class_get_method_from_name(s_classes.Md5Utility, "LoadUnityMD5", 1);
    }

    // --- Patch Md5Utility methods (one-time) ---
    if (s_classes.Md5Utility && mono::compile_method) {
        bool a = patch_md5_method(s_methods.Md5_GetMD5HashFromFile, "Md5Utility.GetMD5HashFromFile");
        bool b = patch_md5_method(s_methods.Md5_CalculateMD5, "Md5Utility.CalculateMD5");
        bool c = patch_md5_method(s_methods.Md5_LoadUnityMD5, "Md5Utility.LoadUnityMD5");
        s_md5_patched = a || b || c;
    }

    // --- Patch GameGuard detection callback (one-time) ---
    s_gameguard_patched = patch_gameguard_callback();

    s_initialized = true;

    char buf[256];
    snprintf(buf, sizeof(buf), "AC: init OK yaw=%p ace=%p md5=%s gg=%s",
        s_fields.LimitYaw_Yaw, s_classes.AceComponent,
        s_md5_patched ? "patched" : "no",
        s_gameguard_patched ? "patched" : "no");
    s_debug = buf;
    return true;
}

// ---------------------------------------------------------------------------
// Per-frame update
// ---------------------------------------------------------------------------
void update() {
    if (!s_initialized) return;

    MonoObject* contexts = invoke_safe(s_methods.Contexts_get_sharedInstance, nullptr);
    if (!contexts) return;

    // ------ Patch 1: Remove yaw speed limit (anti-aimbot bypass) ------
    // Path: Contexts -> userCommand -> inputEntity -> hasLimitYawSpeed -> limitYawSpeed.Yaw
    if (s_methods.UCCtx_get_inputEntity && s_methods.UCEntity_get_limitYaw &&
        s_fields.LimitYaw_Yaw) {
        MonoObject* uc_ctx = invoke_safe(s_methods.Contexts_get_userCommand, contexts);
        if (uc_ctx) {
            MonoObject* uc_entity = invoke_safe(s_methods.UCCtx_get_inputEntity, uc_ctx);
            if (uc_entity) {
                // Check hasLimitYawSpeed first to avoid exception
                bool has_limit = true;
                if (s_methods.UCEntity_get_hasLimitYaw) {
                    MonoObject* has_res = invoke_safe(s_methods.UCEntity_get_hasLimitYaw, uc_entity);
                    if (has_res) has_limit = *static_cast<bool*>(mono::object_unbox(has_res));
                    else has_limit = false;
                }
                if (has_limit) {
                    MonoObject* limit_comp = invoke_safe(s_methods.UCEntity_get_limitYaw, uc_entity);
                    if (limit_comp) {
                        float big_yaw = 99999.0f;
                        mono::field_set_value(limit_comp, s_fields.LimitYaw_Yaw, &big_yaw);
                        s_yaw_patch_count++;
                    }
                }
            }
        }
    }

    // ------ Patch 2: Suppress ACE anti-cheat data refresh ------
    if (s_methods.BattleRoomCtx_get_ace && s_fields.Ace_IsRefresh) {
        MonoObject* br_ctx = invoke_safe(s_methods.Contexts_get_battleRoom, contexts);
        if (br_ctx) {
            // Check if ACE component exists
            bool has_ace = true;
            if (s_methods.BattleRoomCtx_get_hasAce) {
                MonoObject* has_res = invoke_safe(s_methods.BattleRoomCtx_get_hasAce, br_ctx);
                if (has_res) {
                    has_ace = *static_cast<bool*>(mono::object_unbox(has_res));
                }
            }

            if (has_ace) {
                MonoObject* ace_comp = invoke_safe(s_methods.BattleRoomCtx_get_ace, br_ctx);
                if (ace_comp) {
                    // Force IsRefresh = false (prevent sending fresh data)
                    bool no_refresh = false;
                    mono::field_set_value(ace_comp, s_fields.Ace_IsRefresh, &no_refresh);

                    // NOTE: Do NOT null out AntiJObject here.  Writing null to a
                    // managed reference field from C++ while the Boehm GC is
                    // concurrently scanning can corrupt the object graph and crash
                    // inside mono_unity_liveness_calculation_from_statics.
                    // Setting IsRefresh=false is sufficient to suppress ACE reports.
                    s_ace_patch_count++;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Debug / diagnostics
// ---------------------------------------------------------------------------
const char* get_debug_info() { return s_debug.c_str(); }

std::string dump_diagnostics() {
    std::string out;
    char buf[512];

    out += "=== Anti-Cheat Bypass Diagnostics ===\n";
    snprintf(buf, sizeof(buf), "initialized=%d\n", s_initialized);
    out += buf;

    snprintf(buf, sizeof(buf), "LimitYawSpeed: class=%p field=%p entity_get=%p has=%p patches=%d\n",
        s_classes.LimitYawSpeedComponent, s_fields.LimitYaw_Yaw,
        s_methods.UCEntity_get_limitYaw, s_methods.UCEntity_get_hasLimitYaw,
        s_yaw_patch_count);
    out += buf;

    snprintf(buf, sizeof(buf), "ACE: class=%p isRefresh=%p getter=%p patches=%d\n",
        s_classes.AceComponent, s_fields.Ace_IsRefresh,
        s_methods.BattleRoomCtx_get_ace, s_ace_patch_count);
    out += buf;

    snprintf(buf, sizeof(buf), "Md5Utility: class=%p patched=%s\n",
        s_classes.Md5Utility, s_md5_patched ? "YES" : "NO");
    out += buf;

    snprintf(buf, sizeof(buf), "  GetMD5HashFromFile=%p CalculateMD5=%p LoadUnityMD5=%p\n",
        s_methods.Md5_GetMD5HashFromFile, s_methods.Md5_CalculateMD5,
        s_methods.Md5_LoadUnityMD5);
    out += buf;

    HMODULE unity = GetModuleHandleA("UnityPlayer.dll");
    snprintf(buf, sizeof(buf), "GameGuard: patched=%s callback=%p (UnityPlayer+0x%X)\n",
        s_gameguard_patched ? "YES" : "NO",
        unity ? (void*)((BYTE*)unity + GG_CALLBACK_RVA) : nullptr,
        (unsigned)GG_CALLBACK_RVA);
    out += buf;

    out += "debug: " + s_debug + "\n";
    return out;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------
void shutdown() {
    s_classes = {};
    s_fields = {};
    s_methods = {};
    s_initialized = false;
    s_yaw_patch_count = 0;
    s_ace_patch_count = 0;
}

} // namespace anticheat
