#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rawr::mcraw {

// MotionCam's lossless RAW codec ("compressionType": 7), as read by the
// Apache-2.0 reference decoder (github.com/mirsadm/motioncam-decoder,
// lib/RawData.cpp, raw::Decode).
//
// The frame is cut into tiles of 4 rows x 64 columns. Each tile becomes four
// 64-sample blocks, one per CFA phase (even/odd row x even/odd column), each
// stored as residuals from its minimum, bit-packed at the block's bit width.
// Per-block widths and minimums go into two trailing streams encoded the same
// way with an inline 2-byte header (4-bit width, 12-bit reference).
//
// Layout: [u32 encodedWidth][u32 encodedHeight][u32 bitsOffset][u32 refsOffset]
// (little-endian), block data from offset 16, then the bits and refs streams.
inline constexpr int kCompressionType = 7;
inline constexpr uint32_t kTileWidth = 64;
inline constexpr uint32_t kTileHeight = 4;

// True when a frame of this size can be encoded: even width (whole CFA pairs)
// and a height that is a multiple of the 4-row tile, because the reference
// decoder writes whole tiles into a buffer sized width * height.
[[nodiscard]] constexpr bool encodableSize(uint32_t width, uint32_t height) noexcept {
    return width >= 2 && (width % 2) == 0 && height >= kTileHeight && (height % kTileHeight) == 0;
}

// Upper bound on the encoded size of a width x height frame.
[[nodiscard]] size_t maxEncodedBytes(uint32_t width, uint32_t height) noexcept;

// Encodes a Bayer RAW16 frame (any CFA order; the codec is CFA-agnostic).
// stridePixels >= width. Throws std::invalid_argument for an unencodable size.
// The output vector is resized to the encoded length.
void encodeFrame(const uint16_t* pixels, uint32_t width, uint32_t height, size_t stridePixels,
                 std::vector<uint8_t>& out);

// Reference decoder for this codec, written independently of the encoder from
// the same layout description. Used by tests and on-device self checks.
// Returns false on malformed input.
[[nodiscard]] bool decodeFrame(const uint8_t* data, size_t size, uint32_t width, uint32_t height,
                               std::vector<uint16_t>& out);

}  // namespace rawr::mcraw
