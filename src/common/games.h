#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace vx {

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

// Where the payload DLLs for an architecture live: <toolDir>\x86 or <toolDir>\x64.
std::wstring PayloadDir(const std::wstring& toolDir, PeArch arch);

// Every xinput*.dll found in the payload directory, as bare filenames. Whatever
// was built is what gets deployed, so enabling the version aliases at build
// time is all it takes to cover games that load a different XInput version.
std::vector<std::wstring> PayloadDlls(const std::wstring& toolDir, PeArch arch);

// Copies the payload into the game folder and records what was written.
bool InstallGame(const std::wstring& toolDir, GameEntry& game, std::string& err);

// Removes the recorded files. When nothing was recorded (a hand-edited or lost
// library), a file is only deleted if it is byte-identical to our payload, so a
// DLL the game shipped itself is never destroyed.
bool UninstallGame(const std::wstring& toolDir, GameEntry& game, std::string& err);

// True when the primary wrapper DLL is present in the game folder.
bool IsInstalled(const GameEntry& game);

// Byte-for-byte comparison, used by the conservative uninstall path.
bool FilesIdentical(const std::wstring& a, const std::wstring& b);

} // namespace vx
