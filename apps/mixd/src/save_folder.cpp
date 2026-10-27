#include "save_folder.hpp"

#include "audio_file.hpp"
#include "sample_library.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace dc::mixd {

namespace fs = std::filesystem;

namespace {

// Makes and removes a small file: the only check that also sees ACLs, read-only media and Controlled Folder Access.
bool canWriteIn(const fs::path& dir) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path probe = dir / pathFromUtf8(".dc-write-check-" + std::to_string(stamp) + ".tmp");
    bool ok = false;
    {
        std::ofstream f(probe, std::ios::binary);
        ok = f && (f << "x") && f.flush();
    }
    std::error_code ec;
    fs::remove(probe, ec);
    return ok;
}

} // namespace

FolderCheck checkSaveFolder(const std::string& setting, const std::string& defaultFolder) {
    FolderCheck c;
    c.isDefault = setting.empty();
    fs::path p = pathFromUtf8(c.isDefault ? defaultFolder : setting);
    p.make_preferred();
    c.path = pathToUtf8(p);
    c.absolute = p.is_absolute();
    if (!c.absolute) {
        c.problem = "not_absolute";
        return c;
    }
    std::error_code ec;
    if (fs::exists(p, ec) && !fs::is_directory(p, ec)) {
        c.problem = "not_folder";
        return c;
    }
    c.exists = fs::is_directory(p, ec);
    // a missing folder is made when the recording starts: what counts is the nearest folder that is there
    fs::path there = p;
    while (!fs::is_directory(there, ec)) {
        if (fs::exists(there, ec)) {  // a file where a folder should be made
            c.problem = "not_folder";
            return c;
        }
        const fs::path up = there.parent_path();
        if (up.empty() || up == there) {
            c.problem = "no_drive";
            return c;
        }
        there = up;
    }
    const auto space = fs::space(there, ec);
    if (!ec) c.freeBytes = static_cast<std::uint64_t>(space.available);
    c.writable = canWriteIn(there);
    if (!c.writable) c.problem = "not_writable";
    return c;
}

std::string folderCheckJson(const FolderCheck& c) {
    std::string json = "{\"path\":" + jsonString(c.path) + ",\"default\":" + (c.isDefault ? "true" : "false") +
                       ",\"absolute\":" + (c.absolute ? "true" : "false") + ",\"exists\":" +
                       (c.exists ? "true" : "false") + ",\"writable\":" + (c.writable ? "true" : "false");
    if (c.freeBytes) json += ",\"free_bytes\":" + std::to_string(*c.freeBytes);
    return json + ",\"problem\":" + jsonString(c.problem) + "}";
}

std::string defaultVideoDirectory() {
#ifdef _WIN32
    PWSTR path = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_CREATE, nullptr, &path))) out = path;
    CoTaskMemFree(path);
    return out.empty() ? std::string(".") : pathToUtf8(out);
#else
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/Videos" : std::string(".");
#endif
}

} // namespace dc::mixd
