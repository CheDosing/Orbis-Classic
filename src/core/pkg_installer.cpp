// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>
#include <fmt/format.h>
#include <xxhash.h>

#include "common/io_file.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/string_util.h"
#include "common/thread.h"
#include "core/file_format/pkg.h"
#include "core/file_format/pkg_crypto.h"
#include "core/file_format/psf.h"
#include "core/file_sys/fs.h"
#include "core/pkg_installer.h"

namespace PkgInstaller {

namespace {

constexpr u32 ChunkBlocks = 64;
constexpr u64 HashReadSize = 4_MB;

struct Chunk {
    u32 file_index;
    u32 first_block;
    u32 num_blocks;
    u64 file_offset;
    u64 size;
};

class ErrorSlot {
public:
    void Set(std::string message) {
        std::scoped_lock lock{mutex};
        if (!failed) {
            failed = true;
            error = std::move(message);
        }
    }
    bool Failed() const {
        return failed.load(std::memory_order_relaxed);
    }
    std::string Get() {
        std::scoped_lock lock{mutex};
        return error;
    }

private:
    std::mutex mutex;
    std::atomic<bool> failed{false};
    std::string error;
};

u32 WorkerCount(const Options& options, size_t work_items) {
    u32 count = options.threads;
    if (count == 0) {
        count = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
    }
    return static_cast<u32>(std::clamp<size_t>(work_items, 1, count));
}

std::string ReadAppVer(const std::filesystem::path& game_dir) {
    PSF psf;
    if (psf.Open(game_dir / "sce_sys" / "param.sfo")) {
        if (const auto ver = psf.GetString("APP_VER"); ver.has_value()) {
            return std::string{*ver};
        }
    }
    return {};
}

std::filesystem::path GameRootFromBootPath(const std::filesystem::path& boot_path) {
    return Core::FileSys::IsZArchiveFile(boot_path) ? boot_path : boot_path.parent_path();
}

bool BuildPlan(const PKG& pkg, const Options& options, Plan& plan, std::string& failreason) {
    PSF psf;
    if (!psf.Open(pkg.sfo)) {
        failreason = "Could not read the PKG's param.sfo";
        return false;
    }

    plan = Plan{};
    plan.title_id = std::string{pkg.GetTitleID()};
    plan.content_id = std::string{pkg.GetContentID()};
    if (const auto title = psf.GetString("TITLE"); title.has_value()) {
        plan.title = std::string{*title};
    }
    if (const auto ver = psf.GetString("APP_VER"); ver.has_value()) {
        plan.version = std::string{*ver};
    } else if (const auto ver = psf.GetString("VERSION"); ver.has_value()) {
        plan.version = std::string{*ver};
    }

    const auto category = psf.GetString("CATEGORY").value_or("");
    if (category.starts_with("ac")) {
        plan.kind = ContentKind::Addon;
    } else if (category.starts_with("gp") || pkg.IsPatch()) {
        plan.kind = ContentKind::Update;
    } else {
        plan.kind = ContentKind::Game;
    }

    if (plan.kind == ContentKind::Addon) {
        if (options.addon_dir.empty()) {
            failreason = "No DLC folder is set. Set one with --set-addon-folder";
            return false;
        }
        const auto parts = Common::SplitString(plan.content_id, '-');
        if (parts.size() < 3 || parts[2].empty()) {
            failreason = "PKG content ID has no entitlement label";
            return false;
        }
        plan.destination = options.addon_dir / plan.title_id / parts[2];
        plan.already_installed = std::filesystem::exists(plan.destination);
        return true;
    }

    if (options.install_dir.empty()) {
        failreason = "No game folder is set. Add one with --add-game-folder or pass --install-dir";
        return false;
    }

    const auto base_boot = Common::FS::FindGameByID(options.install_dir, plan.title_id, 5);
    if (plan.kind == ContentKind::Game) {
        if (base_boot.has_value() && Core::FileSys::IsZArchiveFile(*base_boot)) {
            failreason = fmt::format("{} is already installed as an archive: {}", plan.title_id,
                                     base_boot->string());
            return false;
        }
        plan.destination =
            base_boot ? base_boot->parent_path() : options.install_dir / plan.title_id;
        plan.already_installed = base_boot.has_value();
        if (plan.already_installed) {
            plan.installed_version = ReadAppVer(plan.destination);
        }
        return true;
    }

    plan.base_game_found = base_boot.has_value();
    const auto base_root =
        base_boot ? GameRootFromBootPath(*base_boot) : options.install_dir / plan.title_id;
    plan.destination = base_root.parent_path() / (plan.title_id + "-UPDATE");
    plan.already_installed = std::filesystem::exists(plan.destination / "sce_sys" / "param.sfo");
    if (plan.already_installed) {
        plan.installed_version = ReadAppVer(plan.destination);
    } else if (base_boot && !Core::FileSys::IsZArchiveFile(*base_boot)) {
        plan.installed_version = ReadAppVer(base_root);
    }
    return true;
}

std::vector<Chunk> BuildChunks(const std::vector<PKG::File>& files) {
    std::vector<Chunk> chunks;
    for (u32 index = 0; index < files.size(); index++) {
        const auto& file = files[index];
        for (u32 block = 0; block < file.num_blocks; block += ChunkBlocks) {
            const u32 count = std::min(ChunkBlocks, file.num_blocks - block);
            const u64 offset = static_cast<u64>(block) * PKG::PfscBlockSize;
            if (offset >= file.size) {
                break;
            }
            const u64 size =
                std::min<u64>(static_cast<u64>(count) * PKG::PfscBlockSize, file.size - offset);
            chunks.push_back(Chunk{index, file.first_block + block, count, offset, size});
        }
    }
    std::ranges::stable_sort(chunks, [&](const Chunk& a, const Chunk& b) {
        return files[a.file_index].size > files[b.file_index].size;
    });
    return chunks;
}

bool PreallocateFiles(const std::vector<PKG::File>& files, std::string& failreason) {
    for (const auto& file : files) {
        Common::FS::IOFile out(file.path, Common::FS::FileAccessMode::Create);
        if (!out.IsOpen() || (file.size > 0 && !out.SetSize(file.size))) {
            failreason = fmt::format("Failed to create {}", file.path.string());
            return false;
        }
    }
    return true;
}

class OutputCache {
public:
    const Common::FS::IOFile* Get(const std::vector<PKG::File>& files, u32 index,
                                  Common::FS::FileAccessMode mode) {
        if (current != index || !handle.IsOpen()) {
            handle.Close();
            handle.Open(files[index].path, mode, Common::FS::FileType::BinaryFile,
                        Common::FS::FileShareFlag::ShareReadWrite);
            current = index;
        }
        return handle.IsOpen() ? &handle : nullptr;
    }

private:
    Common::FS::IOFile handle;
    u32 current = ~0u;
};

void ExtractWorker(const PKG& pkg, const std::vector<Chunk>& chunks, std::atomic<size_t>& next,
                   std::vector<u64>* hashes, Progress& progress, ErrorSlot& error) {
    Common::SetCurrentThreadName("shadPS4:PkgInstall");
    PKG::Reader reader(pkg);
    OutputCache outputs;
    std::vector<u8> data;
    const auto& files = pkg.GetFiles();

    while (!progress.cancel && !error.Failed()) {
        const size_t index = next.fetch_add(1, std::memory_order_relaxed);
        if (index >= chunks.size()) {
            return;
        }
        const Chunk& chunk = chunks[index];
        if (!reader.ReadBlocks(chunk.first_block, chunk.num_blocks, data)) {
            error.Set(fmt::format("Failed to decrypt or decompress {}",
                                  files[chunk.file_index].path.string()));
            return;
        }
        const auto* out =
            outputs.Get(files, chunk.file_index, Common::FS::FileAccessMode::ReadWrite);
        if (out == nullptr || !out->Seek(static_cast<s64>(chunk.file_offset)) ||
            out->WriteRaw<u8>(data.data(), chunk.size) != chunk.size) {
            error.Set(fmt::format("Failed to write {}", files[chunk.file_index].path.string()));
            return;
        }
        if (hashes != nullptr) {
            (*hashes)[index] = XXH3_64bits(data.data(), chunk.size);
        }
        progress.done_bytes.fetch_add(chunk.size, std::memory_order_relaxed);
    }
}

void VerifyWorker(const PKG& pkg, const std::vector<Chunk>& chunks, std::atomic<size_t>& next,
                  const std::vector<u64>& hashes, Progress& progress, ErrorSlot& error) {
    Common::SetCurrentThreadName("shadPS4:PkgVerify");
    OutputCache outputs;
    std::vector<u8> data;
    const auto& files = pkg.GetFiles();

    while (!progress.cancel && !error.Failed()) {
        const size_t index = next.fetch_add(1, std::memory_order_relaxed);
        if (index >= chunks.size()) {
            return;
        }
        const Chunk& chunk = chunks[index];
        const auto* in = outputs.Get(files, chunk.file_index, Common::FS::FileAccessMode::Read);
        data.resize(chunk.size);
        if (in == nullptr || !in->Seek(static_cast<s64>(chunk.file_offset)) ||
            in->ReadRaw<u8>(data.data(), chunk.size) != chunk.size ||
            XXH3_64bits(data.data(), chunk.size) != hashes[index]) {
            error.Set(fmt::format("Installed file does not match the PKG: {}",
                                  files[chunk.file_index].path.string()));
            return;
        }
        progress.done_bytes.fetch_add(chunk.size, std::memory_order_relaxed);
    }
}

void HashImageWorker(const PKG& pkg, Progress& progress, ErrorSlot& error) {
    Common::SetCurrentThreadName("shadPS4:PkgHash");
    const auto& header = pkg.GetPkgHeader();
    Common::FS::IOFile file(pkg.GetPath(), Common::FS::FileAccessMode::Read,
                            Common::FS::FileType::BinaryFile,
                            Common::FS::FileShareFlag::ShareReadWrite);
    if (!file.IsOpen() || !file.Seek(static_cast<s64>(u64{header.pfs_image_offset}))) {
        error.Set("Failed to read the PKG for verification");
        return;
    }
    PkgCrypto::Sha256Stream sha;
    std::vector<u8> buffer(HashReadSize);
    u64 remaining = header.pfs_image_size;
    while (remaining > 0 && !progress.cancel && !error.Failed()) {
        const u64 size = std::min<u64>(remaining, buffer.size());
        if (file.ReadRaw<u8>(buffer.data(), size) != size) {
            error.Set("Failed to read the PKG for verification");
            return;
        }
        sha.Update(std::span(buffer.data(), size));
        remaining -= size;
        progress.done_bytes.fetch_add(size, std::memory_order_relaxed);
    }
    if (remaining == 0 && sha.Finish() != std::to_array(header.pfs_image_digest)) {
        error.Set("PKG is corrupt: the game image checksum does not match");
    }
}

bool HasDigest(const u8 (&digest)[0x20]) {
    return std::ranges::any_of(digest, [](u8 b) { return b != 0; });
}

void RemoveIfNew(const std::filesystem::path& path, bool existed) {
    if (existed) {
        return;
    }
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

} // namespace

void RateEstimator::Reset() {
    started = false;
    rate = 0.0;
    last_bytes = 0;
}

void RateEstimator::Update(u64 done_bytes) {
    const auto now = Clock::now();
    if (!started || done_bytes < last_bytes) {
        started = true;
        last_time = now;
        last_bytes = done_bytes;
        rate = 0.0;
        return;
    }
    const double elapsed = std::chrono::duration<double>(now - last_time).count();
    if (elapsed < 0.25) {
        return;
    }
    const double instant = static_cast<double>(done_bytes - last_bytes) / elapsed;
    rate = rate == 0.0 ? instant : rate * 0.7 + instant * 0.3;
    last_time = now;
    last_bytes = done_bytes;
}

std::chrono::seconds RateEstimator::Eta(u64 done_bytes, u64 total_bytes) const {
    if (rate <= 0.0 || done_bytes >= total_bytes) {
        return std::chrono::seconds{0};
    }
    return std::chrono::seconds{static_cast<s64>((total_bytes - done_bytes) / rate)};
}

bool Inspect(const std::filesystem::path& pkg_path, const Options& options, Plan& plan,
             std::string& failreason) {
    PKG pkg;
    return pkg.Open(pkg_path, failreason) && BuildPlan(pkg, options, plan, failreason);
}

Result Install(const std::filesystem::path& pkg_path, const Options& options, Progress& progress) {
    Result result;
    const auto start = std::chrono::steady_clock::now();
    const auto finish = [&](bool ok, std::string message) {
        result.ok = ok;
        result.cancelled = progress.cancel;
        result.message = std::move(message);
        result.seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        progress.stage = Stage::Done;
        if (ok) {
            LOG_INFO(Loader, "PKG install: {}", result.message);
        } else {
            LOG_ERROR(Loader, "PKG install failed: {}", result.message);
        }
        return result;
    };

    progress.stage = Stage::Preparing;
    progress.done_bytes = 0;
    progress.total_bytes = 0;

    std::string failreason;
    PKG pkg;
    if (!pkg.Open(pkg_path, failreason) || !BuildPlan(pkg, options, result.plan, failreason)) {
        return finish(false, failreason);
    }
    const Plan& plan = result.plan;
    LOG_INFO(Loader, "Installing {} {} ({}) to {}", KindName(plan.kind), plan.title_id,
             plan.version, plan.destination.string());

    const bool existed = std::filesystem::exists(plan.destination);
    if (!pkg.Prepare(plan.destination, failreason)) {
        RemoveIfNew(plan.destination, existed);
        return finish(false, failreason);
    }

    const auto& files = pkg.GetFiles();
    if (!PreallocateFiles(files, failreason)) {
        RemoveIfNew(plan.destination, existed);
        return finish(false, failreason);
    }

    const auto chunks = BuildChunks(files);
    std::vector<u64> hashes(options.verify ? chunks.size() : 0);
    ErrorSlot error;

    progress.total_bytes = pkg.GetExtractedSize();
    progress.stage = Stage::Extracting;
    {
        std::atomic<size_t> next{0};
        std::vector<std::jthread> workers;
        const u32 count = WorkerCount(options, chunks.size());
        for (u32 i = 0; i < count; i++) {
            workers.emplace_back(ExtractWorker, std::cref(pkg), std::cref(chunks), std::ref(next),
                                 options.verify ? &hashes : nullptr, std::ref(progress),
                                 std::ref(error));
        }
    }
    result.bytes_written = progress.done_bytes;

    if (!error.Failed() && !progress.cancel && options.verify) {
        progress.stage = Stage::Verifying;
        progress.done_bytes = 0;
        const bool hash_image = HasDigest(pkg.GetPkgHeader().pfs_image_digest);
        progress.total_bytes =
            pkg.GetExtractedSize() + (hash_image ? u64{pkg.GetPkgHeader().pfs_image_size} : 0);

        if (!pkg.VerifyEntryDigests(failreason)) {
            error.Set(failreason);
        } else {
            std::atomic<size_t> next{0};
            std::vector<std::jthread> workers;
            if (hash_image) {
                workers.emplace_back(HashImageWorker, std::cref(pkg), std::ref(progress),
                                     std::ref(error));
            } else {
                LOG_WARNING(Loader, "PKG has no image checksum, verifying installed files only");
            }
            const u32 count =
                std::max(1u, WorkerCount(options, chunks.size()) - (hash_image ? 1u : 0u));
            for (u32 i = 0; i < count; i++) {
                workers.emplace_back(VerifyWorker, std::cref(pkg), std::cref(chunks),
                                     std::ref(next), std::cref(hashes), std::ref(progress),
                                     std::ref(error));
            }
        }
    }

    if (progress.cancel || error.Failed()) {
        RemoveIfNew(plan.destination, existed);
        auto message = progress.cancel ? std::string{"Installation cancelled"} : error.Get();
        if (existed) {
            message += fmt::format(". The existing install in {} may be incomplete",
                                   plan.destination.string());
        }
        return finish(false, std::move(message));
    }

    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return finish(true,
                  fmt::format("Installed {} {} {} to {} ({} in {}{})", KindName(plan.kind),
                              plan.title_id, plan.version, plan.destination.string(),
                              FormatBytes(result.bytes_written),
                              FormatDuration(std::chrono::seconds{static_cast<s64>(seconds + 0.5)}),
                              options.verify ? ", verified" : ""));
}

std::string_view KindName(ContentKind kind) {
    switch (kind) {
    case ContentKind::Game:
        return "game";
    case ContentKind::Update:
        return "update";
    case ContentKind::Addon:
        return "DLC";
    }
    return "content";
}

std::string_view StageName(Stage stage) {
    switch (stage) {
    case Stage::Idle:
        return "Waiting";
    case Stage::Preparing:
        return "Reading package";
    case Stage::Extracting:
        return "Installing";
    case Stage::Verifying:
        return "Verifying";
    case Stage::Done:
        return "Done";
    }
    return "";
}

std::string FormatBytes(double bytes) {
    static constexpr std::array<std::string_view, 5> units{"B", "KB", "MB", "GB", "TB"};
    size_t unit = 0;
    while (bytes >= 1024.0 && unit + 1 < units.size()) {
        bytes /= 1024.0;
        unit++;
    }
    return unit == 0 ? fmt::format("{:.0f} {}", bytes, units[unit])
                     : fmt::format("{:.1f} {}", bytes, units[unit]);
}

std::string FormatDuration(std::chrono::seconds seconds) {
    const s64 total = seconds.count();
    if (total >= 3600) {
        return fmt::format("{}h {:02}m", total / 3600, (total % 3600) / 60);
    }
    if (total >= 60) {
        return fmt::format("{}m {:02}s", total / 60, total % 60);
    }
    return fmt::format("{}s", total);
}

} // namespace PkgInstaller
