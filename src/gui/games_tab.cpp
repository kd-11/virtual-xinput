// The Games tab: the game library, what is actually in each folder, and the
// four things you can do about it - add, install, verify, redeploy.
//
// The console configurator does the same work through a menu. Both go through
// vx::InstallGame / DeployConfig / AdoptConfig rather than each writing files
// their own way, so the two front-ends cannot drift apart in what they leave on
// disk.

#include "app.h"

#include "platform.h"

#include "../common/strutil.h"

#include <cstdio>
#include <cstring>

namespace vx {
namespace gui {

namespace {

// Long enough that a probe write into a game folder is rare, short enough that
// installing and then looking at the row shows the result.
const DWORD kStatusPollMs = 2500;

const ImVec4 kGood  (0.55f, 0.82f, 0.55f, 1.0f);
const ImVec4 kWarn  (0.95f, 0.78f, 0.42f, 1.0f);
const ImVec4 kBad   (0.95f, 0.55f, 0.45f, 1.0f);
const ImVec4 kQuiet (0.60f, 0.60f, 0.60f, 1.0f);

ImVec4 ConfigStateColour(ConfigState s) {
    switch (s) {
        case ConfigState::InSync:    return kGood;
        case ConfigState::Modified:  return kWarn;
        case ConfigState::OutOfDate: return kWarn;
        case ConfigState::Unknown:   return kQuiet;
        case ConfigState::Missing:   return kBad;
    }
    return kQuiet;
}

// One sentence saying what the state means and what to do about it. The state
// name alone is not much use: "modified" only helps if you also know that
// redeploying would throw the modification away.
const char* ConfigStateHelp(ConfigState s) {
    switch (s) {
        case ConfigState::InSync:
            return "The config in the game folder is exactly what the assigned profile says.";
        case ConfigState::Modified:
            return "The config was edited in the game folder after it was deployed. "
                   "Redeploy would overwrite those edits; Adopt keeps them by saving "
                   "them back into a profile.";
        case ConfigState::OutOfDate:
            return "Untouched in the game folder, but the profile it came from has "
                   "changed since. Redeploy brings the folder up to date.";
        case ConfigState::Unknown:
            return "There is a config here that this tool has no record of writing - "
                   "an older install, or one put there by hand. Adopt saves it as a "
                   "profile; Redeploy replaces it.";
        case ConfigState::Missing:
            return "No virtual-xinput.yml in the folder. The DLL will fall back to its "
                   "own defaults. Install, or Redeploy, writes one.";
    }
    return "";
}

void HelpTip(const char* text) {
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Library
// ---------------------------------------------------------------------------

void App::ReloadGames() {
    gamesError_.clear();
    if (profiles_.toolDir.empty()) {
        gamesError_ = "no tool folder, so there is nowhere to keep the game list";
        return;
    }

    gamesPath_ = GamesFilePath(profiles_.toolDir);
    if (!LoadGames(gamesPath_, games_, gamesError_)) games_.games.clear();

    if (gameSel_ >= (int)games_.games.size()) gameSel_ = (int)games_.games.size() - 1;
    RefreshGameStatus(true);
}

void App::SaveGameLibrary() {
    if (gamesPath_.empty()) return;
    std::string err;
    if (!SaveGames(gamesPath_, games_, err)) SetStatus(err);
}

void App::RefreshGameStatus(bool force) {
    const DWORD now = GetTickCount();
    if (!force && now - gameCheckTick_ < kStatusPollMs) return;
    gameCheckTick_ = now;

    gameStatus_.resize(games_.games.size());
    for (size_t i = 0; i < games_.games.size(); ++i) {
        gameStatus_[i] = CheckGame(profiles_.toolDir, profiles_, games_.games[i]);
    }
}

// ---------------------------------------------------------------------------
// Adding a game
// ---------------------------------------------------------------------------

void App::BeginAddGame(const std::wstring& folder) {
    addFolder_ = folder;
    addExes_   = ScanExecutables(folder);
    addExe_    = PreferredExe(addExes_);
    addError_.clear();
    addOpen_ = true;

    snprintf(addName_, sizeof(addName_), "%s", Narrow(LeafName(folder)).c_str());

    if (addExes_.empty()) {
        addError_ = "No .exe in that folder. Point at the folder the game actually "
                    "runs from, not the one above it.";
    }
}

void App::DrawAddGameSection() {
    if (!addOpen_) return;

    ImGui::SeparatorText("Add a game");

    ImGui::TextDisabled("%s", Narrow(addFolder_).c_str());
    ImGui::Spacing();

    if (!addError_.empty()) {
        ImGui::TextColored(kBad, "%s", addError_.c_str());
        ImGui::Spacing();
    }

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
    ImGui::InputText("Name", addName_, sizeof(addName_));

    if (!addExes_.empty()) {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);

        const int  cur   = (addExe_ >= 0 && addExe_ < (int)addExes_.size()) ? addExe_ : 0;
        const std::string curLabel = Narrow(addExes_[cur].name) + "  (" +
                                     PeArchName(addExes_[cur].arch) + ")";
        if (ImGui::BeginCombo("Executable", curLabel.c_str())) {
            for (int i = 0; i < (int)addExes_.size(); ++i) {
                const std::string label = Narrow(addExes_[i].name) + "  (" +
                                          PeArchName(addExes_[i].arch) + ")";
                if (ImGui::Selectable(label.c_str(), i == addExe_)) addExe_ = i;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        HelpTip("Only used to read the architecture out of the PE header, which decides "
                "whether the 32- or 64-bit DLL is installed. Installing the wrong one "
                "fails silently - the game simply ignores the DLL.");
    }

    ImGui::Spacing();
    if (ImGui::Button("Add")) {
        GameEntry g;
        g.name = addName_;

        const std::wstring exe =
            (addExe_ >= 0 && addExe_ < (int)addExes_.size()) ? addExes_[addExe_].name
                                                             : std::wstring();
        std::string err;
        if (!MakeGameEntry(addFolder_, exe, g, err)) {
            addError_ = err;
        } else {
            games_.games.push_back(g);
            gameSel_ = (int)games_.games.size() - 1;
            SaveGameLibrary();
            RefreshGameStatus(true);
            addOpen_ = false;
            SetStatus("added " + g.name + " (" + PeArchName(g.arch) + ")");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Choose a different folder")) {
        std::wstring picked;
        if (PickFolder(L"Where does the game run from?", addFolder_, picked)) {
            BeginAddGame(picked);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) addOpen_ = false;
}

// ---------------------------------------------------------------------------
// The list
// ---------------------------------------------------------------------------

void App::DrawGameList() {
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_ScrollY;

    // Room for four rows minimum, and never more than half the tab, so the
    // details panel underneath cannot be pushed off the bottom.
    const float rowH  = ImGui::GetTextLineHeightWithSpacing();
    float       tableH = rowH * (games_.games.size() + 1.5f);
    const float maxH   = ImGui::GetContentRegionAvail().y * 0.45f;
    if (tableH > maxH) tableH = maxH;

    if (!ImGui::BeginTable("##games", 5, flags, ImVec2(0.0f, tableH))) return;

    ImGui::TableSetupColumn("Game",    ImGuiTableColumnFlags_WidthStretch, 0.30f);
    ImGui::TableSetupColumn("Arch",    ImGuiTableColumnFlags_WidthStretch, 0.09f);
    ImGui::TableSetupColumn("DLLs",    ImGuiTableColumnFlags_WidthStretch, 0.16f);
    ImGui::TableSetupColumn("Config",  ImGuiTableColumnFlags_WidthStretch, 0.19f);
    ImGui::TableSetupColumn("Profile", ImGuiTableColumnFlags_WidthStretch, 0.26f);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    for (int i = 0; i < (int)games_.games.size(); ++i) {
        const GameEntry&  g  = games_.games[i];
        const GameStatus& st = gameStatus_[i];

        ImGui::PushID(i);
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        if (ImGui::Selectable(g.name.c_str(), i == gameSel_,
                              ImGuiSelectableFlags_SpanAllColumns)) {
            gameSel_ = i;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Narrow(g.folder).c_str());

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(PeArchName(g.arch));

        ImGui::TableNextColumn();
        if (!st.folderExists) {
            ImGui::TextColored(kBad, "folder gone");
        } else if (st.dllsExpected == 0) {
            ImGui::TextColored(kBad, "no payload");
        } else if (st.dllsPresent == 0) {
            ImGui::TextColored(kQuiet, "not installed");
        } else if (st.dllsPresent < st.dllsExpected) {
            ImGui::TextColored(kWarn, "%d of %d", st.dllsPresent, st.dllsExpected);
        } else {
            ImGui::TextColored(kGood, "installed");
        }

        ImGui::TableNextColumn();
        ImGui::TextColored(ConfigStateColour(st.config), "%s", ConfigStateName(st.config));

        ImGui::TableNextColumn();
        if (st.profileMissing) ImGui::TextColored(kBad, "%s (missing)", g.profile.c_str());
        else if (g.profile.empty()) ImGui::TextDisabled("%s", st.profileName.c_str());
        else ImGui::TextUnformatted(g.profile.c_str());

        ImGui::PopID();
    }
    ImGui::EndTable();
}

// ---------------------------------------------------------------------------
// The selected game
// ---------------------------------------------------------------------------

void App::DrawGameDetails(int index) {
    GameEntry&        g  = games_.games[index];
    const GameStatus& st = gameStatus_[index];

    ImGui::SeparatorText(g.name.c_str());

    ImGui::TextDisabled("%s", Narrow(g.folder).c_str());
    if (!g.exe.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("  -  %S", g.exe.c_str());
    }

    // Warnings first: each of these makes an install look like it worked and
    // change nothing, which is the worst failure this tool has.
    if (!st.folderExists) {
        ImGui::TextColored(kBad, "The folder no longer exists. Moved, or on a drive "
                                 "that is not mounted.");
    } else {
        if (st.looksPackaged) {
            ImGui::TextColored(kWarn, "This looks like a Store/UWP install. A DLL placed "
                                      "beside the executable is not loaded there, so "
                                      "installing will appear to work and do nothing.");
        }
        if (!st.folderWritable) {
            ImGui::TextColored(kWarn, "The folder is not writable. Run the configurator "
                                      "as administrator, or the files will not arrive.");
        }
        if (st.dllsExpected == 0) {
            ImGui::TextColored(kBad, "No %s payload beside the configurator - build or "
                                     "unpack the %s folder first.",
                               PeArchName(g.arch), PeArchName(g.arch));
        }
    }
    if (st.profileMissing) {
        ImGui::TextColored(kBad, "Assigned profile '%s' no longer exists; '%s' would be "
                                 "deployed instead.",
                           g.profile.c_str(), st.profileName.c_str());
    }

    ImGui::Spacing();

    // --- profile assignment ------------------------------------------------
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);

    // Unassigned shows the name of whatever the default currently is, rather
    // than the word "default" twice.
    const std::string preview = g.profile.empty() ? st.profileName : g.profile;

    if (ImGui::BeginCombo("Profile", preview.c_str())) {
        // The unassigned entry is not the same as picking the default by name:
        // it tracks whatever the default is, which is what you want for a game
        // you have no particular opinion about.
        if (ImGui::Selectable("Default (follow the default profile)", g.profile.empty())) {
            g.profile = "";
            SaveGameLibrary();
            RefreshGameStatus(true);
        }
        for (size_t i = 0; i < profiles_.profiles.size(); ++i) {
            const ProfileEntry& p = profiles_.profiles[i];
            if (p.isDefault) continue;   // covered by the entry above
            if (ImGui::Selectable(p.name.c_str(), g.profile == p.name)) {
                g.profile = p.name;
                SaveGameLibrary();
                RefreshGameStatus(true);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    HelpTip("The profile deployed into this folder as virtual-xinput.yml. Changing it "
            "here does not touch the folder - use Redeploy for that.");

    // --- config state ------------------------------------------------------
    ImGui::Spacing();
    ImGui::Text("Config:");
    ImGui::SameLine();
    ImGui::TextColored(ConfigStateColour(st.config), "%s", ConfigStateName(st.config));
    if (!st.configProfileName.empty() && st.config != ConfigState::InSync) {
        ImGui::SameLine();
        ImGui::TextDisabled("(the file says it came from '%s')", st.configProfileName.c_str());
    }

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", ConfigStateHelp(st.config));
    ImGui::PopTextWrapPos();

    // --- actions -----------------------------------------------------------
    ImGui::Spacing();

    const bool canTouch = st.folderExists;

    ImGui::BeginDisabled(!canTouch || st.dllsExpected == 0);
    if (ImGui::Button(st.installed ? "Reinstall" : "Install")) {
        std::string err;
        bool        wroteCfg = false;
        if (InstallGame(profiles_.toolDir, g, err, &wroteCfg)) {
            char buf[192];
            snprintf(buf, sizeof(buf), "installed %d file(s) into %s%s",
                     (int)g.installedFiles.size(), g.name.c_str(),
                     wroteCfg ? "; wrote virtual-xinput.yml"
                              : "; left the existing config alone");
            SetStatus(buf);
            SaveGameLibrary();
        } else {
            SetStatus(err);
        }
        RefreshGameStatus(true);
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!canTouch || st.dllsPresent == 0);
    if (ImGui::Button("Uninstall")) {
        std::string err;
        if (UninstallGame(profiles_.toolDir, g, err)) {
            SetStatus(err.empty() ? ("removed from " + g.name) : err);
            SaveGameLibrary();
        } else {
            SetStatus(err);
        }
        RefreshGameStatus(true);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    HelpTip("Removes the DLLs. virtual-xinput.yml is left in place, so a mapping you "
            "worked out survives uninstalling.");

    ImGui::SameLine();
    ImGui::BeginDisabled(!canTouch);
    if (ImGui::Button("Redeploy config")) {
        // Overwriting a file somebody edited is the one destructive thing on
        // this tab, so it is the one thing that asks first.
        if (st.config == ConfigState::Modified || st.config == ConfigState::Unknown) {
            confirm_     = Confirm::Redeploy;
            confirmGame_ = index;
        } else {
            std::string err;
            if (DeployConfig(profiles_.toolDir, g, true, err)) {
                SetStatus("wrote " + st.profileName + " into " + g.name);
                SaveGameLibrary();
            } else {
                SetStatus(err);
            }
            RefreshGameStatus(true);
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(st.config == ConfigState::Missing);
    if (ImGui::Button("Adopt as profile")) {
        confirm_     = Confirm::Adopt;
        confirmGame_ = index;
        snprintf(adoptName_, sizeof(adoptName_), "%s", g.name.c_str());
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    HelpTip("Saves the config that is in the game folder as a profile, exactly as it is - "
            "comments and all - and assigns this game to it. This is how a mapping you "
            "tuned by hand in one game becomes reusable in the next.");

    ImGui::Spacing();
    ImGui::BeginDisabled(!st.folderExists);
    if (ImGui::Button("Open folder")) RevealFolder(g.folder);
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Remove from list")) {
        if (st.dllsPresent > 0) {
            confirm_     = Confirm::Forget;
            confirmGame_ = index;
        } else {
            SetStatus("removed " + g.name + " from the list");
            games_.games.erase(games_.games.begin() + index);
            gameSel_ = -1;
            SaveGameLibrary();
            RefreshGameStatus(true);
        }
    }
}

// ---------------------------------------------------------------------------

void App::DrawGamesTab() {
    ImGui::Spacing();

    if (profiles_.toolDir.empty()) {
        ImGui::TextDisabled("No tool folder, so there is nowhere to keep a game list.");
        return;
    }
    if (!gamesError_.empty()) {
        ImGui::TextColored(kBad, "%s", gamesError_.c_str());
        ImGui::Spacing();
    }

    RefreshGameStatus(false);

    if (ImGui::Button("Add game folder...")) {
        std::wstring picked;
        if (PickFolder(L"Where does the game run from?", std::wstring(), picked)) {
            BeginAddGame(picked);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Re-check")) {
        ReloadProfiles();
        ReloadGames();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::TextDisabled("%d game%s in %s", (int)games_.games.size(),
                        games_.games.size() == 1 ? "" : "s", Narrow(gamesPath_).c_str());

    ImGui::Spacing();

    if (games_.games.empty() && !addOpen_) {
        ImGui::TextDisabled("Nothing here yet. Add the folder a game runs from - the one "
                            "its .exe lives in.");
    } else {
        DrawGameList();
    }

    DrawAddGameSection();

    if (gameSel_ >= 0 && gameSel_ < (int)games_.games.size() &&
        gameSel_ < (int)gameStatus_.size()) {
        ImGui::Spacing();
        DrawGameDetails(gameSel_);
    }

    // --- confirmations -----------------------------------------------------
    //
    // Opened here rather than at the click site: BeginPopupModal has to be
    // reached every frame the popup is up, and the click site sits inside
    // branches that come and go.
    if (confirm_ == Confirm::Redeploy) ImGui::OpenPopup("Overwrite the config?");
    if (confirm_ == Confirm::Forget)   ImGui::OpenPopup("Remove from the list?");
    if (confirm_ == Confirm::Adopt)    ImGui::OpenPopup("Save as a profile");

    const bool haveTarget = confirmGame_ >= 0 && confirmGame_ < (int)games_.games.size();

    if (ImGui::BeginPopupModal("Overwrite the config?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(
            "The config in this game's folder is not the one this tool put there. "
            "Redeploying replaces it with the assigned profile, and whatever is in it "
            "now is gone - no copy of it exists anywhere else.\n\n"
            "Adopt as profile saves it first, if you would rather keep it.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();

        if (ImGui::Button("Overwrite") && haveTarget) {
            GameEntry&  g = games_.games[confirmGame_];
            std::string err;
            if (DeployConfig(profiles_.toolDir, g, true, err)) {
                SetStatus("overwrote the config in " + g.name);
                SaveGameLibrary();
            } else {
                SetStatus(err);
            }
            RefreshGameStatus(true);
            confirm_ = Confirm::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            confirm_ = Confirm::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Remove from the list?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(
            "The wrapper is still installed in this folder. Removing the game from the "
            "list forgets which files are ours, which makes a later uninstall more "
            "cautious than it needs to be. Uninstall first if you can.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();

        if (ImGui::Button("Remove anyway") && haveTarget) {
            SetStatus("removed " + games_.games[confirmGame_].name + " from the list");
            games_.games.erase(games_.games.begin() + confirmGame_);
            gameSel_ = -1;
            SaveGameLibrary();
            RefreshGameStatus(true);
            confirm_ = Confirm::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            confirm_ = Confirm::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Save as a profile", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(
            "Stores the config from the game folder under this name, exactly as it is, "
            "and assigns the game to it.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();

        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20.0f);
        ImGui::InputText("Profile name", adoptName_, sizeof(adoptName_));

        const bool clash = profiles_.FindByName(adoptName_) >= 0;
        if (clash) ImGui::TextColored(kWarn, "'%s' exists and will be replaced.", adoptName_);

        ImGui::Spacing();
        if (ImGui::Button("Save") && haveTarget) {
            GameEntry&  g = games_.games[confirmGame_];
            std::string err;
            if (AdoptConfig(profiles_.toolDir, g, adoptName_, err)) {
                ReloadProfiles();
                SaveGameLibrary();
                SetStatus("saved profile '" + std::string(adoptName_) + "' from " + g.name);
                confirm_ = Confirm::None;
                ImGui::CloseCurrentPopup();
            } else {
                SetStatus(err);
            }
            RefreshGameStatus(true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            confirm_ = Confirm::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace gui
} // namespace vx
