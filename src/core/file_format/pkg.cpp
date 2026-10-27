// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <fmt/format.h>
#include <zlib.h>
#include "core/file_format/pfs.h"
#include "core/file_format/pkg.h"
#include "core/file_format/pkg_type.h"

namespace {

constexpr u32 PkgMagic = 0x7F434E54;
constexpr u32 PfscMagic = 0x43534650;
constexpr u32 EntryKeysId = 0x10;
constexpr u32 ImageKeyId = 0x20;
constexpr u32 DigestsId = 0x1;
constexpr u32 DirentHeaderSize = 16;
constexpr u32 DinodeSize = 0xA8;
constexpr u32 DinodesPerBlock = PKG::PfscBlockSize / DinodeSize;

struct DirentHeader {
    s32 ino;
    s32 type;
    s32 namelen;
    s32 entsize;
};
static_assert(sizeof(DirentHeader) == DirentHeaderSize);

u64 AlignDown(u64 value, u64 alignment) {
    return value & ~(alignment - 1);
}

u64 AlignUp(u64 value, u64 alignment) {
    return AlignDown(value + alignment - 1, alignment);
}

bool IsSafeName(std::string_view name) {
    return !name.empty() && name != "." && name != ".." &&
           name.find_first_of("/\\:") == std::string_view::npos;
}

bool ReadAt(const Common::FS::IOFile& file, u64 offset, void* data, u64 size) {
    return file.Seek(static_cast<s64>(offset)) && file.ReadRaw<u8>(data, size) == size;
}

} // namespace

PKG::Reader::Reader(const PKG& pkg_)
    : pkg(pkg_), file(pkg_.pkgpath, Common::FS::FileAccessMode::Read,
                      Common::FS::FileType::BinaryFile, Common::FS::FileShareFlag::ShareReadWrite),
      xts(pkg_.dataKey, pkg_.tweakKey) {
    auto* stream = new z_stream{};
    if (inflateInit(stream) != Z_OK) {
        delete stream;
        return;
    }
    zstream = stream;
}

PKG::Reader::~Reader() {
    if (auto* stream = static_cast<z_stream*>(zstream)) {
        inflateEnd(stream);
        delete stream;
    }
}

bool PKG::Reader::ReadImage(u64 image_offset, u64 size, std::vector<u8>& out) {
    const u64 image_size = pkg.pkgheader.pfs_image_size;
    if (!file.IsOpen() || !xts.IsValid() || image_offset + size > image_size) {
        return false;
    }
    const u64 start = AlignDown(image_offset, PkgCrypto::XtsSectorSize);
    const u64 end = std::min(AlignUp(image_offset + size, PkgCrypto::XtsSectorSize), image_size);
    const u64 span = AlignUp(end - start, PkgCrypto::XtsSectorSize);

    encrypted.resize(span);
    decrypted.resize(span);
    std::fill(encrypted.begin() + (end - start), encrypted.end(), u8{0});
    if (!ReadAt(file, pkg.pkgheader.pfs_image_offset + start, encrypted.data(), end - start)) {
        return false;
    }
    if (!xts.Decrypt(encrypted, decrypted, start / PkgCrypto::XtsSectorSize)) {
        return false;
    }
    out.resize(size);
    std::memcpy(out.data(), decrypted.data() + (image_offset - start), size);
    return true;
}

bool PKG::Reader::Inflate(std::span<const u8> compressed, std::span<u8> decompressed) {
    auto* stream = static_cast<z_stream*>(zstream);
    if (stream == nullptr || inflateReset(stream) != Z_OK) {
        return false;
    }
    stream->next_in = const_cast<Bytef*>(compressed.data());
    stream->avail_in = static_cast<uInt>(compressed.size());
    stream->next_out = decompressed.data();
    stream->avail_out = static_cast<uInt>(decompressed.size());
    if (inflate(stream, Z_FINISH) != Z_STREAM_END) {
        return false;
    }
    std::fill(decompressed.end() - stream->avail_out, decompressed.end(), u8{0});
    return true;
}

