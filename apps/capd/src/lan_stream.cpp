#include "lan_stream.hpp"

#include "dc/protocol/lan.hpp"

#include <asio/ip/address_v4.hpp>

#include <cstddef>

namespace dc::capd {

namespace {

int even(int v) { return v & ~1; }

struct Nal {
    std::size_t begin = 0;  // first byte of the start code
    std::size_t end = 0;    // first byte of the next start code (or the end)
    int type = 0;
};

// The NAL units of an Annex B access unit, start codes included (3 or 4 bytes).
std::vector<Nal> nalUnits(std::span<const std::uint8_t> au) {
    std::vector<Nal> out;
    const std::size_t n = au.size();
    for (std::size_t i = 0; i + 3 <= n;) {
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
            const std::size_t begin = (i > 0 && au[i - 1] == 0) ? i - 1 : i;
            if (!out.empty()) out.back().end = begin;
            out.push_back({begin, n, i + 3 < n ? (au[i + 3] & 0x1F) : 0});
            i += 3;
        } else {
            ++i;
        }
    }
    return out;
}

}  // namespace

LanStreamConfig lanStreamConfig(const proto::ParamTree& tree, const std::string& output) {
    LanStreamConfig c;
    c.next = output == "next";
    const bool preview = output == "preview" || c.next;
    c.stream = "video/" + output;
    if (!preview) {  // the schema's defaults for /video/lan
        c.width = 1920;
        c.height = 1080;
        c.bitrateKbps = 8000;
    } else {
        c.mix = 1;
    }
    const std::string base = preview ? "/video/preview/" : "/video/" + output + "/";
    auto get = [&](const char* key, int fallback) {
        return static_cast<int>(tree.getAs<std::int64_t>(base + key).value_or(fallback));
    };
    c.mix = get("mix", c.mix);
    c.width = even(get("width", c.width));
    c.height = even(get("height", c.height));
    c.fps = get("fps", c.fps);
    c.bitrateKbps = get("bitrate_kbps", c.bitrateKbps);
    return c;
}

VideoSettings lanVideoSettings(const LanStreamConfig& c) {
    VideoSettings v;
    v.encoder = "auto";
    v.codec = "h264";
    v.width = c.width;
    v.height = c.height;
    v.fps = c.fps;
    v.rateControl = "cbr";
    v.bitrateKbps = c.bitrateKbps;
    v.gopFrames = 0;
    v.lowLatency = true;
    return v;
}

std::vector<lan::UdpTransport::Endpoint> lanDestinations(const proto::ParamTree& tree, const std::string& stream) {
    std::vector<lan::UdpTransport::Endpoint> out;
    for (const auto& s : proto::lanSubscriptions(tree, stream)) {
        asio::error_code ec;
        const auto addr = asio::ip::make_address_v4(s.host, ec);
        if (!ec) out.emplace_back(addr, s.port);
    }
    return out;
}

std::span<const std::uint8_t> ParameterSets::process(std::span<const std::uint8_t> au, bool keyframe) {
    const auto nals = nalUnits(au);
    bool sps = false, pps = false;
    for (const auto& u : nals) {
        sps = sps || u.type == 7;
        pps = pps || u.type == 8;
    }
    if (sps && pps) {
        sets_.clear();
        for (const auto& u : nals)
            if (u.type == 7 || u.type == 8)
                sets_.insert(sets_.end(), au.begin() + std::ptrdiff_t(u.begin), au.begin() + std::ptrdiff_t(u.end));
        return au;
    }
    if (!keyframe || sets_.empty()) return au;
    // After an access unit delimiter, if there is one (it must stay first).
    const std::size_t at = !nals.empty() && nals.front().type == 9 ? nals.front().end : 0;
    out_.assign(au.begin(), au.begin() + std::ptrdiff_t(at));
    out_.insert(out_.end(), sets_.begin(), sets_.end());
    out_.insert(out_.end(), au.begin() + std::ptrdiff_t(at), au.end());
    ++added_;
    return out_;
}

}  // namespace dc::capd
