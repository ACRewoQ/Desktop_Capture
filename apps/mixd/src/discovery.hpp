#pragma once

// LAN discovery of other engines (docs/spec/protocol.md 8.1): advertises this engine as _dcmix._tcp and
// browses for the others. Windows uses the OS DNS-SD API (DnsServiceRegister / DnsServiceBrowse), so
// no UDP 5353 socket of our own competes with the system's mDNS responder and the firewall needs no
// extra rule. Other platforms have no implementation (peers are added by address).

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dc::mixd {

struct DiscoveredPeer {
    std::string id;   // TXT id=<server_id>
    std::string name; // TXT name=
    std::string host; // IPv4 address
    std::uint16_t port = 0;
    bool operator==(const DiscoveredPeer&) const = default;
};

class Discovery {
public:
    struct Service {
        std::string instance; // visible name (computer name or /lan/name)
        std::string id;       // server id
        std::uint16_t port = 0;
    };

    virtual ~Discovery() = default;
    // onChange is called from any thread with the full current list (this engine excluded).
    virtual bool start(const Service& self, std::function<void(std::vector<DiscoveredPeer>)> onChange,
                       std::string* error) = 0;
    virtual void stop() = 0;
    // The advertised name changed.
    virtual void rename(const std::string& instance) = 0;
};

// The platform implementation, or nullptr where there is none.
std::unique_ptr<Discovery> makeDiscovery();

// The computer name (fallback instance name).
std::string computerName();

} // namespace dc::mixd
