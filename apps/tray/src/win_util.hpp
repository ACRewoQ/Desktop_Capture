#pragma once
// Small Windows helpers for dc-tray: text conversion, folders, the tray's own log.

#include <windows.h>

#include <filesystem>
#include <string>

namespace dc::tray {

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& wide);

std::filesystem::path exePath();                 // this program
std::filesystem::path appDataDir();              // %APPDATA%\DesktopCapture (settings, as mixd's stores)
std::filesystem::path localDataDir();            // %LOCALAPPDATA%\DesktopCapture (logs, status)
std::filesystem::path logDir();                  // %LOCALAPPDATA%\DesktopCapture\logs

// Appends a time-stamped line to logs\tray.log.
void logLine(const std::string& text);

// Reads a whole file as bytes (UTF-8 text); empty when it does not exist.
std::string readFile(const std::filesystem::path& path);
// Writes through a temporary file and a rename, so a reader never sees half a file.
bool writeFileAtomic(const std::filesystem::path& path, const std::string& text);

// Text for a Windows error code.
std::string errorText(DWORD code);

// For command-line use (--help, --autostart status): the redirected stdout, or the console of the shell that
// started this GUI program.
void printOut(const std::string& utf8);

} // namespace dc::tray
