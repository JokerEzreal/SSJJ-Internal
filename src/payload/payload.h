#pragma once

#include <cstdint>

namespace payload {

    // Register C++<->C# internal calls, load the embedded C# assembly into
    // the Mono runtime, and invoke Payload.Loader.Init() which creates the
    // persistent MonoBehaviour overlay object.
    bool initialize();

    // Tear down the payload (destroy the overlay GameObject, etc.).
    void shutdown();

    // Heartbeat counter -- incremented each OnGUI callback from C#.
    // Used by the watchdog thread to detect stalls.
    uint64_t get_heartbeat();

    // Attempt to re-create the MonoBehaviour overlay if it was destroyed.
    // Safe to call multiple times -- creates a new GameObject each time.
    bool reinject();

} // namespace payload
