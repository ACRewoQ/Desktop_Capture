#include "autostart.hpp"

#include "tray_core.hpp"
#include "win_util.hpp"

#include <vector>

namespace dc::tray {

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr wchar_t kValue[] = L"DesktopCapture";

bool readValue(const wchar_t* key, std::vector<std::uint8_t>& out, DWORD* type) {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key, kValue, RRF_RT_ANY | RRF_NOEXPAND, type, nullptr, &size) != ERROR_SUCCESS)
        return false;
    out.resize(size);
    if (RegGetValueW(HKEY_CURRENT_USER, key, kValue, RRF_RT_ANY | RRF_NOEXPAND, type, out.data(), &size) != ERROR_SUCCESS)
        return false;
    out.resize(size);
    return true;
}

} // namespace

std::string autostartCommand(const std::filesystem::path& exe) {
    return buildCommandLine(narrow(exe.wstring()), {"--background"});
}

Autostart autostartState(const std::filesystem::path& exe) {
    std::vector<std::uint8_t> data;
    DWORD type = 0;
    if (!readValue(kRunKey, data, &type) || (type != REG_SZ && type != REG_EXPAND_SZ)) return Autostart::Off;
    std::wstring command(reinterpret_cast<const wchar_t*>(data.data()), data.size() / sizeof(wchar_t));
    while (!command.empty() && command.back() == L'\0') command.pop_back();
    if (_wcsicmp(command.c_str(), widen(autostartCommand(exe)).c_str()) != 0) return Autostart::Other;
    std::vector<std::uint8_t> approved;
    if (readValue(kApprovedKey, approved, &type) && !startupApproved(approved)) return Autostart::Disabled;
    return Autostart::On;
}

bool setAutostart(const std::filesystem::path& exe, bool on, std::string* err) {
    LSTATUS s;
    if (on) {
        const std::wstring command = widen(autostartCommand(exe));
        s = RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kValue, REG_SZ, command.c_str(),
                            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        s = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kValue);
        if (s == ERROR_FILE_NOT_FOUND) s = ERROR_SUCCESS;
    }
    // Turning it on here means on: drop Windows' "turned off" mark too (it would win over the Run entry).
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kApprovedKey, kValue);
    if (s != ERROR_SUCCESS) {
        if (err) *err = errorText(static_cast<DWORD>(s));
        return false;
    }
    return true;
}

const char* autostartKey(Autostart a) {
    switch (a) {
    case Autostart::Off: return "off";
    case Autostart::On: return "on";
    case Autostart::Disabled: return "disabled";
    case Autostart::Other: return "other";
    }
    return "?";
}

} // namespace dc::tray