bool PKG::Reader::ReadBlocks(u32 first_block, u32 count, std::vector<u8>& out) {
    const auto& map = pkg.sectorMap;
    if (count == 0 || static_cast<u64>(first_block) + count >= map.size()) {
        return false;
    }
    const u64 begin = map[first_block];
    const u64 end = map[first_block + count];

    std::vector<u8> compressed;
    if (!ReadImage(pkg.pfsc_offset + begin, end - begin, compressed)) {
        return false;
    }

    out.resize(static_cast<u64>(count) * PfscBlockSize);
    for (u32 i = 0; i < count; i++) {
        const u64 offset = map[first_block + i] - begin;
        const u64 size = map[first_block + i + 1] - map[first_block + i];
        const std::span<u8> dst(out.data() + static_cast<u64>(i) * PfscBlockSize, PfscBlockSize);
        if (size == PfscBlockSize) {
            std::memcpy(dst.data(), compressed.data() + offset, PfscBlockSize);
        } else if (size == 0) {
            std::fill(dst.begin(), dst.end(), u8{0});
        } else if (size > PfscBlockSize ||
                   !Inflate(std::span(compressed.data() + offset, size), dst)) {
            return false;
        }
    }
    return true;
}

PKG::PKG() = default;

PKG::~PKG() = default;

bool PKG::Open(const std::filesystem::path& filepath, std::string& failreason) {
    Common::FS::IOFile file(filepath, Common::FS::FileAccessMode::Read,
                            Common::FS::FileType::BinaryFile,
                            Common::FS::FileShareFlag::ShareReadWrite);
    if (!file.IsOpen()) {
        failreason = "Failed to open PKG file";
        return false;
    }
    pkgpath = filepath;
    pkgSize = file.GetSize();
    if (pkgSize < sizeof(PKGHeader) || !ReadAt(file, 0, &pkgheader, sizeof(PKGHeader))) {
        failreason = "File is too small to be a PKG";
        return false;
    }
    if (pkgheader.magic != PkgMagic) {
        failreason = "Not a PS4 PKG file";
        return false;
    }

    pkgFlags.clear();
    for (const auto& flag : flagNames) {
        if (isFlagSet(pkgheader.pkg_content_flags, flag.first)) {
            if (!pkgFlags.empty()) {
                pkgFlags += ", ";
            }
            pkgFlags += flag.second;
        }
    }

    std::memcpy(pkgContentID.data(), pkgheader.pkg_content_id, pkgContentID.size());
    std::memcpy(pkgTitleID.data(), pkgheader.pkg_content_id + 7, pkgTitleID.size());

    if (!ReadEntryTable(file, failreason)) {
        return false;
    }

    sfo.clear();
    for (const auto& entry : entries) {
        if (GetEntryNameByType(entry.id) != "param.sfo") {
            continue;
        }
        sfo.resize(entry.size);
        if (!ReadAt(file, entry.offset, sfo.data(), sfo.size())) {
            failreason = "Failed to read param.sfo";
            return false;
        }
    }
    return true;
}

bool PKG::ReadEntryTable(const Common::FS::IOFile& file, std::string& failreason) {
    const u64 offset = pkgheader.pkg_table_entry_offset;
    const u64 count = pkgheader.pkg_table_entry_count;
    if (offset + count * sizeof(PKGEntry) > pkgSize) {
        failreason = "PKG entry table is out of bounds";
        return false;
    }
    entries.resize(count);
    if (!ReadAt(file, offset, entries.data(), count * sizeof(PKGEntry))) {
        failreason = "Failed to read PKG entry table";
        return false;
    }
    for (const auto& entry : entries) {
        if (static_cast<u64>(entry.offset) + entry.size > pkgSize) {
            failreason = fmt::format("PKG entry {:#x} is out of bounds", u32{entry.id});
            return false;
        }
    }
    return true;
}

