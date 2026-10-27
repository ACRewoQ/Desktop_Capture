#include "sample_library.hpp"

#include "audio_file.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace dc::mixd {

using proto::ErrorCode;
namespace fs = std::filesystem;

namespace {

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

std::string jsonString(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out + "\"";
}

// Splits a relative dir into segments; false if a segment is empty or could leave the library.
bool splitDir(const std::string& dir, std::vector<std::string>& out) {
    if (dir.empty()) return true;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = dir.find('/', start);
        std::string seg = dir.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (seg.empty() || seg == "." || seg == ".." || seg.find_first_of("\\:") != std::string::npos) return false;
        out.push_back(std::move(seg));
        if (end == std::string::npos) return true;
        start = end + 1;
    }
}

LibraryListing fail(ErrorCode code, std::string message) {
    LibraryListing l;
    l.code = code;
    l.message = std::move(message);
    return l;
}

} // namespace

bool isSampleFileName(const std::string& name) {
    const auto dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return false;
    std::string ext = name.substr(dot + 1);
    for (auto& c : ext) c = lower(c);
    return ext == "wav" || ext == "flac" || ext == "mp3" || ext == "ogg";
}

bool naturalLess(const std::string& a, const std::string& b) {
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (isDigit(a[i]) && isDigit(b[j])) {
            std::size_t ie = i, je = j;
            while (ie < a.size() && isDigit(a[ie])) ++ie;
            while (je < b.size() && isDigit(b[je])) ++je;
            std::size_t is = i, js = j; // skip leading zeros for the value
            while (is + 1 < ie && a[is] == '0') ++is;
            while (js + 1 < je && b[js] == '0') ++js;
            if (ie - is != je - js) return ie - is < je - js;
            const int c = a.compare(is, ie - is, b, js, je - js);
            if (c != 0) return c < 0;
            i = ie;
            j = je;
            continue;
        }
        const auto ca = static_cast<unsigned char>(lower(a[i])), cb = static_cast<unsigned char>(lower(b[j]));
        if (ca != cb) return ca < cb;
        ++i;
        ++j;
    }
    if (a.size() - i != b.size() - j) return a.size() - i < b.size() - j;
    return a < b; // same apart from case / zeros: any fixed order
}

LibraryListing listSampleLibrary(const std::string& root, const std::string& dir, std::size_t offset,
                                 std::size_t count) {
    if (root.empty()) return fail(ErrorCode::Conflict, "no sampler library folder set");
    fs::path rootPath = pathFromUtf8(root);
    rootPath.make_preferred(); // C:/Sounds -> C:\Sounds, so the listed paths use one separator
    if (!rootPath.is_absolute()) return fail(ErrorCode::BadRequest, "the library folder must be an absolute path");
    std::vector<std::string> segs;
    if (!splitDir(dir, segs)) return fail(ErrorCode::BadRequest, "dir must be a relative path inside the library");

    std::error_code ec;
    if (!fs::is_directory(rootPath, ec)) return fail(ErrorCode::NotFound, "library folder not found: " + root);
    fs::path here = rootPath;
    std::string norm;
    for (const auto& s : segs) {
        here /= pathFromUtf8(s);
        norm += (norm.empty() ? "" : "/") + s;
    }
    if (!segs.empty() && !fs::is_directory(here, ec)) return fail(ErrorCode::NotFound, "no such folder in the library: " + norm);

    LibraryListing out;
    out.dir = norm;
    std::vector<LibraryEntry> all;
    fs::directory_iterator it(here, fs::directory_options::skip_permission_denied, ec);
    if (ec) return fail(ErrorCode::NotFound, "cannot read the folder: " + ec.message());
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (all.size() >= kLibraryMaxEntries) {
            out.truncated = true;
            break;
        }
        const auto& e = *it;
        LibraryEntry entry;
        entry.name = pathToUtf8(e.path().filename());
        if (entry.name.empty() || entry.name[0] == '.') continue; // hidden (".git", "._x.wav")
        std::error_code fec;
        if (e.is_directory(fec)) {
            entry.folder = true;
        } else if (e.is_regular_file(fec) && isSampleFileName(entry.name)) {
            entry.path = pathToUtf8(e.path());
            entry.size = e.file_size(fec);
            if (fec) entry.size = 0;
        } else {
            continue;
        }
        all.push_back(std::move(entry));
    }
    std::sort(all.begin(), all.end(), [](const LibraryEntry& a, const LibraryEntry& b) {
        if (a.folder != b.folder) return a.folder;
        return naturalLess(a.name, b.name);
    });
    out.total = all.size();
    out.offset = std::min(offset, all.size());
    const std::size_t n = std::min(count, all.size() - out.offset);
    out.entries.assign(std::make_move_iterator(all.begin() + static_cast<std::ptrdiff_t>(out.offset)),
                       std::make_move_iterator(all.begin() + static_cast<std::ptrdiff_t>(out.offset + n)));
    return out;
}

std::string libraryListingJson(const LibraryListing& l) {
    std::string json = "{\"dir\":" + jsonString(l.dir) + ",\"total\":" + std::to_string(l.total) +
                       ",\"offset\":" + std::to_string(l.offset) + ",\"truncated\":" + (l.truncated ? "true" : "false") +
                       ",\"entries\":[";
    bool first = true;
    for (const auto& e : l.entries) {
        if (!first) json += ",";
        first = false;
        json += "{\"name\":" + jsonString(e.name);
        if (e.folder) json += ",\"folder\":true}";
        else json += ",\"path\":" + jsonString(e.path) + ",\"size\":" + std::to_string(e.size) + "}";
    }
    return json + "]}";
}

} // namespace dc::mixd
