#pragma once

#include "dc/protocol/messages.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dc::proto {

using Bytes = std::vector<std::uint8_t>;

// MessagePack payload without the length prefix.
Bytes encodePayload(const Message& msg);

struct DecodeResult {
    std::optional<Message> message;
    std::string error; // set when message is empty
};

DecodeResult decodePayload(std::span<const std::uint8_t> payload);

// Payload prefixed with a 4-byte little-endian length, ready for the socket.
Bytes encodeFrame(const Message& msg);

// Incremental frame splitter for a TCP byte stream.
class FrameDecoder {
public:
    explicit FrameDecoder(std::uint32_t maxFrameBytes);
    FrameDecoder();

    void feed(std::span<const std::uint8_t> data);

    // Returns the next complete payload, or nullopt if more bytes are needed.
    // After an oversize frame is announced, failed() becomes true and nothing more is returned.
    std::optional<Bytes> next();

    bool failed() const { return failed_; }
    std::size_t buffered() const { return buffer_.size() - readPos_; }

private:
    std::uint32_t maxFrameBytes_;
    Bytes buffer_;
    std::size_t readPos_ = 0;
    bool failed_ = false;
};

} // namespace dc::proto
