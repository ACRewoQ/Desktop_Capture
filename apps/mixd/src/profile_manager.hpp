#pragma once

// Profiles and mixer snapshots (docs/spec/protocol.md 5.10), on the network thread: lists the profile
// files under /profile/list, saves the settings of the tree (and the plugins' states) to the current
// profile when they change, restores a profile at start and on /profile/load, keeps the snapshots of
// the current profile and recalls them. The engine's parts it needs (effect racks, device names) come
// through Hooks so the mock server can use it too.

#include "dc/control/server.hpp"
#include "dc/protocol/profile.hpp"

#include <asio.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace dc::mixd {

class ProfileManager {
public:
    struct Hooks {
        // FxManager::restore / restored / pluginStates. Without them effect slots are not restored.
        std::function<std::vector<proto::Change>(const std::map<int, std::vector<proto::FxRestore>>&,
                                                 const std::map<std::string, std::string>&)>
            restoreFx;
        std::function<void()> restored;
        std::function<std::map<std::string, std::string>()> pluginStates;
        // Names of the given device ids, as far as the engine knows them.
        std::function<std::vector<proto::SavedDevice>(const std::vector<std::string>& ids)> deviceNames;
        // A profile was restored: remember its device names and match moved device ids (4.4).
        std::function<void(const std::vector<proto::SavedDevice>&)> rememberDevices;
    };
    struct Options {
        std::string directory;   // the profile files; "" = no files (profiles off, snapshots in memory only)
        std::string statePath;   // current profile and PC-wide settings; "" = not kept
        std::string startProfile; // --profile: use this one instead of the last one
        std::chrono::milliseconds autosaveDelay{2000};
        std::chrono::milliseconds autosaveMaxDelay{10000}; // even while values keep changing
        bool verbose = true;
    };

    ProfileManager(asio::io_context& io, control::ControlServer& server, Hooks hooks, Options options);
    ~ProfileManager();

    // After the engine seeded its tree (and before devices start): remembers the fresh state for
    // /profile/new, registers the methods, reads the state file and restores the last profile.
    void start();
    // Saves what autosave still owes (network thread). Call before the engine parts go away.
    void stop();

    // ControlServer::Options::onDelta (network thread).
    void onDelta(const proto::Delta& delta);
    // Reads the folder again (/devices/refresh).
    void refreshList();

    // %APPDATA%\DesktopCapture\profiles, ~/.config/desktop-capture/profiles
    static std::string defaultDirectory();
    // %APPDATA%\DesktopCapture\mixd-profile.txt
    static std::string defaultStatePath();

private:
    struct Listed {
        std::string name;
        std::int64_t modified = 0;
    };

    proto::Result save(control::CallContext& ctx);
    proto::Result load(control::CallContext& ctx);
    proto::Result create(control::CallContext& ctx);
    proto::Result rename(control::CallContext& ctx);
    proto::Result remove(control::CallContext& ctx);
    proto::Result snapshotSave(control::CallContext& ctx);
    proto::Result snapshotRecall(control::CallContext& ctx);
    proto::Result snapshotRemove(control::CallContext& ctx);

    bool enabled() const { return !options_.directory.empty(); }
    std::optional<std::string> busyReason() const;
    std::string fileOf(const std::string& name) const; // UTF-8 path
    const Listed* findListed(const std::string& name) const; // case-insensitive (ASCII)
    bool writeProfile(const std::string& name, std::string* error);
    // Reads name (or its .bak). On failure: nullopt and why.
    std::optional<proto::ProfileData> readProfile(const std::string& name, std::string* error, bool* fromBackup);
    void apply(const proto::ProfileData& data, const std::string& name, std::uint64_t originId, const std::string& origin);
    bool saveCurrent(); // writes the current profile; publishes /profile/modified and /profile/error
    void setError(const std::string& error);
    void markModified();
    void scheduleSave(std::chrono::milliseconds delay);
    void readState();
    void writeState();

    asio::io_context& io_;
    control::ControlServer& server_;
    Hooks hooks_;
    Options options_;
    asio::steady_timer saveTimer_;
    std::optional<std::chrono::steady_clock::time_point> firstUnsaved_;
    bool applying_ = false; // our own deltas
    bool stopped_ = false;
    bool modified_ = false;
    bool autosave_ = true;
    std::string current_;
    std::string extraDirs_;
    std::vector<Listed> list_;
    std::vector<proto::Change> initial_; // the fresh engine's profile values (/profile/new)
    std::map<int, proto::SavedSnapshot> snapshots_;
};

} // namespace dc::mixd
