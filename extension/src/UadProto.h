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

// "set <path> <value>". Values as UA Midi Control writes them: numbers plain,
// booleans true/false. The caller decides the path, including any trailing
// "/value" the property needs.
inline std::string set(std::string_view path, std::string_view value)
{
    std::string s = "set ";
    s.append(path).append(" ").append(value);
    s.push_back('\0');
    return s;
}

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
// jsonparse with JsonTreeGuard) reads the data.
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
