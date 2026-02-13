// tools/manual_map_injector.cpp
// SSJJ-Internal Manual Map Injector
// Maps ssjj_overlay.dll into the target process without LoadLibrary.
// The DLL never appears in PEB module lists.

#include <Windows.h>
#include <TlHelp32.h>
#include <cstdio>
#include <vector>

// --- Types ---

using NTSTATUS = LONG;

using fn_NtCreateThreadEx = NTSTATUS(NTAPI*)(
    PHANDLE hThread, ACCESS_MASK access, PVOID objAttr,
    HANDLE hProcess, PVOID startRoutine, PVOID argument,
    ULONG flags, SIZE_T zeroBits, SIZE_T stackSize,
    SIZE_T maxStackSize, PVOID attrList);

using fn_DllMain = BOOL(WINAPI*)(HMODULE, DWORD, LPVOID);

// Passed to shellcode running inside the target process.
// Function pointers are valid cross-process because kernel32/ntdll
// share the same base address across all processes on a given boot.
struct ManualMapData {
    BYTE*   image_base;
    HMODULE (WINAPI* pLoadLibraryA)(LPCSTR);
    FARPROC (WINAPI* pGetProcAddress)(HMODULE, LPCSTR);
    BOOLEAN (WINAPI* pRtlAddFunctionTable)(PRUNTIME_FUNCTION, DWORD, DWORD64);
    DWORD   result; // 0=pending, 1=success, 0xF0+=error
};

// =============================================================================
// Shellcode — position-independent code that runs inside the target process.
// MUST NOT reference any global/static data, string literals, or CRT functions.
// =============================================================================

#pragma runtime_checks("", off)
#pragma optimize("", off)

DWORD WINAPI shellcode_stub(ManualMapData* data) {
    if (!data)
        return 0;

    BYTE* base = data->image_base;
    if (!base) {
        data->result = 0xF0;
        return 0;
    }

    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt  = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& opt = nt->OptionalHeader;

    // ---- Resolve imports ----
    if (opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size) {
        auto desc = (IMAGE_IMPORT_DESCRIPTOR*)(
            base + opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

        while (desc->Name) {
            char* mod_name = (char*)(base + desc->Name);
            HMODULE hMod = data->pLoadLibraryA(mod_name);
            if (!hMod) {
                data->result = 0xF1;
                return 0;
            }

            auto thunk = (IMAGE_THUNK_DATA*)(base +
                (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
            auto iat = (IMAGE_THUNK_DATA*)(base + desc->FirstThunk);

            for (; thunk->u1.AddressOfData; ++thunk, ++iat) {
                if (IMAGE_SNAP_BY_ORDINAL64(thunk->u1.Ordinal)) {
                    iat->u1.Function = (ULONGLONG)data->pGetProcAddress(
                        hMod, (LPCSTR)(thunk->u1.Ordinal & 0xFFFF));
                } else {
                    auto ibn = (IMAGE_IMPORT_BY_NAME*)(base + thunk->u1.AddressOfData);
                    iat->u1.Function = (ULONGLONG)data->pGetProcAddress(hMod, ibn->Name);
                }
            }
            ++desc;
        }
    }

    // ---- Register exception handlers (x64 unwind tables) ----
    if (opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size && data->pRtlAddFunctionTable) {
        data->pRtlAddFunctionTable(
            (PRUNTIME_FUNCTION)(base + opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress),
            opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size / sizeof(RUNTIME_FUNCTION),
            (DWORD64)base);
    }

    // ---- Execute TLS callbacks ----
    if (opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size) {
        auto tls = (IMAGE_TLS_DIRECTORY*)(
            base + opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress);
        auto callbacks = (PIMAGE_TLS_CALLBACK*)(tls->AddressOfCallBacks);
        if (callbacks) {
            while (*callbacks) {
                (*callbacks)((PVOID)base, DLL_PROCESS_ATTACH, nullptr);
                ++callbacks;
            }
        }
    }

    // ---- Call entry point (_DllMainCRTStartup -> CRT init -> DllMain) ----
    if (opt.AddressOfEntryPoint) {
        auto entry = (fn_DllMain)(base + opt.AddressOfEntryPoint);
        entry((HMODULE)base, DLL_PROCESS_ATTACH, nullptr);
    }

    data->result = 1;
    return 0;
}

// Marker function placed immediately after shellcode for size calculation.
void shellcode_stub_end() { }

#pragma optimize("", on)
#pragma runtime_checks("", restore)

// =============================================================================
// Utility functions
// =============================================================================

DWORD find_process(const char* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe)) {
        if (_stricmp(pe.szExeFile, name) == 0) {
            CloseHandle(snap);
            return pe.th32ProcessID;
        }
    }
    CloseHandle(snap);
    return 0;
}

bool read_dll(const char* path, std::vector<BYTE>& out) {
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD size = GetFileSize(hFile, nullptr);
    if (size == INVALID_FILE_SIZE || size < sizeof(IMAGE_DOS_HEADER)) {
        CloseHandle(hFile);
        return false;
    }

    out.resize(size);
    DWORD bytesRead = 0;
    BOOL ok = ReadFile(hFile, out.data(), size, &bytesRead, nullptr);
    CloseHandle(hFile);
    if (!ok || bytesRead != size) return false;

    // Validate PE
    auto dos = (IMAGE_DOS_HEADER*)out.data();
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        printf("[!] Invalid DOS signature\n");
        return false;
    }

    auto nt = (IMAGE_NT_HEADERS*)(out.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        printf("[!] Invalid NT signature\n");
        return false;
    }
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        printf("[!] Not an x64 DLL (machine: 0x%X)\n", nt->FileHeader.Machine);
        return false;
    }
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        printf("[!] Invalid optional header magic\n");
        return false;
    }

    return true;
}

