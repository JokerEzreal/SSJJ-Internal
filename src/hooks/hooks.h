#pragma once

namespace hooks {

    // Hide our DLL from memory scanners. Call as early as possible.
    // - Hooks NtReadVirtualMemory to zero out reads overlapping our DLL
    // - Unlinks our DLL from PEB module lists
    // - Erases our PE header
    bool install_stealth();

    // Install any additional hooks (currently defers to install_stealth).
    bool initialize();

    // Remove installed hooks and restore original function pointers.
    void shutdown();

} // namespace hooks
