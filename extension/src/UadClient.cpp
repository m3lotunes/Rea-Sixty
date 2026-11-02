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

#include <algorithm>
#include <chrono>

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
    for (auto& c : o.cmds) client_.send(std::move(c));
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
    if (!shown) return;
    uadsurf::Out o;
    surf_.paint(nowMs, o);
    for (auto& c : o.cmds) client_.send(std::move(c));
    for (auto& x : o.uf8) uf8.push_back(std::move(x));
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
    st.channels = static_cast<int>(model_.strips().size());
    return st;
}

}  // namespace uadside
