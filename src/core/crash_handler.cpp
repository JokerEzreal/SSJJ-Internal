#include "crash_handler.h"
#include <Windows.h>
#include <DbgHelp.h>
#include <cstdio>
#include <ctime>
#include <cstdlib>

#pragma comment(lib, "dbghelp.lib")

namespace crash_handler {

static PVOID s_handler = nullptr;

// ---------------------------------------------------------------------------
// Map exception code to a human-readable string
// ---------------------------------------------------------------------------
static const char* exception_name(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:   return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
        case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
        default:                              return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------------
// Build crash log path on Desktop
// ---------------------------------------------------------------------------
static bool build_crash_path(char* out, size_t out_size) {
    const char* userprofile = getenv("USERPROFILE");
    if (!userprofile) return false;

    char timestamp[64];
    time_t now = time(nullptr);
    struct tm t_buf;
    localtime_s(&t_buf, &now);
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &t_buf);

    snprintf(out, out_size, "%s\\Desktop\\ssjj_crash_%s.txt", userprofile, timestamp);
    return true;
}

// ---------------------------------------------------------------------------
// Walk the stack and write frames to file
// ---------------------------------------------------------------------------
static void write_stack_trace(FILE* f, CONTEXT* ctx) {
    HANDLE process = GetCurrentProcess();
    HANDLE thread  = GetCurrentThread();

    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(process, NULL, TRUE);

    STACKFRAME64 frame = {};
    frame.AddrPC.Offset    = ctx->Rip;
    frame.AddrPC.Mode      = AddrModeFlat;
    frame.AddrFrame.Offset = ctx->Rbp;
    frame.AddrFrame.Mode   = AddrModeFlat;
    frame.AddrStack.Offset = ctx->Rsp;
    frame.AddrStack.Mode   = AddrModeFlat;

    fprintf(f, "\n=== STACK TRACE ===\n");
    for (int i = 0; i < 32; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread,
                         &frame, ctx, NULL,
                         SymFunctionTableAccess64,
                         SymGetModuleBase64, NULL))
            break;

        DWORD64 addr = frame.AddrPC.Offset;
        if (addr == 0) break;

        // Get module name
        HMODULE mod = NULL;
        char mod_name[MAX_PATH] = "???";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)addr, &mod) && mod) {
            GetModuleFileNameA(mod, mod_name, MAX_PATH);
            // Extract just the filename
            char* slash = strrchr(mod_name, '\\');
            if (slash) memmove(mod_name, slash + 1, strlen(slash + 1) + 1);
        }

        DWORD64 mod_base = SymGetModuleBase64(process, addr);
        DWORD64 offset   = mod_base ? (addr - mod_base) : 0;

        // Try to get symbol name
        char sym_buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* sym = (SYMBOL_INFO*)sym_buf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen   = 255;

        DWORD64 displacement = 0;
        if (SymFromAddr(process, addr, &displacement, sym)) {
            fprintf(f, "  [%2d] %s+0x%llX  %s+0x%llX\n",
                i, mod_name, (unsigned long long)offset,
                sym->Name, (unsigned long long)displacement);
        } else {
            fprintf(f, "  [%2d] %s+0x%llX  (no symbol)\n",
                i, mod_name, (unsigned long long)offset);
        }
    }

    SymCleanup(process);
}

