/**
 * @file utf.hpp
 * @brief UTF table parser for CRIWARE formats
 * 
 * Based on format documentation from PyCriCodecs and donmai's writeup:
 * https://listed.to/@donmai/24921/criware-s-usm-format-part-1
 */
#ifndef UTF_HPP
#define UTF_HPP

#include <cstdint>
#include <vector>
#include <string>
#include <variant>
#include <map>
#include <memory>

namespace utf {

// UTF type values
enum class TypeId : uint8_t {
    UChar   = 0,
    Char    = 1,
    UShort  = 2,
    Short   = 3,
    UInt    = 4,
    Int     = 5,
    ULLong  = 6,
    LLong   = 7,
    Float   = 8,
    Double  = 9,
    String  = 10,
    Bytes   = 11
};

// UTF storage flags (from high nibble)
enum class StorageFlag : uint8_t {
    Constant = 0x10,  // Zero/null value for all rows (no data stored)
    PerRow   = 0x30,  // Same value for all rows, stored in column definition
    Data     = 0x50   // Value stored per-row in row data section
};

// Value variant type
using Value = std::variant<
    uint8_t,                    // uchar
    int8_t,                     // char
    uint16_t,                   // ushort
    int16_t,                    // short
    uint32_t,                   // uint
    int32_t,                    // int
    uint64_t,                   // ullong
    int64_t,                    // llong
    float,                      // float
    double,                     // double
    std::string,                // string
    std::vector<uint8_t>        // bytes
>;

// Column definition
struct Column {
    std::string name;
    TypeId type;
    StorageFlag storage;
    Value constant_value;  // Used for Constant and PerRow storage
};

// Row is a map of column name to value
using Row = std::map<std::string, Value>;

// UTF Table structure
class Table {
public:
    std::string name;
    std::vector<Column> columns;
    std::vector<Row> rows;

    // Parse a UTF table from raw bytes
    static Table parse(const uint8_t* data, size_t size);
    
    // Build a UTF table to raw bytes
    std::vector<uint8_t> build(bool encrypt = false) const;
    
    // Get a value from row
    template<typename T>
    T get(size_t row, const std::string& column) const {
        if (row >= rows.size()) {
            throw std::runtime_error("Row index out of range");
        }
        auto it = rows[row].find(column);
        if (it == rows[row].end()) {
            throw std::runtime_error("Column not found: " + column);
        }
        return std::get<T>(it->second);
    }

    // Check if column exists
    bool has_column(const std::string& name) const {
        for (const auto& col : columns) {
            if (col.name == name) return true;
        }
        return false;
    }

    // Get number of rows
    size_t row_count() const { return rows.size(); }
};

// Utility functions for byte order conversion (big-endian)
inline uint16_t read_be16(const uint8_t* p) {
    return (static_cast<uint16_t>(p[0]) << 8) | p[1];
}

inline uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | 
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

inline uint64_t read_be64(const uint8_t* p) {
    return (static_cast<uint64_t>(read_be32(p)) << 32) | read_be32(p + 4);
}

inline void write_be16(uint8_t* p, uint16_t v) {
    p[0] = (v >> 8) & 0xff;
    p[1] = v & 0xff;
}

inline void write_be32(uint8_t* p, uint32_t v) {
    p[0] = (v >> 24) & 0xff;
    p[1] = (v >> 16) & 0xff;
    p[2] = (v >> 8) & 0xff;
    p[3] = v & 0xff;
}

inline void write_be64(uint8_t* p, uint64_t v) {
    write_be32(p, static_cast<uint32_t>(v >> 32));
    write_be32(p + 4, static_cast<uint32_t>(v & 0xffffffff));
}

} // namespace utf

#endif // UTF_HPP
