#pragma once

// The engine's effect racks on the network thread (docs/spec/protocol.md 5.7, 5.8): implements the
// /track/#/fx/... and /master/fx/... methods and /plugins/scan, keeps one Effect per slot in tree
// order, forwards parameter, bypass and plugin-window changes from the tree to the effects, and
// publishes what plugins change by themselves. The mix plan picks the slots up through rack().

#include "fx/effect.hpp"
#include "fx/plugin_host.hpp"
#include "plan.hpp"

#include "dc/control/server.hpp"
#include "dc/protocol/profile.hpp"

#include <asio.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace dc::mixd {

class FxManager {
public:
    // host may be null (no VST3 in this build). onRackChanged is called on the network thread when
    // rack() changed and the mix plan must be rebuilt.
    FxManager(asio::io_context& io, control::ControlServer& server, PluginHost* host, std::function<void()> onRackChanged,
              bool verbose);
    ~FxManager();

    void start(); // registers the methods, publishes the plugin list
    void stop();

    // ControlServer onDelta (network thread): parameter, bypass and editor changes of fx slots.
    void onDelta(const proto::Delta& delta);
    // The track was removed from the tree (its fx subtree went with it).
    void clearStrip(int strip);

    const FxRack& rack() const { return rack_; }

    // Profiles (docs/spec/protocol.md 5.10). restore() replaces every rack by the plan's slots and returns
    // their tree values, to be published together with the rest of the profile (the caller removes the
    // old /…/fx subtrees in the same delta); call restored() right after publishing. states holds the
    // saved plugin states by the slot's base in the file. VST3 slots load in the background and get
    // their state back before they enter the rack; plugins this PC does not have become "missing" and
    // keep their state for the next save.
    std::vector<proto::Change> restore(const std::map<int, std::vector<proto::FxRestore>>& slots,
                                       const std::map<std::string, std::string>& states);
    void restored();
    // The state of every VST3 slot, by its base path ("/track/3/fx/2"): asked from the loaded plugins,
    // the saved one for slots that are still loading, missing or failed.
    std::map<std::string, std::string> pluginStates() const;

    // Paths that are handled here and never need a plan rebuild (fx slot contents, the plugin list).
    static bool handlesPath(const std::string& path);

private:
    struct Entry {
        std::uint64_t serial = 0;
        std::string type;
        std::shared_ptr<Effect> effect;         // null while a plugin loads or after it failed
        std::shared_ptr<PluginInstance> plugin; // VST3 only
        std::shared_ptr<FxSlot> slot;
        std::set<std::string> publishedKeys;    // VST3 parameters in the tree
        std::string state;                      // VST3: the saved state (set on load, kept while not loaded)
    };
    using SlotValues = std::vector<std::pair<std::string, proto::Value>>; // path suffix after /fx/N -> value

    // A new slot of the given type with its tree values (suffixes after /fx/N). Starts loading a VST3.
    // restoring: an unknown or unusable plugin gives a "missing" / "failed" slot instead of an error.
    struct NewSlot {
        Entry entry;
        SlotValues values;
        proto::Result error; // code != Ok: nothing was made
    };
    NewSlot makeSlot(const std::string& type, bool restoring);

    proto::Result add(control::CallContext& ctx);
    proto::Result remove(control::CallContext& ctx);
    proto::Result move(control::CallContext& ctx);
    proto::Result params(control::CallContext& ctx);
    proto::Result scan(control::CallContext& ctx);

    int stripOfMethod(const std::string& method) const; // -1 when the track does not exist
    std::vector<SlotValues> readSlots(int strip) const;
    void writeSlots(int strip, const std::vector<SlotValues>& slots, std::uint64_t originId, const std::string& origin);
    Entry* findSerial(std::uint64_t serial, int* strip = nullptr, int* index = nullptr);
    void updateRack(int strip);
    void onPluginLoaded(std::uint64_t serial, std::shared_ptr<PluginInstance> plugin, const std::string& error);
    void queuePluginParam(std::uint64_t serial, const std::string& key, double value);
    void flushPluginParams();
    void publishPlugins();

    asio::io_context& io_;
    control::ControlServer& server_;
    PluginHost* host_;
    std::function<void()> onRackChanged_;
    bool verbose_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0); // callbacks from the host check it
    std::array<std::vector<Entry>, kMaxStrips> strips_;
    FxRack rack_;
    std::uint64_t nextSerial_ = 1;
    int publishedPlugins_ = 0;

    std::mutex pendingMutex_;
    std::map<std::pair<std::uint64_t, std::string>, double> pendingParams_;
    bool flushScheduled_ = false;
    asio::steady_timer flushTimer_;
};

// "/track/3" for strip 3, "/master" for strip 0.
std::string stripBase(int strip);

} // namespace dc::mixd
