#pragma once

// What the engine remembers about the LAN across restarts: its own id, the clients it paired with
// (remote controllers and peer engines that may connect here), the tokens it holds for other engines,
// and peers added by address. A small text file in the user's settings folder.

#include "dc/protocol/messages.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace dc::mixd {

struct PairedClient {
    std::string id;   // Hello.client_id
    std::string name;
    proto::ClientRole role = proto::ClientRole::Remote;
    std::string token;
    bool operator==(const PairedClient&) const = default;
};

class LanStore {
public:
    // path "" = keep everything in memory (tests).
    explicit LanStore(std::string path);

    // Loads the file; creates the server id (and saves) when there is none.
    void load();
    void save() const;

    const std::string& serverId() const { return serverId_; }

    const std::vector<PairedClient>& paired() const { return paired_; }
    const PairedClient* findByToken(const std::string& token) const;
    // Adds or replaces the entry with the same id. Returns the new token.
    std::string addPaired(const std::string& id, const std::string& name, proto::ClientRole role);
    bool removePaired(const std::string& id);

    // Tokens this engine was given by other engines (peer id -> token).
    const std::map<std::string, std::string>& peerTokens() const { return peerTokens_; }
    void setPeerToken(const std::string& peerId, const std::string& token);
    void removePeerToken(const std::string& peerId);

    // Peers added by address ("ip:port"), with their id once a connection told it.
    struct ManualPeer {
        std::string address;
        std::string id;
        bool operator==(const ManualPeer&) const = default;
    };
    const std::vector<ManualPeer>& manualPeers() const { return manual_; }
    void addManualPeer(const std::string& address);
    void setManualPeerId(const std::string& address, const std::string& id);
    void removeManualPeer(const std::string& address);

    // Settings folder file used by mixd: %APPDATA%\DesktopCapture\mixd-lan.txt, ~/.config/desktop-capture/...
    static std::string defaultPath();

private:
    std::string path_;
    std::string serverId_;
    std::vector<PairedClient> paired_;
    std::map<std::string, std::string> peerTokens_;
    std::vector<ManualPeer> manual_;
};

// 128 random bits as 32 hex digits.
std::string randomHex(std::size_t bytes = 16);

// The 6-digit code the pairing window shows, with its expiry and failure count.
class PairingWindow {
public:
    static constexpr double kSeconds = 120;
    static constexpr int kMaxFailures = 5;

    std::string start(double now); // returns the code
    void stop() { active_ = false; }
    bool active(double now) const { return active_ && now < until_; }

    enum class Check { Ok, Wrong, Closed };
    // A wrong code counts towards kMaxFailures, after which the window closes. Ok closes it too.
    Check check(const std::string& code, double now);

private:
    bool active_ = false;
    double until_ = 0;
    int failures_ = 0;
    std::string code_;
};

} // namespace dc::mixd
