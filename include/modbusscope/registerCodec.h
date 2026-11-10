#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace modbusscope {

// Register decoding helpers.
//
// These functions are intentionally Qt-free: the public aggregate header and a
// consumer that only needs decoding must not pull in QtCore. Every accessor
// returns std::nullopt when the requested word range is not fully present, so a
// malformed PDU cannot index past the vector in either Debug or Release.

namespace register_codec_detail {

inline bool has_words(const std::vector<std::uint16_t> &words,
                      std::size_t index,
                      std::size_t count) noexcept {
    return index <= words.size() && count <= words.size() - index;
}

} // namespace register_codec_detail

// Decode two consecutive 16-bit words into one 32-bit value.
// highWordFirst=true reads [high, low], false reads [low, high].
inline std::optional<std::uint32_t>
toUint32(const std::vector<std::uint16_t> &words,
         std::size_t index,
         bool highWordFirst) noexcept {
    if (!register_codec_detail::has_words(words, index, 2)) {
        return std::nullopt;
    }
    return highWordFirst
               ? (static_cast<std::uint32_t>(words[index]) << 16) |
                     words[index + 1]
               : (static_cast<std::uint32_t>(words[index + 1]) << 16) |
                     words[index];
}

// Reinterpret the 32-bit pattern as IEEE-754 binary32.
inline std::optional<float>
toFloat32(const std::vector<std::uint16_t> &words,
          std::size_t index,
          bool highWordFirst) noexcept {
    const auto raw = toUint32(words, index, highWordFirst);
    if (!raw) {
        return std::nullopt;
    }
    static_assert(sizeof(float) == sizeof(std::uint32_t),
                  "binary32 must be 32 bits");
    float value = 0.0F;
    std::memcpy(&value, &*raw, sizeof(value));
    return value;
}

// Reinterpret the 32-bit pattern as a two's-complement signed integer.
inline std::optional<std::int32_t>
toI32(const std::vector<std::uint16_t> &words,
      std::size_t index,
      bool highWordFirst) noexcept {
    const auto raw = toUint32(words, index, highWordFirst);
    if (!raw) {
        return std::nullopt;
    }
    std::int32_t value = 0;
    std::memcpy(&value, &*raw, sizeof(value));
    return value;
}

enum class WordOrder64 {
    ABCD,   // words[0] is the most significant 16 bits
    CDAB,   // words[0] is the least significant 16 bits
};

// Decode four consecutive 16-bit words into one 64-bit value.
inline std::optional<std::uint64_t>
toUint64(const std::vector<std::uint16_t> &words,
         std::size_t index,
         WordOrder64 order) noexcept {
    if (!register_codec_detail::has_words(words, index, 4)) {
        return std::nullopt;
    }
    switch (order) {
    case WordOrder64::ABCD:
        return (static_cast<std::uint64_t>(words[index]) << 48) |
               (static_cast<std::uint64_t>(words[index + 1]) << 32) |
               (static_cast<std::uint64_t>(words[index + 2]) << 16) |
               static_cast<std::uint64_t>(words[index + 3]);
    case WordOrder64::CDAB:
        return (static_cast<std::uint64_t>(words[index + 3]) << 48) |
               (static_cast<std::uint64_t>(words[index + 2]) << 32) |
               (static_cast<std::uint64_t>(words[index + 1]) << 16) |
               static_cast<std::uint64_t>(words[index]);
    }
    return std::nullopt;
}

// Decode four consecutive 16-bit words as a two's-complement signed integer.
inline std::optional<std::int64_t>
toI64(const std::vector<std::uint16_t> &words,
      std::size_t index,
      WordOrder64 order) noexcept {
    const auto raw = toUint64(words, index, order);
    if (!raw) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    std::memcpy(&value, &*raw, sizeof(value));
    return value;
}

} // namespace modbusscope
