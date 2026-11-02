// UadClient and UadSide over a real socket: a small TCP server on localhost plays
// the UA Mixer Engine with the virtual engine (UadSim.h, the user's Twin MkII).
// Connect, walk, subscribe, paint the virtual UF8, a fader write arriving at the
// engine, a push from the engine moving the motor, the engine going away and
// coming back (reconnect and a fresh walk), and a clean stop.
// POSIX only (the test); the client itself builds on Windows too.

#include "UadClient.h"
#include "UadProto.h"
#include "UadSim.h"
#include "Uf8Sim.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

static int g_fail = 0;
#define EXPECT(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// The engine on a socket. One client at a time; serve() returns when it leaves or
// when kill() closes it.
struct Server {
    uadsim::Engine  eng = uadsim::twinMkII();
    std::mutex      mx;
    int             lfd = -1, cfd = -1;
    std::uint16_t   port = 0;
    std::atomic<bool> quit{false};
    std::thread     th;

    Server()
    {
        lfd = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = 0;
        ::bind(lfd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
        socklen_t len = sizeof(sa);
        ::getsockname(lfd, reinterpret_cast<sockaddr*>(&sa), &len);
        port = ntohs(sa.sin_port);
        ::listen(lfd, 4);
        th = std::thread([this] { run(); });
    }
    ~Server()
    {
        quit.store(true);
        kill();
        ::shutdown(lfd, SHUT_RDWR);
        ::close(lfd);
        th.join();
    }
    void sendMsg(const std::string& m)
    {
        std::string w = m;
        w.push_back('\0');
        if (cfd >= 0) (void)!::send(cfd, w.data(), w.size(), MSG_NOSIGNAL);
    }
    void run()
    {
        while (!quit.load()) {
            pollfd pl{lfd, POLLIN, 0};
            if (::poll(&pl, 1, 50) <= 0) continue;
            const int c = ::accept(lfd, nullptr, nullptr);
            if (c < 0) continue;
            { std::lock_guard<std::mutex> lk(mx); cfd = c; }
            uad::Framer fr;
            char buf[8192];
            while (!quit.load()) {
                pollfd p{c, POLLIN, 0};
                const int r = ::poll(&p, 1, 50);
                if (r < 0) break;
                if (r == 0) { std::lock_guard<std::mutex> lk(mx); if (cfd < 0) break; continue; }
                const ssize_t n = ::recv(c, buf, sizeof(buf), 0);
                if (n <= 0) break;
                std::vector<std::string> cmds;
                fr.feed(buf, static_cast<std::size_t>(n), cmds);
                std::lock_guard<std::mutex> lk(mx);
                for (const auto& cmd : cmds)
                    for (const auto& reply : eng.handle(cmd)) sendMsg(reply);
            }
            std::lock_guard<std::mutex> lk(mx);
            if (cfd >= 0) ::close(cfd);
            cfd = -1;
        }
    }
    // A change made in the Console: pushed to the client if it subscribed.
    void console(const std::string& prop, const std::string& v)
    {
        std::lock_guard<std::mutex> lk(mx);
        for (const auto& m : eng.change(prop, v)) sendMsg(m);
    }
    void kill()
    {
        std::lock_guard<std::mutex> lk(mx);
        if (cfd >= 0) { ::shutdown(cfd, SHUT_RDWR); }
    }
    std::size_t sets()
    {
        std::lock_guard<std::mutex> lk(mx);
        return eng.sets.size();
    }
    std::string lastSet()
    {
        std::lock_guard<std::mutex> lk(mx);
        return eng.sets.empty() ? "" : eng.sets.back();
    }
};

// Ticks the side (and paints the virtual UF8) until `done` or the deadline.
template <class F>
bool until(uadside::Side& side, uf8sim::Uf8& u, F done, int ms = 4000)
{
    const int64_t end = nowMs() + ms;
    while (nowMs() < end) {
        std::vector<uadside::Frame> f;
        side.tick(nowMs(), true, f);
        for (auto& x : f) u.feed(x);
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    return false;
}

}  // namespace

int main()
{
    // ---- nothing listening: offline on the surface, no crash, retries quietly --------
    {
        uadside::Side side;
        uf8sim::Uf8 u;
        side.enable(true, 1);                       // port 1: refused
        until(side, u, [] { return false; }, 300);
        EXPECT(!side.status().connected && side.status().running);
        EXPECT(u.upper[0] == "UAD" && u.lower[0] == "offline");
        side.enable(false);
        EXPECT(!side.status().running);
    }

    Server srv;
    uadside::Side side;
    uf8sim::Uf8 u;
    side.enable(true, srv.port);

    // ---- connect, walk, subscribe, paint ---------------------------------------------------
    EXPECT(until(side, u, [&] { return u.upper[0] == "UAD 1" && u.upper[2] == "AVALON"; }));
    // the first names show while the walk still reads the last channels
    EXPECT(until(side, u, [&] { return side.status().channels == 14; }));
    const auto st = side.status();
    EXPECT(st.connected && st.online && st.device == "Apollo Twin MkII" && st.channels == 14);
    EXPECT(srv.sets() == 0);                        // reading writes nothing

    // ---- a fader on the UF8 writes the Console ----------------------------------------
    {
        std::vector<uadside::Frame> f;
        EXPECT(side.fromUf8(uf8sim::touch(3, true), nowMs(), f));
        EXPECT(side.fromUf8(uf8sim::fader(3, 15583), nowMs(), f));
        EXPECT(!f.empty());                          // limp + echo for the UF8
        EXPECT(until(side, u, [&] { return srv.lastSet() == "set /devices/0/inputs/3/FaderLevelTapered/value 1"; }));
        EXPECT(side.fromUf8(uf8sim::touch(3, false), nowMs(), f));
        // a layer key is not the layer's
        EXPECT(!side.fromUf8(uf8sim::key(0x40, true), nowMs(), f));
    }

    // ---- the Console moves a fader: the motor follows ----------------------------------
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));   // past the hand hold
        srv.console("/devices/0/inputs/5/FaderLevelTapered", "0.25");
        EXPECT(until(side, u, [&] { return u.motorPb[5] == static_cast<int>(0.25 * 15583 + 0.5); }));
        srv.console("/devices/0/inputs/2/meters/0/MeterLevel", "-6.0");
        EXPECT(until(side, u, [&] { return u.vu[4] > 0; }));
    }

    // ---- the engine goes away and comes back: reconnect, read the tree again ------------
    {
        // The client reconnects at once, so "disconnected" may last less than a
        // tick: wait for the next connection instead.
        const std::uint32_t before = side.status().connection;
        srv.kill();
        EXPECT(until(side, u, [&] { return side.status().connection > before; }, 4000));
        srv.console("/devices/0/inputs/2/Name", uadsim::q("AVALON 737"));   // changed while away
        EXPECT(until(side, u, [&] { return side.status().connected && u.upper[2] == "AVALON 7"; }, 6000));
        EXPECT(until(side, u, [&] { return side.status().channels == 14; }));
    }

    side.enable(false);
    EXPECT(!side.status().running && !side.status().connected);

    if (g_fail) std::printf("%d failure(s)\n", g_fail);
    else        std::printf("uad_client: all passed\n");
    return g_fail ? 1 : 0;
}
