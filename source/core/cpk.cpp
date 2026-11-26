#include "cpk.hpp"
#include "crilayla.hpp"
#include "cpktoolkit/cpk.hpp"
#include <chrono>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>

namespace mm::cpk {

namespace {

constexpr uint64_t kChunkAlign = 0x800;
constexpr uint64_t kHeaderChunkSize = 0x800;

uint64_t alignUp(uint64_t v, uint64_t a) { return a ? (v + a - 1) / a * a : v; }

[[noreturn]] void corrupt(const std::string& what) { throw std::runtime_error("damaged or unsupported CPK file (" + what + ")"); }

uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]; }

void putBe16(std::vector<uint8_t>& b, size_t at, uint16_t v) {
    b[at] = uint8_t(v >> 8);
    b[at + 1] = uint8_t(v);
}
void putBe32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (24 - 8 * i));
}
void putBe64(std::vector<uint8_t>& b, size_t at, uint64_t v) {
    putBe32(b, at, uint32_t(v >> 32));
    putBe32(b, at + 4, uint32_t(v));
}
void putLe32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (8 * i));
}

// ------------------------------------------------------------------ @UTF tables

enum Type : uint8_t {
    U8 = 0x0, S8 = 0x1, U16 = 0x2, S16 = 0x3, U32 = 0x4, S32 = 0x5, U64 = 0x6, S64 = 0x7,
    F32 = 0x8, F64 = 0x9, Str = 0xA, Data = 0xB, Guid = 0xC,
};
constexpr uint8_t kFlagName = 0x10, kFlagDefault = 0x20, kFlagRow = 0x40;

uint32_t typeSize(uint8_t t) {
    switch (t) {
        case U8: case S8: return 1;
        case U16: case S16: return 2;
        case U32: case S32: case F32: case Str: return 4;
        case U64: case S64: case F64: case Data: return 8;
        case Guid: return 16;
        default: return 0;
    }
}

struct Value {
    enum Kind { None, Int, Text, Bytes } kind = None;
    uint64_t i = 0;
    std::string s;
    uint32_t dataOffset = 0, dataSize = 0;  // Bytes, when read
    bool operator==(const Value& o) const { return kind == o.kind && i == o.i && s == o.s; }
};

Value intValue(uint64_t v) { return {Value::Int, v, {}, 0, 0}; }
Value textValue(std::string v) { return {Value::Text, 0, std::move(v), 0, 0}; }
Value emptyBytes() { return {Value::Bytes, 0, {}, 0, 0}; }

struct Table {
    std::string name;
    std::vector<std::string> columns;
    std::vector<uint8_t> types;
    std::vector<std::vector<Value>> rows;

    int col(std::string_view n) const {
        for (size_t i = 0; i < columns.size(); ++i)
            if (columns[i] == n) return int(i);
        return -1;
    }
    uint64_t num(size_t row, std::string_view n, uint64_t fallback = 0) const {
        int c = col(n);
        if (c < 0 || row >= rows.size() || rows[row][c].kind != Value::Int) return fallback;
        return rows[row][c].i;
    }
    std::string text(size_t row, std::string_view n) const {
        int c = col(n);
        if (c < 0 || row >= rows.size() || rows[row][c].kind != Value::Text) return {};
        return rows[row][c].s;
    }
};

// CRI's table scrambling (used by some games; P5R's archives are plain).
void unscramble(uint8_t* p, size_t n) {
    uint32_t m = 0x655f;
    const uint32_t t = 0x4115;
    for (size_t i = 0; i < n; ++i) {
        p[i] ^= uint8_t(m & 0xff);
        m *= t;
    }
}

