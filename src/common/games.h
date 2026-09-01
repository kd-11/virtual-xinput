#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace vx {

struct ProfileStore;

// Architecture of a game executable, which decides which build of the DLL has
// to be installed. Getting this wrong is the single most common reason a
// dropped-in wrapper silently does nothing, so it is read from the PE header
// rather than guessed.
enum class PeArch { Unknown, X86, X64 };

const char* PeArchName(PeArch a);
PeArch      PeArchFromName(const std::string& s);

// Reads the machine type out of an executable's PE header.
PeArch DetectExeArch(const std::wstring& exePath);

struct GameEntry {
    std::string  name;      // display name; defaults to the folder's leaf
    std::wstring folder;    // where the DLL goes, i.e. where the .exe lives
    std::wstring exe;       // filename only, used to detect the architecture
    PeArch       arch = PeArch::Unknown;

    // Which profile to deploy into this folder. Empty means the default, which
    // is what makes an entry written before profiles existed still install.
    std::string profile;

    // Fingerprint of the config we last wrote into the folder. This is the
    // whole basis of divergence detection: without a record of what we put
    // there, a file that differs from the profile is indistinguishable from one
    // someone tuned by hand, and the tool would have to guess. Empty means we
    // have no record.
    std::string configHash;

    // Exactly what we copied in, so uninstalling removes our files and only
    // ours.
    std::vector<std::wstring> installedFiles;
};

struct GameLibrary {
    std::vector<GameEntry> games;

    int FindByName(const std::string& name) const;
};

// games.yml lives beside the configurator, so the whole thing stays portable:
// copy the folder anywhere and the list comes with it.
std::wstring GamesFilePath(const std::wstring& toolDir);

bool LoadGames(const std::wstring& path, GameLibrary& lib, std::string& err);
bool SaveGames(const std::wstring& path, const GameLibrary& lib, std::string& err);

// Directory helpers.
bool DirectoryExists(const std::wstring& path);
bool FileExists(const std::wstring& path);
std::wstring JoinPath(const std::wstring& dir, const std::wstring& leaf);
std::wstring LeafName(const std::wstring& path);

// Executables directly inside `folder`, as bare filenames.
std::vector<std::wstring> FindExecutables(const std::wstring& folder);

// The same list with each one's architecture already read, which is what both
// front-ends actually want to show when asking which executable is the game.
struct ExeCandidate {
    std::wstring name;
    PeArch       arch = PeArch::Unknown;
};
std::vector<ExeCandidate> ScanExecutables(const std::wstring& folder);

// Index of the executable to offer first: the first 32-bit one, since that is
// overwhelmingly what the games needing this tool are, else the first at all.
// Returns -1 for an empty list.
int PreferredExe(const std::vector<ExeCandidate>& exes);

// Fills in name, folder, exe and architecture. `exe` is a bare filename that
// must live directly in `folder`; pass it empty to take PreferredExe's pick.
bool MakeGameEntry(const std::wstring& folder, const std::wstring& exe,
                   GameEntry& game, std::string& err);

// Where the payload DLLs for an architecture live: <toolDir>\x86 or <toolDir>\x64.
std::wstring PayloadDir(const std::wstring& toolDir, PeArch arch);

// Every xinput*.dll found in the payload directory, as bare filenames. Whatever
// was built is what gets deployed, so enabling the version aliases at build
// time is all it takes to cover games that load a different XInput version.
std::vector<std::wstring> PayloadDlls(const std::wstring& toolDir, PeArch arch);

// The config file a game folder holds, or would hold.
std::wstring GameConfigPath(const GameEntry& game);

// Fingerprint of a config's *content*. Line endings are normalised and trailing
// blank space ignored, so an editor that rewrites CRLF on save does not read as
// a deliberate edit.
//
// FNV-1a, deliberately not a cryptographic hash: this tells two files we wrote
// apart from each other, and there is nobody to defend against.
std::string ConfigFingerprint(const std::string& text);

