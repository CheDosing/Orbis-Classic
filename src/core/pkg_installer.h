// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include "common/types.h"

namespace PkgInstaller {

enum class ContentKind {
    Game,
    Update,
    Addon,
};

enum class Stage {
    Idle,
    Preparing,
    Extracting,
    Verifying,
    Done,
};

struct Options {
    std::filesystem::path install_dir;
    std::filesystem::path addon_dir;
    bool verify = false;
    u32 threads = 0;
};

struct Plan {
    ContentKind kind = ContentKind::Game;
    std::string title_id;
    std::string content_id;
    std::string title;
    std::string version;
    std::string installed_version;
    std::filesystem::path destination;
    bool already_installed = false;
    bool base_game_found = true;
};

struct Progress {
    std::atomic<Stage> stage{Stage::Idle};
    std::atomic<u64> done_bytes{0};
    std::atomic<u64> total_bytes{0};
    std::atomic<bool> cancel{false};

    void Reset() {
        stage = Stage::Idle;
        done_bytes = 0;
        total_bytes = 0;
        cancel = false;
    }
};

struct Result {
    bool ok = false;
    bool cancelled = false;
    std::string message;
    Plan plan;
    u64 bytes_written = 0;
    double seconds = 0.0;
};

class RateEstimator {
public:
    void Reset();
    void Update(u64 done_bytes);
    double BytesPerSecond() const {
        return rate;
    }
    std::chrono::seconds Eta(u64 done_bytes, u64 total_bytes) const;

private:
    using Clock = std::chrono::steady_clock;
    Clock::time_point last_time{};
    u64 last_bytes = 0;
    double rate = 0.0;
    bool started = false;
};

bool Inspect(const std::filesystem::path& pkg_path, const Options& options, Plan& plan,
             std::string& failreason);
Result Install(const std::filesystem::path& pkg_path, const Options& options, Progress& progress);

std::string_view KindName(ContentKind kind);
std::string_view StageName(Stage stage);
std::string FormatBytes(double bytes);
std::string FormatDuration(std::chrono::seconds seconds);

} // namespace PkgInstaller
