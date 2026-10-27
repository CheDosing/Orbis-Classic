// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <ImGuiFileDialog.h>
#include <fmt/format.h>
#include <imgui.h>

#include "core/emulator_settings.h"
#include "imgui/big_picture/pkg_install_dialog.h"

namespace BigPictureMode {

namespace {

constexpr const char* PickerKey = "InstallPkg";
constexpr const char* ConfirmPopup = "Install PKG";
constexpr const char* ProgressPopup = "Installing PKG";
constexpr const char* ResultsPopup = "PKG Install Results";

std::filesystem::path DefaultInstallDir() {
    for (const auto& dir : EmulatorSettings.GetAllGameInstallDirs()) {
        if (dir.enabled) {
            return dir.path;
        }
    }
    return {};
}

void CenterNextWindow() {
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
}

} // namespace

PkgInstallDialog::~PkgInstallDialog() {
    progress.cancel = true;
}

PkgInstaller::Options PkgInstallDialog::MakeOptions() const {
    PkgInstaller::Options options;
    options.install_dir = DefaultInstallDir();
    options.addon_dir = EmulatorSettings.GetAddonInstallDir();
    options.verify = verify;
    return options;
}

void PkgInstallDialog::OpenFilePicker() {
    if (state != State::Closed) {
        return;
    }
    ImGuiFileDialog::Instance()->OpenDialog(PickerKey, "Select PKG files to install", ".pkg,.PKG",
                                            ".", 0, nullptr,
                                            ImGuiFileDialogFlags_DisableCreateDirectoryButton |
                                                ImGuiFileDialogFlags_DontShowHiddenFiles);
    state = State::Picking;
}

void PkgInstallDialog::Queue(const std::vector<std::filesystem::path>& paths) {
    queue.clear();
    const auto options = MakeOptions();
    for (const auto& path : paths) {
        QueueItem item;
        item.path = path;
        if (!PkgInstaller::Inspect(path, options, item.plan, item.error)) {
            item.message = item.error;
        }
        queue.push_back(std::move(item));
    }
    current = 0;
    state = State::Confirming;
}

void PkgInstallDialog::StartNext() {
    while (current < queue.size() && !queue[current].error.empty()) {
        current++;
    }
    if (current >= queue.size() || progress.cancel) {
        state = State::Finished;
        return;
    }
    progress.Reset();
    rate.Reset();
    last_stage = PkgInstaller::Stage::Idle;
    worker = std::jthread([this, path = queue[current].path, options = MakeOptions()] {
        result = PkgInstaller::Install(path, options, progress);
    });
    state = State::Installing;
}

void PkgInstallDialog::Draw(float ui_scale, bool& games_changed) {
    switch (state) {
    case State::Closed:
        return;
    case State::Picking:
        DrawPicker(ui_scale);
        return;
    case State::Confirming:
        DrawConfirm(ui_scale);
        return;
    case State::Installing:
        DrawProgress(ui_scale);
        return;
    case State::Finished:
        DrawResults(ui_scale, games_changed);
        return;
    }
}

void PkgInstallDialog::DrawPicker(float ui_scale) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    if (!ImGuiFileDialog::Instance()->Display(PickerKey, ImGuiWindowFlags_NoMove)) {
        return;
    }
    std::vector<std::filesystem::path> paths;
    if (ImGuiFileDialog::Instance()->IsOk()) {
        for (const auto& [name, path] : ImGuiFileDialog::Instance()->GetSelection()) {
            paths.emplace_back(std::u8string(path.begin(), path.end()));
        }
    }
    ImGuiFileDialog::Instance()->Close();
    if (paths.empty()) {
        state = State::Closed;
        return;
    }
    Queue(paths);
}

