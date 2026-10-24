#include "dc/lanmedia/packet.hpp"

#include "bytes.hpp"

#include <cstring>

namespace dc::lan {

namespace {

void writeCommon(std::uint8_t* out, PacketType type, std::uint8_t kind, std::uint8_t flags, std::uint32_t streamId) {
    std::memcpy(out, kMagic, 4);
    out[4] = static_cast<std::uint8_t>(type);
    out[5] = kind;
    out[6] = flags;
    out[7] = 0; // reserved
    put<std::uint32_t>(out + 8, streamId);
}

bool checkCommon(std::span<const std::uint8_t> data) {
    return data.size() >= kCommonHeaderBytes && std::memcmp(data.data(), kMagic, 4) == 0;
}

} // namespace

void writeMediaHeader(std::uint8_t* out, const MediaHeader& h) {
    writeCommon(out, PacketType::Media, static_cast<std::uint8_t>(h.kind), h.flags, h.streamId);
    put<std::uint32_t>(out + 12, h.seq);
    put<std::uint64_t>(out + 16, h.timeUs);
    put<std::uint64_t>(out + 24, h.position);
    put<std::uint16_t>(out + 32, h.fragIndex);
    put<std::uint16_t>(out + 34, h.fragCount);
}

void writeAudioHeader(std::uint8_t* out, const AudioHeader& h) {
    out[0] = static_cast<std::uint8_t>(h.codec);
    out[1] = h.channels;
    put<std::uint16_t>(out + 2, h.frames);
}

std::vector<std::uint8_t> encodeReport(const Report& r) {
    std::vector<std::uint8_t> out(kReportBytes);
    std::uint8_t* p = out.data();
    writeCommon(p, PacketType::Report, 0, r.flags, r.streamId);
    put<std::uint32_t>(p + 12, r.received);
    put<std::uint32_t>(p + 16, r.lost);
    put<std::uint32_t>(p + 20, r.late);
    put<std::uint32_t>(p + 24, r.concealed);
    put<std::uint32_t>(p + 28, r.jitterUs);
    put<std::uint32_t>(p + 32, r.latencyUs);
    put<std::uint64_t>(p + 36, r.echoTimeUs);
    put<std::uint32_t>(p + 44, r.echoHoldUs);
    put<std::uint32_t>(p + 48, 0); // reserved
    return out;
}

std::optional<PacketType> peekType(std::span<const std::uint8_t> data) {
    if (!checkCommon(data)) return std::nullopt;
    const auto t = data[4];
    if (t != static_cast<std::uint8_t>(PacketType::Media) && t != static_cast<std::uint8_t>(PacketType::Report))
        return std::nullopt;
    return static_cast<PacketType>(t);
}

std::optional<MediaView> parseMedia(std::span<const std::uint8_t> data) {
    if (data.size() < kMediaHeaderBytes || peekType(data) != PacketType::Media) return std::nullopt;
    const std::uint8_t* p = data.data();
    MediaView v;
    if (p[5] != static_cast<std::uint8_t>(MediaKind::Audio) && p[5] != static_cast<std::uint8_t>(MediaKind::Video))
        return std::nullopt;
    v.header.kind = static_cast<MediaKind>(p[5]);
    v.header.flags = p[6];
    v.header.streamId = get<std::uint32_t>(p + 8);
    v.header.seq = get<std::uint32_t>(p + 12);
    v.header.timeUs = get<std::uint64_t>(p + 16);
    v.header.position = get<std::uint64_t>(p + 24);
    v.header.fragIndex = get<std::uint16_t>(p + 32);
    v.header.fragCount = get<std::uint16_t>(p + 34);
    if (v.header.fragCount == 0 || v.header.fragIndex >= v.header.fragCount) return std::nullopt;
    v.payload = data.subspan(kMediaHeaderBytes);
    return v;
}

std::optional<AudioView> parseAudio(std::span<const std::uint8_t> payload) {
    if (payload.size() < kAudioHeaderBytes) return std::nullopt;
    AudioView v;
    const auto codec = payload[0];
    if (codec < 1 || codec > 3) return std::nullopt;
    v.header.codec = static_cast<AudioCodec>(codec);
    v.header.channels = payload[1];
    v.header.frames = get<std::uint16_t>(payload.data() + 2);
    if (v.header.channels != 1 && v.header.channels != 2) return std::nullopt;
    if (v.header.frames == 0) return std::nullopt;
    v.data = payload.subspan(kAudioHeaderBytes);
    const std::size_t pcmBytes = std::size_t(v.header.frames) * v.header.channels *
                                 (v.header.codec == AudioCodec::Pcm16 ? 2u : 3u);
    if (v.header.codec != AudioCodec::Opus && v.data.size() != pcmBytes) return std::nullopt;
    if (v.header.codec == AudioCodec::Opus && v.data.empty()) return std::nullopt;
    return v;
}

std::optional<Report> parseReport(std::span<const std::uint8_t> data) {
    if (data.size() < kReportBytes || peekType(data) != PacketType::Report) return std::nullopt;
    const std::uint8_t* p = data.data();
    Report r;
    r.flags = p[6];
    r.streamId = get<std::uint32_t>(p + 8);
    r.received = get<std::uint32_t>(p + 12);
    r.lost = get<std::uint32_t>(p + 16);
    r.late = get<std::uint32_t>(p + 20);
    r.concealed = get<std::uint32_t>(p + 24);
    r.jitterUs = get<std::uint32_t>(p + 28);
    r.latencyUs = get<std::uint32_t>(p + 32);
    r.echoTimeUs = get<std::uint64_t>(p + 36);
    r.echoHoldUs = get<std::uint32_t>(p + 44);
    return r;
}

} // namespace dc::lan
