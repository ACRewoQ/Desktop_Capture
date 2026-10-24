#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace dc::lan {

// Wire format of the LAN media transport (UDP). Layout (little-endian) is documented in
// docs/spec/lan-media.md. Only this application talks it; there is no compatibility with VBAN etc.
//
// Every datagram starts with a 12 byte common header; media packets add 24 bytes (sequence number,
// sender time, media position, fragment) and then the payload of their kind.
inline constexpr std::uint8_t kMagic[4] = {'D', 'C', 'N', '1'};
inline constexpr std::size_t kCommonHeaderBytes = 12;
inline constexpr std::size_t kMediaHeaderBytes = 36;
inline constexpr std::size_t kAudioHeaderBytes = 4;
inline constexpr std::size_t kVideoHeaderBytes = 12;
// Largest datagram we send. Stays below a 1500 byte Ethernet MTU with IPv4/IPv6 + UDP headers and
// leaves room for a VPN or PPPoE encapsulation, so nothing is fragmented by IP.
inline constexpr std::size_t kMaxDatagramBytes = 1400;
inline constexpr std::size_t kMaxMediaPayloadBytes = kMaxDatagramBytes - kMediaHeaderBytes;
inline constexpr std::size_t kMaxVideoFragmentBytes = kMaxMediaPayloadBytes - kVideoHeaderBytes;
inline constexpr std::uint16_t kDefaultMediaPort = 47810; // mixd (audio streams)
inline constexpr std::uint16_t kDefaultVideoPort = 47811; // capd (video streams)

enum class PacketType : std::uint8_t { Media = 1, Report = 2 };
enum class MediaKind : std::uint8_t { Audio = 1, Video = 2 };

// Media packet flags.
enum : std::uint8_t {
    kFlagStreamStart = 1u << 0, // first packet of a stream (or after the sender reset its position)
    kFlagKeyframe = 1u << 1,    // video: the frame is a keyframe
};
// Report flags.
enum : std::uint8_t {
    kReportKeyframeRequest = 1u << 0, // video: the receiver lost data and needs a keyframe
};

struct MediaHeader {
    MediaKind kind = MediaKind::Audio;
    std::uint8_t flags = 0;
    std::uint32_t streamId = 0;  // chosen by the sender, random per stream start
    std::uint32_t seq = 0;       // +1 per datagram of the stream, wraps
    std::uint64_t timeUs = 0;    // sender clock (QPC in microseconds) when the first sample / frame was taken
    std::uint64_t position = 0;  // audio: frame index of the first frame; video: frame number
    std::uint16_t fragIndex = 0; // video: fragment of the frame (audio: 0 / 1)
    std::uint16_t fragCount = 1;
    bool operator==(const MediaHeader&) const = default;
};

enum class AudioCodec : std::uint8_t { Pcm16 = 1, Pcm24 = 2, Opus = 3 };

struct AudioHeader {
    AudioCodec codec = AudioCodec::Pcm24;
    std::uint8_t channels = 2; // 1 or 2
    std::uint16_t frames = 0;  // frames carried by the packet (after decoding)
    bool operator==(const AudioHeader&) const = default;
};

enum class VideoCodec : std::uint8_t { H264 = 1 };

// In every fragment of a video frame (access unit), after the media header.
struct VideoHeader {
    VideoCodec codec = VideoCodec::H264;
    std::uint32_t frameBytes = 0; // size of the whole access unit
    std::uint32_t offset = 0;     // where this fragment's bytes go in the access unit
    bool operator==(const VideoHeader&) const = default;
};

// Receiver -> sender, about once per second per stream. Cumulative counters since the stream started.
struct Report {
    std::uint8_t flags = 0;
    std::uint32_t streamId = 0;
    std::uint32_t received = 0;  // datagrams
    std::uint32_t lost = 0;      // datagrams never seen (sequence gaps)
    std::uint32_t late = 0;      // arrived after their play time
    std::uint32_t concealed = 0; // audio packets replaced by concealment
    std::uint32_t jitterUs = 0;  // arrival lateness peak over the last seconds
    std::uint32_t latencyUs = 0; // receiver buffer target
    std::uint64_t echoTimeUs = 0; // timeUs of the newest media packet received
    std::uint32_t echoHoldUs = 0; // time between receiving that packet and sending this report
    bool operator==(const Report&) const = default;
};
inline constexpr std::size_t kReportBytes = kCommonHeaderBytes + 40;

// --- encoding -------------------------------------------------------------------------------------

// Writes common + media header into out[0..kMediaHeaderBytes).
void writeMediaHeader(std::uint8_t* out, const MediaHeader& h);
void writeAudioHeader(std::uint8_t* out, const AudioHeader& h);
void writeVideoHeader(std::uint8_t* out, const VideoHeader& h);
std::vector<std::uint8_t> encodeReport(const Report& r);

// --- decoding -------------------------------------------------------------------------------------

std::optional<PacketType> peekType(std::span<const std::uint8_t> data);

struct MediaView {
    MediaHeader header;
    std::span<const std::uint8_t> payload;
};
std::optional<MediaView> parseMedia(std::span<const std::uint8_t> data);

struct AudioView {
    AudioHeader header;
    std::span<const std::uint8_t> data; // codec payload
};
std::optional<AudioView> parseAudio(std::span<const std::uint8_t> mediaPayload);

struct VideoView {
    VideoHeader header;
    std::span<const std::uint8_t> data; // this fragment's bytes
};
// Checks that the fragment lies inside the frame.
std::optional<VideoView> parseVideo(std::span<const std::uint8_t> mediaPayload);

std::optional<Report> parseReport(std::span<const std::uint8_t> data);

// --- sequence numbers -----------------------------------------------------------------------------

// Extends 32-bit wrapping sequence numbers to 64 bits around the newest one seen.
class SeqUnwrapper {
public:
    std::uint64_t unwrap(std::uint32_t seq) {
        if (!have_) {
            have_ = true;
            last_ = (std::uint64_t{1} << 32) | seq; // headroom so that older packets stay positive
            return last_;
        }
        const auto delta = static_cast<std::int32_t>(seq - static_cast<std::uint32_t>(last_));
        const auto v = static_cast<std::uint64_t>(static_cast<std::int64_t>(last_) + delta);
        if (delta > 0) last_ = v;
        return v;
    }
    void reset() { have_ = false; last_ = 0; }

private:
    bool have_ = false;
    std::uint64_t last_ = 0;
};

} // namespace dc::lan