void PkgInstallDialog::DrawConfirm(float ui_scale) {
    if (!ImGui::IsPopupOpen(ConfirmPopup)) {
        ImGui::OpenPopup(ConfirmPopup);
    }
    CenterNextWindow();
    if (!ImGui::BeginPopupModal(ConfirmPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    size_t installable = 0;
    for (const auto& item : queue) {
        ImGui::TextUnformatted(item.path.filename().string().c_str());
        ImGui::Indent();
        if (!item.error.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", item.error.c_str());
        } else {
            installable++;
            const auto& plan = item.plan;
            ImGui::Text("%s %s %s  %s", std::string{PkgInstaller::KindName(plan.kind)}.c_str(),
                        plan.title_id.c_str(), plan.version.c_str(), plan.title.c_str());
            ImGui::TextDisabled("%s", plan.destination.string().c_str());
            if (plan.kind == PkgInstaller::ContentKind::Update && !plan.base_game_found) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                                   "Base game not found, the update will be installed alone");
            }
            if (plan.already_installed) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                                   "Already installed (%s), files will be overwritten",
                                   plan.installed_version.empty() ? "unknown version"
                                                                  : plan.installed_version.c_str());
            }
        }
        ImGui::Unindent();
    }

    ImGui::Separator();
    ImGui::Checkbox("Verify after install", &verify);
    ImGui::Separator();

    ImGui::BeginDisabled(installable == 0);
    if (ImGui::Button("Install", ImVec2(160 * ui_scale, 0))) {
        ImGui::CloseCurrentPopup();
        progress.cancel = false;
        StartNext();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(160 * ui_scale, 0))) {
        ImGui::CloseCurrentPopup();
        queue.clear();
        state = State::Closed;
    }
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetItemDefaultFocus();
    }
    ImGui::EndPopup();
}

void PkgInstallDialog::DrawProgress(float ui_scale) {
    if (progress.stage == PkgInstaller::Stage::Done && worker.joinable()) {
        worker.join();
        queue[current].ok = result.ok;
        queue[current].message = result.message;
        current++;
        StartNext();
        return;
    }

    if (!ImGui::IsPopupOpen(ProgressPopup)) {
        ImGui::OpenPopup(ProgressPopup);
    }
    CenterNextWindow();
    ImGui::SetNextWindowSize(ImVec2(700 * ui_scale, 0));
    if (!ImGui::BeginPopupModal(ProgressPopup, nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        return;
    }

    const auto& item = queue[current];
    ImGui::Text("[%zu/%zu] %s", current + 1, queue.size(),
                item.plan.title.empty() ? item.path.filename().string().c_str()
                                        : item.plan.title.c_str());

    const auto stage = progress.stage.load();
    if (stage != last_stage) {
        last_stage = stage;
        rate.Reset();
    }
    const u64 done = progress.done_bytes;
    const u64 total = progress.total_bytes;
    rate.Update(done);

    const float fraction = total > 0 ? static_cast<float>(static_cast<double>(done) / total) : 0.f;
    ImGui::TextUnformatted(std::string{PkgInstaller::StageName(stage)}.c_str());
    ImGui::ProgressBar(fraction, ImVec2(-1, 0));
    if (total > 0) {
        const auto details = fmt::format("{} / {}   {}/s   ETA {}", PkgInstaller::FormatBytes(done),
                                         PkgInstaller::FormatBytes(total),
                                         PkgInstaller::FormatBytes(rate.BytesPerSecond()),
                                         PkgInstaller::FormatDuration(rate.Eta(done, total)));
        ImGui::TextUnformatted(details.c_str());
    }

    ImGui::BeginDisabled(progress.cancel);
    if (ImGui::Button("Cancel", ImVec2(160 * ui_scale, 0))) {
        progress.cancel = true;
    }
    ImGui::EndDisabled();
    ImGui::EndPopup();
}

void PkgInstallDialog::DrawResults(float ui_scale, bool& games_changed) {
    if (!ImGui::IsPopupOpen(ResultsPopup)) {
        ImGui::OpenPopup(ResultsPopup);
    }
    CenterNextWindow();
    if (!ImGui::BeginPopupModal(ResultsPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    for (const auto& item : queue) {
        const ImVec4 color =
            item.ok ? ImVec4(0.5f, 1.0f, 0.5f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
        ImGui::TextUnformatted(item.path.filename().string().c_str());
        ImGui::Indent();
        ImGui::TextColored(color, "%s", item.message.empty() ? "Skipped" : item.message.c_str());
        ImGui::Unindent();
    }
    ImGui::Separator();
    if (ImGui::Button("OK", ImVec2(160 * ui_scale, 0))) {
        ImGui::CloseCurrentPopup();
        games_changed = std::ranges::any_of(queue, [](const auto& item) { return item.ok; });
        queue.clear();
        state = State::Closed;
    }
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetItemDefaultFocus();
    }
    ImGui::EndPopup();
}

} // namespace BigPictureMode
