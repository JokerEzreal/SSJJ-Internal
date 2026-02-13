#include "pch.h"
#include "hooks.h"
#include "core/globals.h"
#include <intrin.h>

namespace hooks {

// ============================================================================
// Types & structures
// ============================================================================

typedef LONG NTSTATUS;
#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

typedef NTSTATUS(NTAPI* fn_NtReadVirtualMemory)(
    HANDLE  ProcessHandle,
    PVOID   BaseAddress,
    PVOID   Buffer,
    SIZE_T  NumberOfBytesToRead,
    PSIZE_T NumberOfBytesRead
);

// PEB structures (SDK definitions are incomplete)
struct MY_LDR_DATA_TABLE_ENTRY {
    LIST_ENTRY InLoadOrderLinks;
    LIST_ENTRY InMemoryOrderLinks;
    LIST_ENTRY InInitializationOrderLinks;
    PVOID      DllBase;
    PVOID      EntryPoint;
    ULONG      SizeOfImage;
    // ... more fields follow, but we only need up to here
};

struct MY_PEB_LDR_DATA {
    ULONG      Length;
    BOOLEAN    Initialized;
    PVOID      SsHandle;
    LIST_ENTRY InLoadOrderModuleList;
    LIST_ENTRY InMemoryOrderModuleList;
    LIST_ENTRY InInitializationOrderModuleList;
};

struct MY_PEB {
    BYTE       Reserved1[2];
    BYTE       BeingDebugged;
    BYTE       Reserved2[1];
    PVOID      Reserved3[2];
    MY_PEB_LDR_DATA* Ldr;
};

// ============================================================================
// State
// ============================================================================

static fn_NtReadVirtualMemory s_orig_NtReadVirtualMemory = nullptr;
static uint8_t  s_saved_bytes[20] = {};
static void*    s_ntread_addr     = nullptr;
static size_t   s_patch_size      = 0;

static uintptr_t s_dll_base = 0;
static size_t    s_dll_size = 0;

static void* s_trampoline_page = nullptr;

static bool s_stealth_installed = false;

// ============================================================================
// Helpers
// ============================================================================

static bool is_own_process(HANDLE h)
{
    if (h == GetCurrentProcess())
        return true;
    // GetProcessId returns 0 on failure; our PID is never 0
    DWORD pid = GetProcessId(h);
    return pid != 0 && pid == GetCurrentProcessId();
}

// ============================================================================
// NtReadVirtualMemory hook
// ============================================================================

static NTSTATUS NTAPI hook_NtReadVirtualMemory(
    HANDLE  ProcessHandle,
    PVOID   BaseAddress,
    PVOID   Buffer,
    SIZE_T  NumberOfBytesToRead,
    PSIZE_T NumberOfBytesRead)
{
    // Forward to original syscall
    NTSTATUS status = s_orig_NtReadVirtualMemory(
        ProcessHandle, BaseAddress, Buffer, NumberOfBytesToRead, NumberOfBytesRead);

    if (!NT_SUCCESS(status))
        return status;

    // Only scrub reads targeting our own process
    if (!is_own_process(ProcessHandle))
        return status;

    uintptr_t read_base = reinterpret_cast<uintptr_t>(BaseAddress);
    size_t    read_size = (NumberOfBytesRead && *NumberOfBytesRead)
                              ? *NumberOfBytesRead
                              : NumberOfBytesToRead;

    // Scrub bytes that overlap our DLL range
    uintptr_t dll_end = s_dll_base + s_dll_size;
    if (read_base < dll_end && (read_base + read_size) > s_dll_base) {
        auto* buf = static_cast<uint8_t*>(Buffer);
        uintptr_t scrub_start = (read_base > s_dll_base) ? read_base : s_dll_base;
        uintptr_t scrub_end   = ((read_base + read_size) < dll_end)
                                    ? (read_base + read_size) : dll_end;
        size_t buf_offset = static_cast<size_t>(scrub_start - read_base);
        size_t scrub_len  = static_cast<size_t>(scrub_end - scrub_start);
        memset(buf + buf_offset, 0, scrub_len);
    }

    // Also hide the trampoline page
    if (s_trampoline_page) {
        uintptr_t tp_base = reinterpret_cast<uintptr_t>(s_trampoline_page);
        uintptr_t tp_end  = tp_base + 0x1000;
        if (read_base < tp_end && (read_base + read_size) > tp_base) {
            auto* buf = static_cast<uint8_t*>(Buffer);
            uintptr_t scrub_start = (read_base > tp_base) ? read_base : tp_base;
            uintptr_t scrub_end   = ((read_base + read_size) < tp_end)
                                        ? (read_base + read_size) : tp_end;
            size_t buf_offset = static_cast<size_t>(scrub_start - read_base);
            size_t scrub_len  = static_cast<size_t>(scrub_end - scrub_start);
            memset(buf + buf_offset, 0, scrub_len);
        }
    }

    return status;
}

// ============================================================================
// Syscall trampoline builder
//
// x64 ntdll syscall stubs look like:
//   4C 8B D1          mov r10, rcx
//   B8 XX XX XX XX    mov eax, <syscall_number>
//   0F 05             syscall
//   C3                ret
//
// We extract the syscall number and build our own minimal stub.
// ============================================================================

static fn_NtReadVirtualMemory build_syscall_trampoline(uint8_t* func)
{
    // Validate ntdll stub pattern: mov r10,rcx = 4C 8B D1; mov eax,imm32 = B8
    if (func[0] != 0x4C || func[1] != 0x8B || func[2] != 0xD1 || func[3] != 0xB8)
        return nullptr;

    DWORD syscall_number = *reinterpret_cast<DWORD*>(func + 4);

    s_trampoline_page = VirtualAlloc(
        nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!s_trampoline_page)
        return nullptr;

    auto* code = static_cast<uint8_t*>(s_trampoline_page);
    int i = 0;

    // mov r10, rcx
    code[i++] = 0x4C; code[i++] = 0x8B; code[i++] = 0xD1;
    // mov eax, <syscall_number>
    code[i++] = 0xB8;
    memcpy(code + i, &syscall_number, 4); i += 4;
    // syscall
    code[i++] = 0x0F; code[i++] = 0x05;
    // ret
    code[i++] = 0xC3;

    return reinterpret_cast<fn_NtReadVirtualMemory>(code);
}

// ============================================================================
// Inline hook installer (absolute jump via mov rax, imm64; jmp rax)
// ============================================================================

static bool install_abs_jump(void* target, void* destination)
{
    DWORD old_protect;
    // 12 bytes: mov rax, imm64 (10) + jmp rax (2)
    constexpr size_t JUMP_SIZE = 12;

    if (!VirtualProtect(target, JUMP_SIZE, PAGE_EXECUTE_READWRITE, &old_protect))
        return false;

    auto* p = static_cast<uint8_t*>(target);

    // Save original bytes
    memcpy(s_saved_bytes, p, JUMP_SIZE);
    s_patch_size = JUMP_SIZE;

    // mov rax, <absolute address>   (48 B8 + 8-byte imm)
    p[0] = 0x48;
    p[1] = 0xB8;
    auto addr = reinterpret_cast<uintptr_t>(destination);
    memcpy(p + 2, &addr, 8);
    // jmp rax
    p[10] = 0xFF;
    p[11] = 0xE0;

    VirtualProtect(target, JUMP_SIZE, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), target, JUMP_SIZE);
    return true;
}