bool PKG::Prepare(const std::filesystem::path& extract_path, std::string& failreason) {
    Common::FS::IOFile file(pkgpath, Common::FS::FileAccessMode::Read,
                            Common::FS::FileType::BinaryFile,
                            Common::FS::FileShareFlag::ShareReadWrite);
    if (!file.IsOpen()) {
        failreason = "Failed to open PKG file";
        return false;
    }
    if (pkgheader.pkg_size > pkgSize) {
        failreason = "PKG file is truncated";
        return false;
    }
    if (pkgheader.pkg_content_offset + pkgheader.pkg_content_size > pkgheader.pkg_size) {
        failreason = "Content size is bigger than pkg size";
        return false;
    }
    if (pkgheader.pfs_image_offset + pkgheader.pfs_image_size > pkgSize) {
        failreason = "PFS image is out of bounds";
        return false;
    }

    const auto find_entry = [this](u32 id) -> const PKGEntry* {
        const auto it = std::ranges::find_if(entries, [id](const auto& e) { return e.id == id; });
        return it == entries.end() ? nullptr : &*it;
    };
    const PKGEntry* entry_keys = find_entry(EntryKeysId);
    const PKGEntry* image_key = find_entry(ImageKeyId);
    if (entry_keys == nullptr || image_key == nullptr) {
        failreason = "PKG has no key entries";
        return false;
    }

    std::array<u8, 256> key3{};
    const u64 key3_offset = u64{entry_keys->offset} + 32 + 7 * 32 + 3 * key3.size();
    if (!ReadAt(file, key3_offset, key3.data(), key3.size()) ||
        !PkgCrypto::RsaDecrypt(PkgCrypto::RsaKeyset::PkgDerivedKey3, key3, dk3_)) {
        failreason = "Failed to decrypt PKG entry key";
        return false;
    }

    std::array<u8, 256> image_key_data{};
    std::array<u8, 64> iv_input{};
    std::array<u8, 256> image_key_plain{};
    std::memcpy(iv_input.data(), image_key, sizeof(PKGEntry));
    std::memcpy(iv_input.data() + sizeof(PKGEntry), dk3_.data(), dk3_.size());
    const auto iv_key = PkgCrypto::Sha256(iv_input);
    if (!ReadAt(file, image_key->offset, image_key_data.data(), image_key_data.size()) ||
        !PkgCrypto::AesCbcDecrypt(iv_key, image_key_data, image_key_plain) ||
        !PkgCrypto::RsaDecrypt(PkgCrypto::RsaKeyset::Fake, image_key_plain, ekpfsKey)) {
        failreason = "Failed to decrypt PFS image key. Only fake-signed (fPKG) packages "
                     "can be installed";
        return false;
    }

    std::array<u8, 16> seed{};
    if (!ReadAt(file, pkgheader.pfs_image_offset + 0x370, seed.data(), seed.size())) {
        failreason = "Failed to read PFS seed";
        return false;
    }
    PkgCrypto::PfsGenCryptoKey(ekpfsKey, seed, dataKey, tweakKey);

    std::error_code ec;
    std::filesystem::create_directories(extract_path, ec);
    if (ec) {
        failreason = fmt::format("Failed to create install folder: {}", ec.message());
        return false;
    }
    if (!ExtractEntries(file, extract_path, failreason)) {
        return false;
    }
    file.Close();

    Reader reader(*this);
    return LocatePfsc(reader, failreason) && ReadDirectoryTree(reader, extract_path, failreason);
}

bool PKG::ExtractEntries(const Common::FS::IOFile& file, const std::filesystem::path& extract_path,
                         std::string& failreason) {
    std::vector<u8> data;
    std::vector<u8> plain;
    for (const auto& entry : entries) {
        const auto name = GetEntryNameByType(entry.id);
        const auto out_path = extract_path / "sce_sys" /
                              (name.empty() ? std::to_string(u32{entry.id}) : std::string{name});
        std::error_code ec;
        std::filesystem::create_directories(out_path.parent_path(), ec);

        data.resize(entry.size);
        if (!ReadAt(file, entry.offset, data.data(), data.size())) {
            failreason = fmt::format("Failed to read PKG entry {:#x}", u32{entry.id});
            return false;
        }

        if (entry.IsEncrypted() && entry.KeyIndex() == 3) {
            std::array<u8, 64> iv_input{};
            std::memcpy(iv_input.data(), &entry, sizeof(PKGEntry));
            std::memcpy(iv_input.data() + sizeof(PKGEntry), dk3_.data(), dk3_.size());
            plain.resize(data.size());
            if (PkgCrypto::AesCbcDecrypt(PkgCrypto::Sha256(iv_input), data, plain)) {
                data.swap(plain);
            }
        }

        Common::FS::IOFile out(out_path, Common::FS::FileAccessMode::Create);
        if (!out.IsOpen() || out.WriteRaw<u8>(data.data(), data.size()) != data.size()) {
            failreason = fmt::format("Failed to write {}", out_path.string());
            return false;
        }
    }
    return true;
}

