#if defined(_WIN32)
  // ⛔ winsock2.h BEFORE anything that drags in windows.h (UadModel pulls WDL in
  // through JsonTree only in its .cpp, but keep the RmeManager order anyway).
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/select.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
#endif

#include "UadClient.h"

#include "UadProto.h"
#include "Uf8Fader.h"
#include "Uf8Sim.h"   // the UF8's input frames, for the virtual UF8 in Settings

#include <algorithm>
#include <chrono>
#include <cmath>

#if defined(_WIN32)
  using uad_socket_t = SOCKET;
  static constexpr uad_socket_t kNoSocket = INVALID_SOCKET;
  #define UAD_CLOSE closesocket
#else
  using uad_socket_t = int;
  static constexpr uad_socket_t kNoSocket = -1;
  #define UAD_CLOSE ::close
#endif

namespace uad {

namespace {

bool sendAll(uad_socket_t s, const std::string& data)
{
    std::size_t off = 0;
    while (off < data.size()) {
#if defined(_WIN32)
        const int n = ::send(s, data.data() + off, static_cast<int>(data.size() - off), 0);
#elif defined(MSG_NOSIGNAL)
        const ssize_t n = ::send(s, data.data() + off, data.size() - off, MSG_NOSIGNAL);
#else
        const ssize_t n = ::send(s, data.data() + off, data.size() - off, 0);   // SO_NOSIGPIPE set
#endif
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace

void Client::start(std::uint16_t port)
{
    if (th_.joinable()) return;
    port_ = port;
    stop_.store(false);
    th_ = std::thread([this] { loop_(); });
}

void Client::stop()
{
    if (!th_.joinable()) return;
    stop_.store(true);
    th_.join();
    connected_.store(false);
}

void Client::send(std::string cmd)
{
    if (!connected_.load()) return;
    std::lock_guard<std::mutex> lk(mx_);
    out_.push_back(std::move(cmd));
}

std::vector<std::string> Client::take()
{
    std::lock_guard<std::mutex> lk(mx_);
    std::vector<std::string> v;
    v.swap(in_);
    return v;
}

void Client::loop_()
{
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    using clock = std::chrono::steady_clock;
    int backoffMs = 500;
    while (!stop_.load()) {
        uad_socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port_);
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);      // localhost only, see the header
        bool ok = s != kNoSocket
               && ::connect(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0;
        if (!ok) {
            if (s != kNoSocket) UAD_CLOSE(s);
            const auto until = clock::now() + std::chrono::milliseconds(backoffMs);
            while (!stop_.load() && clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            backoffMs = std::min(backoffMs * 2, 8000);
            continue;
        }
        backoffMs = 500;
        int one = 1;
        ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
#if defined(SO_NOSIGPIPE)
        ::setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        {
            std::lock_guard<std::mutex> lk(mx_);
            out_.clear();
        }
        Framer framer;
        connected_.store(true);
        conn_.fetch_add(1);
        char buf[65536];
        while (!stop_.load()) {
            std::vector<std::string> out;
            {
                std::lock_guard<std::mutex> lk(mx_);
                out.swap(out_);
            }
            bool alive = true;
            for (const auto& c : out) if (!sendAll(s, c)) { alive = false; break; }
            if (!alive) break;
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(s, &rd);
            timeval tv{0, 10000};                          // 10 ms: writes leave quickly
            const int r = ::select(static_cast<int>(s) + 1, &rd, nullptr, nullptr, &tv);
            if (r < 0) break;
            if (r == 0) continue;
#if defined(_WIN32)
            const int n = ::recv(s, buf, static_cast<int>(sizeof(buf)), 0);
#else
            const ssize_t n = ::recv(s, buf, sizeof(buf), 0);
#endif
            if (n <= 0) break;
            std::vector<std::string> msgs;
            framer.feed(buf, static_cast<std::size_t>(n), msgs);
            if (!msgs.empty()) {
                std::lock_guard<std::mutex> lk(mx_);
                for (auto& m : msgs) in_.push_back(std::move(m));
            }
        }
        connected_.store(false);
        UAD_CLOSE(s);
    }
#if defined(_WIN32)
    WSACleanup();
#endif
}

}  // namespace uad

namespace uadside {

void Side::enable(bool on, std::uint16_t port)
{
    if (on) {
        client_.start(port);
        return;
    }
    if (!client_.running()) return;          // called every tick: nothing to stop
    client_.stop();
    std::lock_guard<std::mutex> lk(mx_);
    model_ = uad::Model{};
    subscribed_.clear();
    conn_ = 0;
}

bool Side::fromUf8(const uf8frames::Frame& f, std::int64_t nowMs, std::vector<Frame>& uf8)
{
    std::lock_guard<std::mutex> lk(mx_);
    uadsurf::Out o;
    const bool mine = surf_.fromUf8(f, nowMs, o);
    for (auto& c : o.cmds) { ++sent_; client_.send(std::move(c)); }
    for (auto& x : o.uf8) uf8.push_back(std::move(x));
    return mine;
}

void Side::tick(std::int64_t nowMs, bool shown, std::vector<Frame>& uf8)
{
    std::lock_guard<std::mutex> lk(mx_);
    // A new connection: the engine's tree from the top, subscriptions anew.
    if (client_.connected() && client_.connection() != conn_) {
        conn_ = client_.connection();
        model_ = uad::Model{};
        subscribed_.clear();
        surf_.invalidate();
        client_.send(uad::get("/devices"));
    }
    for (const auto& m : client_.take())
        for (const auto& next : model_.onMessage(m)) client_.send(uad::get(next));
    if (client_.connected()) {
        surf_.refresh();
        const auto wanted = surf_.wantedSubscriptions();
        const std::set<std::string> want(wanted.begin(), wanted.end());
        for (const auto& p : wanted)
            if (subscribed_.insert(p).second) client_.send(uad::subscribe(p));
        // Meters stream all the time: drop those of strips no longer on show.
        for (auto it = subscribed_.begin(); it != subscribed_.end();) {
            if (it->find("/meters/") != std::string::npos && !want.count(*it)) {
                client_.send(uad::command("unsubscribe", *it));
                it = subscribed_.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (!shown) {
        // No UF8 to paint (or another layer on it): time still has to pass for
        // the virtual UF8's touches and the writes waiting for their window.
        uadsurf::Out o;
        surf_.service(nowMs, o);
        for (auto& c : o.cmds) { ++sent_; client_.send(std::move(c)); }
        return;
    }
    uadsurf::Out o;
    surf_.paint(nowMs, o);
    for (auto& c : o.cmds) { ++sent_; client_.send(std::move(c)); }
    for (auto& x : o.uf8) uf8.push_back(std::move(x));
}

void Side::virtualFrame_(const uf8frames::Frame& f, std::int64_t nowMs, bool offlineTest)
{
    std::lock_guard<std::mutex> lk(mx_);
    if (!client_.connected()) return;
    surf_.refresh();
    surf_.setOfflineWrites(offlineTest);
    uadsurf::Out o;
    surf_.fromUf8(f, nowMs, o);
    surf_.setOfflineWrites(false);
    for (auto& c : o.cmds) { ++sent_; client_.send(std::move(c)); }
    // o.uf8 (limp, echo) is for a real fader under a hand; the virtual one has none.
}

void Side::virtualTouch(int strip, bool on, std::int64_t nowMs, bool offlineTest)
{
    virtualFrame_(uf8sim::touch(strip, on), nowMs, offlineTest);
}

void Side::virtualFader(int strip, double tapered, std::int64_t nowMs, bool offlineTest)
{
    const int pb = static_cast<int>(std::lround(std::clamp(tapered, 0.0, 1.0) * uf8fader::kPbMax));
    virtualFrame_(uf8sim::fader(strip, pb, /*hiBit*/ true), nowMs, offlineTest);
}

void Side::virtualKey(std::uint8_t id, bool pressed, std::int64_t nowMs, bool offlineTest)
{
    virtualFrame_(uf8sim::key(id, pressed), nowMs, offlineTest);
}

void Side::virtualPot(int pot, int detents, std::int64_t nowMs, bool offlineTest)
{
    virtualFrame_(uf8sim::vpot(pot, std::clamp(detents, -31, 31)), nowMs, offlineTest);
}

void Side::invalidate()
{
    std::lock_guard<std::mutex> lk(mx_);
    surf_.invalidate();
}

std::uint32_t Side::stripRgb(int s)
{
    std::lock_guard<std::mutex> lk(mx_);
    return surf_.stripRgb(s);
}

Status Side::status()
{
    std::lock_guard<std::mutex> lk(mx_);
    Status st;
    st.running = client_.running();
    st.connected = client_.connected();
    st.connection = client_.connection();
    st.online = model_.online();
    if (!model_.devices.empty()) st.device = model_.devices[0].name;
    const std::vector<uad::Strip> strips = model_.strips();
    st.channels = static_cast<int>(strips.size());
    st.sent     = sent_;
    st.banks    = strips.empty() ? 0 : (st.channels + 7) / 8;
    st.bank     = surf_.bank();
    st.softBank = surf_.softBank();
    st.mode     = static_cast<int>(surf_.mode());
    st.send     = surf_.send();
    st.talkbackOn = model_.talkbackOn;
    for (const uad::Device& D : model_.devices) {
        const int o = D.monitorOutput();
        if (o < 0) continue;
        const uad::Output& mon = D.outputs[static_cast<std::size_t>(o)];
        st.hasMonitor   = true;
        st.monitorMute  = mon.mute;
        st.monitorDim   = mon.dimOn;
        st.monitorMono  = mon.mixToMono;
        st.monitorDb    = mon.monitorDb;
        st.monitorMinDb = mon.monitorMinDb;
        st.monitorMaxDb = mon.monitorMaxDb;
        break;
    }
    for (int i = 0; i < 8; ++i) {
        const std::size_t idx = static_cast<std::size_t>(st.bank * 8 + i);
        if (idx < strips.size()) st.names[i] = model_.name(strips[idx]);
    }
    const int sel = surf_.selected();
    if (sel >= 0 && sel < st.channels) {
        const uad::Strip& s = strips[static_cast<std::size_t>(sel)];
        st.selectedName = model_.name(s);
        const uad::Input* in = model_.input(s);
        st.selectedPreamp = in && in->hasPreamp;
    }
    st.list.reserve(strips.size());
    for (const uad::Strip& s : strips) {
        Status::Channel ch;
        ch.name = model_.name(s);
        if (const uad::Input* in = model_.input(s)) {
            ch.type = in->ioType;
            ch.stereo = in->stereo;
            ch.preamp = in->hasPreamp;
            ch.mute = in->mute;
            ch.solo = in->solo;
            ch.phantom = in->hasPreamp && in->preamp.phantom;
            ch.faderDb = in->faderDb;
            ch.faderTapered = in->faderTapered;
            ch.pan = in->pan;
            ch.gainDb = in->preamp.gainDb;
            if (st.send >= 0 && st.send < static_cast<int>(in->sends.size())) {
                const uad::Send& sd = in->sends[static_cast<std::size_t>(st.send)];
                ch.hasSend = true;
                ch.sendBypass = sd.bypass;
                ch.sendDb = sd.gainDb;
                ch.sendTapered = sd.gainTapered;
            }
        } else if (const uad::Aux* ax = model_.aux(s)) {
            ch.type = "AUX";
            ch.aux = true;
            ch.stereo = true;
            ch.mute = ax->mute;
            ch.faderDb = ax->faderDb;
            ch.faderTapered = ax->faderTapered;
        }
        st.list.push_back(std::move(ch));
    }
    for (const uad::Strip& s : strips) {
        if (const uad::Input* in = model_.input(s)) {
            for (std::size_t k = 0; k < in->sends.size() && k < 8; ++k)
                st.sendNames[k] = in->sends[k].name.empty() ? in->sends[k].id : in->sends[k].name;
            break;
        }
    }
    return st;
}

}  // namespace uadside
