#include "games.h"

#include "di_device.h"   // Narrow / Widen
#include "yaml.h"

#include <cstdio>

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

bool InstallGame(const std::wstring& toolDir, GameEntry& game, std::string& err) {
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
