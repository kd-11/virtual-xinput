#include "games.h"

#include "profiles.h"

#include "di_device.h"   // Narrow / Widen
#include "yaml.h"

#include <cstdio>
#include <cwctype>

namespace vx {
namespace {

const wchar_t* kPrimaryDll = L"xinput1_3.dll";

// Names the tool may ever install. Used only by the conservative uninstall
// path, and every candidate is still content-checked before deletion.
const wchar_t* kKnownDlls[] = {
    L"xinput1_1.dll", L"xinput1_2.dll", L"xinput1_3.dll",
    L"xinput1_4.dll", L"xinput9_1_0.dll",
};

// YAML single-quoted style: the only escape is a doubled quote, which leaves
// Windows backslashes completely alone.
std::string QuoteYaml(const std::string& in) {
    std::string out = "'";
    for (size_t i = 0; i < in.size(); ++i) {
        out += in[i];
        if (in[i] == '\'') out += '\'';
    }
    out += "'";
    return out;
}

bool ReadWholeFile(const std::wstring& path, std::vector<char>& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size;
    size.QuadPart = 0;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > (64 << 20)) { CloseHandle(h); return false; }

    data.resize((size_t)size.QuadPart);
    DWORD read = 0;
    BOOL  ok   = data.empty() ? TRUE
                              : ReadFile(h, &data[0], (DWORD)data.size(), &read, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    data.resize(read);
    return true;
}

std::string LastErrorText() {
    DWORD e = GetLastError();
    char  buf[128];
    snprintf(buf, sizeof(buf), "windows error %lu", e);
    return buf;
}

} // namespace

// ---------------------------------------------------------------------------

const char* PeArchName(PeArch a) {
    switch (a) {
        case PeArch::X86: return "x86";
        case PeArch::X64: return "x64";
        default:          return "unknown";
    }
}

PeArch PeArchFromName(const std::string& s) {
    if (s == "x86" || s == "32" || s == "win32") return PeArch::X86;
    if (s == "x64" || s == "64" || s == "amd64") return PeArch::X64;
    return PeArch::Unknown;
}

PeArch DetectExeArch(const std::wstring& exePath) {
    HANDLE h = CreateFileW(exePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return PeArch::Unknown;

    PeArch result = PeArch::Unknown;
    DWORD  read   = 0;

    IMAGE_DOS_HEADER dos;
    if (ReadFile(h, &dos, sizeof(dos), &read, nullptr) && read == sizeof(dos) &&
        dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0) {

        if (SetFilePointer(h, dos.e_lfanew, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER) {
            DWORD signature = 0;
            if (ReadFile(h, &signature, sizeof(signature), &read, nullptr) &&
                read == sizeof(signature) && signature == IMAGE_NT_SIGNATURE) {

                IMAGE_FILE_HEADER fh;
                if (ReadFile(h, &fh, sizeof(fh), &read, nullptr) && read == sizeof(fh)) {
                    if (fh.Machine == IMAGE_FILE_MACHINE_I386)       result = PeArch::X86;
                    else if (fh.Machine == IMAGE_FILE_MACHINE_AMD64) result = PeArch::X64;
                }
            }
        }
    }

    CloseHandle(h);
    return result;
}

// ---------------------------------------------------------------------------

int GameLibrary::FindByName(const std::string& name) const {
    for (size_t i = 0; i < games.size(); ++i) {
        if (games[i].name == name) return (int)i;
    }
    // Fall back to a case-insensitive comparison so typing the name is forgiving.
    for (size_t i = 0; i < games.size(); ++i) {
        if (_stricmp(games[i].name.c_str(), name.c_str()) == 0) return (int)i;
    }
    return -1;
}

bool DirectoryExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& leaf) {
    if (dir.empty()) return leaf;
    std::wstring out = dir;
    if (out.back() != L'\\' && out.back() != L'/') out += L'\\';
    out += leaf;
    return out;
}

std::wstring LeafName(const std::wstring& path) {
    std::wstring p = path;
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    size_t slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? p : p.substr(slash + 1);
}

std::wstring GamesFilePath(const std::wstring& toolDir) {
    return JoinPath(toolDir, L"games.yml");
}

std::vector<std::wstring> FindExecutables(const std::wstring& folder) {
    std::vector<std::wstring> out;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(JoinPath(folder, L"*.exe").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        out.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    return out;
}

std::vector<ExeCandidate> ScanExecutables(const std::wstring& folder) {
    std::vector<std::wstring> names = FindExecutables(folder);
    std::vector<ExeCandidate> out;
    out.reserve(names.size());
    for (size_t i = 0; i < names.size(); ++i) {
        ExeCandidate c;
        c.name = names[i];
        c.arch = DetectExeArch(JoinPath(folder, names[i]));
        out.push_back(c);
    }
    return out;
}

int PreferredExe(const std::vector<ExeCandidate>& exes) {
    if (exes.empty()) return -1;
    for (size_t i = 0; i < exes.size(); ++i) {
        if (exes[i].arch == PeArch::X86) return (int)i;
    }
    return 0;
}

bool MakeGameEntry(const std::wstring& folder, const std::wstring& exe,
                   GameEntry& game, std::string& err) {
    if (!DirectoryExists(folder)) {
        err = "no such folder: " + Narrow(folder);
        return false;
    }

    game.folder = folder;
    if (game.name.empty()) game.name = Narrow(LeafName(folder));

    std::vector<ExeCandidate> exes = ScanExecutables(folder);
    if (exes.empty()) {
        err = "no .exe found in that folder - is it the folder the game runs from?";
        return false;
    }

    int pick = -1;
    if (!exe.empty()) {
        for (size_t i = 0; i < exes.size(); ++i) {
            if (_wcsicmp(exes[i].name.c_str(), exe.c_str()) == 0) { pick = (int)i; break; }
        }
        if (pick < 0) {
            err = Narrow(exe) + " is not in that folder";
            return false;
        }
    } else {
        pick = PreferredExe(exes);
    }

    game.exe  = exes[pick].name;
    game.arch = exes[pick].arch;
    if (game.arch == PeArch::Unknown) {
        err = "could not read the PE header of " + Narrow(game.exe);
        return false;
    }
    return true;
}

std::wstring PayloadDir(const std::wstring& toolDir, PeArch arch) {
    return JoinPath(toolDir, arch == PeArch::X64 ? L"x64" : L"x86");
}

std::vector<std::wstring> PayloadDlls(const std::wstring& toolDir, PeArch arch) {
    std::vector<std::wstring> out;
    if (arch == PeArch::Unknown) return out;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(JoinPath(PayloadDir(toolDir, arch), L"xinput*.dll").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        out.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    return out;
}

bool IsInstalled(const GameEntry& game) {
    return FileExists(JoinPath(game.folder, kPrimaryDll));
}

bool FilesIdentical(const std::wstring& a, const std::wstring& b) {
    std::vector<char> da, db;
    if (!ReadWholeFile(a, da)) return false;
    if (!ReadWholeFile(b, db)) return false;
    if (da.size() != db.size()) return false;
    if (da.empty()) return true;
    return memcmp(&da[0], &db[0], da.size()) == 0;
}

// ---------------------------------------------------------------------------
// Configuration: what is in the folder, and how it got there
// ---------------------------------------------------------------------------

std::wstring GameConfigPath(const GameEntry& game) {
    return JoinPath(game.folder, L"virtual-xinput.yml");
}

std::string ConfigFingerprint(const std::string& text) {
    // Normalise before hashing so that a round trip through an editor that
    // rewrites line endings, or adds a trailing newline, is not reported as
    // somebody's deliberate edit. Those are the two changes a text editor makes
    // without being asked; anything else in the file is a real change.
    std::string norm;
    norm.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') continue;
        norm += text[i];
    }
    while (!norm.empty() && (norm[norm.size() - 1] == '\n' || norm[norm.size() - 1] == ' ' ||
                             norm[norm.size() - 1] == '\t')) {
        norm.erase(norm.size() - 1);
    }

    unsigned long long h = 1469598103934665603ULL;   // FNV-1a 64-bit
    for (size_t i = 0; i < norm.size(); ++i) {
        h ^= (unsigned char)norm[i];
        h *= 1099511628211ULL;
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "%016llx", h);
    return buf;
}

const char* ConfigStateName(ConfigState s) {
    switch (s) {
        case ConfigState::Missing:   return "missing";
        case ConfigState::Unknown:   return "unrecognised";
        case ConfigState::InSync:    return "in sync";
        case ConfigState::Modified:  return "modified";
        case ConfigState::OutOfDate: return "out of date";
    }
    return "unknown";
}

bool LooksPackaged(const std::wstring& folder) {
    // An AppxManifest beside the executable is conclusive; the WindowsApps path
    // catches the case where the manifest sits a level up.
    if (FileExists(JoinPath(folder, L"AppxManifest.xml"))) return true;

    std::wstring lower = folder;
    for (size_t i = 0; i < lower.size(); ++i) lower[i] = (wchar_t)towlower(lower[i]);
    return lower.find(L"\\windowsapps\\") != std::wstring::npos;
}

bool FolderWritable(const std::wstring& folder) {
    // Asking the filesystem beats reasoning about ACLs, and DELETE_ON_CLOSE
    // means the probe cleans up after itself even if we are killed here.
    const std::wstring probe = JoinPath(folder, L".virtual-xinput-write-test");
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

// Resolves the game's profile to a store index, falling back to the default.
// LoadProfiles regenerates the default when it is missing, so on a healthy
// store this cannot come back empty-handed.
static int ResolveProfileIndex(const ProfileStore& store, const GameEntry& game,
                               bool* missing) {
    const int named = game.profile.empty() ? -1 : store.FindByName(game.profile);
    if (missing) *missing = (!game.profile.empty() && named < 0);
    return (named >= 0) ? named : store.DefaultIndex();
}

static bool ResolveProfileText(const std::wstring& toolDir, const GameEntry& game,
                               std::string& text, std::string& err) {
    ProfileStore store;
    if (!LoadProfiles(toolDir, store, err)) return false;

    const int i = ResolveProfileIndex(store, game, nullptr);
    if (i < 0) {
        err = "no profile to deploy and no default to fall back on";
        return false;
    }
    return ReadTextFile(store.profiles[i].path, text);
}

GameStatus CheckGame(const std::wstring& toolDir, const ProfileStore& profiles,
                     const GameEntry& game) {
    GameStatus st;
    st.configPath   = GameConfigPath(game);
    st.folderExists = DirectoryExists(game.folder);

    const int pi = ResolveProfileIndex(profiles, game, &st.profileMissing);
    if (pi >= 0) st.profileName = profiles.profiles[pi].name;

    if (!st.folderExists) return st;   // everything below would be a lie

    const std::vector<std::wstring> dlls = PayloadDlls(toolDir, game.arch);
    st.dllsExpected = (int)dlls.size();
    for (size_t i = 0; i < dlls.size(); ++i) {
        if (FileExists(JoinPath(game.folder, dlls[i]))) ++st.dllsPresent;
    }
    st.installed      = IsInstalled(game);
    st.looksPackaged  = LooksPackaged(game.folder);
    st.folderWritable = FolderWritable(game.folder);

    std::string onDisk;
    if (!ReadTextFile(st.configPath, onDisk)) return st;   // stays Missing

    st.configProfileName = ProfileNameFromText(onDisk);
    const std::string diskHash = ConfigFingerprint(onDisk);

    std::string profileText;
    const bool  haveProfile =
        (pi >= 0) && ReadTextFile(profiles.profiles[pi].path, profileText);
    const bool matchesProfile =
        haveProfile && ConfigFingerprint(profileText) == diskHash;

    if (game.configHash.empty()) {
        // No record of writing it: either it predates fingerprinting or someone
        // put it there. If it is identical to the profile it would be deployed
        // from, saying "unrecognised" would be pedantic rather than useful.
        st.config = matchesProfile ? ConfigState::InSync : ConfigState::Unknown;
    } else if (diskHash != game.configHash) {
        // Edited in the game folder. This wins over out-of-date because it is
        // the one with work in it that nothing else holds a copy of.
        st.config = ConfigState::Modified;
    } else {
        st.config = matchesProfile ? ConfigState::InSync : ConfigState::OutOfDate;
    }
    return st;
}

bool DeployConfig(const std::wstring& toolDir, GameEntry& game, bool overwrite,
                  std::string& err, bool* wrote) {
    if (wrote) *wrote = false;

    if (!DirectoryExists(game.folder)) {
        err = "game folder does not exist: " + Narrow(game.folder);
        return false;
    }

    const std::wstring cfgPath = GameConfigPath(game);
    if (FileExists(cfgPath) && !overwrite) return true;

    std::string text;
    if (!ResolveProfileText(toolDir, game, text, err)) return false;
    if (!WriteTextFile(cfgPath, text)) {
        err = "could not write " + Narrow(cfgPath);
        return false;
    }

    game.configHash = ConfigFingerprint(text);
    if (wrote) *wrote = true;
    return true;
}

bool AdoptConfig(const std::wstring& toolDir, GameEntry& game,
                 const std::string& profileName, std::string& err) {
    std::string text;
    if (!ReadTextFile(GameConfigPath(game), text)) {
        err = "there is no config in " + Narrow(game.folder) + " to adopt";
        return false;
    }

    ProfileStore store;
    if (!LoadProfiles(toolDir, store, err)) return false;

    // Stored verbatim, not round-tripped through the parser: the reason to
    // adopt a file rather than re-derive it is that somebody's hand is in it,
    // and re-emitting would throw away their comments and their layout.
    if (!SaveProfileText(store, profileName, text, err)) return false;
    game.profile = profileName;

    // Write the stored profile straight back out. The only difference from what
    // is already there is the header line naming the new profile - but without
    // this the folder and the profile it now claims to come from would differ
    // by that line, and the game would report itself out of date the moment it
    // was adopted.
    return DeployConfig(toolDir, game, true, err);
}

bool InstallGame(const std::wstring& toolDir, GameEntry& game, std::string& err,
                 bool* configWritten) {
    if (!DirectoryExists(game.folder)) {
        err = "game folder does not exist: " + Narrow(game.folder);
        return false;
    }

    if (game.arch == PeArch::Unknown && !game.exe.empty()) {
        game.arch = DetectExeArch(JoinPath(game.folder, game.exe));
    }
    if (game.arch == PeArch::Unknown) {
        err = "cannot tell whether the game is 32- or 64-bit; set its exe first";
        return false;
    }

    std::vector<std::wstring> dlls = PayloadDlls(toolDir, game.arch);
    if (dlls.empty()) {
        err = "no DLLs found in " + Narrow(PayloadDir(toolDir, game.arch)) +
              " - build the dist folder first";
        return false;
    }

    game.installedFiles.clear();
    for (size_t i = 0; i < dlls.size(); ++i) {
        std::wstring src = JoinPath(PayloadDir(toolDir, game.arch), dlls[i]);
        std::wstring dst = JoinPath(game.folder, dlls[i]);

        if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
            err = "could not write " + Narrow(dst) + " (" + LastErrorText() + ")";
            return false;
        }
        game.installedFiles.push_back(dlls[i]);
    }

    // The config is deliberately not added to installedFiles: uninstall leaves
    // it behind so a mapping someone worked out survives removing the DLLs.
    //
    // overwrite is false, so a config already in the folder is left exactly as
    // it is. Replacing one is a separate, deliberate act - see DeployConfig.
    bool wrote = false;
    if (!DeployConfig(toolDir, game, false, err, &wrote)) {
        err = "installed the DLLs, but " + err;
        return false;
    }
    if (configWritten) *configWritten = wrote;
    return true;
}

bool UninstallGame(const std::wstring& toolDir, GameEntry& game, std::string& err) {
    std::vector<std::wstring> targets = game.installedFiles;
    bool recorded = !targets.empty();

    if (!recorded) {
        for (size_t i = 0; i < sizeof(kKnownDlls) / sizeof(kKnownDlls[0]); ++i) {
            targets.push_back(kKnownDlls[i]);
        }
    }

    int  removed = 0;
    int  skipped = 0;
    for (size_t i = 0; i < targets.size(); ++i) {
        std::wstring path = JoinPath(game.folder, targets[i]);
        if (!FileExists(path)) continue;

        // Without a record of having installed it, only remove a file we can
        // prove is ours - a game may ship an XInput DLL of its own.
        if (!recorded) {
            std::wstring payload = JoinPath(PayloadDir(toolDir, game.arch), targets[i]);
            if (!FileExists(payload) || !FilesIdentical(path, payload)) {
                ++skipped;
                continue;
            }
        }

        if (DeleteFileW(path.c_str())) ++removed;
        else ++skipped;
    }

    // The log is unambiguously ours, so it always goes.
    std::wstring log = JoinPath(game.folder, L"virtual-xinput.log");
    if (FileExists(log)) DeleteFileW(log.c_str());

    game.installedFiles.clear();

    if (removed == 0) {
        err = skipped > 0
                  ? "nothing removed: the files present were not recognised as ours"
                  : "nothing to remove";
        return false;
    }
    if (skipped > 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "removed %d file(s); left %d unrecognised file(s) alone",
                 removed, skipped);
        err = buf;
    }
    return true;
}

// ---------------------------------------------------------------------------

bool LoadGames(const std::wstring& path, GameLibrary& lib, std::string& err) {
    lib.games.clear();

    std::string text;
    if (!ReadFileUtf8(path, text)) return true;   // no library yet is not an error

    YamlNode root;
    if (!YamlParse(text, root, err)) return false;

    const YamlNode* games = root.Find("games");
    if (!games || !games->IsSeq()) return true;

    for (size_t i = 0; i < games->seq.size(); ++i) {
        const YamlNode& n = games->seq[i];

        GameEntry g;
        g.name   = n.Str("name", "");
        g.folder = Widen(n.Str("folder", ""));
        g.exe    = Widen(n.Str("exe", ""));
        g.arch   = PeArchFromName(n.Str("arch", "unknown"));
        g.profile    = n.Str("profile", "");
        g.configHash = n.Str("config_hash", "");

        if (g.folder.empty()) continue;
        if (g.name.empty()) g.name = Narrow(LeafName(g.folder));

        if (const YamlNode* files = n.Find("installed")) {
            if (files->IsSeq()) {
                for (size_t k = 0; k < files->seq.size(); ++k) {
                    if (files->seq[k].IsScalar() && !files->seq[k].scalar.empty()) {
                        g.installedFiles.push_back(Widen(files->seq[k].scalar));
                    }
                }
            }
        }

        lib.games.push_back(g);
    }
    return true;
}

bool SaveGames(const std::wstring& path, const GameLibrary& lib, std::string& err) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w") != 0 || !f) {
        err = "could not write " + Narrow(path);
        return false;
    }

