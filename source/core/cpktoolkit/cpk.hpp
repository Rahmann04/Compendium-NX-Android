/**
 * @file cpk.hpp
 * @brief CPK archive handler for CRIWARE formats
 */
#ifndef CPK_HPP
#define CPK_HPP

#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <fstream>
#include <functional>
#include "utf.hpp"

namespace cpk_toolkit {

// File entry information
struct FileEntry {
    std::string dir_name;
    std::string file_name;
    uint64_t offset;
    uint32_t file_size;         // Size in archive (compressed)
    uint32_t extract_size;      // Original size (uncompressed)
    uint32_t id;
    std::string user_string;
    
    // Is this file compressed?
    bool is_compressed() const { return extract_size > file_size; }
    
    // Get full path
    std::string full_path() const {
        if (dir_name.empty()) return file_name;
        return dir_name + "/" + file_name;
    }
};

// CPK Mode determines the structure of the archive
enum class CpkMode {
    IdOnly = 0,      // ITOC only, files identified by ID
    FileNames = 1,   // TOC only, files have names
    Both = 2,        // TOC + ITOC
    WithGroups = 3   // TOC + GTOC
};

// CPK Archive class
class Archive {
public:
    // Open an existing CPK file
    explicit Archive(const std::string& filename);
    
    // Open from memory
    Archive(const uint8_t* data, size_t size);
    
    ~Archive();
    
    // Get list of all files
    const std::vector<FileEntry>& files() const { return files_; }
    
    // Find a file by name
    const FileEntry* find_file(const std::string& name) const;
    
    // Find a file by ID (for IdOnly mode)
    const FileEntry* find_file_by_id(uint32_t id) const;
    
    // Extract a single file (returns decompressed data)
    std::vector<uint8_t> extract_file(const FileEntry& entry) const;
    
    // Extract a single file by name
    std::vector<uint8_t> extract_file(const std::string& name) const;
    
    // Extract raw file data (without decompression)
    std::vector<uint8_t> extract_file_raw(const FileEntry& entry) const;
    uint64_t raw_file_offset(const FileEntry& entry) const;
    
    // Extract all files to a directory
    void extract_all(const std::string& output_dir, 
                    std::function<void(const std::string&, size_t, size_t)> progress = nullptr) const;
    
    // Copy archive to new file (byte-identical)
    void copy_to(const std::string& output_path,
                 std::function<void(const std::string&, size_t, size_t)> progress = nullptr) const;
    
    // Get archive info
    CpkMode mode() const { return mode_; }
    uint32_t file_count() const { return static_cast<uint32_t>(files_.size()); }
    uint64_t content_size() const { return content_size_; }
    uint64_t content_offset() const { return content_offset_; }
    uint16_t alignment() const { return align_; }
    std::string version() const { return tvers_; }
    
    // Get raw CPK header table for preservation
    const utf::Table& cpk_header() const { return cpk_header_; }
    const utf::Table& toc_table() const { return toc_table_; }
    uint32_t toc_crc_raw() const { return toc_crc_raw_; }
    
    // Get raw binary data for exact reproduction
    const std::vector<uint8_t>& cpk_header_raw() const { return cpk_header_raw_; }
    const std::vector<uint8_t>& toc_chunk_raw() const { return toc_chunk_raw_; }
    uint64_t files_start_offset() const { return files_start_offset_; }
    
    // Get total archive size
    uint64_t total_size() const;

private:
    void parse_header();
    void parse_toc(const uint8_t* data, size_t size);
    void parse_itoc(const uint8_t* data, size_t size);
    
    std::unique_ptr<std::ifstream> file_;
    std::vector<uint8_t> data_;  // For memory mode
    
    std::vector<FileEntry> files_;
    
    CpkMode mode_ = CpkMode::FileNames;
    uint64_t content_offset_ = 0;
    uint64_t content_size_ = 0;
    uint64_t toc_offset_ = 0;
    uint64_t toc_size_ = 0;
    uint64_t itoc_offset_ = 0;
    uint64_t itoc_size_ = 0;
    uint16_t align_ = 0x800;
    std::string tvers_;
    uint64_t files_start_offset_ = 0;
    uint32_t toc_crc_raw_ = 0;
    
    // Preserved tables for exact reproduction
    utf::Table cpk_header_;
    utf::Table toc_table_;
    
    // Raw binary data for byte-identical reproduction
    std::vector<uint8_t> cpk_header_raw_;  // First 0x800 bytes
    std::vector<uint8_t> toc_chunk_raw_;   // TOC chunk with padding
    
    // Helper to read from file or memory
    std::vector<uint8_t> read_data(uint64_t offset, size_t size) const;
};

// CPK Builder for creating new archives
class Builder {
public:
    explicit Builder(CpkMode mode = CpkMode::FileNames);
    
    // Create builder from existing archive (preserves structure)
    static Builder from_archive(const Archive& archive);
    
    // Add files from a directory
    void add_directory(const std::string& dir_path);
    
    // Add a single file
    void add_file(const std::string& path, const std::string& archive_path);
    
    // Add file from memory
    void add_file(const std::vector<uint8_t>& data, const std::string& archive_path);
    
    // Add pre-compressed file (raw CRILAYLA data)
    void add_file_raw(const std::vector<uint8_t>& raw_data, const std::string& archive_path,
                      uint32_t extract_size);

    void add_file_raw_from_file(const std::string& source_path, uint64_t source_offset,
                                uint32_t stored_size, const std::string& archive_path,
                                uint32_t extract_size);
    
    // Set alignment (default 0x800)
    void set_alignment(uint16_t align) { align_ = align; }
    
    // Enable compression
    void set_compression(bool enable) { compress_ = enable; }
    
    // Set version string
    void set_version(const std::string& version) { tvers_ = version; }
    
    // Build the archive
    void build(const std::string& output_path,
              std::function<void(const std::string&, size_t, size_t)> progress = nullptr);
    uint64_t predict_size() const;

private:
    struct PendingFile {
        std::string archive_path;
        std::string source_path;  // Empty if from memory
        std::vector<uint8_t> data;  // Used if source_path is empty
        uint64_t source_offset = 0;
        uint64_t source_size = 0;
        bool is_raw = false;        // True if data is already compressed
        uint32_t extract_size = 0;  // Original size for raw files
        uint32_t id = 0;
        std::string user_string = "<NULL>";
        bool has_ads_metadata = false;
        uint32_t toc_crc_raw = 0;
    };
    
    std::vector<PendingFile> pending_files_;
    CpkMode mode_;
    uint16_t align_ = 0x800;
    bool compress_ = false;
    std::string tvers_ = "CPKMC2.30.07, DLL3.00.07";
    bool has_ads_archive_ = false;
    std::vector<uint8_t> ads_archive_data_;
    
    // Preserved header structure from source archive
    bool has_preserved_structure_ = false;
    utf::Table preserved_cpk_header_;
    std::vector<uint8_t> preserved_cpk_header_raw_;
};

// Utility to create directory path
void create_directories(const std::string& path);

// Utility to get parent path
std::string parent_path(const std::string& path);

// Utility to get filename from path
std::string filename(const std::string& path);

} // namespace cpk_toolkit

#endif // CPK_HPP