Table parseTable(std::vector<uint8_t> buf) {
    if (buf.size() >= 4 && std::memcmp(buf.data(), "@UTF", 4) != 0) unscramble(buf.data(), buf.size());
    if (buf.size() < 0x20 || std::memcmp(buf.data(), "@UTF", 4) != 0) corrupt("no @UTF table");
    const uint8_t* p = buf.data();
    uint64_t size = uint64_t(be32(p + 4)) + 8;
    uint64_t rowsOff = uint64_t(be32(p + 8)) + 8;
    uint64_t stringsOff = uint64_t(be32(p + 12)) + 8;
    uint64_t dataOff = uint64_t(be32(p + 16)) + 8;
    uint32_t nameOff = be32(p + 20);
    uint16_t ncols = be16(p + 24);
    uint16_t rowWidth = be16(p + 26);
    uint32_t nrows = be32(p + 28);
    if (size > buf.size() || rowsOff < 0x20 || rowsOff > stringsOff || stringsOff > dataOff || dataOff > size)
        corrupt("table offsets");
    auto str = [&](uint32_t off) -> std::string {
        uint64_t at = stringsOff + off;
        if (at >= dataOff) corrupt("string offset");
        const char* s = reinterpret_cast<const char*>(p + at);
        size_t len = strnlen(s, size_t(dataOff - at));
        return std::string(s, len);
    };
    auto readValue = [&](uint8_t type, uint64_t at) -> Value {
        if (at + typeSize(type) > size) corrupt("value out of bounds");
        const uint8_t* v = p + at;
        switch (type) {
            case U8: case S8: return intValue(v[0]);
            case U16: case S16: return intValue(be16(v));
            case U32: case S32: case F32: return intValue(be32(v));
            case U64: case S64: case F64: return intValue(be64(v));
            case Str: return textValue(str(be32(v)));
            case Data: {
                Value d = emptyBytes();
                d.dataOffset = be32(v);
                d.dataSize = be32(v + 4);
                return d;
            }
            default: return {};
        }
    };

    Table t;
    t.name = str(nameOff);
    struct ColInfo {
        uint8_t flags, type;
        Value constant;
        uint32_t rowOffset;
    };
    std::vector<ColInfo> info;
    uint64_t pos = 0x20;
    uint32_t rowPos = 0;
    for (uint16_t c = 0; c < ncols; ++c) {
        if (pos + 5 > rowsOff) corrupt("column list");
        uint8_t flags = p[pos] & 0xF0, type = p[pos] & 0x0F;
        if (!typeSize(type)) corrupt("column type");
        t.columns.push_back(str(be32(p + pos + 1)));
        t.types.push_back(type);
        pos += 5;
        ColInfo ci{flags, type, {}, 0};
        if (flags & kFlagDefault) {
            ci.constant = readValue(type, pos);
            pos += typeSize(type);
        }
        if (flags & kFlagRow) {
            ci.rowOffset = rowPos;
            rowPos += typeSize(type);
        }
        info.push_back(ci);
    }
    if (rowsOff + uint64_t(nrows) * rowWidth > stringsOff && nrows) corrupt("rows");
    t.rows.resize(nrows);
    for (uint32_t r = 0; r < nrows; ++r) {
        t.rows[r].resize(ncols);
        for (uint16_t c = 0; c < ncols; ++c) {
            const auto& ci = info[c];
            if (ci.flags & kFlagRow)
                t.rows[r][c] = readValue(ci.type, rowsOff + uint64_t(r) * rowWidth + ci.rowOffset);
            else if (ci.flags & kFlagDefault)
                t.rows[r][c] = ci.constant;
        }
    }
    return t;
}

// Columns marked constant are written once (CRI's "default" storage); the rest per row.
enum class Storage { Constant, PerRow, Data };

struct OutColumn {
    std::string name;
    uint8_t type;
    Storage storage = Storage::Data;
};

