// UadProto: NUL framing over a TCP stream, commands, paths, the reply's path.

#include "UadProto.h"

#include <cstdio>
#include <string>

static int g_fail = 0;
#define EXPECT(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

int main()
{
    // ---- commands ----------------------------------------------------------------
    {
        const std::string g = uad::get("/devices");
        EXPECT(g.size() == 13 && g.back() == '\0' && g.substr(0, 12) == "get /devices");
        EXPECT(uad::subscribe("/devices/0/DeviceOnline") == std::string("subscribe /devices/0/DeviceOnline", 33) + '\0');
        const std::string s = uad::set("/devices/0/inputs/1/FaderLevelTapered/value/", "0.5");
        EXPECT(s == std::string("set /devices/0/inputs/1/FaderLevelTapered/value/ 0.5") + '\0');
        EXPECT(uad::child("/devices", "0") == "/devices/0");
        EXPECT(uad::child("/devices/0/", "inputs") == "/devices/0/inputs");
        EXPECT(uad::child("", "x") == "/x");
    }

    // ---- framing ------------------------------------------------------------------
    {
        uad::Framer f;
        std::vector<std::string> out;
        const std::string a = "{\"path\":\"/devices\"}";
        const std::string b = "{\"path\":\"/devices/0/DeviceOnline/value\",\"data\":true}";
        std::string wire = a + '\0' + b + '\0';
        // split at every possible point: the messages always come out whole
        for (std::size_t cut = 0; cut <= wire.size(); ++cut) {
            uad::Framer g;
            std::vector<std::string> o;
            g.feed(wire.data(), cut, o);
            g.feed(wire.data() + cut, wire.size() - cut, o);
            EXPECT(o.size() == 2 && o[0] == a && o[1] == b);
            EXPECT(g.pending() == 0);
        }
        // empty messages (two NULs in a row) are skipped
        const char twoNul[] = {'x', '\0', '\0', 'y', '\0'};
        f.feed(twoNul, sizeof(twoNul), out);
        EXPECT(out.size() == 2 && out[0] == "x" && out[1] == "y");
        // a message never closed waits
        out.clear();
        f.feed("abc", 3, out);
        EXPECT(out.empty() && f.pending() == 3);
        f.reset();
        EXPECT(f.pending() == 0);
        // a runaway message is dropped, the stream resynchronises at the next NUL
        std::string big(uad::kMaxMessage + 10, 'z');
        big.push_back('\0');
        big += "ok";
        big.push_back('\0');
        out.clear();
        EXPECT(!f.feed(big.data(), big.size(), out));
        EXPECT(out.size() == 1 && out[0] == "ok");
    }

    // ---- reply path -------------------------------------------------------------
    {
        EXPECT(uad::replyPath(R"({"path": "/devices", "data": {"properties": {}}})") == "/devices");
        EXPECT(uad::replyPath(R"({"data":1,"path":"/devices/0/inputs/3/Name/value"})") == "/devices/0/inputs/3/Name/value");
        EXPECT(uad::replyPath(R"({"path":"/a\"b"})") == "/a\"b");
        EXPECT(uad::replyPath(R"({"data":1})").empty());
        EXPECT(uad::replyPath(R"({"path": 5})").empty());
        EXPECT(uad::replyPath("").empty());
    }

    if (g_fail) std::printf("%d failure(s)\n", g_fail);
    else        std::printf("uad_proto: all passed\n");
    return g_fail ? 1 : 0;
}