// =============================================================================
// Anti-cheat patching (pre-injection, in-memory)
// =============================================================================

uintptr_t find_remote_module(DWORD pid, const char* mod_name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    MODULEENTRY32 me{};
    me.dwSize = sizeof(me);
    for (BOOL ok = Module32First(snap, &me); ok; ok = Module32Next(snap, &me)) {
        if (_stricmp(me.szModule, mod_name) == 0) {
            uintptr_t base = (uintptr_t)me.modBaseAddr;
            CloseHandle(snap);
            return base;
        }
    }
    CloseHandle(snap);
    return 0;
}

bool patch_remote(HANDLE hProc, uintptr_t addr, const BYTE* patch, size_t len) {
    DWORD old_protect;
    if (!VirtualProtectEx(hProc, (LPVOID)addr, len, PAGE_EXECUTE_READWRITE, &old_protect)) {
        printf("[!] VirtualProtectEx failed at 0x%llX: %lu\n",
               (unsigned long long)addr, GetLastError());
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(hProc, (LPVOID)addr, patch, len, &written) || written != len) {
        printf("[!] WriteProcessMemory failed at 0x%llX: %lu\n",
               (unsigned long long)addr, GetLastError());
        VirtualProtectEx(hProc, (LPVOID)addr, len, old_protect, &old_protect);
        return false;
    }

    // Verify write
    BYTE verify[16];
    SIZE_T read_count = 0;
    ReadProcessMemory(hProc, (LPVOID)addr, verify, len, &read_count);
    if (read_count != len || memcmp(verify, patch, len) != 0) {
        printf("[!] Patch verification failed at 0x%llX\n", (unsigned long long)addr);
        VirtualProtectEx(hProc, (LPVOID)addr, len, old_protect, &old_protect);
        return false;
    }

    VirtualProtectEx(hProc, (LPVOID)addr, len, old_protect, &old_protect);
    return true;
}

// MicroClient.exe GameGuard patch data (RVAs relative to module base 0x140000000)
struct PatchEntry {
    uintptr_t   rva;
    BYTE        patch[16];
    size_t      patch_len;
    BYTE        original[16];   // expected original bytes for safety check
    size_t      original_len;   // 0 = skip verification
    const char* name;
};