std::vector<uint8_t> buildTable(const std::string& name, const std::vector<OutColumn>& cols,
                                const std::vector<std::vector<Value>>& rows) {
    std::vector<uint8_t> strings, dataTable;
    std::map<std::string, uint32_t> stringAt;

    auto addString = [&](const std::string& value) -> uint32_t {
        auto it = stringAt.find(value);
        if (it != stringAt.end()) return it->second;
        uint32_t offset = uint32_t(strings.size());
        strings.insert(strings.end(), value.begin(), value.end());
        strings.push_back(0);
        stringAt.emplace(value, offset);
        return offset;
    };

    addString("<NULL>");
    uint32_t nameOff = addString(name);

    std::vector<uint32_t> colNames;
    for (const auto& c : cols) colNames.push_back(addString(c.name));

    for (size_t c = 0; c < cols.size(); ++c) {
        if (cols[c].type != Str) continue;
        if (cols[c].storage == Storage::PerRow && !rows.empty() && !rows[0][c].s.empty())
            addString(rows[0][c].s);
    }

    if (name == "CpkTocInfo" || name == "CpkEtocInfo") addString("");

    for (const auto& row : rows) {
        for (size_t c = 0; c < cols.size(); ++c) {
            if (cols[c].storage != Storage::Data || cols[c].type != Str) continue;
            if (!row[c].s.empty()) addString(row[c].s);
        }
    }

    auto writeValue = [&](std::vector<uint8_t>& out, uint8_t type, const Value& v) {
        size_t at = out.size();
        switch (type) {
            case U8: case S8:
                out.push_back(uint8_t(v.i));
                break;
            case U16: case S16:
                out.resize(at + 2);
                putBe16(out, at, uint16_t(v.i));
                break;
            case U32: case S32:
                out.resize(at + 4);
                putBe32(out, at, uint32_t(v.i));
                break;
            case U64: case S64:
                out.resize(at + 8);
                putBe64(out, at, v.i);
                break;
            case Str: {
                auto it = stringAt.find(v.s);
                uint32_t offset = it == stringAt.end() ? 0 : it->second;
                out.resize(at + 4);
                putBe32(out, at, offset);
                break;
            }
            case Data:
                out.resize(at + 8, 0);
                break;
            default:
                break;
        }
    };

    std::vector<uint8_t> columnData;
    uint16_t rowWidth = 0;

    for (size_t c = 0; c < cols.size(); ++c) {
        const auto& col = cols[c];
        uint8_t flag = kFlagName;
        if (col.storage == Storage::PerRow) flag |= kFlagDefault;
        else if (col.storage == Storage::Data) flag |= kFlagRow;

        columnData.push_back(uint8_t(flag | col.type));
        size_t at = columnData.size();
        columnData.resize(at + 4);
        putBe32(columnData, at, colNames[c]);

        if (col.storage == Storage::PerRow) {
            Value value = rows.empty() ? Value{} : rows[0][c];
            writeValue(columnData, col.type, value);
        } else if (col.storage == Storage::Data) {
            rowWidth += uint16_t(typeSize(col.type));
        }
    }

    std::vector<uint8_t> rowData;
    for (const auto& row : rows) {
        for (size_t c = 0; c < cols.size(); ++c) {
            if (cols[c].storage == Storage::Data)
                writeValue(rowData, cols[c].type, row[c]);
        }
    }

    if (name == "CpkHeader") strings.insert(strings.end(), 7, 0);
    else if (name == "CpkTocInfo") strings.insert(strings.end(), 4, 0);
    else if (name == "CpkEtocInfo") strings.insert(strings.end(), 6, 0);

    uint32_t rowsOff = uint32_t(columnData.size()) + 32;
    uint32_t stringsOff = rowsOff + uint32_t(rowData.size());
    uint32_t dataOff = stringsOff + uint32_t(strings.size());
    uint32_t tableSize = dataOff + uint32_t(dataTable.size());

    std::vector<uint8_t> out(32, 0);
    std::memcpy(out.data(), "@UTF", 4);
    putBe32(out, 4, tableSize - 8);
    putBe32(out, 8, rowsOff - 8);
    putBe32(out, 12, stringsOff - 8);
    putBe32(out, 16, dataOff - 8);
    putBe32(out, 20, nameOff);
    putBe16(out, 24, uint16_t(cols.size()));
    putBe16(out, 26, rowWidth);
    putBe32(out, 28, uint32_t(rows.size()));

    out.insert(out.end(), columnData.begin(), columnData.end());
    out.insert(out.end(), rowData.begin(), rowData.end());
    out.insert(out.end(), strings.begin(), strings.end());
    out.insert(out.end(), dataTable.begin(), dataTable.end());
    return out;
}

