#pragma once

namespace hooks {

    // Install any low-level hooks (e.g. SwapBuffers, wndproc).
    // Currently unused because the C# MonoBehaviour payload provides the
    // OnGUI / Update callbacks natively.  Kept as an extension point.
    bool initialize();

    // Remove installed hooks and restore original function pointers.
    void shutdown();

} // namespace hooks
