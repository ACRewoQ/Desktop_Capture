// The video streams capd sends over the LAN (docs/spec/protocol.md 10, lan-media.md 7): the remote preview
// (/video/preview/*, stream "video/preview") and the "LAN送信" output of a video mix (/video/lan/*, stream
// "video/lan"). The parts that need no GPU: the stream settings from the engine's tree, where the frames go (the
// subscribers the engine lists under /lan/sub/#) and making every keyframe decodable on its own.
// Portable: no Windows types, tested on Linux.
#pragma once

#include "settings.hpp"

#include "dc/lanmedia/udp_transport.hpp"
#include "dc/protocol/param_tree.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace dc::capd {

struct LanStreamConfig {
    std::string stream;  // "video/preview" | "video/lan"
    int mix = 0;         // 0 = none
    int width = 960, height = 540, fps = 30;
    int bitrateKbps = 3000;
    bool operator==(const LanStreamConfig&) const = default;
};

// output: "preview" or "lan" (/video/<output>/*). Missing values keep the schema's defaults; the size is rounded
// down to even numbers (NV12).
LanStreamConfig lanStreamConfig(const proto::ParamTree& tree, const std::string& output);

// What the stream's encoder is opened with: H.264 (the only codec of the LAN video packets), CBR, the encoders'
// low-latency tuning, a keyframe every 2 seconds (receivers that lose a frame ask for one sooner).
VideoSettings lanVideoSettings(const LanStreamConfig& c);

// The subscribers of a stream as UDP endpoints (entries whose address is not an IPv4 address are left out).
std::vector<lan::UdpTransport::Endpoint> lanDestinations(const proto::ParamTree& tree, const std::string& stream);

// H.264 Annex B: every keyframe must carry SPS and PPS so that a viewer can start from it. Encoders without a
// global header normally repeat them before each IDR; this remembers the last ones seen and puts them in front of
// a keyframe that comes without.
class ParameterSets {
public:
    // Returns the access unit to send: `au` itself, or a copy with SPS/PPS in front (valid until the next call).
    std::span<const std::uint8_t> process(std::span<const std::uint8_t> au, bool keyframe);
    bool haveSets() const { return !sets_.empty(); }
    std::uint64_t added() const { return added_; }  // keyframes that needed them

private:
    std::vector<std::uint8_t> sets_;  // SPS and PPS NAL units with their start codes
    std::vector<std::uint8_t> out_;
    std::uint64_t added_ = 0;
};

}  // namespace dc::capd