    fprintf(f, "# virtual-xinput game library\n");
    fprintf(f, "# Written by virtual-xinput-config. Paths are single-quoted, so\n");
    fprintf(f, "# backslashes need no escaping.\n\n");
    fprintf(f, "games:\n");

    for (size_t i = 0; i < lib.games.size(); ++i) {
        const GameEntry& g = lib.games[i];

        fprintf(f, "  - name:   %s\n", QuoteYaml(g.name).c_str());
        fprintf(f, "    folder: %s\n", QuoteYaml(Narrow(g.folder)).c_str());
        if (!g.exe.empty()) {
            fprintf(f, "    exe:    %s\n", QuoteYaml(Narrow(g.exe)).c_str());
        }
        fprintf(f, "    arch:   %s\n", PeArchName(g.arch));
        if (!g.profile.empty()) {
            fprintf(f, "    profile: %s\n", QuoteYaml(g.profile).c_str());
        }
        if (!g.configHash.empty()) {
            fprintf(f, "    config_hash: %s\n", QuoteYaml(g.configHash).c_str());
        }

        if (!g.installedFiles.empty()) {
            fprintf(f, "    installed:\n");
            for (size_t k = 0; k < g.installedFiles.size(); ++k) {
                fprintf(f, "      - %s\n", QuoteYaml(Narrow(g.installedFiles[k])).c_str());
            }
        }
        fprintf(f, "\n");
    }

    fclose(f);
    return true;
}

} // namespace vx