// "CPK " / "TOC " chunk: 16-byte header, then the table, padded to `padTo`.
std::vector<uint8_t> wrapChunk(const char* magic, const std::vector<uint8_t>& table, uint64_t padTo) {
    std::vector<uint8_t> chunk(0x10 + table.size(), 0);
    std::memcpy(chunk.data(), magic, 4);
    putLe32(chunk, 4, 0xFF);
    putLe32(chunk, 8, uint32_t(table.size()));
    std::memcpy(chunk.data() + 0x10, table.data(), table.size());
    chunk.resize(size_t(alignUp(chunk.size(), padTo)), 0);
    return chunk;
}

std::vector<uint8_t> readAt(std::ifstream& in, uint64_t offset, uint64_t size) {
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    in.seekg(std::streamoff(offset));
    if (!in.read(reinterpret_cast<char*>(buf.data()), std::streamsize(size))) corrupt("unexpected end of file");
    return buf;
}

Table readChunk(std::ifstream& in, uint64_t offset, const char* magic, uint64_t fileSize) {
    if (offset + 0x10 > fileSize) corrupt(std::string(magic) + " chunk offset");
    auto head = readAt(in, offset, 0x10);
    if (std::memcmp(head.data(), magic, 4) != 0) corrupt(std::string("no ") + magic + " chunk");
    uint64_t size = le32(head.data() + 8);
    if (size == 0 || offset + 0x10 + size > fileSize) size = std::min<uint64_t>(fileSize - offset - 0x10, 64ull << 20);
    return parseTable(readAt(in, offset + 0x10, size));
}

struct Prepared {
    std::string path, dir, name;
    uint32_t storedSize = 0, size = 0;
    const Source* src = nullptr;
    std::vector<uint8_t> compressedData;
};

std::vector<Prepared> prepare(const std::vector<Source>& sources, const BuildOptions& opts) {
    // Last source wins for each path.
    std::map<std::string, size_t> byKey;
    for (size_t i = 0; i < sources.size(); ++i) byKey[pathKey(sources[i].path)] = i;
    std::vector<Prepared> out;
    out.reserve(byKey.size());
    for (const auto& [key, i] : byKey) {
        const auto& s = sources[i];
        Prepared p;
        p.path = s.path;
        std::replace(p.path.begin(), p.path.end(), '\\', '/');
        while (!p.path.empty() && p.path.front() == '/') p.path.erase(0, 1);
        if (p.path.empty() || !isSafeRelativePath(p.path)) throw std::runtime_error("bad file path for a CPK: " + s.path);
        auto slash = p.path.rfind('/');
        p.dir = slash == std::string::npos ? std::string() : p.path.substr(0, slash);
        p.name = slash == std::string::npos ? p.path : p.path.substr(slash + 1);
        if (s.fromArchive()) {
            p.storedSize = s.entry.storedSize;
            p.size = s.entry.size;
        } else {
            std::error_code ec;
            auto sz = fs::file_size(s.file, ec);
            if (ec) throw std::runtime_error("can't read " + s.file.string());
            if (sz > 0xFFFFFFFFull) throw std::runtime_error(p.path + " is too big for a CPK (over 4 GB)");
            p.storedSize = p.size = uint32_t(sz);
            if (opts.compress && sz >= 256) {
                std::ifstream in(s.file, std::ios::binary);
                if (!in) throw std::runtime_error("can't open " + s.file.string());
                std::vector<uint8_t> raw(static_cast<size_t>(sz));
                if (!in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size())))
                    throw std::runtime_error("can't read " + s.file.string());
                auto packed = crilayla::compress(raw.data(), raw.size());
                if (packed.size() < raw.size()) {
                    p.storedSize = static_cast<uint32_t>(packed.size());
                    p.compressedData = std::move(packed);
                }
            }
        }
        p.src = &s;
        out.push_back(std::move(p));
    }
    std::sort(out.begin(), out.end(), [](const Prepared& a, const Prepared& b) {
        auto key = [](const std::string& path) {
            std::string result;
            result.reserve(path.size());
            for (unsigned char c : path) {
                if (c == '_') c = '~';
                result.push_back(char(std::tolower(c)));
            }
            return result;
        };
        return key(a.path) < key(b.path);
    });
    return out;
}

