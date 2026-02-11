#include <Windows.h>
#include <TlHelp32.h>
#include <cstdio>
#include <cstring>

DWORD find_process(const char* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32 pe = { sizeof(pe) };
    for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe)) {
        if (_stricmp(pe.szExeFile, name) == 0) {
            CloseHandle(snap);
            return pe.th32ProcessID;
        }
    }
    CloseHandle(snap);
    return 0;
}

bool inject(DWORD pid, const char* dll_path) {
    HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!proc) {
        printf("[!] Failed to open process (error: %lu)\n", GetLastError());
        return false;
    }

    size_t path_len = strlen(dll_path) + 1;
    void* remote_mem = VirtualAllocEx(proc, nullptr, path_len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_mem) {
        printf("[!] Failed to allocate remote memory\n");
        CloseHandle(proc);
        return false;
    }

    WriteProcessMemory(proc, remote_mem, dll_path, path_len, nullptr);

    HANDLE thread = CreateRemoteThread(proc, nullptr, 0,
        (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA"),
        remote_mem, 0, nullptr);

    if (!thread) {
        printf("[!] Failed to create remote thread (error: %lu)\n", GetLastError());
        VirtualFreeEx(proc, remote_mem, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    WaitForSingleObject(thread, INFINITE);

    DWORD exit_code = 0;
    GetExitCodeThread(thread, &exit_code);

    CloseHandle(thread);
    VirtualFreeEx(proc, remote_mem, 0, MEM_RELEASE);
    CloseHandle(proc);

    printf("[+] Injection %s (module handle: 0x%lX)\n",
           exit_code ? "succeeded" : "FAILED", exit_code);
    return exit_code != 0;
}

int main(int argc, char* argv[]) {
    printf("=== SSJJ-Internal Injector ===\n\n");

    const char* process_name = "SSJJ_BattleClient_Unity.exe";
    const char* dll_name = "ssjj_overlay.dll";

    if (argc >= 2) process_name = argv[1];
    if (argc >= 3) dll_name = argv[2];

    // Get full DLL path
    char dll_path[MAX_PATH];
    GetFullPathNameA(dll_name, MAX_PATH, dll_path, nullptr);

    printf("[*] Target process: %s\n", process_name);
    printf("[*] DLL path: %s\n", dll_path);

    // Check DLL exists
    if (GetFileAttributesA(dll_path) == INVALID_FILE_ATTRIBUTES) {
        printf("[!] DLL file not found: %s\n", dll_path);
        return 1;
    }

    printf("[*] Waiting for %s...\n", process_name);
    DWORD pid = 0;
    while (!(pid = find_process(process_name))) {
        Sleep(500);
    }
    printf("[+] Found PID: %lu\n", pid);

    Sleep(5000); // Wait for game to fully load
    printf("[*] Injecting...\n");

    if (inject(pid, dll_path)) {
        printf("[+] Done! Press END in-game to unload.\n");
    } else {
        printf("[!] Injection failed.\n");
        return 1;
    }

    return 0;
}