// Strategy: Let GameGuard run normally (init, heartbeat, auth, communication).
// Only suppress the CLIENT-SIDE kill actions so that even if GameMon detects
// something, it doesn't show a dialog or terminate the game.
// GameMon64.des launches and handles crypto challenges → server sees valid client.
// Our manual-mapped DLL is invisible to module enumeration.
static const PatchEntry MC_PATCHES[] = {
    // NotifyCallback: called by NProtect when detection occurs (error 1013/1015/1020/670).
    // Original action: show MessageBox, kill game process, PostMessage(WM_QUIT).
    // Patched: return 1 immediately (suppress dialog + kill).
    { 0x4E9F0,
      {0xB8,0x01,0x00,0x00,0x00, 0xC3}, 6,
      {0x40,0x55,0x56,0x57,0x48,0x8D}, 6,
      "GG_NotifyCallback -> 1 (suppress detection dialog)" },
    // CheckHeartbeat: safety net. If NProtect_Check returns non-1877 for any reason,
    // original action shows error dialog, kills game, posts WM_QUIT.
    // Patched: return 1 immediately.
    { 0x4D780,
      {0xB8,0x01,0x00,0x00,0x00, 0xC3}, 6,
      {0x48,0x8B,0xC4,0x55,0x48,0x8D}, 6,
      "CheckHeartbeat -> 1 (safety net)" },
    // ReceiveFromServer: type 2/3 branch = "detected game hack" → kill game.
    // Patch the conditional ja (if NOT type 2/3, skip) to unconditional jmp,
    // so type 2/3 messages are silently discarded. Type 1 auth still works.
    // Original: 0F 87 24 05 00 00 (ja +0x524)
    // Patched:  E9 25 05 00 00 90 (jmp +0x525; nop)
    { 0x4F942,
      {0xE9,0x25,0x05,0x00,0x00, 0x90}, 6,
      {0x0F,0x87,0x24,0x05,0x00,0x00}, 6,
      "ReceiveFromServer: skip type 2/3 detection" },
};

static constexpr size_t MC_PATCH_COUNT = sizeof(MC_PATCHES) / sizeof(MC_PATCHES[0]);

bool patch_microclient() {
    DWORD pid = find_process("MicroClient.exe");
    if (!pid) {
        printf("[!] MicroClient.exe process not found\n");
        return false;
    }

    HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProc) {
        printf("[!] OpenProcess(MicroClient) failed: %lu\n", GetLastError());
        return false;
    }

    uintptr_t base = find_remote_module(pid, "MicroClient.exe");
    if (!base) {
        printf("[!] Could not find MicroClient.exe module base\n");
        CloseHandle(hProc);
        return false;
    }
    printf("[+] MicroClient.exe base: 0x%llX (PID %lu)\n", (unsigned long long)base, pid);

    int success = 0, skipped = 0, failed = 0;
    for (size_t i = 0; i < MC_PATCH_COUNT; ++i) {
        const auto& p = MC_PATCHES[i];
        uintptr_t addr = base + p.rva;

        // Check if already patched
        BYTE current[16];
        SIZE_T read_count = 0;
        ReadProcessMemory(hProc, (LPVOID)addr, current, p.patch_len, &read_count);
        if (read_count == p.patch_len && memcmp(current, p.patch, p.patch_len) == 0) {
            printf("  [SKIP] %s (already patched)\n", p.name);
            skipped++;
            continue;
        }

        // Verify original bytes (safety check, non-fatal)
        if (p.original_len > 0) {
            BYTE orig[16];
            ReadProcessMemory(hProc, (LPVOID)addr, orig, p.original_len, &read_count);
            if (read_count == p.original_len &&
                memcmp(orig, p.original, p.original_len) != 0) {
                printf("  [WARN] %s: unexpected bytes at +0x%llX, patching anyway\n",
                       p.name, (unsigned long long)p.rva);
            }
        }

        if (patch_remote(hProc, addr, p.patch, p.patch_len)) {
            printf("  [OK]   %s\n", p.name);
            success++;
        } else {
            printf("  [FAIL] %s\n", p.name);
            failed++;
        }
    }

    CloseHandle(hProc);
    printf("[+] MicroClient patches: %d applied, %d skipped, %d failed\n",
           success, skipped, failed);
    return failed == 0;
}

// =============================================================================
// Manual mapping core
// =============================================================================

