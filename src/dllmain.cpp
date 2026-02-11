#include "pch.h"
#include "core/globals.h"
#include "mono/mono_api.h"
#include "unity/unity_classes.h"
#include "gui/gui.h"
#include "hooks/hooks.h"
#include "menu/menu.h"
#include "payload/payload.h"
#include "game/player_info.h"
#include "game/esp.h"
#include "game/aimbot.h"

static DWORD WINAPI init_thread(LPVOID param)
{
    // Wait for mono to be ready
    while (!GetModuleHandleA("mono-2.0-bdwgc.dll") && !GetModuleHandleA("mono.dll")) {
        Sleep(100);
    }
    Sleep(2000); // Wait for Unity to fully initialize

    // Step 1: Initialize Mono API
    if (!mono::initialize()) {
        MessageBoxA(nullptr, "Failed to initialize Mono API", "SSJJ-Internal", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Step 2: Attach to Mono domain
    MonoDomain* domain = mono::get_root_domain();
    if (!domain) {
        MessageBoxA(nullptr, "Failed to get root domain", "SSJJ-Internal", MB_OK | MB_ICONERROR);
        return 1;
    }
    mono::thread_attach(domain);

    // Step 3: Cache Unity classes and methods
    if (!unity::cache_initialize()) {
        MessageBoxA(nullptr, "Failed to cache Unity classes", "SSJJ-Internal", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Step 4: Initialize GUI wrappers
    gui::initialize();

    // Step 5: Initialize menu
    menu::initialize();

    // Step 6: Initialize hooks (if any)
    hooks::initialize();

    // Step 7: Initialize player info reader
    player_info::initialize();

    // Step 8: Initialize ESP system
    esp::initialize();

    // Step 8b: Initialize aimbot module (no-recoil, etc.)
    aimbot::initialize();

    // Step 9: Load and execute C# payload
    if (!payload::initialize()) {
        MessageBoxA(nullptr, "Failed to load payload", "SSJJ-Internal", MB_OK | MB_ICONERROR);
        return 1;
    }

    globals::initialized = true;

    // Keep thread alive, wait for unload signal
    while (globals::running) {
        // Press END key to unload
        if (GetAsyncKeyState(VK_END) & 1) {
            globals::running = false;
        }
        Sleep(100);
    }

    // Cleanup
    payload::shutdown();
    aimbot::shutdown();
    esp::shutdown();
    player_info::shutdown();
    hooks::shutdown();
    unity::cache_shutdown();
    mono::shutdown();

    FreeLibraryAndExitThread(globals::dll_handle, 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        globals::dll_handle = hModule;
        CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
    }
    return TRUE;
}
