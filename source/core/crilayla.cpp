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

namespace crilayla {

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

    // Allocate output buffer (worst case: same size as input + header)
    std::vector<uint8_t> output(size + 0x110);
    uint8_t* dest = output.data();
    
    uint32_t n = static_cast<uint32_t>(size) - 1;
    uint32_t m = static_cast<uint32_t>(size) - 1;
    uint32_t T = 0, d = 0;
    
    while (n >= 0x100) {
        uint32_t j = n + 3 + 0x2000;
        if (j > size) j = static_cast<uint32_t>(size);
        
        uint32_t p = 0, q = 0;
        for (uint32_t i = n + 3; i < j; i++) {
            uint32_t k;
            for (k = 0; k <= n - 0x100; k++) {
                if (data[n - k] != data[i - k]) break;
            }
            if (k > p) {
                q = i - n - 3;
                p = k;
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
                        d = d & ((1 << T) - 1);
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
            d = d & ((1 << T) - 1);
        }
    }

    if (T != 0) {
        dest[m--] = d << (8 - T);
    }
    dest[m--] = 0;
    dest[m] = 0;

    while ((((uint32_t)size - m) & 3) != 0) {
        dest[m--] = 0;
    }

    uint32_t compressed_size = static_cast<uint32_t>(size) - m;
    uint8_t* compressed_start = dest + m;

    // Build final output
    std::vector<uint8_t> result(16 + compressed_size + 256);
    
    // CRILAYLA header (little-endian)
    std::memcpy(result.data(), "CRILAYLA", 8);
    uint32_t uncompressed_size = static_cast<uint32_t>(size) - 256;
    std::memcpy(result.data() + 8, &uncompressed_size, 4);
    std::memcpy(result.data() + 12, &compressed_size, 4);
    
    // Compressed data
    std::memcpy(result.data() + 16, compressed_start, compressed_size);
    
    // First 256 bytes (uncompressed prefix)
    std::memcpy(result.data() + 16 + compressed_size, data, 256);

    return result;
}

} // namespace crilayla