std::vector<uint8_t> tocTable(const std::vector<Prepared>& files, const std::vector<uint64_t>& offsets) {
    std::vector<OutColumn> cols = {
        {"DirName", Str}, {"FileName", Str}, {"FileSize", U32}, {"ExtractSize", U32},
        {"FileOffset", U64}, {"ID", U32}, {"UserString", Str, Storage::Constant},
    };
    std::vector<std::vector<Value>> rows;
    rows.reserve(files.size());
    for (size_t i = 0; i < files.size(); ++i)
        rows.push_back({textValue(files[i].dir), textValue(files[i].name), intValue(files[i].storedSize),
                        intValue(files[i].size), intValue(offsets[i]), intValue(i), textValue("<NULL>")});
    return buildTable("CpkTocInfo", cols, rows);
}

struct Layout {
    std::vector<uint8_t> tocChunk, etocChunk;
    uint64_t contentOffset = 0, contentSize = 0, fileSize = 0, etocOffset = 0;
    uint64_t tocSize = 0;
    std::vector<uint64_t> offsets;  // TOC FileOffset per file (relative to the TOC chunk)
};

Layout layout(const std::vector<Prepared>& files, uint16_t align) {
    Layout l;
    l.offsets.assign(files.size(), 0);
    // The table's size doesn't depend on the offsets (fixed-width column), so one sizing pass.
    auto tocData = tocTable(files, l.offsets);
    l.tocSize = 16 + tocData.size();
    l.tocChunk = wrapChunk("TOC ", tocData, kChunkAlign);
    l.contentOffset = kHeaderChunkSize + l.tocChunk.size();
    uint64_t at = l.contentOffset;
    for (size_t i = 0; i < files.size(); ++i) {
        l.offsets[i] = at - kHeaderChunkSize;
        at += alignUp(files[i].storedSize, align);
    }
    l.contentSize = at - l.contentOffset;

    // Match SwitchPatch's ETOC structure: one row per file plus a terminator.
    std::vector<OutColumn> etocCols = {
        {"UpdateDateTime", U64}, {"LocalDir", Str, Storage::PerRow},
    };
    std::vector<std::vector<Value>> etocRows;
    etocRows.reserve(files.size() + 1);
    for (size_t i = 0; i < files.size(); ++i)
        etocRows.push_back({intValue(569143605520244480ULL), textValue("")});
    etocRows.push_back({intValue(0), textValue("")});
    l.etocChunk = wrapChunk("ETOC", buildTable("CpkEtocInfo", etocCols, etocRows), 1);
    l.etocOffset = at;
    l.fileSize = at + l.etocChunk.size();

    l.tocChunk = wrapChunk("TOC ", tocTable(files, l.offsets), kChunkAlign);
    if (kHeaderChunkSize + l.tocChunk.size() != l.contentOffset) throw std::logic_error("CPK table size changed");
    return l;
}

