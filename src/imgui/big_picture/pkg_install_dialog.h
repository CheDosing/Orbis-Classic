// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#include "core/pkg_installer.h"

namespace BigPictureMode {

class PkgInstallDialog {
public:
    ~PkgInstallDialog();

    void OpenFilePicker();
    void Draw(float ui_scale, bool& games_changed);

    bool IsBusy() const {
        return worker.joinable();
    }

private:
    enum class State {
        Closed,
        Picking,
        Confirming,
        Installing,
        Finished,
    };

    struct QueueItem {
        std::filesystem::path path;
        PkgInstaller::Plan plan;
        std::string error;
        bool ok = false;
        std::string message;
    };

    void Queue(const std::vector<std::filesystem::path>& paths);
    void StartNext();
    void DrawPicker(float ui_scale);
    void DrawConfirm(float ui_scale);
    void DrawProgress(float ui_scale);
    void DrawResults(float ui_scale, bool& games_changed);
    PkgInstaller::Options MakeOptions() const;

    State state = State::Closed;
    std::vector<QueueItem> queue;
    size_t current = 0;
    bool verify = false;
    PkgInstaller::Progress progress;
    PkgInstaller::RateEstimator rate;
    PkgInstaller::Stage last_stage = PkgInstaller::Stage::Idle;
    PkgInstaller::Result result;
    std::jthread worker;
};

} // namespace BigPictureMode
