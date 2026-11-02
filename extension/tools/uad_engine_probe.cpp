//
// uad_engine_probe: what does the UA Mixer Engine (UAD Console's background
// process) offer on this Mac? Measurement for docs/uad-console-plan.md.
//
// READ ONLY. It sends `get` and `subscribe`, never `set`: no fader, mute, preamp
// or monitor value changes. It connects to localhost:4710, walks the tree from
// /devices down (breadth first, bounded), writes every reply to a log, then
// optionally subscribes to a few paths and logs the updates for a while.
//
// Usage:
//   uad_engine_probe [--host 127.0.0.1] [--port 4710] [--depth 6] [--max 4000]
//                    [--sub PATH]... [--secs 20] [--out uad_tree.log]
//
//   --depth  how many levels below /devices to walk (devices/0/inputs/3/sends/1 = 5)
//   --max    stop after this many `get`s
//   --sub    subscribe to PATH after the walk (repeatable), e.g.
//            --sub /devices/0/inputs/0/FaderLevelTapered
//            --sub /devices/0/inputs/0/meters/0/MeterLevel
//
// Build on the Mac (from the repository root, one line):
//   clang++ -std=c++20 -O1 -Iextension/src -Iextension/vendor/WDL extension/tools/uad_engine_probe.cpp
//           -o uad_engine_probe
// Builds the same on Linux (POSIX sockets); the CMake target is uad_engine_probe.

#include "UadProto.h"

#include "JsonTree.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

int64_t nowMs()
{
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return duration_cast<milliseconds>(steady_clock::now() - t0).count();
}