bool PKG::LocatePfsc(Reader& reader, std::string& failreason) {
    const u64 image_size = pkgheader.pfs_image_size;
    std::vector<u8> sector;
    bool found = false;
    for (u64 offset = 0x20000; offset + sizeof(u32) <= image_size; offset += 0x10000) {
        if (!reader.ReadImage(offset, sizeof(u32), sector)) {
            break;
        }
        u32 magic{};
        std::memcpy(&magic, sector.data(), sizeof(magic));
        if (magic == PfscMagic) {
            pfsc_offset = offset;
            found = true;
            break;
        }
    }
    if (!found) {
        failreason = "PFSC image not found. The PKG is corrupt or not a fake-signed (fPKG) "
                     "package";
        return false;
    }

    std::vector<u8> raw;
    PFSCHdr header{};
    if (!reader.ReadImage(pfsc_offset, sizeof(header), raw)) {
        failreason = "Failed to read PFSC header";
        return false;
    }
    std::memcpy(&header, raw.data(), sizeof(header));
    if (header.block_sz2 != static_cast<s64>(PfscBlockSize) || header.data_length <= 0) {
        failreason = "Unsupported PFSC block layout";
        return false;
    }

    const u64 num_blocks = AlignUp(header.data_length, PfscBlockSize) / PfscBlockSize;
    if (!reader.ReadImage(pfsc_offset + header.block_offsets, (num_blocks + 1) * sizeof(u64),
                          raw)) {
        failreason = "Failed to read PFSC block table";
        return false;
    }
    sectorMap.resize(num_blocks + 1);
    std::memcpy(sectorMap.data(), raw.data(), raw.size());

    for (u64 i = 0; i < num_blocks; i++) {
        if (sectorMap[i + 1] < sectorMap[i] ||
            pfsc_offset + sectorMap[i + 1] > pkgheader.pfs_image_size) {
            failreason = "PFSC block table is corrupt";
            return false;
        }
    }
    return true;
}

