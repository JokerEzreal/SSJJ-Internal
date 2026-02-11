#pragma once
#include <Windows.h>
#include <atomic>

namespace globals {
    inline HMODULE   dll_handle = nullptr;
    inline std::atomic<bool> running{true};
    inline std::atomic<bool> initialized{false};
}
