#pragma once
// Start dc-tray at logon: the per-user Run key (no elevation). Windows' Settings > Apps > Startup can turn the
// entry off without removing it; that is kept under Explorer\StartupApproved\Run and counted here as off.

#include <filesystem>
#include <string>

namespace dc::tray {

enum class Autostart {
    Off,       // no entry
    On,        // entry for this exe, allowed
    Disabled,  // entry for this exe, turned off in Windows' startup apps
    Other,     // entry pointing to another dc-tray.exe (an older folder)
};

Autostart autostartState(const std::filesystem::path& exe);
// On: writes "<exe>" --background and clears a "turned off" mark. Off: removes both.
bool setAutostart(const std::filesystem::path& exe, bool on, std::string* err);
std::string autostartCommand(const std::filesystem::path& exe);
const char* autostartKey(Autostart a);  // on, off, disabled, other

} // namespace dc::tray
