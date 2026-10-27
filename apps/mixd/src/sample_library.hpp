#pragma once

// The sampler's library folder (/sampler/library/folder, protocol.md 5.4.3): lists one folder level
// at a time (subfolders and the audio files the sampler can read) for /sampler/library/list, so that
// the UI and remote controllers can pick pad files without typing paths. Paths are UTF-8.

#include "dc/protocol/errors.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dc::mixd {

inline constexpr std::size_t kLibraryListDefaultCount = 200;
inline constexpr std::size_t kLibraryListMaxCount = 1000;
inline constexpr std::size_t kLibraryMaxEntries = 20000; // per folder; the rest is left out (truncated)

struct LibraryEntry {
    std::string name;
    bool folder = false;
    std::string path;       // absolute, files only
    std::uint64_t size = 0; // bytes, files only
};

struct LibraryListing {
    proto::ErrorCode code = proto::ErrorCode::Ok;
    std::string message;
    std::string dir;         // normalized, "" = the library folder itself
    std::size_t total = 0;   // entries in the folder (before offset / count)
    bool truncated = false;  // the folder had more than kLibraryMaxEntries entries
    std::size_t offset = 0;
    std::vector<LibraryEntry> entries; // folders first, then files, by name (numbers in natural order)
};

// WAV / FLAC / MP3 / Ogg Vorbis by extension, any case.
bool isSampleFileName(const std::string& name);

// "Kick 10" after "Kick 9"; case-insensitive for ASCII.
bool naturalLess(const std::string& a, const std::string& b);

// root: /sampler/library/folder. dir: relative, "/" separated ("" = top); "..", ".", "\", ":" and
// empty segments are refused (bad_request). Errors: conflict (no library set), bad_request (root not
// absolute), not_found (root or dir is not a folder).
LibraryListing listSampleLibrary(const std::string& root, const std::string& dir, std::size_t offset,
                                 std::size_t count);

// s as a JSON string literal (quotes and escapes).
std::string jsonString(const std::string& s);

// The JSON returned by /sampler/library/list.
std::string libraryListingJson(const LibraryListing& listing);

} // namespace dc::mixd
