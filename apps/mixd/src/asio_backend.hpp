#pragma once

// ASIO drivers (Windows only), used by the WASAPI backend for the devices it lists with api "asio".
// An ASIO driver drives all its inputs and outputs from one callback, and only one driver can be open
// in a process. The host lists each driver as one input device with all its input channels
// ("asio:<driver>") and one output device per stereo pair ("asio:<driver>:1-2", protocol.md 4.4);
// every stream on the open driver is fed from that callback. Drivers that cannot run at 48 kHz run at
// their own rate, converted per stream.

#include "device.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dc::mixd {

class AsioHost {
public:
    virtual ~AsioHost() = default;
    // Drivers from the registry. The first call loads each driver once to read its channels.
    virtual std::vector<DeviceInfo> enumerate() = 0;
    virtual std::unique_ptr<Stream> openInput(const DeviceInfo& device, const StreamConfig& cfg, InputCallback cb,
                                              std::string* error) = 0;
    virtual std::unique_ptr<Stream> openOutput(const DeviceInfo& device, const StreamConfig& cfg, OutputCallback cb,
                                               std::string* error) = 0;
};

// onChange: called (any thread) when the open driver asks to be reset or changed its rate.
std::unique_ptr<AsioHost> makeAsioHost(std::function<void()> onChange);

} // namespace dc::mixd