// What the config sitting in a game folder is, relative to what we put there.
enum class ConfigState {
    Missing,     // no virtual-xinput.yml in the folder at all
    Unknown,     // one is there, but we have no record of writing it
    InSync,      // matches what we wrote, and the profile still says the same
    Modified,    // changed in the game folder since we wrote it
    OutOfDate,   // untouched there, but the profile it came from has moved on
};
const char* ConfigStateName(ConfigState s);

// Everything worth checking about a game folder without touching it. Cheap
// enough to poll, and every field is something that otherwise fails silently.
struct GameStatus {
    bool folderExists = false;
    bool installed    = false;   // the primary wrapper DLL is present
    int  dllsPresent  = 0;
    int  dllsExpected = 0;

    ConfigState  config = ConfigState::Missing;
    std::wstring configPath;

    // The profile that would be deployed now, and the one the file already
    // there says it came from. They differ after a reassignment.
    std::string profileName;
    std::string configProfileName;
    bool        profileMissing = false;  // assigned a profile that is gone

    // Both of these fail in ways that look like the tool simply not working,
    // so they are worth saying out loud before an install rather than after.
    bool folderWritable = true;
    bool looksPackaged  = false;
};

// `profiles` is passed in rather than loaded here because this is polled: a
// directory scan per game per frame would be silly. The mutating calls below
// load their own, since they run on a click.
GameStatus CheckGame(const std::wstring& toolDir, const ProfileStore& profiles,
                     const GameEntry& game);

// True when the folder is inside a packaged (Store/UWP) install, where dropping
// a DLL beside the executable does not take effect.
bool LooksPackaged(const std::wstring& folder);

// Whether a file can actually be created in the folder. Answers the "installed
// fine, changed nothing" case that a virtualised or read-only Program Files
// directory produces.
bool FolderWritable(const std::wstring& folder);

// Writes the game's assigned profile into its folder and records the
// fingerprint. With `overwrite` false an existing config is left exactly as it
// is; `wrote` reports which of the two happened.
bool DeployConfig(const std::wstring& toolDir, GameEntry& game, bool overwrite,
                  std::string& err, bool* wrote = nullptr);

// Takes the config sitting in the game folder and stores it as a profile,
// byte for byte, so a mapping tuned by hand keeps its comments and its
// formatting. The game is then assigned to that profile, and the folder's copy
// is rewritten from it so the two agree from the start.
//
// Refuses to write the default profile, which has to stay the auto-detecting
// one for install to always have something safe to deploy.
bool AdoptConfig(const std::wstring& toolDir, GameEntry& game,
                 const std::string& profileName, std::string& err);

// Copies the payload into the game folder, records what was written, and
// deploys the game's profile as virtual-xinput.yml.
//
// A game is never installed configuration-less: if `game.profile` names nothing
// that exists, the default profile is used, and the default is regenerated if
// it has been deleted.
//
// An existing virtual-xinput.yml is left alone rather than overwritten. Someone
// may have tuned it by hand, and silently replacing that with a profile would
// destroy work no copy of which exists anywhere else. `configWritten` reports
// which of the two happened; use DeployConfig to overwrite deliberately.
bool InstallGame(const std::wstring& toolDir, GameEntry& game, std::string& err,
                 bool* configWritten = nullptr);

// Removes the recorded files. When nothing was recorded (a hand-edited or lost
// library), a file is only deleted if it is byte-identical to our payload, so a
// DLL the game shipped itself is never destroyed.
bool UninstallGame(const std::wstring& toolDir, GameEntry& game, std::string& err);

// True when the primary wrapper DLL is present in the game folder.
bool IsInstalled(const GameEntry& game);

// Byte-for-byte comparison, used by the conservative uninstall path.
bool FilesIdentical(const std::wstring& a, const std::wstring& b);

} // namespace vx