bool sendAll(int fd, const std::string& s)
{
    std::size_t off = 0;
    while (off < s.size()) {
        const ssize_t n = ::send(fd, s.data() + off, s.size() - off, 0);
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

// Child keys of a container reply: data.children's names.
std::vector<std::string> childKeys(const std::string& json)
{
    std::vector<std::string> keys;
    wdl_json_parser p;
    const wdl_json_element* root = p.parse(json.c_str(), static_cast<int>(json.size()));
    JsonTreeGuard guard{p, root};
    if (!root || !root->is_object()) return keys;
    const wdl_json_element* data = root->get_item_by_name("data");
    if (!data || !data->is_object()) return keys;
    const wdl_json_element* ch = data->get_item_by_name("children");
    if (!ch || !ch->is_object()) return keys;
    for (int i = 0; ch->enum_item(i); ++i)
        if (const char* k = ch->enum_item_name(i)) keys.emplace_back(k);
    return keys;
}

// A short one-line description of a reply's properties, for the console.
std::string propSummary(const std::string& json)
{
    std::string out;
    wdl_json_parser p;
    const wdl_json_element* root = p.parse(json.c_str(), static_cast<int>(json.size()));
    JsonTreeGuard guard{p, root};
    if (!root || !root->is_object()) return out;
    const wdl_json_element* data = root->get_item_by_name("data");
    if (!data) return out;
    if (!data->is_object()) {
        const char* v = data->get_string_value(true);
        return v ? std::string("= ") + v : out;
    }
    const wdl_json_element* props = data->get_item_by_name("properties");
    if (!props || !props->is_object()) return out;
    for (int i = 0; props->enum_item(i) && i < 12; ++i) {
        const wdl_json_element* pr = props->enum_item(i);
        const char* name = props->enum_item_name(i);
        const char* val = pr && pr->is_object() ? pr->get_string_by_name("value", true) : nullptr;
        out += std::string(name ? name : "?") + "=" + (val ? val : "…") + " ";
    }
    return out;
}

}  // namespace

int main(int argc, char** argv)
{
    std::string host = "127.0.0.1", outPath = "uad_tree.log";
    int port = uad::kPort, depth = 6, maxGets = 4000, secs = 20;
    std::vector<std::string> subs;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--host") host = next();
        else if (a == "--port") port = std::atoi(next().c_str());
        else if (a == "--depth") depth = std::atoi(next().c_str());
        else if (a == "--max") maxGets = std::atoi(next().c_str());
        else if (a == "--sub") subs.push_back(next());
        else if (a == "--secs") secs = std::atoi(next().c_str());
        else if (a == "--out") outPath = next();
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<uint16_t>(port));
    if (fd < 0 || ::inet_pton(AF_INET, host.c_str(), &sa.sin_addr) != 1
        || ::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
        std::fprintf(stderr, "cannot connect to %s:%d (is UAD Console / the UA Mixer Engine running?)\n",
                     host.c_str(), port);
        return 1;
    }
    FILE* log = std::fopen(outPath.c_str(), "w");
    std::printf("connected to %s:%d, log: %s (read only: get/subscribe, never set)\n",
                host.c_str(), port, outPath.c_str());

    uad::Framer framer;
    std::deque<std::pair<std::string, int>> todo{{"/devices", 0}};
    std::set<std::string> asked;
    struct Ask { int depth; int64_t sentMs; };
    std::map<std::string, Ask> waiting;        // asked and not answered yet
    int timeouts = 0;
    int gets = 0, replies = 0, updates = 0;
    const int kWindow = 8;                     // gets in flight at once: gentle on the engine
    bool walking = true, subscribed = false;
    int64_t walkEnd = 0;
    char buf[65536];

    auto record = [&](const std::string& m, const char* kind) {
        if (log) { std::fprintf(log, "%8lld %s %s\n", static_cast<long long>(nowMs()), kind, m.c_str()); }
    };

    while (true) {
        // Keep up to kWindow gets in flight while walking.
        while (walking && static_cast<int>(waiting.size()) < kWindow && !todo.empty() && gets < maxGets) {
            auto [path, d] = todo.front();
            todo.pop_front();
            if (!asked.insert(path).second) continue;
            if (!sendAll(fd, uad::get(path))) { std::fprintf(stderr, "send failed\n"); return 1; }
            waiting[path] = Ask{d, nowMs()};
            ++gets;
        }
        if (walking && waiting.empty() && (todo.empty() || gets >= maxGets)) {
            walking = false;
            walkEnd = nowMs();
            std::printf("walk done: %d gets, %d replies%s\n", gets, replies,
                        gets >= maxGets ? " (stopped at --max)" : "");
            for (const auto& s : subs) {
                sendAll(fd, uad::subscribe(s));
                std::printf("subscribed %s\n", s.c_str());
            }
            subscribed = true;
        }
        if (!walking && (subs.empty() || nowMs() - walkEnd > int64_t(secs) * 1000)) break;

        // A get the engine never answers (or answers under another path) must not
        // stall the walk.
        for (auto it = waiting.begin(); it != waiting.end();) {
            if (nowMs() - it->second.sentMs > 3000) {
                std::printf("%-60s (no reply)\n", it->first.c_str());
                it = waiting.erase(it);
                ++timeouts;
            } else {
                ++it;
            }
        }
        pollfd pf{fd, POLLIN, 0};
        const int pr = ::poll(&pf, 1, 500);
        if (pr < 0) break;
        if (pr == 0) {
            if (walking && nowMs() > 30000 && replies == 0) {
                std::fprintf(stderr, "no answer from the engine after 30 s\n");
                break;
            }
            continue;
        }
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) { std::fprintf(stderr, "connection closed by the engine\n"); break; }
        std::vector<std::string> msgs;
        framer.feed(buf, static_cast<std::size_t>(n), msgs);
        for (const auto& m : msgs) {
            const std::string path = uad::replyPath(m);
            auto it = waiting.find(path);
            // The engine may answer a property under ".../value" or with a slash.
            for (const char* suffix : {"/value", "/"})
                if (it == waiting.end() && path.size() > std::strlen(suffix)
                    && path.compare(path.size() - std::strlen(suffix), std::string::npos, suffix) == 0)
                    it = waiting.find(path.substr(0, path.size() - std::strlen(suffix)));
            if (it != waiting.end()) {
                ++replies;
                record(m, "GET");
                const int d = it->second.depth;
                waiting.erase(it);
                std::printf("%-60s %s\n", path.c_str(), propSummary(m).substr(0, 150).c_str());
                if (d < depth)
                    for (const auto& k : childKeys(m)) todo.emplace_back(uad::child(path, k), d + 1);
            } else {
                ++updates;
                record(m, subscribed ? "UPD" : "???");
                if (subscribed) std::printf("update %s %s\n", path.c_str(), propSummary(m).substr(0, 100).c_str());
            }
        }
    }
    std::printf("\n%d gets, %d replies, %d without reply, %d other messages (subscription updates). Log: %s\n",
                gets, replies, timeouts, updates, outPath.c_str());
    if (log) std::fclose(log);
    ::close(fd);
    return 0;
}
