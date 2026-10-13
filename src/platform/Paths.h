// Where the executable lives. Assets resolve from here on every platform;
// inside a macOS .app that is the bundle's Resources directory, which is
// where a bundle keeps them. Windows also keeps settings/layout here; Linux
// and macOS use the desktop's own configuration directory.
#pragma once

#include <string>

namespace platform
{

// UTF-8, no trailing separator.
std::string ExecutableDir();

// The desktop's per-user configuration root: XDG on Linux, Application
// Support on macOS. Not implemented on Windows, which keeps settings beside
// the executable instead.
std::string UserConfigDir();

} // namespace platform