std::vector<uint8_t> headerChunk(const std::vector<Prepared>& files, const Layout& l, const BuildOptions& o) {
    uint64_t packed = 0, data = 0;
    for (const auto& f : files) {
        packed += f.storedSize;
        data += f.size;
    }
    uint64_t enabledTotal = packed + data;

    // Match CPK-Toolkit's CRI-compatible 35-column CpkHeader.
    std::vector<OutColumn> cols = {
        {"UpdateDateTime", U64},
        {"FileSize", U64, Storage::Constant},
        {"ContentOffset", U64},
        {"ContentSize", U64},
        {"TocOffset", U64},
        {"TocSize", U64},
        {"TocCrc", U32, Storage::Constant},
        {"EtocOffset", U64},
        {"EtocSize", U64},
        {"ItocOffset", U64, Storage::Constant},
        {"ItocSize", U64, Storage::Constant},
        {"ItocCrc", U32, Storage::Constant},
        {"GtocOffset", U64, Storage::Constant},
        {"GtocSize", U64, Storage::Constant},
        {"GtocCrc", U32, Storage::Constant},
        {"EnabledPackedSize", U64},
        {"EnabledDataSize", U64},
        {"TotalDataSize", U64, Storage::Constant},
        {"Tocs", U32, Storage::Constant},
        {"Files", U32},
        {"Groups", U32},
        {"Attrs", U32},
        {"TotalFiles", U32, Storage::Constant},
        {"Directories", U32, Storage::Constant},
        {"Updates", U32, Storage::Constant},
        {"Version", U16},
        {"Revision", U16},
        {"Align", U16},
        {"Sorted", U16},
        {"EID", U16, Storage::Constant},
        {"CpkMode", U32},
        {"Tvers", Str},
        {"Comment", Str, Storage::Constant},
        {"Codec", U32},
        {"DpkItoc", U32},
    };

    std::vector<Value> row = {
        intValue(1),
        intValue(l.fileSize),
        intValue(l.contentOffset),
        intValue(l.contentSize),
        intValue(kHeaderChunkSize),
        intValue(l.tocSize),
        intValue(0),
        intValue(l.etocOffset),
        intValue(128),
        intValue(0),
        intValue(0),
        intValue(0),
        intValue(0),
        intValue(0),
        intValue(0),
        intValue(enabledTotal),
        intValue(enabledTotal),
        intValue(0),
        intValue(0),
        intValue(files.size()),
        intValue(0),
        intValue(0),
        intValue(0),
        intValue(0),
        intValue(0),
        intValue(7),
        intValue(1),
        intValue(o.align),
        intValue(1),
        intValue(0),
        intValue(1),
        textValue(o.tool),
        textValue(""),
        intValue(0),
        intValue(0),
    };

    auto chunk = wrapChunk("CPK ", buildTable("CpkHeader", cols, {row}), kChunkAlign);
    if (chunk.size() != kHeaderChunkSize) throw std::logic_error("CPK header too big");
    std::memcpy(chunk.data() + kHeaderChunkSize - 6, "(c)CRI", 6);
    return chunk;
}

void copyRange(std::ifstream& in, uint64_t offset, uint64_t size, std::ofstream& out, std::vector<char>& buf,
               const std::function<void(uint64_t)>& advanced) {
    in.seekg(std::streamoff(offset));
    while (size) {
        auto n = size_t(std::min<uint64_t>(size, buf.size()));
        if (!in.read(buf.data(), std::streamsize(n))) throw std::runtime_error("read error");
        if (!out.write(buf.data(), std::streamsize(n))) throw std::runtime_error("write error (is the SD card full?)");
        size -= n;
        if (advanced) advanced(n);
    }
}

}  // namespace

std::string Entry::path() const { return dir.empty() ? name : dir + "/" + name; }

int comparePaths(std::string_view a, std::string_view b) {
    auto norm = [](char c) { return c == '\\' ? '/' : char(std::tolower(static_cast<unsigned char>(c))); };
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        char x = norm(a[i]), y = norm(b[i]);
        if (x != y) return x < y ? -1 : 1;
    }
    return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}

std::string pathKey(std::string_view path) {
    std::string k(path);
    std::replace(k.begin(), k.end(), '\\', '/');
    while (!k.empty() && k.front() == '/') k.erase(0, 1);
    return toLower(k);
}


Info read(const fs::path& cpk) {
    cpk_toolkit::Archive archive(cpk.string());
    Info info;
    info.sorted = true;
    info.mode = static_cast<uint32_t>(archive.mode());
    info.align = archive.alignment();
    info.tool = archive.version();
    info.fileSize = archive.total_size();
    const auto& files = archive.files();
    info.entries.reserve(files.size());
    for (size_t i = 0; i < files.size(); ++i) {
        const auto& f = files[i];
        Entry e;
        e.dir = f.dir_name;
        e.name = f.file_name;
        e.offset = archive.content_offset() + f.offset;
        if (archive.cpk_header().has_column("TocOffset"))
            e.offset = 0x800ULL + f.offset;
        e.storedSize = f.file_size;
        e.size = f.extract_size;
        e.id = f.id;
        info.entries.push_back(std::move(e));
        if (i && comparePaths(info.entries[i-1].path(), info.entries[i].path()) > 0)
            info.sorted = false;
    }
    if (info.entries.size() < 2) info.sorted = true;
    return info;
}

