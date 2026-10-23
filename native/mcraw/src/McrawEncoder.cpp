#include "mcraw/McrawEncoder.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace rawr::mcraw {
namespace {
constexpr size_t kHeaderBytes = 16;
constexpr uint32_t kBlock = 64;
// Packed bytes per 64-sample block, by stored bit width (the decoder's table).
constexpr std::array<uint32_t, 17> kBlockBytes{0, 8, 16, 24, 32, 40, 48, 64, 64, 80, 80, 128, 128, 128, 128, 128, 128};
// The inline header of the bits/refs streams has a 12-bit reference.
constexpr uint32_t kMaxStreamReference = 0x0FFF;

using Block = std::array<uint16_t, kBlock>;

uint32_t bitLength(uint32_t value) noexcept {
    uint32_t bits = 0;
    while (value != 0) {
        ++bits;
        value >>= 1;
    }
    return bits;
}

// Widths the decoder unpacks: 0-6 as is, 7-8 as 8, 9-10 as 10, anything wider as 16.
uint32_t storedBits(uint32_t span) noexcept {
    const uint32_t bits = bitLength(span);
    if (bits <= 6) return bits;
    if (bits <= 8) return 8;
    if (bits <= 10) return 10;
    return 16;
}

// How the decoder unpacks a stored width (7 as 8, 9 as 10, 11-16 as 16).
uint32_t unpackedBits(uint32_t bits) noexcept {
    if (bits == 7) return 8;
    if (bits == 9) return 10;
    return bits > 10 ? 16 : bits;
}

uint32_t roundUp(uint32_t value, uint32_t multiple) noexcept { return (value + multiple - 1) / multiple * multiple; }

void putLe32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

uint32_t getLe32(const uint8_t* p) noexcept {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// Packs 64 residuals that fit in `bits` into kBlockBytes[bits] bytes. Each layout is the
// inverse of the matching DecodeN in the reference decoder: the decoder loads 8 bytes as
// 8 lanes, so v(k, l) = r[k * 8 + l] is row k of lane l.
void pack(const uint16_t* r, uint32_t bits, uint8_t* out) noexcept {
    const auto v = [r](int k, int l) { return static_cast<uint32_t>(r[k * 8 + l]); };
    switch (bits) {
        case 0:
            return;
        case 1:
            for (int l = 0; l < 8; ++l) {
                uint32_t b = 0;
                for (int k = 0; k < 8; ++k) b |= (v(k, l) & 1u) << k;
                out[l] = static_cast<uint8_t>(b);
            }
            return;
        case 2:
            // Two groups of 32: four 2-bit rows per byte.
            for (int h = 0; h < 2; ++h)
                for (int l = 0; l < 8; ++l) {
                    uint32_t b = 0;
                    for (int k = 0; k < 4; ++k) b |= (v(h * 4 + k, l) & 3u) << (2 * k);
                    out[h * 8 + l] = static_cast<uint8_t>(b);
                }
            return;
        case 3:
            for (int l = 0; l < 8; ++l) {
                out[l] = static_cast<uint8_t>(v(0, l) | (v(1, l) << 3) | ((v(2, l) & 3u) << 6));
                out[8 + l] = static_cast<uint8_t>(v(3, l) | (v(4, l) << 3) | ((v(5, l) & 3u) << 6));
                out[16 + l] = static_cast<uint8_t>(v(6, l) | (v(7, l) << 3) | (((v(2, l) >> 2) & 1u) << 6) |
                                                   (((v(5, l) >> 2) & 1u) << 7));
            }
            return;
        case 4:
            // Four groups of 16: low and high nibble.
            for (int q = 0; q < 4; ++q)
                for (int l = 0; l < 8; ++l) out[q * 8 + l] = static_cast<uint8_t>(v(q * 2, l) | (v(q * 2 + 1, l) << 4));
            return;
        case 5:
            for (int l = 0; l < 8; ++l) {
                out[l] = static_cast<uint8_t>(v(0, l) | ((v(5, l) & 7u) << 5));
                out[8 + l] = static_cast<uint8_t>(v(1, l) | ((v(6, l) & 7u) << 5));
                out[16 + l] = static_cast<uint8_t>(v(2, l) | ((v(7, l) & 7u) << 5));
                out[24 + l] = static_cast<uint8_t>(v(3, l) | (((v(5, l) >> 3) & 3u) << 5) | (((v(7, l) >> 3) & 1u) << 7));
                out[32 + l] = static_cast<uint8_t>(v(4, l) | (((v(6, l) >> 3) & 3u) << 5) | (((v(7, l) >> 4) & 1u) << 7));
            }
            return;
        case 6:
            for (int l = 0; l < 8; ++l) {
                out[l] = static_cast<uint8_t>(v(0, l) | ((v(6, l) & 3u) << 6));
                out[8 + l] = static_cast<uint8_t>(v(1, l) | (((v(6, l) >> 2) & 3u) << 6));
                out[16 + l] = static_cast<uint8_t>(v(2, l) | (((v(6, l) >> 4) & 3u) << 6));
                out[24 + l] = static_cast<uint8_t>(v(3, l) | ((v(7, l) & 3u) << 6));
                out[32 + l] = static_cast<uint8_t>(v(4, l) | (((v(7, l) >> 2) & 3u) << 6));
                out[40 + l] = static_cast<uint8_t>(v(5, l) | (((v(7, l) >> 4) & 3u) << 6));
            }
            return;
        case 8:
            for (uint32_t i = 0; i < kBlock; ++i) out[i] = static_cast<uint8_t>(r[i]);
            return;
        case 10:
            // Two halves: four low-byte rows, then one byte of 2-bit high parts.
            for (int h = 0; h < 2; ++h)
                for (int l = 0; l < 8; ++l) {
                    uint32_t high = 0;
                    for (int k = 0; k < 4; ++k) {
                        const uint32_t value = v(h * 4 + k, l);
                        out[h * 40 + k * 8 + l] = static_cast<uint8_t>(value);
                        high |= ((value >> 8) & 3u) << (2 * k);
                    }
                    out[h * 40 + 32 + l] = static_cast<uint8_t>(high);
                }
            return;
        default:  // 16: little-endian samples
            for (uint32_t i = 0; i < kBlock; ++i) {
                out[2 * i] = static_cast<uint8_t>(r[i]);
                out[2 * i + 1] = static_cast<uint8_t>(r[i] >> 8);
            }
            return;
    }
}

void unpack(const uint8_t* in, uint32_t bits, uint16_t* r) noexcept {
    const auto set = [r](int k, int l, uint32_t value) { r[k * 8 + l] = static_cast<uint16_t>(value); };
    switch (bits) {
        case 0:
            std::fill(r, r + kBlock, uint16_t(0));
            return;
        case 1:
            for (int l = 0; l < 8; ++l)
                for (int k = 0; k < 8; ++k) set(k, l, (in[l] >> k) & 1u);
            return;
        case 2:
            for (int h = 0; h < 2; ++h)
                for (int l = 0; l < 8; ++l)
                    for (int k = 0; k < 4; ++k) set(h * 4 + k, l, (in[h * 8 + l] >> (2 * k)) & 3u);
            return;
        case 3:
            for (int l = 0; l < 8; ++l) {
                const uint32_t a = in[l], b = in[8 + l], c = in[16 + l];
                set(0, l, a & 7u);
                set(1, l, (a >> 3) & 7u);
                set(2, l, ((a >> 6) & 3u) | (((c >> 6) & 1u) << 2));
                set(3, l, b & 7u);
                set(4, l, (b >> 3) & 7u);
                set(5, l, ((b >> 6) & 3u) | (((c >> 7) & 1u) << 2));
                set(6, l, c & 7u);
                set(7, l, (c >> 3) & 7u);
            }
            return;
        case 4:
            for (int q = 0; q < 4; ++q)
                for (int l = 0; l < 8; ++l) {
                    set(q * 2, l, in[q * 8 + l] & 15u);
                    set(q * 2 + 1, l, in[q * 8 + l] >> 4);
                }
            return;
        case 5:
            for (int l = 0; l < 8; ++l) {
                const uint32_t p0 = in[l], p1 = in[8 + l], p2 = in[16 + l], p3 = in[24 + l], p4 = in[32 + l];
                set(0, l, p0 & 31u);
                set(1, l, p1 & 31u);
                set(2, l, p2 & 31u);
                set(3, l, p3 & 31u);
                set(4, l, p4 & 31u);
                set(5, l, ((p0 >> 5) & 7u) | (((p3 >> 5) & 3u) << 3));
                set(6, l, ((p1 >> 5) & 7u) | (((p4 >> 5) & 3u) << 3));
                set(7, l, ((p2 >> 5) & 7u) | (((p3 >> 7) & 1u) << 3) | (((p4 >> 7) & 1u) << 4));
            }
            return;
        case 6:
            for (int l = 0; l < 8; ++l) {
                uint32_t p[6];
                for (int k = 0; k < 6; ++k) {
                    p[k] = in[k * 8 + l];
                    set(k, l, p[k] & 63u);
                }
                set(6, l, ((p[0] >> 6) & 3u) | (((p[1] >> 6) & 3u) << 2) | (((p[2] >> 6) & 3u) << 4));
                set(7, l, ((p[3] >> 6) & 3u) | (((p[4] >> 6) & 3u) << 2) | (((p[5] >> 6) & 3u) << 4));
            }
            return;
        case 8:
            for (uint32_t i = 0; i < kBlock; ++i) r[i] = in[i];
            return;
        case 10:
            for (int h = 0; h < 2; ++h)
                for (int l = 0; l < 8; ++l) {
                    const uint32_t high = in[h * 40 + 32 + l];
                    for (int k = 0; k < 4; ++k)
                        set(h * 4 + k, l, uint32_t(in[h * 40 + k * 8 + l]) | (((high >> (2 * k)) & 3u) << 8));
                }
            return;
        default:
            for (uint32_t i = 0; i < kBlock; ++i) r[i] = static_cast<uint16_t>(in[2 * i] | (in[2 * i + 1] << 8));
            return;
    }
}

// Appends one block's residuals; returns the stored width. `ref` is the block minimum.
uint32_t appendBlock(const Block& values, uint16_t ref, std::vector<uint8_t>& out) {
    Block residuals{};
    uint32_t span = 0;
    for (uint32_t i = 0; i < kBlock; ++i) {
        residuals[i] = static_cast<uint16_t>(values[i] - ref);
        span = std::max<uint32_t>(span, residuals[i]);
    }
    const uint32_t bits = storedBits(span);
    const size_t at = out.size();
    out.resize(at + kBlockBytes[bits]);
    pack(residuals.data(), bits, out.data() + at);
    return bits;
}

// The bits and refs streams: [u32 count] then per 64 values a 2-byte header
// (width << 12 | reference, big-endian nibble order) and the packed residuals.
// The decoder writes whole blocks into a buffer of `count` values, so the
// count is padded to a whole number of blocks.
void appendStream(const std::vector<uint16_t>& values, std::vector<uint8_t>& out) {
    const uint32_t count = roundUp(static_cast<uint32_t>(std::max<size_t>(values.size(), 1)), kBlock);
    const size_t at = out.size();
    out.resize(at + 4);
    putLe32(out.data() + at, count);
    for (uint32_t start = 0; start < count; start += kBlock) {
        Block block{};
        for (uint32_t i = 0; i < kBlock; ++i) {
            const size_t index = std::min<size_t>(start + i, values.size() - 1);
            block[i] = values.empty() ? 0 : values[index];
        }
        const uint16_t minimum = *std::min_element(block.begin(), block.end());
        const auto ref = static_cast<uint16_t>(std::min<uint32_t>(minimum, kMaxStreamReference));
        const size_t header = out.size();
        out.resize(header + 2);
        const uint32_t bits = appendBlock(block, ref, out);
        // The header width field is 4 bits; the decoder unpacks every width above 10 as 16.
        const uint32_t headerBits = std::min<uint32_t>(bits, 15);
        out[header] = static_cast<uint8_t>((headerBits << 4) | (ref >> 8));
        out[header + 1] = static_cast<uint8_t>(ref & 0xFF);
    }
}

bool readStream(const uint8_t* data, size_t size, size_t offset, std::vector<uint16_t>& values) {
    if (offset + 4 > size) return false;
    const uint32_t count = getLe32(data + offset);
    if (count % kBlock != 0) return false;
    offset += 4;
    values.assign(count, 0);
    for (uint32_t start = 0; start < count; start += kBlock) {
        if (offset + 2 > size) return false;
        const uint32_t bits = unpackedBits(data[offset] >> 4u);
        const uint32_t ref = ((data[offset] & 0x0Fu) << 8) | data[offset + 1];
        offset += 2;
        if (offset + kBlockBytes[bits] > size) return false;
        unpack(data + offset, bits, values.data() + start);
        offset += kBlockBytes[bits];
        for (uint32_t i = 0; i < kBlock; ++i) values[start + i] = static_cast<uint16_t>(values[start + i] + ref);
    }
    return true;
}
}  // namespace

size_t maxEncodedBytes(uint32_t width, uint32_t height) noexcept {
    const size_t tiles = size_t(roundUp(width, kTileWidth) / kTileWidth) * (height / kTileHeight);
    const size_t blocks = tiles * 4;
    const size_t streamBlocks = (blocks + kBlock - 1) / kBlock + 1;
    return kHeaderBytes + blocks * 128 + 2 * (4 + streamBlocks * (2 + 128));
}

void encodeFrame(const uint16_t* pixels, uint32_t width, uint32_t height, size_t stridePixels,
                 std::vector<uint8_t>& out) {
    if (!pixels || !encodableSize(width, height) || stridePixels < width)
        throw std::invalid_argument("MCRAW frame must have even width, height a multiple of 4, and stride >= width");
    const uint32_t encodedWidth = roundUp(width, kTileWidth);
    const size_t tiles = size_t(encodedWidth / kTileWidth) * (height / kTileHeight);
    std::vector<uint16_t> bits, refs;
    bits.reserve(tiles * 4);
    refs.reserve(tiles * 4);
    out.clear();
    out.reserve(maxEncodedBytes(width, height));
    out.resize(kHeaderBytes);

    // Columns past the frame repeat the last column of the same CFA phase, so padding
    // never widens a block.
    const auto column = [width](uint32_t x) { return x < width ? x : width - 2 + (x & 1u); };
    std::array<Block, 4> phase{};
    for (uint32_t y = 0; y < height; y += kTileHeight) {
        const uint16_t* rows[4];
        for (uint32_t r = 0; r < 4; ++r) rows[r] = pixels + size_t(y + r) * stridePixels;
        for (uint32_t x = 0; x < encodedWidth; x += kTileWidth) {
            // Block 0: even columns of rows 0 and 2; 1: odd columns of rows 0 and 2;
            // 2 and 3: the same for rows 1 and 3. 32 samples per row each.
            for (uint32_t j = 0; j < kTileWidth / 2; ++j) {
                const uint32_t even = column(x + 2 * j);
                const uint32_t odd = column(x + 2 * j + 1);
                phase[0][j] = rows[0][even];
                phase[0][32 + j] = rows[2][even];
                phase[1][j] = rows[0][odd];
                phase[1][32 + j] = rows[2][odd];
                phase[2][j] = rows[1][even];
                phase[2][32 + j] = rows[3][even];
                phase[3][j] = rows[1][odd];
                phase[3][32 + j] = rows[3][odd];
            }
            for (const Block& block : phase) {
                const uint16_t ref = *std::min_element(block.begin(), block.end());
                bits.push_back(static_cast<uint16_t>(appendBlock(block, ref, out)));
                refs.push_back(ref);
            }
        }
    }
    const auto bitsOffset = static_cast<uint32_t>(out.size());
    appendStream(bits, out);
    const auto refsOffset = static_cast<uint32_t>(out.size());
    appendStream(refs, out);
    putLe32(out.data(), encodedWidth);
    putLe32(out.data() + 4, height);
    putLe32(out.data() + 8, bitsOffset);
    putLe32(out.data() + 12, refsOffset);
}

bool decodeFrame(const uint8_t* data, size_t size, uint32_t width, uint32_t height, std::vector<uint16_t>& out) {
    if (!data || size < kHeaderBytes) return false;
    const uint32_t encodedWidth = getLe32(data), encodedHeight = getLe32(data + 4);
    const uint32_t bitsOffset = getLe32(data + 8), refsOffset = getLe32(data + 12);
    if (encodedWidth % kTileWidth != 0 || encodedWidth < width || encodedHeight != height ||
        height % kTileHeight != 0 || bitsOffset > size || refsOffset > size)
        return false;
    std::vector<uint16_t> bits, refs;
    if (!readStream(data, size, bitsOffset, bits) || !readStream(data, size, refsOffset, refs)) return false;
    const size_t tiles = size_t(encodedWidth / kTileWidth) * (height / kTileHeight);
    if (bits.size() < tiles * 4 || refs.size() < tiles * 4) return false;
    out.assign(size_t(width) * height, 0);
    size_t offset = kHeaderBytes, index = 0;
    std::array<Block, 4> phase{};
    std::vector<uint16_t> rows(size_t(encodedWidth) * 4);
    for (uint32_t y = 0; y < height; y += kTileHeight) {
        for (uint32_t x = 0; x < encodedWidth; x += kTileWidth) {
            for (uint32_t p = 0; p < 4; ++p, ++index) {
                if (bits[index] > 16) return false;
                const uint32_t b = unpackedBits(bits[index]);
                if (offset + kBlockBytes[b] > bitsOffset) return false;
                unpack(data + offset, b, phase[p].data());
                offset += kBlockBytes[b];
                for (uint16_t& value : phase[p]) value = static_cast<uint16_t>(value + refs[index]);
            }
            for (uint32_t j = 0; j < kTileWidth / 2; ++j) {
                rows[0 * encodedWidth + x + 2 * j] = phase[0][j];
                rows[0 * encodedWidth + x + 2 * j + 1] = phase[1][j];
                rows[2 * encodedWidth + x + 2 * j] = phase[0][32 + j];
                rows[2 * encodedWidth + x + 2 * j + 1] = phase[1][32 + j];
                rows[1 * encodedWidth + x + 2 * j] = phase[2][j];
                rows[1 * encodedWidth + x + 2 * j + 1] = phase[3][j];
                rows[3 * encodedWidth + x + 2 * j] = phase[2][32 + j];
                rows[3 * encodedWidth + x + 2 * j + 1] = phase[3][32 + j];
            }
        }
        for (uint32_t r = 0; r < 4; ++r)
            std::memcpy(out.data() + size_t(y + r) * width, rows.data() + size_t(r) * encodedWidth, width * 2);
    }
    return true;
}

}  // namespace rawr::mcraw
