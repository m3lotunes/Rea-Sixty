#pragma once
//
// UadProto: the text protocol of the UA Mixer Engine, the background process of
// UAD Console (Universal Audio Apollo). Pure: no socket, no thread, no REAPER. The
// socket half lives in the manager and in tools/uad_engine_probe; this is the part
// with a test (tests/test_uad_proto.cpp), as RmeOsc is for TotalMix.
//
// What is known, and from where (plan docs/uad-console-plan.md):
//   - TCP on localhost:4710, no authentication. The engine keeps running after the
//     Console window is closed. (UA Midi Control's README, by its author, who found
//     it with Wireshark; UA does not document it.)
//   - Every message, both ways, ends with a NUL byte. TCP is a stream: a read can
//     hold half a message or several; split at NUL, never at read boundaries.
//   - Commands are text: "<method> <path>" or "set <path> <value>", methods get,
//     set, subscribe. Replies are JSON: {"path": "...", "data": ...}; a container's
//     data is {"properties": {...}, "children": {"0": {}, ...}}.
//
// Everything else (which paths exist on this Apollo, value ranges, meters) comes
// from tools/uad_engine_probe on the user's Mac, not from here.
//
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace uad {

constexpr std::uint16_t kPort = 4710;
// A whole-tree reply can be large; a message past this is not one we can use, and
// a stream that never sends NUL must not grow the buffer without end.
constexpr std::size_t kMaxMessage = 8u * 1024u * 1024u;

// Splits the byte stream at NUL. Bytes after the last NUL wait for the next feed.
class Framer {
public:
    // Returns false when a message ran past kMaxMessage (it is dropped, and the
    // framer resynchronises at the next NUL).
    bool feed(const char* p, std::size_t n, std::vector<std::string>& out)
    {
        bool ok = true;
        for (std::size_t i = 0; i < n; ++i) {
            const char c = p[i];
            if (c == '\0') {
                if (!overflow_ && !buf_.empty()) out.push_back(std::move(buf_));
                buf_.clear();
                overflow_ = false;
                continue;
            }
            if (overflow_) continue;
            if (buf_.size() >= kMaxMessage) {
                overflow_ = true;
                buf_.clear();
                ok = false;
                continue;
            }
            buf_.push_back(c);
        }
        return ok;
    }
    void reset() { buf_.clear(); overflow_ = false; }
    std::size_t pending() const { return buf_.size(); }

private:
    std::string buf_;
    bool        overflow_ = false;
};

// One command on the wire, NUL included.
inline std::string command(std::string_view method, std::string_view path)
{
    std::string s;
    s.reserve(method.size() + path.size() + 2);
    s.append(method).append(" ").append(path);
    s.push_back('\0');
    return s;
}

inline std::string get(std::string_view path)       { return command("get", path); }
inline std::string subscribe(std::string_view path) { return command("subscribe", path); }

// "set <path> <value>", the value as JSON text: numbers bare, true/false, strings
// quoted (setString). A set always targets the property's "/value" leaf (the
// engine's own canonical form, without UA Midi Control's trailing slash); the
// caller passes the property path and setValuePath adds the leaf.
//
// ⛔ THE ENGINE DOES NOT ENFORCE ITS OWN "readonly" FLAGS: a write to DeviceHwID
// was applied on real hardware (uad-console-mcp, docs/PROTOCOL.md). Whatever
// sends sets keeps its own allowlist of properties; this file only spells them.
inline std::string setValuePath(std::string_view property)
{
    std::string p(property);
    while (!p.empty() && p.back() == '/') p.pop_back();
    const std::string_view leaf = "/value";
    if (p.size() < leaf.size() || p.compare(p.size() - leaf.size(), leaf.size(), leaf) != 0)
        p.append(leaf);
    return p;
}

inline std::string set(std::string_view property, std::string_view jsonValue)
{
    std::string s = "set ";
    s.append(setValuePath(property)).append(" ").append(jsonValue);
    s.push_back('\0');
    return s;
}

inline std::string jsonQuote(std::string_view text)
{
    std::string q = "\"";
    for (const char c : text) {
        switch (c) {
            case '"':  q += "\\\""; break;
            case '\\': q += "\\\\"; break;
            case '\n': q += "\\n"; break;
            case '\r': q += "\\r"; break;
            case '\t': q += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    q += b;
                } else {
                    q.push_back(c);
                }
        }
    }
    q.push_back('"');
    return q;
}

inline std::string setNumber(std::string_view property, double v)
{
    char b[32];
    std::snprintf(b, sizeof(b), "%.6g", v);
    return set(property, b);
}
inline std::string setBool(std::string_view property, bool v) { return set(property, v ? "true" : "false"); }
inline std::string setString(std::string_view property, std::string_view v) { return set(property, jsonQuote(v)); }

// Joins a parent path and a child key: "/devices" + "0" = "/devices/0".
inline std::string child(std::string_view parent, std::string_view key)
{
    std::string s(parent);
    if (s.empty() || s.back() != '/') s.push_back('/');
    s.append(key);
    return s;
}

// The path a reply is about, without parsing the whole JSON: the first "path"
// string. Enough to route replies and subscription updates; a full parse (WDL's
// jsonparse with JsonTreeGuard) reads the data. A batched push (only after
// "subscribe mode=multi", which we do not send) reads "multi" and carries a list
// of {path, data} to flatten first.
inline std::string replyPath(std::string_view json)
{
    const std::string_view key = "\"path\"";
    std::size_t k = json.find(key);
    if (k == std::string_view::npos) return {};
    k = json.find(':', k + key.size());
    if (k == std::string_view::npos) return {};
    k = json.find('"', k + 1);
    if (k == std::string_view::npos) return {};
    std::string out;
    for (std::size_t i = k + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '\\' && i + 1 < json.size()) { out.push_back(json[++i]); continue; }
        if (c == '"') return out;
        out.push_back(c);
    }
    return {};
}

}  // namespace uad
