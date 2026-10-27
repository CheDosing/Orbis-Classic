// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "common/endian.h"
#include "common/io_file.h"
#include "core/file_format/pkg_crypto.h"

struct PKGHeader {
    u32_be magic;
    u32_be pkg_type;
    u32_be pkg_0x8;
    u32_be pkg_file_count;
    u32_be pkg_table_entry_count;
    u16_be pkg_sc_entry_count;
    u16_be pkg_table_entry_count_2;
    u32_be pkg_table_entry_offset;
    u32_be pkg_sc_entry_data_size;
    u64_be pkg_body_offset;
    u64_be pkg_body_size;
    u64_be pkg_content_offset;
    u64_be pkg_content_size;
    u8 pkg_content_id[0x24];
    u8 pkg_padding[0xC];
    u32_be pkg_drm_type;
    u32_be pkg_content_type;
    u32_be pkg_content_flags;
    u32_be pkg_promote_size;
    u32_be pkg_version_date;
    u32_be pkg_version_hash;
    u32_be pkg_0x088;
    u32_be pkg_0x08C;
    u32_be pkg_0x090;
    u32_be pkg_0x094;
    u32_be pkg_iro_tag;
    u32_be pkg_drm_type_version;

    u8 pkg_zeroes_1[0x60];

    u8 digest_entries1[0x20];
    u8 digest_entries2[0x20];
    u8 digest_table_digest[0x20];
    u8 digest_body_digest[0x20];

    u8 pkg_zeroes_2[0x280];

    u32_be pkg_0x400;

    u32_be pfs_image_count;
    u64_be pfs_image_flags;
    u64_be pfs_image_offset;
    u64_be pfs_image_size;
    u64_be mount_image_offset;
    u64_be mount_image_size;
    u64_be pkg_size;
    u32_be pfs_signed_size;
    u32_be pfs_cache_size;
    u8 pfs_image_digest[0x20];
    u8 pfs_signed_digest[0x20];
    u64_be pfs_split_size_nth_0;
    u64_be pfs_split_size_nth_1;

    u8 pkg_zeroes_3[0xB50];

    u8 pkg_digest[0x20];
};
static_assert(sizeof(PKGHeader) == 0x1000);

enum class PKGContentFlag {
    FIRST_PATCH = 0x100000,
    PATCHGO = 0x200000,
    REMASTER = 0x400000,
    PS_CLOUD = 0x800000,
    GD_AC = 0x2000000,
    NON_GAME = 0x4000000,
    UNKNOWN_0x8000000 = 0x8000000,
    SUBSEQUENT_PATCH = 0x40000000,
    DELTA_PATCH = 0x41000000,
    CUMULATIVE_PATCH = 0x60000000
};

struct PKGEntry {
    u32_be id;
    u32_be filename_offset;
    u32_be flags1;
    u32_be flags2;
    u32_be offset;
    u32_be size;
    u64_be padding;

    bool IsEncrypted() const {
        return (flags1 & 0x80000000u) != 0;
    }

    u32 KeyIndex() const {
        return (flags2 & 0xF000u) >> 12;
    }
};
static_assert(sizeof(PKGEntry) == 32);

class PKG {
public:
    static constexpr u64 PfscBlockSize = 0x10000;

    struct File {
        std::filesystem::path path;
        u64 size;
        u32 first_block;
        u32 num_blocks;
    };

    class Reader {
    public:
        explicit Reader(const PKG& pkg);
        ~Reader();

        Reader(const Reader&) = delete;
        Reader& operator=(const Reader&) = delete;

        bool ReadImage(u64 image_offset, u64 size, std::vector<u8>& out);
        bool ReadBlocks(u32 first_block, u32 count, std::vector<u8>& out);

    private:
        bool Inflate(std::span<const u8> compressed, std::span<u8> decompressed);

        const PKG& pkg;
        Common::FS::IOFile file;
        PkgCrypto::XtsDecryptor xts;
        void* zstream{};
        std::vector<u8> encrypted;
        std::vector<u8> decrypted;
    };

    PKG();
    ~PKG();

    bool Open(const std::filesystem::path& filepath, std::string& failreason);
    bool Prepare(const std::filesystem::path& extract_path, std::string& failreason);
    bool VerifyEntryDigests(std::string& failreason) const;

    const std::vector<File>& GetFiles() const {
        return files;
    }

    u64 GetExtractedSize() const;

    u64 GetPkgSize() const {
        return pkgSize;
    }

    const std::string& GetPkgFlags() const {
        return pkgFlags;
    }

    std::string_view GetTitleID() const {
        return std::string_view(pkgTitleID.data(), pkgTitleID.size());
    }

    std::string_view GetContentID() const {
        return std::string_view(pkgContentID.data(), pkgContentID.size());
    }

    const PKGHeader& GetPkgHeader() const {
        return pkgheader;
    }

    const std::filesystem::path& GetPath() const {
        return pkgpath;
    }

    bool IsPatch() const {
        return pkgFlags.find("PATCH") != std::string::npos;
    }

    static bool isFlagSet(u32_be variable, PKGContentFlag flag) {
        return (variable) & static_cast<u32>(flag);
    }

    static constexpr std::array<std::pair<PKGContentFlag, std::string_view>, 10> flagNames = {
        {{PKGContentFlag::FIRST_PATCH, "FIRST_PATCH"},
         {PKGContentFlag::PATCHGO, "PATCHGO"},
         {PKGContentFlag::REMASTER, "REMASTER"},
         {PKGContentFlag::PS_CLOUD, "PS_CLOUD"},
         {PKGContentFlag::GD_AC, "GD_AC"},
         {PKGContentFlag::NON_GAME, "NON_GAME"},
         {PKGContentFlag::UNKNOWN_0x8000000, "UNKNOWN_0x8000000"},
         {PKGContentFlag::SUBSEQUENT_PATCH, "SUBSEQUENT_PATCH"},
         {PKGContentFlag::DELTA_PATCH, "DELTA_PATCH"},
         {PKGContentFlag::CUMULATIVE_PATCH, "CUMULATIVE_PATCH"}}};

    std::vector<u8> sfo;

private:
    bool ReadEntryTable(const Common::FS::IOFile& file, std::string& failreason);
    bool ExtractEntries(const Common::FS::IOFile& file, const std::filesystem::path& extract_path,
                        std::string& failreason);
    bool LocatePfsc(Reader& reader, std::string& failreason);
    bool ReadDirectoryTree(Reader& reader, const std::filesystem::path& extract_path,
                           std::string& failreason);

    u64 pkgSize = 0;
    std::array<char, 9> pkgTitleID{};
    std::array<char, 36> pkgContentID{};
    PKGHeader pkgheader{};
    std::string pkgFlags;
    std::filesystem::path pkgpath;

    std::vector<PKGEntry> entries;
    std::vector<File> files;
    std::vector<u64> sectorMap;
    u64 pfsc_offset = 0;

    std::array<u8, 32> dk3_{};
    std::array<u8, 32> ekpfsKey{};
    std::array<u8, 16> dataKey{};
    std::array<u8, 16> tweakKey{};
};