bool isCrilayla(const uint8_t* data, size_t size) {
    return crilayla::is_compressed(data, size);
}

std::vector<uint8_t> decompressCrilayla(const uint8_t* data, size_t size) {
    return crilayla::decompress(data, size);
}

void extract(const fs::path& cpk, const Entry& e, const fs::path& dest) {
    cpk_toolkit::Archive archive(cpk.string());
    const auto* file = archive.find_file(e.path());
    if (!file) throw std::runtime_error("File not found: " + e.path());
    auto data = archive.extract_file(*file);
    if (!dest.parent_path().empty()) fs::create_directories(dest.parent_path());
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("can't write " + dest.string());
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!out) throw std::runtime_error("write error: " + dest.string());
}

std::vector<std::string> extractAll(const fs::path& cpk, const fs::path& destDir, const ProgressFn& progress) {
    auto info = read(cpk);
    std::vector<std::string> written;
    uint64_t total = 0, done = 0;
    for (const auto& e : info.entries) total += e.storedSize;
    for (const auto& e : info.entries) {
        const auto path = e.path();
        if (!isSafeRelativePath(path)) continue;
        extract(cpk, e, destDir / fs::path(path));
        written.push_back(path);
        done += e.storedSize;
        if (progress && !progress(done, total)) throw std::runtime_error("cancelled");
    }
    return written;
}

static cpk_toolkit::Builder makeToolkitBuilder(
    const std::vector<Source>& sources, const BuildOptions& opts) {
    cpk_toolkit::Builder builder(cpk_toolkit::CpkMode::FileNames);
    builder.set_alignment(opts.align);
    builder.set_version(opts.tool);
    builder.set_compression(opts.compress);

    std::map<std::string, std::unique_ptr<cpk_toolkit::Archive>> archives;
    for (const auto& source : sources) {
        if (!source.fromArchive()) {
            builder.add_file(source.file.string(), source.path);
            continue;
        }
        const std::string key = source.fromCpk.string();
        auto& archive = archives[key];
        if (!archive) archive = std::make_unique<cpk_toolkit::Archive>(key);
        const auto* entry = archive->find_file(source.entry.path());
        if (!entry) throw std::runtime_error("Source entry not found: " + source.entry.path());
        builder.add_file_raw_from_file(
            key, archive->raw_file_offset(*entry), entry->file_size,
            source.path, source.entry.size);
    }
    return builder;
}

uint64_t predictSize(std::vector<Source> sources, const BuildOptions& opts) {
    auto builder = makeToolkitBuilder(sources, opts);
    return builder.predict_size();
}

BuildResult build(const fs::path& out, std::vector<Source> sources,
                  const BuildOptions& opts, const ProgressFn& progress) {
    auto builder = makeToolkitBuilder(sources, opts);
    uint64_t totalBytes = 0;
    for (const auto& source : sources) {
        if (source.fromArchive()) totalBytes += source.entry.storedSize;
        else {
            std::error_code ec;
            totalBytes += fs::file_size(source.file, ec);
        }
    }
    // Ensure Android output directory exists before Toolkit opens the temporary CPK.
    if (!out.parent_path().empty()) {
        std::error_code dir_ec;
        fs::create_directories(out.parent_path(), dir_ec);
        if (dir_ec)
            throw std::runtime_error("Cannot create CPK output directory: " +
                                     out.parent_path().string() + ": " + dir_ec.message());
    }

    builder.build(out.string(), [&](const std::string&, size_t current, size_t total) {
        if (!progress) return;
        const uint64_t done = total ? totalBytes * current / total : totalBytes;
        if (!progress(done, totalBytes)) throw std::runtime_error("cancelled");
    });
    return {read(out).entries.size(), fs::file_size(out)};
}

}  // namespace mm::cpk