bool manual_map(HANDLE hProc, const std::vector<BYTE>& raw) {
    auto dos = (const IMAGE_DOS_HEADER*)raw.data();
    auto nt  = (const IMAGE_NT_HEADERS*)(raw.data() + dos->e_lfanew);
    auto& opt = nt->OptionalHeader;

    // 1. Allocate image memory in target
    BYTE* remote_base = (BYTE*)VirtualAllocEx(hProc, nullptr, opt.SizeOfImage,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!remote_base) {
        printf("[!] VirtualAllocEx failed for image (%u bytes): %lu\n",
               opt.SizeOfImage, GetLastError());
        return false;
    }
    printf("[+] Remote image: 0x%p (%u bytes)\n", remote_base, opt.SizeOfImage);

    // 2. Build mapped image locally (headers + sections)
    std::vector<BYTE> mapped(opt.SizeOfImage, 0);
    memcpy(mapped.data(), raw.data(), opt.SizeOfHeaders);

    auto section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (section->SizeOfRawData == 0) continue;
        memcpy(mapped.data() + section->VirtualAddress,
               raw.data() + section->PointerToRawData,
               section->SizeOfRawData);
    }

    // 3. Apply base relocations (patch addresses for new base)
    ULONGLONG delta = (ULONGLONG)remote_base - opt.ImageBase;
    if (delta) {
        auto& reloc_dir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (!reloc_dir.Size) {
            printf("[!] No relocations but rebasing is required\n");
            VirtualFreeEx(hProc, remote_base, 0, MEM_RELEASE);
            return false;
        }

        auto reloc = (IMAGE_BASE_RELOCATION*)(mapped.data() + reloc_dir.VirtualAddress);
        BYTE* reloc_end = (BYTE*)reloc + reloc_dir.Size;

        while ((BYTE*)reloc < reloc_end && reloc->SizeOfBlock) {
            DWORD count = (reloc->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
            WORD* entries = (WORD*)(reloc + 1);

            for (DWORD j = 0; j < count; ++j) {
                WORD type   = entries[j] >> 12;
                WORD offset = entries[j] & 0xFFF;

                if (type == IMAGE_REL_BASED_DIR64) {
                    *(ULONGLONG*)(mapped.data() + reloc->VirtualAddress + offset) += delta;
                } else if (type == IMAGE_REL_BASED_HIGHLOW) {
                    *(DWORD*)(mapped.data() + reloc->VirtualAddress + offset) += (DWORD)delta;
                }
                // IMAGE_REL_BASED_ABSOLUTE = padding, skip
            }

            reloc = (IMAGE_BASE_RELOCATION*)((BYTE*)reloc + reloc->SizeOfBlock);
        }
        printf("[+] Relocations applied (delta: 0x%llX)\n", delta);
    }

    // 4. Write mapped image to target process
    if (!WriteProcessMemory(hProc, remote_base, mapped.data(), opt.SizeOfImage, nullptr)) {
        printf("[!] WriteProcessMemory failed: %lu\n", GetLastError());
        VirtualFreeEx(hProc, remote_base, 0, MEM_RELEASE);
        return false;
    }
    printf("[+] Image written to target\n");

    // 5. Prepare shellcode region (code + ManualMapData struct)
    size_t sc_size = (BYTE*)shellcode_stub_end - (BYTE*)shellcode_stub;
    if (sc_size == 0 || sc_size > 0x2000) {
        printf("[*] Shellcode size heuristic failed (%zu), using 4KB fallback\n", sc_size);
        sc_size = 0x1000;
    }

    size_t data_offset = (sc_size + 15) & ~(size_t)15; // 16-byte align data
    size_t alloc_size  = data_offset + sizeof(ManualMapData);

    BYTE* remote_shell = (BYTE*)VirtualAllocEx(hProc, nullptr, alloc_size,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!remote_shell) {
        printf("[!] VirtualAllocEx failed for shellcode: %lu\n", GetLastError());
        VirtualFreeEx(hProc, remote_base, 0, MEM_RELEASE);
        return false;
    }

    BYTE* remote_data_ptr = remote_shell + data_offset;

    // Fill ManualMapData with function pointers from kernel32/ntdll
    ManualMapData map_data{};
    map_data.image_base          = remote_base;
    map_data.pLoadLibraryA       = (decltype(map_data.pLoadLibraryA))
        GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    map_data.pGetProcAddress     = (decltype(map_data.pGetProcAddress))
        GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetProcAddress");
    map_data.pRtlAddFunctionTable = (decltype(map_data.pRtlAddFunctionTable))
        GetProcAddress(GetModuleHandleA("kernel32.dll"), "RtlAddFunctionTable");
    if (!map_data.pRtlAddFunctionTable) {
        map_data.pRtlAddFunctionTable = (decltype(map_data.pRtlAddFunctionTable))
            GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlAddFunctionTable");
    }
    map_data.result = 0;

    // Write shellcode bytes + data struct
    WriteProcessMemory(hProc, remote_shell, shellcode_stub, sc_size, nullptr);
    WriteProcessMemory(hProc, remote_data_ptr, &map_data, sizeof(map_data), nullptr);

    printf("[+] Shellcode: 0x%p (%zu bytes), data: 0x%p\n",
           remote_shell, sc_size, remote_data_ptr);

    // 6. Execute shellcode — try NtCreateThreadEx first, fallback to CreateRemoteThread
    HANDLE hThread = nullptr;

    auto pNtCreateThreadEx = (fn_NtCreateThreadEx)
        GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtCreateThreadEx");

    if (pNtCreateThreadEx) {
        NTSTATUS status = pNtCreateThreadEx(
            &hThread, THREAD_ALL_ACCESS, nullptr,
            hProc, remote_shell, remote_data_ptr,
            0, 0, 0, 0, nullptr);
        if (status < 0 || !hThread) {
            printf("[*] NtCreateThreadEx: 0x%lX, falling back to CreateRemoteThread\n", status);
            hThread = nullptr;
        }
    }

    if (!hThread) {
        hThread = CreateRemoteThread(hProc, nullptr, 0,
            (LPTHREAD_START_ROUTINE)remote_shell, remote_data_ptr, 0, nullptr);
    }

    if (!hThread) {
        printf("[!] Failed to create remote thread: %lu\n", GetLastError());
        VirtualFreeEx(hProc, remote_shell, 0, MEM_RELEASE);
        VirtualFreeEx(hProc, remote_base, 0, MEM_RELEASE);
        return false;
    }

    // 7. Wait for shellcode to finish (DllMain spawns its own thread, returns quickly)
    printf("[*] Waiting for shellcode...\n");
    DWORD wait = WaitForSingleObject(hThread, 30000);
    CloseHandle(hThread);

    if (wait == WAIT_TIMEOUT) {
        printf("[!] Shellcode timed out (30s) — not freeing allocations\n");
        return false;
    }

    // 8. Read result and free shellcode allocation (image stays mapped)
    ManualMapData result_data{};
    ReadProcessMemory(hProc, remote_data_ptr, &result_data, sizeof(result_data), nullptr);
    VirtualFreeEx(hProc, remote_shell, 0, MEM_RELEASE);

    if (result_data.result == 1) {
        printf("[+] Manual map succeeded! Base: 0x%p\n", remote_base);
        return true;
    }

    printf("[!] Shellcode error: 0x%lX\n", result_data.result);
    VirtualFreeEx(hProc, remote_base, 0, MEM_RELEASE);
    return false;
}

