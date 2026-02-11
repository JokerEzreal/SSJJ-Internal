#include "hooks.h"

namespace hooks {

// ---------------------------------------------------------------------------
// No native hooks are required at the moment.
//
// The C# payload injects a MonoBehaviour whose OnGUI() and Update() methods
// call back into C++ via Mono internal calls, so we already have a valid
// rendering context (OnGUI runs inside Unity's IMGUI pass) and a per-frame
// tick (Update) without touching SwapBuffers, Present, or WndProc.
//
// If future work requires intercepting DirectX/OpenGL calls or the window
// procedure, install those hooks here.
// ---------------------------------------------------------------------------

bool initialize()
{
    // Nothing to hook -- the C# MonoBehaviour handles the callback context.
    return true;
}

void shutdown()
{
    // Restore any hooks that were installed.  Currently a no-op.
}

} // namespace hooks
