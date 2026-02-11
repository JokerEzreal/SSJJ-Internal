#pragma once

namespace payload {

    // Register C++<->C# internal calls, load the embedded C# assembly into
    // the Mono runtime, and invoke Payload.Loader.Init() which creates the
    // persistent MonoBehaviour overlay object.
    bool initialize();

    // Tear down the payload (destroy the overlay GameObject, etc.).
    void shutdown();

} // namespace payload
