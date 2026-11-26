/**
 * @file crilayla.hpp
 * @brief CRILAYLA compression/decompression for CRIWARE CPK archives
 * 
 * Decompression algorithm by tpu (XeNTaX forum)
 * https://forum.xentax.com/viewtopic.php?f=21&t=5137&p=44220
 * 
 * Compression algorithm by KenTse, from wmltogether's CriPakTools fork
 */
#ifndef CRILAYLA_HPP
#define CRILAYLA_HPP

#include <cstdint>
#include <vector>
#include <cstring>

namespace crilayla {

#pragma pack(push, 1)
struct CrilaylaHeader {
    char magic[8];          // "CRILAYLA"
    uint32_t uncompressed_size;
    uint32_t compressed_size;
};
#pragma pack(pop)

/**
 * Check if data is CRILAYLA compressed
 */
inline bool is_compressed(const uint8_t* data, size_t size) {
    if (size < 16) return false;
    return std::memcmp(data, "CRILAYLA", 8) == 0;
}

/**
 * Decompress CRILAYLA data
 * @param data Input compressed data (including header)
 * @param size Size of input data
 * @return Decompressed data
 */
std::vector<uint8_t> decompress(const uint8_t* data, size_t size);

/**
 * Compress data using CRILAYLA algorithm
 * @param data Input uncompressed data
 * @param size Size of input data
 * @return Compressed data (including header)
 */
std::vector<uint8_t> compress(const uint8_t* data, size_t size);

} // namespace crilayla

#endif // CRILAYLA_HPP