// =============================================================================
// Entry point
// =============================================================================

int main(int argc, char* argv[]) {
    printf("=== SSJJ-Internal Manual Map Injector ===\n\n");

    const char* process_name = "SSJJ_BattleClient_Unity.exe";
    const char* dll_name = "ssjj_overlay.dll";

    if (argc >= 2) process_name = argv[1];
    if (argc >= 3) dll_name = argv[2];

    char dll_path[MAX_PATH];
    GetFullPathNameA(dll_name, MAX_PATH, dll_path, nullptr);

    printf("[*] Target: %s\n", process_name);
    printf("[*] DLL:    %s\n\n", dll_path);

    // Read and validate DLL
    std::vector<BYTE> dll_raw;
    if (!read_dll(dll_path, dll_raw)) {
        printf("[!] Failed to read or validate DLL: %s\n", dll_path);
        return 1;
    }
    printf("[+] DLL loaded (%zu bytes)\n\n", dll_raw.size());

    // --- Step 1: Patch MicroClient.exe (suppress detection actions only) ---
    // Let GameGuard init, communicate, and handle auth normally.
    // Only patch NotifyCallback (suppress kill) and CheckHeartbeat (safety net).
    printf("[*] Waiting for MicroClient.exe...\n");
    while (!find_process("MicroClient.exe")) {
        Sleep(500);
    }
    Sleep(1000); // Brief delay for process initialization
    printf("[+] MicroClient.exe found, patching detection handlers...\n");
    patch_microclient();
    printf("\n");

    // --- Step 2: Wait for game process ---
    printf("[*] Waiting for %s...\n", process_name);
    DWORD pid = 0;
    while (!(pid = find_process(process_name))) {
        Sleep(500);
    }
    printf("[+] Found PID: %lu\n", pid);

    Sleep(5000); // Wait for game to fully initialize

    HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProc) {
        printf("[!] OpenProcess failed: %lu\n", GetLastError());
        return 1;
    }

    // --- Step 3: Manual map inject ---
    printf("[*] Mapping DLL...\n");
    bool ok = manual_map(hProc, dll_raw);
    CloseHandle(hProc);

    if (ok) {
        printf("[+] Done! Press END in-game to unload.\n");
    } else {
        printf("[!] Manual map injection failed.\n");
        return 1;
    }

    return 0;
}
