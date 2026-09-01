#pragma once

#include "../common/detect.h"
#include "../common/di_device.h"
#include "../common/games.h"
#include "../common/profiles.h"

#include "draw_pad.h"

#include <string>
#include <vector>

namespace vx {
namespace gui {

// One queued "press the thing you want here" step.
struct CaptureTarget {
    PadControl kind  = PadControl::None;
    int        index = -1;
};

class App {
public:
    App();
    ~App();

    bool Init(std::string& err);
    void Shutdown();

    // One frame: poll the open device, advance any capture in progress, draw.
    void Frame();

private:
    void PollDevice();
    void AdvanceCapture();
    void RefreshDevices();
    void SelectDevice(int index);
    void CloseDevice();

    void QueueCapture(PadControl kind, int index);
    void QueueAll();
    void ClearBinding(PadControl kind, int index);
    void CancelCapture();
    void SkipCapture();

    void DrawDeviceBar();
    void DrawPadTab();
    void DrawRawTab();
    void DrawDeviceTab();
    void DrawProfilesTab();
    void DrawGamesTab();
    void DrawCaptureBanner();
    void DrawMappingTable();
    void DrawDeadzones();
    void DrawSaveBar();

    const char* CurrentPrompt() const;

    // The current mapping as a whole config, ready to be saved as a profile or
    // written into a game folder.
    Config CurrentConfig() const;

    void ReloadProfiles();
    void LoadProfileIntoEditor(int index);
    void SaveCurrentAs(const std::string& name);
    void SetStatus(const std::string& text);

    DiSystem                di_;
    std::vector<DeviceInfo> devices_;
    int                     selected_;
    DiDevice*               dev_;

    RawState      raw_;
    bool          polled_;
    DeviceProfile profile_;
    PadView       view_;

    InputDetector              detector_;
    std::vector<CaptureTarget> queue_;

    // Devices are re-enumerated periodically so a pad plugged in while the
    // window is open turns up without the user hunting for a refresh button.
    DWORD lastEnumTick_;

    ProfileStore profiles_;
    int          activeProfile_;     // index in profiles_, or -1 for unsaved
    char         nameBuf_[64];       // the name field on the Pad tab's save bar
    std::string  profileError_;      // sticky: a broken store needs to stay visible

    // --- games ------------------------------------------------------------
    void ReloadGames();
    void SaveGameLibrary();

    // Re-reads what is actually in each game folder. Only called while the
    // Games tab is on screen, and rate-limited, because it touches the disk -
    // including a write probe, which is not something to do sixty times a
    // second to somebody's game directory.
    void RefreshGameStatus(bool force);

    void DrawGameList();
    void DrawGameDetails(int index);
    void DrawAddGameSection();
    void BeginAddGame(const std::wstring& folder);

    GameLibrary             games_;
    std::wstring            gamesPath_;
    std::vector<GameStatus> gameStatus_;   // parallel to games_.games
    DWORD                   gameCheckTick_;
    int                     gameSel_;
    std::string             gamesError_;

    // The add-a-game flow: pick a folder, confirm which executable is the game,
    // name it. Held here rather than in a modal so the folder can be re-picked
    // without starting over.
    bool                      addOpen_;
    std::wstring              addFolder_;
    std::vector<ExeCandidate> addExes_;
    int                       addExe_;
    char                      addName_[96];
    std::string               addError_;

    // Deferred confirmations, so nothing destructive happens on the same frame
    // as the click that asked for it.
    enum class Confirm { None, Redeploy, Forget, Adopt };
    Confirm confirm_;
    int     confirmGame_;
    char    adoptName_[64];

    std::string status_;
    DWORD       statusTick_;
};

} // namespace gui
} // namespace vx