// ---------------------------------------------------------------------------
// Vectored Exception Handler
// ---------------------------------------------------------------------------
static LONG WINAPI crash_veh(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;

    // Only handle fatal exceptions (skip C++ exceptions, breakpoints, etc.)
    if (code == EXCEPTION_ACCESS_VIOLATION ||
        code == EXCEPTION_STACK_OVERFLOW ||
        code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
        code == EXCEPTION_FLT_DIVIDE_BY_ZERO ||
        code == EXCEPTION_IN_PAGE_ERROR ||
        code == EXCEPTION_ARRAY_BOUNDS_EXCEEDED ||
        code == EXCEPTION_PRIV_INSTRUCTION)
    {
        char path[MAX_PATH];
        if (!build_crash_path(path, sizeof(path)))
            return EXCEPTION_CONTINUE_SEARCH;

        FILE* f = fopen(path, "w");
        if (!f) return EXCEPTION_CONTINUE_SEARCH;

        fprintf(f, "========================================\n");
        fprintf(f, "SSJJ Internal - Crash Report\n");
        fprintf(f, "========================================\n\n");

        fprintf(f, "Exception: 0x%08lX (%s)\n", code, exception_name(code));
        fprintf(f, "Address:   0x%016llX\n",
            (unsigned long long)ep->ExceptionRecord->ExceptionAddress);

        // For ACCESS_VIOLATION, show read/write and target address
        if (code == EXCEPTION_ACCESS_VIOLATION &&
            ep->ExceptionRecord->NumberParameters >= 2) {
            const char* op = (ep->ExceptionRecord->ExceptionInformation[0] == 0)
                ? "READ" : "WRITE";
            fprintf(f, "Type:      %s at 0x%016llX\n", op,
                (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
        }

        // Find which module the crash is in
        HMODULE mod = NULL;
        char mod_name[MAX_PATH] = "???";
        void* crash_addr = ep->ExceptionRecord->ExceptionAddress;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)crash_addr, &mod) && mod) {
            GetModuleFileNameA(mod, mod_name, MAX_PATH);
            char* slash = strrchr(mod_name, '\\');
            if (slash) memmove(mod_name, slash + 1, strlen(slash + 1) + 1);

            DWORD64 base = (DWORD64)mod;
            DWORD64 off  = (DWORD64)crash_addr - base;
            fprintf(f, "Module:    %s + 0x%llX (base=0x%llX)\n",
                mod_name, (unsigned long long)off, (unsigned long long)base);
        }

        // Registers
        CONTEXT* ctx = ep->ContextRecord;
        fprintf(f, "\n=== REGISTERS ===\n");
        fprintf(f, "RAX=%016llX  RBX=%016llX  RCX=%016llX  RDX=%016llX\n",
            ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx);
        fprintf(f, "RSI=%016llX  RDI=%016llX  RBP=%016llX  RSP=%016llX\n",
            ctx->Rsi, ctx->Rdi, ctx->Rbp, ctx->Rsp);
        fprintf(f, "R8 =%016llX  R9 =%016llX  R10=%016llX  R11=%016llX\n",
            ctx->R8, ctx->R9, ctx->R10, ctx->R11);
        fprintf(f, "R12=%016llX  R13=%016llX  R14=%016llX  R15=%016llX\n",
            ctx->R12, ctx->R13, ctx->R14, ctx->R15);
        fprintf(f, "RIP=%016llX  EFLAGS=%08lX\n", ctx->Rip, ctx->EFlags);

        // Stack trace
        // Note: StackWalk64 may not work in stack overflow, but we try
        if (code != EXCEPTION_STACK_OVERFLOW) {
            write_stack_trace(f, ctx);
        } else {
            fprintf(f, "\n(Stack trace skipped for STACK_OVERFLOW)\n");
        }

        // Loaded modules summary
        fprintf(f, "\n=== KEY MODULES ===\n");
        const char* key_mods[] = {
            "ssjj_overlay.dll", "mono-2.0-bdwgc.dll", "UnityPlayer.dll",
            "GameAssembly.dll", "ntdll.dll", "KERNELBASE.dll"
        };
        for (const char* name : key_mods) {
            HMODULE h = GetModuleHandleA(name);
            if (h) {
                fprintf(f, "  %s = 0x%016llX\n", name, (unsigned long long)h);
            }
        }

        fprintf(f, "\n========================================\n");
        fprintf(f, "Feed this file to AI for crash analysis\n");
        fprintf(f, "========================================\n");

        fclose(f);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void install() {
    if (!s_handler) {
        // Use VEH (first-chance) so we catch crashes before they're swallowed
        s_handler = AddVectoredExceptionHandler(1, crash_veh);
    }
}

void uninstall() {
    if (s_handler) {
        RemoveVectoredExceptionHandler(s_handler);
        s_handler = nullptr;
    }
}

} // namespace crash_handler
