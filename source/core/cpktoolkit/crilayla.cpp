/**
 * @file crilayla.cpp
 * @brief CRILAYLA compression/decompression implementation
 * 
 * Decompression algorithm originally by tpu (XeNTaX forum)
 * https://forum.xentax.com/viewtopic.php?f=21&t=5137&p=44220
 * 
 * Compression algorithm by KenTse, from wmltogether's CriPakTools fork
 * 
 * Adapted to modern C++ by this project.
 */
#include "crilayla.hpp"
#include <stdexcept>
#include <algorithm>

namespace cpk_toolkit::crilayla {

// Bit reader state for decompression
class BitReader {
private:
    const uint8_t* buffer;
    const uint8_t* current;
    uint32_t bit_count = 0;
    uint32_t bit_data = 0;

public:
    BitReader(const uint8_t* data, size_t size) 
        : buffer(data), current(data + size - 1) {}

    uint32_t get_bits(uint32_t n) {
        while (bit_count < n) {
            uint32_t bytes_needed = ((24 - bit_count) >> 3) + 1;
            bit_count += bytes_needed * 8;
            while (bytes_needed--) {
                bit_data = (bit_data << 8) | (*current--);
            }
        }

        uint32_t data = bit_data >> (bit_count - n);
        bit_count -= n;
        uint32_t mask = (1 << n) - 1;
        return data & mask;
    }
};

std::vector<uint8_t> decompress(const uint8_t* data, size_t size) {
    if (size < sizeof(CrilaylaHeader)) {
        throw std::runtime_error("Data too small for CRILAYLA header");
    }

    // Check magic
    if (!is_compressed(data, size)) {
        throw std::runtime_error("Invalid CRILAYLA magic");
    }

    const CrilaylaHeader* header = reinterpret_cast<const CrilaylaHeader*>(data);
    uint32_t uncompressed_size = header->uncompressed_size;
    uint32_t compressed_size = header->compressed_size;

    // Validate sizes
    if (16 + compressed_size + 256 > size) {
        throw std::runtime_error("Invalid CRILAYLA data size");
    }

    // Output buffer: 256 bytes header + uncompressed data
    std::vector<uint8_t> output(256 + uncompressed_size);
    
    // Copy the 256 byte uncompressed header
    const uint8_t* prefix = data + 16 + compressed_size;
    std::memcpy(output.data(), prefix, 256);

    // Decompress the rest
    const uint8_t* compressed_data = data + 16;
    BitReader reader(compressed_data, compressed_size);
    
    uint8_t* dst = output.data() + 256 + uncompressed_size - 1;
    uint8_t* dst_start = output.data() + 256;

    while (dst >= dst_start) {
        if (reader.get_bits(1) == 0) {
            // Literal byte
            *dst-- = static_cast<uint8_t>(reader.get_bits(8));
        } else {
            // Back reference
            uint32_t offset = reader.get_bits(13);
            uint32_t length = reader.get_bits(2);

            if (length == 3) {
                length += reader.get_bits(3);
                if (length == 10) {
                    length += reader.get_bits(5);
                    if (length == 41) {
                        uint32_t byte;
                        do {
                            byte = reader.get_bits(8);
                            length += byte;
                        } while (byte == 255);
                    }
                }
            }

            length += 3;
            const uint8_t* src_ptr = dst + offset + 3;

            while (length-- > 0 && dst >= dst_start) {
                *dst-- = *src_ptr--;
            }
        }
    }

    return output;
}

std::vector<uint8_t> compress(const uint8_t* data, size_t size) {
    if (size < 256) {
        throw std::runtime_error("Data too small for CRILAYLA compression (min 256 bytes)");
    }
    if (size > UINT32_MAX) {
        throw std::runtime_error("CRILAYLA input exceeds 32-bit size limit");
    }

    constexpr uint32_t kWindow = 0x2000;
    constexpr uint32_t kMask = kWindow - 1;
    constexpr uint32_t kNone = UINT32_MAX;
    constexpr uint32_t kHashSize = 1u << 16;
    constexpr uint32_t kMaxCandidates = 64;

    // Fixed-size hash-chain storage: independent of input file size.
    std::vector<uint32_t> head(kHashSize, kNone);
    std::vector<uint32_t> prev(kWindow, kNone);
    std::vector<uint32_t> next(kWindow, kNone);
    std::vector<uint32_t> slot_pos(kWindow, kNone);
    std::vector<uint32_t> slot_hash(kWindow, 0);

    auto hash_at = [&](uint32_t pos) -> uint32_t {
        uint32_t h = uint32_t(data[pos]) * 251u;
        h = (h + uint32_t(data[pos - 1])) * 251u;
        h = (h + uint32_t(data[pos - 2]));
        return h & (kHashSize - 1);
    };

    auto remove_candidate = [&](uint32_t pos) {
        const uint32_t slot = pos & kMask;
        if (slot_pos[slot] != pos) return;

        const uint32_t h = slot_hash[slot];
        const uint32_t p = prev[slot];
        const uint32_t q = next[slot];

        if (p != kNone) {
            next[p & kMask] = q;
        } else {
            head[h] = q;
        }
        if (q != kNone) {
            prev[q & kMask] = p;
        }

        slot_pos[slot] = kNone;
        prev[slot] = kNone;
        next[slot] = kNone;
    };

    auto add_candidate = [&](uint32_t pos) {
        const uint32_t slot = pos & kMask;
        const uint32_t h = hash_at(pos);
        const uint32_t old_head = head[h];

        slot_pos[slot] = pos;
        slot_hash[slot] = h;
        prev[slot] = kNone;
        next[slot] = old_head;

        if (old_head != kNone) {
            prev[old_head & kMask] = pos;
        }
        head[h] = pos;
    };

    // The bitstream is written backwards and can expand to 9 bits per
    // input byte when data is incompressible. Reserve worst-case headroom.
    const size_t scratch_headroom = (size + 7) / 8 + 0x110;
    std::vector<uint8_t> output(size + scratch_headroom);
    uint8_t* dest = output.data();

    uint32_t n = static_cast<uint32_t>(size) - 1;
    size_t m = output.size() - 1;
    uint32_t T = 0, d = 0;

    // The active candidate window corresponds to the previous n value.
    uint32_t window_n = n;

    while (n >= 0x100) {
        // Move the candidate window when n moves backwards. Remove expired
        // positions before adding new ones because ring slots can overlap.
        const uint64_t old_upper64 =
            uint64_t(window_n) + 3u + kWindow;
        const uint64_t new_upper64 =
            uint64_t(n) + 3u + kWindow;
        const uint32_t old_upper =
            static_cast<uint32_t>(std::min<uint64_t>(old_upper64, size));
        const uint32_t new_upper =
            static_cast<uint32_t>(std::min<uint64_t>(new_upper64, size));

        for (uint32_t pos = new_upper; pos < old_upper; ++pos) {
            remove_candidate(pos);
        }

        const uint64_t new_lower64 = uint64_t(n) + 3u;
        const uint64_t add_end64 = std::min(
            uint64_t(window_n) + 2u, new_upper64 - 1u);
        if (new_lower64 < size && add_end64 >= new_lower64) {
            const uint32_t add_end = static_cast<uint32_t>(
                std::min<uint64_t>(add_end64, size - 1));
            for (uint64_t pos = uint64_t(add_end) + 1;
                 pos-- > new_lower64;) {
                add_candidate(static_cast<uint32_t>(pos));
            }
        }
        window_n = n;

        uint32_t p = 0, q = 0;
        const uint32_t max_match = n - 0xFF;

        // A 3-byte hash is sufficient because matches shorter than 3 bytes
        // are encoded as literals.
        if (max_match >= 3) {
            const uint32_t h = hash_at(n);
            uint32_t candidate = head[h];
            uint32_t checked = 0;

            while (candidate != kNone && checked < kMaxCandidates) {
                ++checked;

                uint32_t k = 0;

                // Compare eight bytes at a time; fall back to byte checks
                // for the final block or when a block differs.
                while (k + 8 <= max_match &&
                       std::memcmp(data + n - k - 7,
                                   data + candidate - k - 7, 8) == 0) {
                    k += 8;
                }
                while (k < max_match &&
                       data[n - k] == data[candidate - k]) {
                    ++k;
                }

                if (k > p) {
                    p = k;
                    q = candidate - n - 3;
                    if (p == max_match) break;
                }

                candidate = next[candidate & kMask];
            }
        }

        if (p < 3) {
            d = (d << 9) | data[n--];
            T += 9;
        } else {
            d = (((d << 1) | 1) << 13) | q;
            T += 14;
            n -= p;

            if (p < 6) {
                d = (d << 2) | (p - 3);
                T += 2;
            } else if (p < 13) {
                d = (((d << 2) | 3) << 3) | (p - 6);
                T += 5;
            } else if (p < 44) {
                d = (((d << 5) | 0x1f) << 5) | (p - 13);
                T += 10;
            } else {
                d = ((d << 10) | 0x3ff);
                T += 10;
                p -= 44;

                while (true) {
                    while (T >= 8) {
                        dest[m--] = (d >> (T - 8)) & 0xff;
                        T -= 8;
                        d = d & ((1u << T) - 1);
                    }
                    if (p < 255) break;
                    d = (d << 8) | 0xff;
                    T += 8;
                    p -= 255;
                }
                d = (d << 8) | p;
                T += 8;
            }
        }

        while (T >= 8) {
            dest[m--] = (d >> (T - 8)) & 0xff;
            T -= 8;
            d = d & ((1u << T) - 1);
        }
    }

    if (T != 0) {
        dest[m--] = d << (8 - T);
    }
    dest[m--] = 0;
    dest[m] = 0;

    while (((output.size() - m) & 3) != 0) {
        dest[m--] = 0;
    }

    const size_t compressed_size_size = output.size() - m;
    if (compressed_size_size > UINT32_MAX) {
        throw std::runtime_error("CRILAYLA compressed output exceeds 32-bit size limit");
    }
    const uint32_t compressed_size = static_cast<uint32_t>(compressed_size_size);
    uint8_t* compressed_start = dest + m;

    std::vector<uint8_t> result(16 + compressed_size + 256);

    std::memcpy(result.data(), "CRILAYLA", 8);
    const uint32_t uncompressed_size = static_cast<uint32_t>(size) - 256;
    std::memcpy(result.data() + 8, &uncompressed_size, 4);
    std::memcpy(result.data() + 12, &compressed_size, 4);
    std::memcpy(result.data() + 16, compressed_start, compressed_size);
    std::memcpy(result.data() + 16 + compressed_size, data, 256);

    return result;
}

} // namespace cpk_toolkit::crilayla
