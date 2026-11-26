/**
 * @file utf.cpp
 * @brief UTF table parser implementation for CRIWARE formats
 */
#include "utf.hpp"
#include <cstring>
#include <stdexcept>
#include <algorithm>

namespace utf {

// UTF header structure (24 bytes)
#pragma pack(push, 1)
struct UTFHeader {
    uint8_t magic[4];           // "@UTF" or encrypted
    uint32_t table_size;        // Size of table data
    uint32_t rows_offset;       // Offset to row data
    uint32_t string_offset;     // Offset to string data
    uint32_t data_offset;       // Offset to binary data
    uint32_t table_name_offset; // Offset to table name in strings
    uint16_t num_columns;       // Number of columns
    uint16_t row_length;        // Length of each row
    uint32_t num_rows;          // Number of rows
};
#pragma pack(pop)

// Encrypted UTF magic bytes
static const uint8_t EUTF_MAGIC[] = {0x1F, 0x9E, 0xF3, 0xF5};

// Decrypt encrypted UTF data
static std::vector<uint8_t> decrypt_utf(const uint8_t* data, size_t size) {
    std::vector<uint8_t> result(data, data + size);
    uint32_t m = 0x655f;
    uint32_t t = 0x4115;
    for (size_t i = 0; i < size; i++) {
        result[i] ^= (m & 0xFF);
        m = (m * t) & 0xFFFFFFFF;
    }
    return result;
}

// Encrypt UTF data
static std::vector<uint8_t> encrypt_utf(const uint8_t* data, size_t size) {
    // Same algorithm for encrypt and decrypt
    return decrypt_utf(data, size);
}

// Get type size in bytes
static size_t type_size(TypeId type) {
    switch (type) {
        case TypeId::UChar:
        case TypeId::Char:
            return 1;
        case TypeId::UShort:
        case TypeId::Short:
            return 2;
        case TypeId::UInt:
        case TypeId::Int:
        case TypeId::Float:
        case TypeId::String:
            return 4;
        case TypeId::ULLong:
        case TypeId::LLong:
        case TypeId::Double:
            return 8;
        case TypeId::Bytes:
            return 8; // offset (4) + size (4)
        default:
            throw std::runtime_error("Unknown type");
    }
}

// Read a value from data
static Value read_value(const uint8_t* data, TypeId type, 
                       const uint8_t* string_table, size_t string_table_size,
                       const uint8_t* data_table, size_t data_table_size) {
    switch (type) {
        case TypeId::UChar:
            return static_cast<uint8_t>(data[0]);
        case TypeId::Char:
            return static_cast<int8_t>(data[0]);
        case TypeId::UShort:
            return read_be16(data);
        case TypeId::Short:
            return static_cast<int16_t>(read_be16(data));
        case TypeId::UInt:
            return read_be32(data);
        case TypeId::Int:
            return static_cast<int32_t>(read_be32(data));
        case TypeId::ULLong:
            return read_be64(data);
        case TypeId::LLong:
            return static_cast<int64_t>(read_be64(data));
        case TypeId::Float: {
            uint32_t raw = read_be32(data);
            float f;
            std::memcpy(&f, &raw, 4);
            return f;
        }
        case TypeId::Double: {
            uint64_t raw = read_be64(data);
            double d;
            std::memcpy(&d, &raw, 8);
            return d;
        }
        case TypeId::String: {
            uint32_t offset = read_be32(data);
            if (offset < string_table_size) {
                const char* str = reinterpret_cast<const char*>(string_table + offset);
                return std::string(str);
            }
            return std::string("");
        }
        case TypeId::Bytes: {
            uint32_t offset = read_be32(data);
            uint32_t size = read_be32(data + 4);
            std::vector<uint8_t> result;
            if (offset + size <= data_table_size) {
                result.assign(data_table + offset, data_table + offset + size);
            }
            return result;
        }
        default:
            throw std::runtime_error("Unknown type");
    }
}

// Find string offset in string table
static uint32_t find_string_offset(const std::vector<uint8_t>& string_table, const std::string& str) {
    if (str.empty()) return 0;
    
    // Search for the string in the table
    for (size_t i = 0; i < string_table.size();) {
        const char* s = reinterpret_cast<const char*>(&string_table[i]);
        if (str == s) {
            return static_cast<uint32_t>(i);
        }
        i += std::strlen(s) + 1;
    }
    
    throw std::runtime_error("String not found in table: " + str);
}

Table Table::parse(const uint8_t* data, size_t size) {
    if (size < sizeof(UTFHeader)) {
        throw std::runtime_error("Data too small for UTF header");
    }

    std::vector<uint8_t> decrypted;
    const uint8_t* table_data = data;

    // Check for encrypted UTF
    if (std::memcmp(data, EUTF_MAGIC, 4) == 0) {
        decrypted = decrypt_utf(data, size);
        table_data = decrypted.data();
    }

    // Check magic
    if (std::memcmp(table_data, "@UTF", 4) != 0) {
        throw std::runtime_error("Invalid UTF magic");
    }

    // Parse header (big-endian)
    uint32_t table_size = read_be32(table_data + 4);
    uint32_t rows_offset = read_be32(table_data + 8);
    uint32_t string_offset = read_be32(table_data + 12);
    uint32_t data_offset = read_be32(table_data + 16);
    uint32_t table_name_offset = read_be32(table_data + 20);
    uint16_t num_columns = read_be16(table_data + 24);
    uint16_t row_length = read_be16(table_data + 26);
    uint32_t num_rows = read_be32(table_data + 28);

    // All offsets are relative to byte 8 (after magic + size)
    const size_t base_offset = 8;
    const uint8_t* column_data = table_data + 32;  // Header is 32 bytes total with magic/size
    const uint8_t* row_data = table_data + base_offset + rows_offset;
    const uint8_t* string_table = table_data + base_offset + string_offset;
    const uint8_t* binary_data = table_data + base_offset + data_offset;

    size_t string_table_size = data_offset - string_offset;
    size_t data_table_size = table_size - data_offset;

    Table table;

    // Read table name
    if (table_name_offset < string_table_size) {
        table.name = reinterpret_cast<const char*>(string_table + table_name_offset);
    }

    // Parse columns
    const uint8_t* col_ptr = column_data;
    for (uint16_t i = 0; i < num_columns; i++) {
        Column col;
        
        uint8_t flag = *col_ptr++;
        col.storage = static_cast<StorageFlag>(flag & 0xF0);
        col.type = static_cast<TypeId>(flag & 0x0F);

        // Read column name
        uint32_t name_offset = read_be32(col_ptr);
        col_ptr += 4;
        if (name_offset < string_table_size) {
            col.name = reinterpret_cast<const char*>(string_table + name_offset);
        }

        // Read constant value if present (PerRow stores value in column def)
        if (col.storage == StorageFlag::PerRow) {
            col.constant_value = read_value(col_ptr, col.type, 
                                           string_table, string_table_size,
                                           binary_data, data_table_size);
            col_ptr += type_size(col.type);
        } else if (col.storage == StorageFlag::Constant) {
            // Constant flag (0x10) means no data stored - use default/null value
            switch (col.type) {
                case TypeId::UChar: col.constant_value = uint8_t(0); break;
                case TypeId::Char: col.constant_value = int8_t(0); break;
                case TypeId::UShort: col.constant_value = uint16_t(0); break;
                case TypeId::Short: col.constant_value = int16_t(0); break;
                case TypeId::UInt: col.constant_value = uint32_t(0); break;
                case TypeId::Int: col.constant_value = int32_t(0); break;
                case TypeId::ULLong: col.constant_value = uint64_t(0); break;
                case TypeId::LLong: col.constant_value = int64_t(0); break;
                case TypeId::Float: col.constant_value = float(0); break;
                case TypeId::Double: col.constant_value = double(0); break;
                case TypeId::String: col.constant_value = std::string("<NULL>"); break;
                case TypeId::Bytes: col.constant_value = std::vector<uint8_t>(); break;
            }
        }

        table.columns.push_back(col);
    }

    // Parse rows
    for (uint32_t row_idx = 0; row_idx < num_rows; row_idx++) {
        Row row;
        const uint8_t* row_ptr = row_data + (row_idx * row_length);

        for (const auto& col : table.columns) {
            if (col.storage == StorageFlag::Data) {
                row[col.name] = read_value(row_ptr, col.type,
                                          string_table, string_table_size,
                                          binary_data, data_table_size);
                row_ptr += type_size(col.type);
            } else {
                // Constant or PerRow - use the stored constant value
                row[col.name] = col.constant_value;
            }
        }

        table.rows.push_back(row);
    }

    return table;
}

static Value default_value_for(TypeId type) {
    switch (type) {
        case TypeId::UChar: return uint8_t(0);
        case TypeId::Char: return int8_t(0);
        case TypeId::UShort: return uint16_t(0);
        case TypeId::Short: return int16_t(0);
        case TypeId::UInt: return uint32_t(0);
        case TypeId::Int: return int32_t(0);
        case TypeId::ULLong: return uint64_t(0);
        case TypeId::LLong: return int64_t(0);
        case TypeId::Float: return float(0);
        case TypeId::Double: return double(0);
        case TypeId::String: return std::string("");
        case TypeId::Bytes: return std::vector<uint8_t>{};
        default: throw std::runtime_error("Unknown type");
    }
}

std::vector<uint8_t> Table::build(bool encrypt) const {
    std::vector<uint8_t> string_table;
    std::vector<uint8_t> data_table;
    std::map<std::string, uint32_t> string_offsets;

    auto add_string = [&](const std::string& s) -> uint32_t {
        auto it = string_offsets.find(s);
        if (it != string_offsets.end()) {
            return it->second;
        }
        uint32_t off = static_cast<uint32_t>(string_table.size());
        string_offsets[s] = off;
        string_table.insert(string_table.end(), s.begin(), s.end());
        string_table.push_back(0);
        return off;
    };

    // CRI-compatible order: <NULL>, table name, column names, then values.
    add_string("<NULL>");
    uint32_t table_name_offset = add_string(name);
    for (const auto& col : columns) {
        add_string(col.name);
    }
    for (const auto& col : columns) {
        if (col.type == TypeId::String &&
            (col.storage == StorageFlag::PerRow || col.storage == StorageFlag::Data)) {
            const auto& s = std::get<std::string>(col.constant_value);
            if (!s.empty()) {
                add_string(s);
            }
        }
    }
    if (name == "CpkTocInfo" || name == "CpkEtocInfo") {
        add_string("");
    }

    for (const auto& row : rows) {
        for (const auto& col : columns) {
            if (col.storage != StorageFlag::Data || col.type != TypeId::String) {
                continue;
            }
            auto it = row.find(col.name);
            const Value& v = (it != row.end()) ? it->second : col.constant_value;
            const auto& s = std::get<std::string>(v);
            if (!s.empty()) {
                add_string(s);
            }
        }
    }

    auto write_typed_value = [&](std::vector<uint8_t>& buffer, const Value& value, TypeId type) {
        size_t pos = buffer.size();
        switch (type) {
            case TypeId::UChar: buffer.push_back(std::get<uint8_t>(value)); break;
            case TypeId::Char: buffer.push_back(static_cast<uint8_t>(std::get<int8_t>(value))); break;
            case TypeId::UShort:
                buffer.resize(pos + 2);
                write_be16(&buffer[pos], std::get<uint16_t>(value));
                break;
            case TypeId::Short:
                buffer.resize(pos + 2);
                write_be16(&buffer[pos], static_cast<uint16_t>(std::get<int16_t>(value)));
                break;
            case TypeId::UInt:
                buffer.resize(pos + 4);
                write_be32(&buffer[pos], std::get<uint32_t>(value));
                break;
            case TypeId::Int:
                buffer.resize(pos + 4);
                write_be32(&buffer[pos], static_cast<uint32_t>(std::get<int32_t>(value)));
                break;
            case TypeId::ULLong:
                buffer.resize(pos + 8);
                write_be64(&buffer[pos], std::get<uint64_t>(value));
                break;
            case TypeId::LLong:
                buffer.resize(pos + 8);
                write_be64(&buffer[pos], static_cast<uint64_t>(std::get<int64_t>(value)));
                break;
            case TypeId::Float: {
                float f = std::get<float>(value);
                uint32_t raw;
                std::memcpy(&raw, &f, 4);
                buffer.resize(pos + 4);
                write_be32(&buffer[pos], raw);
                break;
            }
            case TypeId::Double: {
                double d = std::get<double>(value);
                uint64_t raw;
                std::memcpy(&raw, &d, 8);
                buffer.resize(pos + 8);
                write_be64(&buffer[pos], raw);
                break;
            }
            case TypeId::String: {
                const auto& s = std::get<std::string>(value);
                uint32_t off = 0;
                auto it = string_offsets.find(s);
                if (it != string_offsets.end()) {
                    off = it->second;
                } else if (s.empty()) {
                    auto eit = string_offsets.find("");
                    if (eit != string_offsets.end()) {
                        off = eit->second;
                    }
                }
                buffer.resize(pos + 4);
                write_be32(&buffer[pos], off);
                break;
            }
            case TypeId::Bytes: {
                const auto& bytes = std::get<std::vector<uint8_t>>(value);
                uint32_t off = static_cast<uint32_t>(data_table.size());
                data_table.insert(data_table.end(), bytes.begin(), bytes.end());
                buffer.resize(pos + 8);
                write_be32(&buffer[pos], off);
                write_be32(&buffer[pos + 4], static_cast<uint32_t>(bytes.size()));
                break;
            }
            default:
                throw std::runtime_error("Unknown type");
        }
    };

    // Build column data
    std::vector<uint8_t> column_data;
    uint16_t row_length = 0;
    
    for (const auto& col : columns) {
        // Flag byte
        column_data.push_back(static_cast<uint8_t>(col.storage) | static_cast<uint8_t>(col.type));
        
        // Column name offset
        size_t pos = column_data.size();
        column_data.resize(pos + 4);
        write_be32(&column_data[pos], string_offsets[col.name]);
        
        // Only PerRow stores a value in column definition.
        if (col.storage == StorageFlag::PerRow) {
            write_typed_value(column_data, col.constant_value, col.type);
        } else if (col.storage == StorageFlag::Data) {
            row_length += static_cast<uint16_t>(type_size(col.type));
        }
    }
    
    // Build row data
    std::vector<uint8_t> row_data;
    for (const auto& row : rows) {
        for (const auto& col : columns) {
            if (col.storage == StorageFlag::Data) {
                auto it = row.find(col.name);
                const Value& v = (it != row.end()) ? it->second : col.constant_value;
                write_typed_value(row_data, v, col.type);
            }
        }
    }

    // CRI CPK tables keep some trailing empty strings in the string table.
    if (name == "CpkHeader") {
        for (int i = 0; i < 7; ++i) {
            string_table.push_back(0);
        }
    } else if (name == "CpkTocInfo") {
        for (int i = 0; i < 4; ++i) {
            string_table.push_back(0);
        }
    } else if (name == "CpkEtocInfo") {
        for (int i = 0; i < 6; ++i) {
            string_table.push_back(0);
        }
    }
    
    // Calculate offsets
    uint32_t rows_offset = static_cast<uint32_t>(column_data.size()) + 24;  // +24 for header minus magic/size
    uint32_t string_offset_val = rows_offset + static_cast<uint32_t>(row_data.size());
    uint32_t data_offset_val = string_offset_val + static_cast<uint32_t>(string_table.size());
    uint32_t table_size = data_offset_val + static_cast<uint32_t>(data_table.size());
    
    // Build final buffer
    std::vector<uint8_t> result;
    result.reserve(8 + table_size);
    
    // Magic
    result.push_back('@');
    result.push_back('U');
    result.push_back('T');
    result.push_back('F');
    
    // Table size (big-endian)
    result.resize(8);
    write_be32(&result[4], table_size);
    
    // Header fields
    result.resize(32);
    write_be32(&result[8], rows_offset);
    write_be32(&result[12], string_offset_val);
    write_be32(&result[16], data_offset_val);
    write_be32(&result[20], table_name_offset);
    write_be16(&result[24], static_cast<uint16_t>(columns.size()));
    write_be16(&result[26], row_length);
    write_be32(&result[28], static_cast<uint32_t>(rows.size()));
    
    // Append data sections
    result.insert(result.end(), column_data.begin(), column_data.end());
    result.insert(result.end(), row_data.begin(), row_data.end());
    result.insert(result.end(), string_table.begin(), string_table.end());
    result.insert(result.end(), data_table.begin(), data_table.end());
    
    // Encrypt if requested
    if (encrypt) {
        result = encrypt_utf(result.data(), result.size());
    }
    
    return result;
}

} // namespace utf
