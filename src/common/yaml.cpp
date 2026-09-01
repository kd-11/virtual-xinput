#include "yaml.h"

#include <windows.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vx {
namespace {

struct Line {
    int         indent;
    std::string text;   // comment-stripped, de-indented, right-trimmed
    int         no;     // 1-based source line, for error messages
};

const char kSingleQuote = '\'';
const char kDoubleQuote = '"';

std::string TrimRight(const std::string& s) {
    size_t e = s.size();
    while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(0, e);
}

std::string TrimLeft(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
    return s.substr(b);
}

std::string Trim(const std::string& s) { return TrimLeft(TrimRight(s)); }

// Removes a trailing '#' comment, honouring quotes so a '#' inside a string
// survives. A '#' only starts a comment at line start or after whitespace.
std::string StripComment(const std::string& s) {
    char quote = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (quote) {
            if (c == '\\' && quote == kDoubleQuote && i + 1 < s.size()) { ++i; continue; }
            if (c == quote) quote = 0;
        } else if (c == kDoubleQuote || c == kSingleQuote) {
            quote = c;
        } else if (c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) {
            return s.substr(0, i);
        }
    }
    return s;
}

std::string Unquote(const std::string& in) {
    std::string s = Trim(in);
    if (s.size() >= 2 && s.front() == kSingleQuote && s.back() == kSingleQuote) {
        std::string body = s.substr(1, s.size() - 2);
        std::string out;
        for (size_t i = 0; i < body.size(); ++i) {
            out += body[i];
            if (body[i] == kSingleQuote && i + 1 < body.size() &&
                body[i + 1] == kSingleQuote) {
                ++i;   // '' inside single quotes is one literal quote
            }
        }
        return out;
    }
    if (s.size() >= 2 && s.front() == kDoubleQuote && s.back() == kDoubleQuote) {
        std::string out;
        for (size_t i = 1; i + 1 < s.size(); ++i) {
            if (s[i] == '\\' && i + 2 < s.size()) {
                char n = s[++i];
                switch (n) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    default:  out += n;    break;
                }
            } else {
                out += s[i];
            }
        }
        return out;
    }
    return s;
}

bool IsSeqEntry(const std::string& text) {
    return !text.empty() && text[0] == '-' && (text.size() == 1 || text[1] == ' ');
}

// Finds the ':' separating key from value: the first one outside quotes that is
// followed by a space or ends the line. This lets values keep colons of their
// own, e.g. `left_trigger: button:6`.
size_t FindKeySep(const std::string& s) {
    char quote = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (quote) {
            if (c == '\\' && quote == kDoubleQuote && i + 1 < s.size()) { ++i; continue; }
            if (c == quote) quote = 0;
        } else if (c == kDoubleQuote || c == kSingleQuote) {
            quote = c;
        } else if (c == ':' && (i + 1 == s.size() || s[i + 1] == ' ' || s[i + 1] == '\t')) {
            return i;
        }
    }
    return std::string::npos;
}

std::string Err(int lineNo, const std::string& msg) {
    char buf[64];
    snprintf(buf, sizeof(buf), "line %d: ", lineNo);
    return std::string(buf) + msg;
}

bool ParseBlock(const std::vector<Line>& L, size_t& i, int ind, YamlNode& out, std::string& err);

// Parses the content that followed a "- " on a sequence line, together with any
// continuation lines indented to that content's column, as one node.
bool ParseInlineItem(const std::vector<Line>& L, size_t& i, int contentCol,
                     const std::string& rest, YamlNode& item, std::string& err) {
    std::vector<Line> sub;
    sub.push_back(Line{contentCol, rest, L[i].no});
    ++i;
    while (i < L.size() && L[i].indent >= contentCol) {
        sub.push_back(L[i]);
        ++i;
    }

    // A lone "- value" with nothing indented under it is a plain scalar item,
    // not a one-entry map.
    if (sub.size() == 1 && FindKeySep(rest) == std::string::npos) {
        item.type   = YamlNode::Type::Scalar;
        item.scalar = Unquote(rest);
        return true;
    }

    size_t j = 0;
    return ParseBlock(sub, j, contentCol, item, err);
}

bool ParseSeq(const std::vector<Line>& L, size_t& i, int ind, YamlNode& out, std::string& err) {
    out.type = YamlNode::Type::Seq;
    while (i < L.size() && L[i].indent == ind && IsSeqEntry(L[i].text)) {
        const std::string text = L[i].text;

        size_t restPos = 1;
        while (restPos < text.size() && (text[restPos] == ' ' || text[restPos] == '\t')) ++restPos;
        std::string rest = text.substr(restPos);

        YamlNode item;
        if (rest.empty()) {
            // Content lives on the following, more-indented lines.
            ++i;
            if (i < L.size() && L[i].indent > ind) {
                if (!ParseBlock(L, i, L[i].indent, item, err)) return false;
            } else {
                item.type = YamlNode::Type::Scalar;
            }
        } else {
            if (!ParseInlineItem(L, i, ind + (int)restPos, rest, item, err)) return false;
        }
        out.seq.push_back(item);
    }
    return true;
}

