#include "profiles.h"

#include "games.h"
#include "strutil.h"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstring>

namespace vx {

const char* kDefaultProfileName = "Default";

namespace {

// A profile's display name is carried in a header comment, so that it can have
// spaces and capitals while the file itself is named after its slug. The line
// is a comment, which means a deployed profile stays a perfectly ordinary
// config file that the DLL neither notices nor cares about.
const char* kNameMarker = "# profile: ";

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) ++a;
    while (b > a && isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::wstring ProfilePath(const std::wstring& dir, const std::string& slug) {
    return JoinPath(dir, Widen(slug) + L".yml");
}

std::string WithNameHeader(const std::string& name, const std::string& yaml) {
    return std::string(kNameMarker) + name + "\n" + yaml;
}

// Adds a numeric suffix until the slug is free. Used when two display names
// differ only in punctuation, where silently overwriting would lose work.
std::string UniqueSlug(const ProfileStore& store, const std::string& base,
                       const std::string& allowSlug) {
    if (store.FindBySlug(base) < 0 || base == allowSlug) return base;
    for (int n = 2; n < 1000; ++n) {
        char buf[16];
        snprintf(buf, sizeof(buf), "-%d", n);
        const std::string cand = base + buf;
        if (store.FindBySlug(cand) < 0 || cand == allowSlug) return cand;
    }
    return base;
}

} // namespace

// ---------------------------------------------------------------------------

int ProfileStore::FindByName(const std::string& name) const {
    for (size_t i = 0; i < profiles.size(); ++i) {
        if (_stricmp(profiles[i].name.c_str(), name.c_str()) == 0) return (int)i;
    }
    return -1;
}

int ProfileStore::FindBySlug(const std::string& slug) const {
    for (size_t i = 0; i < profiles.size(); ++i) {
        if (profiles[i].slug == slug) return (int)i;
    }
    return -1;
}

int ProfileStore::DefaultIndex() const {
    for (size_t i = 0; i < profiles.size(); ++i) {
        if (profiles[i].isDefault) return (int)i;
    }
    return -1;
}

std::wstring ProfilesDir(const std::wstring& toolDir) {
    return JoinPath(toolDir, L"profiles");
}

std::string ProfileSlug(const std::string& name) {
    std::string out;
    bool lastDash = false;
    for (size_t i = 0; i < name.size(); ++i) {
        const unsigned char c = (unsigned char)name[i];
        if (isalnum(c)) {
            out += (char)tolower(c);
            lastDash = false;
        } else if (!out.empty() && !lastDash) {
            out += '-';
            lastDash = true;
        }
    }
    while (!out.empty() && out[out.size() - 1] == '-') out.erase(out.size() - 1);
    return out.empty() ? "profile" : out;
}

std::string ProfileNameFromText(const std::string& text) {
    if (text.compare(0, strlen(kNameMarker), kNameMarker) != 0) return std::string();
    const size_t nl = text.find('\n');
    const size_t n  = strlen(kNameMarker);
    return Trim(text.substr(n, (nl == std::string::npos ? text.size() : nl) - n));
}

// ---------------------------------------------------------------------------

bool ReadTextFile(const std::wstring& path, std::string& out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;

    out.clear();
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

bool WriteTextFile(const std::wstring& path, const std::string& text) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    const size_t n = text.empty() ? 0 : fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    return n == text.size();
}

// ---------------------------------------------------------------------------

Config DefaultProfileConfig() {
    Config cfg;
    cfg.log    = false;
    cfg.pollHz = 250;
    // No device block on purpose. Every pad is then auto-detected, which is the
    // configuration that works on the widest range of hardware and the only
    // sensible thing to deploy when nobody has chosen anything.
    cfg.devices.clear();
    return cfg;
}

bool LoadProfiles(const std::wstring& toolDir, ProfileStore& store, std::string& err) {
    store.toolDir = toolDir;
    store.dir     = ProfilesDir(toolDir);
    store.profiles.clear();

    if (!DirectoryExists(store.dir) && !CreateDirectoryW(store.dir.c_str(), nullptr)) {
        err = "could not create the profiles folder";
        return false;
    }

    const std::string defaultSlug = ProfileSlug(kDefaultProfileName);

    WIN32_FIND_DATAW fd;
    const std::wstring pattern = JoinPath(store.dir, L"*.yml");
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

            ProfileEntry e;
            e.path = JoinPath(store.dir, fd.cFileName);

            std::wstring stem = fd.cFileName;
            const size_t dot  = stem.rfind(L'.');
            if (dot != std::wstring::npos) stem = stem.substr(0, dot);
            e.slug = Narrow(stem);

            // The display name comes from the file, so renaming a file by hand
            // does not silently rename the profile.
            std::string text;
            if (ReadTextFile(e.path, text)) e.name = ProfileNameFromText(text);
            if (e.name.empty()) e.name = e.slug;

            e.isDefault = (e.slug == defaultSlug);
            store.profiles.push_back(e);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // The default must always exist: without it there is a state in which
    // installing into a game has no profile to deploy.
    if (store.FindBySlug(defaultSlug) < 0) {
        const std::wstring path = ProfilePath(store.dir, defaultSlug);
        const std::string  text = WithNameHeader(kDefaultProfileName,
                                                 ConfigToYaml(DefaultProfileConfig()));
        if (!WriteTextFile(path, text)) {
            err = "could not write the default profile";
            return false;
        }
        ProfileEntry e;
        e.name      = kDefaultProfileName;
        e.slug      = defaultSlug;
        e.path      = path;
        e.isDefault = true;
        store.profiles.push_back(e);
    }

    return true;
}

bool ReadProfile(const ProfileEntry& entry, Config& out, std::string& err) {
    std::string text;
    if (!ReadTextFile(entry.path, text)) {
        err = "could not read " + entry.slug + ".yml";
        return false;
    }
    if (!ConfigParse(text, out)) {
        err = out.error.empty() ? ("could not parse " + entry.slug + ".yml") : out.error;
        return false;
    }
    out.fileFound = true;
    out.path      = entry.path;
    return true;
}

bool SaveProfile(ProfileStore& store, const std::string& name, const Config& cfg,
                 std::string& err) {
    const std::string trimmed = Trim(name);
    if (trimmed.empty()) {
        err = "a profile needs a name";
        return false;
    }

    // Saving over an existing profile keeps its file; a new name gets a free
    // slug rather than colliding with one that merely punctuates differently.
    const int         existing = store.FindByName(trimmed);
    const std::string keepSlug = (existing >= 0) ? store.profiles[existing].slug : std::string();
    const std::string slug     = (existing >= 0)
                                   ? keepSlug
                                   : UniqueSlug(store, ProfileSlug(trimmed), std::string());

    const std::wstring path = ProfilePath(store.dir, slug);
    if (!WriteTextFile(path, WithNameHeader(trimmed, ConfigToYaml(cfg)))) {
        err = "could not write " + slug + ".yml";
        return false;
    }

    return LoadProfiles(store.toolDir, store, err);
}

bool SaveProfileText(ProfileStore& store, const std::string& name,
                     const std::string& text, std::string& err) {
    const std::string trimmed = Trim(name);
    if (trimmed.empty()) {
        err = "a profile needs a name";
        return false;
    }
    if (_stricmp(trimmed.c_str(), kDefaultProfileName) == 0) {
        err = "the default profile cannot be overwritten - save under another name";
        return false;
    }
    // Checked but not rewritten. A profile that does not parse would install
    // into a game folder and leave the DLL quietly falling back to its own
    // defaults, with nothing anywhere saying why.
    Config check;
    if (!ConfigParse(text, check)) {
        err = "that config does not parse: " +
              (check.error.empty() ? std::string("unknown error") : check.error);
        return false;
    }

    // The incoming text may already carry a header naming some other profile -
    // a deployed config does. Strip it so the stored file names itself.
    std::string body = text;
    if (body.compare(0, strlen(kNameMarker), kNameMarker) == 0) {
        const size_t nl = body.find('\n');
        body = (nl == std::string::npos) ? std::string() : body.substr(nl + 1);
    }

    const int         existing = store.FindByName(trimmed);
    const std::string slug     = (existing >= 0)
                                   ? store.profiles[existing].slug
                                   : UniqueSlug(store, ProfileSlug(trimmed), std::string());

    if (!WriteTextFile(ProfilePath(store.dir, slug), WithNameHeader(trimmed, body))) {
        err = "could not write " + slug + ".yml";
        return false;
    }
    return LoadProfiles(store.toolDir, store, err);
}

bool DeleteProfile(ProfileStore& store, const std::string& name, std::string& err) {
    const int i = store.FindByName(name);
    if (i < 0) {
        err = "no profile named '" + name + "'";
        return false;
    }
    if (store.profiles[i].isDefault) {
        // Deleting it would leave install with nothing to deploy.
        err = "the default profile cannot be deleted";
        return false;
    }
    if (!DeleteFileW(store.profiles[i].path.c_str())) {
        err = "could not delete " + store.profiles[i].slug + ".yml";
        return false;
    }

    return LoadProfiles(store.toolDir, store, err);
}

bool DuplicateProfile(ProfileStore& store, const std::string& from,
                      const std::string& to, std::string& err) {
    const int i = store.FindByName(from);
    if (i < 0) {
        err = "no profile named '" + from + "'";
        return false;
    }
    if (store.FindByName(to) >= 0) {
        err = "a profile called '" + to + "' already exists";
        return false;
    }

    Config cfg;
    if (!ReadProfile(store.profiles[i], cfg, err)) return false;
    return SaveProfile(store, to, cfg, err);
}

bool RenameProfile(ProfileStore& store, const std::string& from,
                   const std::string& to, std::string& err) {
    const int i = store.FindByName(from);
    if (i < 0) {
        err = "no profile named '" + from + "'";
        return false;
    }
    if (store.profiles[i].isDefault) {
        err = "the default profile cannot be renamed";
        return false;
    }
    const std::string trimmed = Trim(to);
    if (trimmed.empty()) {
        err = "a profile needs a name";
        return false;
    }
    if (store.FindByName(trimmed) >= 0 && store.FindByName(trimmed) != i) {
        err = "a profile called '" + trimmed + "' already exists";
        return false;
    }

    Config cfg;
    if (!ReadProfile(store.profiles[i], cfg, err)) return false;

    const std::wstring oldPath = store.profiles[i].path;
    const std::string  newSlug = UniqueSlug(store, ProfileSlug(trimmed), store.profiles[i].slug);
    const std::wstring newPath = ProfilePath(store.dir, newSlug);

    if (!WriteTextFile(newPath, WithNameHeader(trimmed, ConfigToYaml(cfg)))) {
        err = "could not write " + newSlug + ".yml";
        return false;
    }
    if (newPath != oldPath) DeleteFileW(oldPath.c_str());

    return LoadProfiles(store.toolDir, store, err);
}

} // namespace vx
