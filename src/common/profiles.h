#pragma once

#include "config.h"

#include <string>
#include <vector>

// Named mappings, stored centrally and deployed into game folders.
//
// A profile is a complete `virtual-xinput.yml` - not a fragment and not a
// second format. That is the whole design: a profile can be inspected,
// hand-edited, copied out of the folder or dropped into a game by hand, and
// there is exactly one config schema in the project rather than two that have
// to be kept in step.
//
// Storage is one file per profile in `<toolDir>/profiles`, so a profile broken
// by hand takes only itself down rather than the whole library.
namespace vx {

// Display name of the profile that always exists.
extern const char* kDefaultProfileName;

struct ProfileEntry {
    std::string  name;      // display name, from the file's own header comment
    std::string  slug;      // file stem, derived from the name
    std::wstring path;      // full path to the .yml

    // The default cannot be deleted or renamed; it is regenerated when absent.
    bool isDefault = false;
};

struct ProfileStore {
    std::wstring              toolDir; // where the configurator lives
    std::wstring              dir;     // <toolDir>/profiles
    std::vector<ProfileEntry> profiles;

    int FindByName(const std::string& name) const;
    int FindBySlug(const std::string& slug) const;

    // Index of the default profile, which is always present after a
    // successful Load.
    int DefaultIndex() const;
};

std::wstring ProfilesDir(const std::wstring& toolDir);

// Filename stem for a display name: lower-cased, non-alphanumerics folded to
// '-'. Two names that differ only in punctuation collide, which Save resolves
// by appending a number rather than by overwriting.
std::string ProfileSlug(const std::string& name);

// Enumerates <toolDir>/profiles, creating the directory and the default
// profile if either is missing. A profile that fails to parse is still listed,
// so it can be seen and repaired rather than silently vanishing.
bool LoadProfiles(const std::wstring& toolDir, ProfileStore& store, std::string& err);

// Reads one profile's config. Returns false with err set on a parse error.
bool ReadProfile(const ProfileEntry& entry, Config& out, std::string& err);

// Writes `cfg` as the profile called `name`, creating it or replacing it.
// On success `store` is reloaded so the caller sees the new entry.
bool SaveProfile(ProfileStore& store, const std::string& name, const Config& cfg,
                 std::string& err);

// Removes a profile. Refuses to remove the default.
bool DeleteProfile(ProfileStore& store, const std::string& name, std::string& err);

// Copies a profile under a new name.
bool DuplicateProfile(ProfileStore& store, const std::string& from,
                      const std::string& to, std::string& err);

bool RenameProfile(ProfileStore& store, const std::string& from,
                   const std::string& to, std::string& err);

// The config the default profile contains: deadzones set, no device block at
// all, so every pad is auto-detected. It is the configuration that works on the
// widest range of hardware, which is what makes it a safe thing to deploy when
// nothing else has been chosen.
Config DefaultProfileConfig();

// Text file helpers, shared with the game installer.
bool ReadTextFile(const std::wstring& path, std::string& out);
bool WriteTextFile(const std::wstring& path, const std::string& text);

// The name a config file carries in its header comment, if any. This is how a
// profile keeps a display name with spaces and capitals while living in a file
// named after its slug.
std::string ProfileNameFromText(const std::string& text);

} // namespace vx