bool ParseMap(const std::vector<Line>& L, size_t& i, int ind, YamlNode& out, std::string& err) {
    out.type = YamlNode::Type::Map;
    while (i < L.size()) {
        if (L[i].indent < ind) break;
        if (L[i].indent > ind) {
            err = Err(L[i].no, "unexpected indentation");
            return false;
        }
        if (IsSeqEntry(L[i].text)) break;

        size_t sep = FindKeySep(L[i].text);
        if (sep == std::string::npos) {
            err = Err(L[i].no, "expected 'key: value'");
            return false;
        }
        std::string key  = Unquote(L[i].text.substr(0, sep));
        std::string rest = Trim(L[i].text.substr(sep + 1));
        int         no   = L[i].no;
        ++i;

        YamlNode child;
        if (rest.empty()) {
            if (i < L.size() && L[i].indent > ind) {
                if (!ParseBlock(L, i, L[i].indent, child, err)) return false;
            } else {
                child.type = YamlNode::Type::Scalar;   // empty value
            }
        } else {
            child.type   = YamlNode::Type::Scalar;
            child.scalar = Unquote(rest);
        }

        if (key.empty()) {
            err = Err(no, "empty key");
            return false;
        }
        out.map.push_back(std::make_pair(key, child));
    }
    return true;
}

bool ParseBlock(const std::vector<Line>& L, size_t& i, int ind, YamlNode& out, std::string& err) {
    if (i >= L.size()) { out.type = YamlNode::Type::Map; return true; }
    if (IsSeqEntry(L[i].text) && L[i].indent == ind) return ParseSeq(L, i, ind, out, err);
    return ParseMap(L, i, ind, out, err);
}

} // namespace

const YamlNode* YamlNode::Find(const std::string& key) const {
    if (type != Type::Map) return nullptr;
    for (size_t i = 0; i < map.size(); ++i) {
        if (map[i].first == key) return &map[i].second;
    }
    return nullptr;
}

std::string YamlNode::Str(const std::string& key, const std::string& def) const {
    const YamlNode* n = Find(key);
    if (!n || !n->IsScalar() || n->scalar.empty()) return def;
    return n->scalar;
}

double YamlNode::Num(const std::string& key, double def) const {
    const YamlNode* n = Find(key);
    if (!n || !n->IsScalar()) return def;
    const char* s   = n->scalar.c_str();
    char*       end = nullptr;
    double      v   = strtod(s, &end);
    if (end == s) return def;
    return v;
}

int YamlNode::Int(const std::string& key, int def) const {
    const YamlNode* n = Find(key);
    if (!n || !n->IsScalar()) return def;
    const char* s   = n->scalar.c_str();
    char*       end = nullptr;
    long        v   = strtol(s, &end, 10);
    if (end == s) return def;
    return (int)v;
}

bool YamlNode::Bool(const std::string& key, bool def) const {
    const YamlNode* n = Find(key);
    if (!n || !n->IsScalar()) return def;
    std::string s = n->scalar;
    for (size_t i = 0; i < s.size(); ++i) s[i] = (char)tolower((unsigned char)s[i]);
    if (s == "true" || s == "yes" || s == "on" || s == "1") return true;
    if (s == "false" || s == "no" || s == "off" || s == "0") return false;
    return def;
}

bool YamlParse(const std::string& text, YamlNode& out, std::string& err) {
    std::vector<Line> lines;

    size_t pos = 0;
    int    no  = 0;
    while (pos <= text.size()) {
        size_t      nl  = text.find('\n', pos);
        std::string raw = text.substr(pos, (nl == std::string::npos ? text.size() : nl) - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
        ++no;

        // Measure indent, expanding tabs so tab-indented files still work.
        int    indent = 0;
        size_t b      = 0;
        while (b < raw.size() && (raw[b] == ' ' || raw[b] == '\t')) {
            indent += (raw[b] == '\t') ? 4 : 1;
            ++b;
        }

        std::string body = TrimRight(StripComment(raw.substr(b)));
        if (body.empty()) continue;
        if (body == "---" || body == "...") continue;   // document markers

        lines.push_back(Line{indent, body, no});
    }

    if (lines.empty()) { out = YamlNode(); return true; }

    size_t i          = 0;
    int    baseIndent = lines[0].indent;
    if (!ParseBlock(lines, i, baseIndent, out, err)) return false;

    if (i < lines.size()) {
        err = Err(lines[i].no, "unexpected content");
        return false;
    }
    return true;
}

bool ReadFileUtf8(const std::wstring& path, std::string& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size;
    size.QuadPart = 0;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > (16 << 20)) { CloseHandle(h); return false; }

    text.resize((size_t)size.QuadPart);
    DWORD read = 0;
    BOOL  ok   = text.empty() ? TRUE : ReadFile(h, &text[0], (DWORD)text.size(), &read, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    text.resize(read);

    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
        text.erase(0, 3);
    }
    return true;
}

} // namespace vx
