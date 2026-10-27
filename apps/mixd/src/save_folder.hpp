#pragma once

// Save folders of the recordings (/capture/directory, capd) and the per-track takes (/recorder/directory):
// what /system/folder/check reports about one (protocol.md 5.4.2), so that the UI can say before a
// recording whether the folder will work, and the checks a take does before it starts. Paths are UTF-8.

#include <cstdint>
#include <optional>
#include <string>

namespace dc::mixd {

// A take refuses to start with less than this free on the folder's drive (capd keeps its own reserve,
// which is at least as large, for the video file).
inline constexpr std::uint64_t kRecorderMinFreeBytes = 256ull << 20;

struct FolderCheck {
    std::string path;          // the folder that is used: the setting, or the default when it is ""
    bool isDefault = false;    // the setting is ""
    bool absolute = false;
    bool exists = false;       // is a folder now
    bool writable = false;     // a file can be made in it (or, when it does not exist yet, in the nearest existing
                               // folder above it, where the recording would create it)
    std::optional<std::uint64_t> freeBytes;  // on its drive
    // "" (usable), not_absolute, no_drive (not even the drive / share exists), not_folder (a file has the name),
    // not_writable
    std::string problem;
};

// setting: the parameter's value; defaultFolder: used when setting is "".
FolderCheck checkSaveFolder(const std::string& setting, const std::string& defaultFolder);

// The JSON returned by /system/folder/check.
std::string folderCheckJson(const FolderCheck& check);

// The Videos known folder (Windows) or ~/Videos: capd's folder when /capture/directory is "".
std::string defaultVideoDirectory();

} // namespace dc::mixd
