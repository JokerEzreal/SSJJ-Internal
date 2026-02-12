#pragma once

namespace crash_handler {

// Install a Vectored Exception Handler that writes crash info to
// %USERPROFILE%\Desktop\ssjj_crash_<timestamp>.txt on fatal exceptions.
void install();

// Remove the handler (called during cleanup).
void uninstall();

} // namespace crash_handler