// ============================================================================
// PEB module unlinking
// ============================================================================

static void unlink_entry(LIST_ENTRY* entry)
{
    if (entry->Flink && entry->Blink &&
        entry->Flink != entry && entry->Blink != entry) {
        entry->Blink->Flink = entry->Flink;
        entry->Flink->Blink = entry->Blink;
        entry->Flink = entry;
        entry->Blink = entry;
    }
}

static void unlink_from_peb(HMODULE hmod)
{
    // Read PEB pointer from the TEB (GS:[0x60] on x64)
    auto* peb = reinterpret_cast<MY_PEB*>(__readgsqword(0x60));
    if (!peb || !peb->Ldr)
        return;

    auto* ldr = peb->Ldr;

    // Walk InLoadOrderModuleList to find our entry
    LIST_ENTRY* head = &ldr->InLoadOrderModuleList;
    for (LIST_ENTRY* cur = head->Flink; cur != head; cur = cur->Flink) {
        auto* mod = CONTAINING_RECORD(cur, MY_LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
        if (mod->DllBase == static_cast<void*>(hmod)) {
            unlink_entry(&mod->InLoadOrderLinks);
            unlink_entry(&mod->InMemoryOrderLinks);
            unlink_entry(&mod->InInitializationOrderLinks);
            break;
        }
    }
}

// ============================================================================
// PE header erasure
// ============================================================================

static void erase_pe_header(HMODULE hmod)
{
    DWORD old_protect;
    if (VirtualProtect(hmod, 0x1000, PAGE_READWRITE, &old_protect)) {
        SecureZeroMemory(hmod, 0x1000);
        VirtualProtect(hmod, 0x1000, old_protect, &old_protect);
    }
}

// ============================================================================
// Public API
// ============================================================================

bool install_stealth()
{
    if (s_stealth_installed)
        return true;

    HMODULE hmod = globals::dll_handle;
    if (!hmod)
        return false;

    // --- Get our DLL's memory range (before erasing PE header!) ---
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(hmod);
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<uint8_t*>(hmod) + dos->e_lfanew);
    s_dll_base = reinterpret_cast<uintptr_t>(hmod);
    s_dll_size = nt->OptionalHeader.SizeOfImage;

    // --- 1. Hook NtReadVirtualMemory ---
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll) {
        auto* func = reinterpret_cast<uint8_t*>(
            GetProcAddress(ntdll, "NtReadVirtualMemory"));
        if (func) {
            s_ntread_addr = func;
            s_orig_NtReadVirtualMemory = build_syscall_trampoline(func);
            if (s_orig_NtReadVirtualMemory) {
                if (!install_abs_jump(func, reinterpret_cast<void*>(&hook_NtReadVirtualMemory))) {
                    // Hook installation failed, clear the original pointer
                    s_orig_NtReadVirtualMemory = nullptr;
                }
            }
        }
    }

    // --- 2. Unlink from PEB module lists ---
    unlink_from_peb(hmod);

    // --- 3. Erase PE header (must be last — we already read SizeOfImage) ---
    erase_pe_header(hmod);

    s_stealth_installed = true;
    return true;
}

bool initialize()
{
    // Stealth should already be installed from early init.
    // If not, install now as a fallback.
    if (!s_stealth_installed)
        install_stealth();
    return true;
}

void shutdown()
{
    // Restore NtReadVirtualMemory
    if (s_ntread_addr && s_patch_size > 0) {
        DWORD old_protect;
        if (VirtualProtect(s_ntread_addr, s_patch_size,
                           PAGE_EXECUTE_READWRITE, &old_protect)) {
            memcpy(s_ntread_addr, s_saved_bytes, s_patch_size);
            VirtualProtect(s_ntread_addr, s_patch_size, old_protect, &old_protect);
            FlushInstructionCache(GetCurrentProcess(), s_ntread_addr, s_patch_size);
        }
    }

    // Free trampoline
    if (s_trampoline_page) {
        VirtualFree(s_trampoline_page, 0, MEM_RELEASE);
        s_trampoline_page = nullptr;
    }

    s_stealth_installed = false;
}

} // namespace hooks
