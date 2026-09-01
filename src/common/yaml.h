#pragma once

#include <string>
#include <utility>
#include <vector>

namespace vx {

// A deliberately small YAML subset: nested block maps, block sequences,
// scalars, quotes and '#' comments. No anchors, flow collections, multi-line
// scalars or tags. That covers everything virtual-xinput.yml needs and keeps
// the DLL dependency-free.
struct YamlNode {
    enum class Type { Scalar, Map, Seq };

    Type        type = Type::Map;
    std::string scalar;
    std::vector<std::pair<std::string, YamlNode>> map;
    std::vector<YamlNode>                         seq;

    bool IsScalar() const { return type == Type::Scalar; }
    bool IsMap()    const { return type == Type::Map; }
    bool IsSeq()    const { return type == Type::Seq; }

    // Map lookup by key; returns nullptr when absent or when this is not a map.
    const YamlNode* Find(const std::string& key) const;

    // Typed accessors that fall back to `def` when the key is missing or the
    // value does not parse.
    std::string Str(const std::string& key, const std::string& def = "") const;
    double      Num(const std::string& key, double def = 0.0) const;
    int         Int(const std::string& key, int def = 0) const;
    bool        Bool(const std::string& key, bool def = false) const;
};

// Parses `text`. On failure returns false and fills `err` with a line-numbered
// message. Never throws.
bool YamlParse(const std::string& text, YamlNode& out, std::string& err);

// Reads a UTF-8 (or ASCII) file into `text`. Strips a UTF-8 BOM if present.
bool ReadFileUtf8(const std::wstring& path, std::string& text);

} // namespace vx