bool PKG::ReadDirectoryTree(Reader& reader, const std::filesystem::path& extract_path,
                            std::string& failreason) {
    const u64 num_blocks = sectorMap.size() - 1;
    std::vector<u8> block;
    if (!reader.ReadBlocks(0, 1, block)) {
        failreason = "Failed to read PFS superblock";
        return false;
    }
    u64 dinode_count{};
    u64 dinode_blocks{};
    std::memcpy(&dinode_count, block.data() + 0x30, sizeof(dinode_count));
    std::memcpy(&dinode_blocks, block.data() + 0x40, sizeof(dinode_blocks));
    const u64 needed_blocks = (dinode_count + DinodesPerBlock - 1) / DinodesPerBlock;
    if (dinode_count == 0 || dinode_blocks < needed_blocks || 1 + dinode_blocks > num_blocks) {
        failreason = "PFS inode table is corrupt";
        return false;
    }
    if (!reader.ReadBlocks(1, static_cast<u32>(dinode_blocks), block)) {
        failreason = "Failed to read PFS inode table";
        return false;
    }
    std::vector<Inode> inodes(dinode_count);
    for (u64 i = 0; i < dinode_count; i++) {
        const u64 offset =
            (i / DinodesPerBlock) * PfscBlockSize + (i % DinodesPerBlock) * DinodeSize;
        std::memcpy(&inodes[i], block.data() + offset, sizeof(Inode));
    }

    struct Entry {
        u32 ino;
        s32 type;
        std::string name;
    };
    const auto read_dir = [&](u32 ino, std::vector<Entry>& out) -> bool {
        out.clear();
        if (ino >= inodes.size()) {
            failreason = fmt::format("Directory inode {} is out of range", ino);
            return false;
        }
        const Inode& node = inodes[ino];
        if (node.Blocks == 0 || static_cast<u64>(node.loc) + node.Blocks > num_blocks) {
            failreason = fmt::format("Directory inode {} is corrupt", ino);
            return false;
        }
        if (!reader.ReadBlocks(node.loc, node.Blocks, block)) {
            failreason = fmt::format("Failed to read directory inode {}", ino);
            return false;
        }
        for (u64 base = 0; base < block.size(); base += PfscBlockSize) {
            for (u64 offset = 0; offset + DirentHeaderSize <= PfscBlockSize;) {
                DirentHeader header{};
                std::memcpy(&header, block.data() + base + offset, sizeof(header));
                if (header.entsize == 0) {
                    break;
                }
                if (header.entsize < static_cast<s32>(DirentHeaderSize) || header.namelen < 0 ||
                    DirentHeaderSize + header.namelen > static_cast<u32>(header.entsize) ||
                    offset + header.entsize > PfscBlockSize) {
                    failreason = fmt::format("Corrupt directory entry in inode {}", ino);
                    return false;
                }
                const auto* name =
                    reinterpret_cast<const char*>(block.data()) + base + offset + DirentHeaderSize;
                out.push_back(Entry{static_cast<u32>(header.ino), header.type,
                                    std::string(name, header.namelen)});
                offset += header.entsize;
            }
        }
        return true;
    };

    std::vector<Entry> entries_buf;
    if (!read_dir(0, entries_buf)) {
        return false;
    }
    const auto uroot = std::ranges::find_if(
        entries_buf, [](const Entry& e) { return e.type == PFS_DIR && e.name == "uroot"; });
    if (uroot == entries_buf.end()) {
        failreason = "PFS image has no uroot directory";
        return false;
    }

    files.clear();
    std::vector<bool> visited(inodes.size(), false);
    std::vector<std::pair<u32, std::filesystem::path>> pending{{uroot->ino, extract_path}};
    while (!pending.empty()) {
        auto [dir_ino, dir_path] = std::move(pending.back());
        pending.pop_back();
        if (dir_ino >= visited.size() || visited[dir_ino]) {
            failreason = "PFS directory tree is corrupt";
            return false;
        }
        visited[dir_ino] = true;
        if (!read_dir(dir_ino, entries_buf)) {
            return false;
        }
        for (const auto& entry : entries_buf) {
            if (entry.type != PFS_FILE && entry.type != PFS_DIR) {
                continue;
            }
            if (!IsSafeName(entry.name)) {
                failreason = fmt::format("Unsafe file name in PKG: {}", entry.name);
                return false;
            }
            auto path = dir_path / std::filesystem::path(std::u8string(
                                       reinterpret_cast<const char8_t*>(entry.name.data()),
                                       entry.name.size()));
            if (entry.type == PFS_DIR) {
                std::error_code ec;
                std::filesystem::create_directories(path, ec);
                if (ec) {
                    failreason =
                        fmt::format("Failed to create {}: {}", path.string(), ec.message());
                    return false;
                }
                pending.emplace_back(entry.ino, std::move(path));
                continue;
            }
            if (entry.ino >= inodes.size()) {
                failreason = fmt::format("File inode {} is out of range", entry.ino);
                return false;
            }
            const Inode& node = inodes[entry.ino];
            const u64 size = static_cast<u64>(node.Size);
            if (node.Size < 0 || static_cast<u64>(node.Blocks) * PfscBlockSize < size ||
                static_cast<u64>(node.loc) + node.Blocks > num_blocks) {
                failreason = fmt::format("File {} has an invalid block range", path.string());
                return false;
            }
            files.push_back(File{std::move(path), size, node.loc, node.Blocks});
        }
    }
    return true;
}

u64 PKG::GetExtractedSize() const {
    u64 total = 0;
    for (const auto& file : files) {
        total += file.size;
    }
    return total;
}

bool PKG::VerifyEntryDigests(std::string& failreason) const {
    const auto digests =
        std::ranges::find_if(entries, [](const auto& e) { return e.id == DigestsId; });
    if (digests == entries.end()) {
        return true;
    }
    Common::FS::IOFile file(pkgpath, Common::FS::FileAccessMode::Read,
                            Common::FS::FileType::BinaryFile,
                            Common::FS::FileShareFlag::ShareReadWrite);
    std::vector<u8> table(digests->size);
    if (!file.IsOpen() || !ReadAt(file, digests->offset, table.data(), table.size())) {
        failreason = "Failed to read PKG digest table";
        return false;
    }
    std::vector<u8> data;
    for (size_t i = 1; i < entries.size() && (i + 1) * 32 <= table.size(); i++) {
        const auto& entry = entries[i];
        data.resize(entry.size);
        if (!ReadAt(file, entry.offset, data.data(), data.size())) {
            failreason = fmt::format("Failed to read PKG entry {:#x}", u32{entry.id});
            return false;
        }
        if (std::memcmp(PkgCrypto::Sha256(data).data(), table.data() + i * 32, 32) != 0) {
            const auto name = GetEntryNameByType(entry.id);
            failreason =
                fmt::format("PKG entry {} is corrupt",
                            name.empty() ? fmt::format("{:#x}", u32{entry.id}) : std::string{name});
            return false;
        }
    }
    return true;
}
