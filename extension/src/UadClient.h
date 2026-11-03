#pragma once
//
// UadClient: the socket to the UA Mixer Engine, and UadSide, the UAD Console layer
// as one object (docs/uad-console-plan.md, Bausteine D and E).
//
// Client is a pipe, nothing more: a worker thread connects to 127.0.0.1:4710,
// sends what it is given, splits what arrives at NUL (UadProto's Framer) and
// hands the messages over. It reconnects on its own (0.5 s doubling to 8 s), and
// every new connection bumps connection() so the owner knows to read the tree
// again. Commands queued while disconnected are dropped: a write for a mixer state
// nobody has seen again must not land later. Only localhost: the engine listens
// on every interface with no authentication (measured), and Rea-Sixty does not
// reach for other machines.
//
// Side owns the client, the model (UadModel) and the surface (UadSurface) under
// one mutex: main.cpp calls fromUf8 from the USB thread and tick from the timer.
// tick drains the engine's messages into the model, walks the tree after every
// (re)connection, keeps the subscriptions the surface wants (meters of the bank on
// show only), and paints the UF8 while the layer is shown. The model is kept up to
// date while another layer is shown, so coming back shows the Console at once.
//
#include "UadModel.h"
#include "UadSurface.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace uad {

class Client {
public:
    Client() = default;
    ~Client() { stop(); }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    void start(std::uint16_t port = 4710);
    void stop();
    bool running() const { return th_.joinable(); }

    void send(std::string cmdWithNul);          // dropped while disconnected
    std::vector<std::string> take();            // messages since the last call
    bool connected() const { return connected_.load(); }
    std::uint32_t connection() const { return conn_.load(); }

private:
    void loop_();

    std::thread                th_;
    std::atomic<bool>          stop_{false};
    std::atomic<bool>          connected_{false};
    std::atomic<std::uint32_t> conn_{0};
    std::uint16_t              port_ = 4710;
    std::mutex                 mx_;
    std::vector<std::string>   out_, in_;
};

}  // namespace uad

namespace uadside {

using Frame = std::vector<std::uint8_t>;

struct Status {
    bool        running = false, connected = false, online = false;
    std::uint32_t connection = 0;   // bumps with every new connection
    std::string device;          // first device's name
    int         channels = 0;
    // What the surface shows now, for the Settings picture of the UF8.
    int         bank = 0, banks = 0;   // current bank of 8, number of banks
    int         softBank = 0;          // 0 V-POT (modes), 1 Monitor, 2 Preamp
    int         mode = 0;              // uadsurf::PotMode: 0 Pan, 1 Send, 2 Preamp, 3 Plugin
    int         quick = 0;             // 0 QUICK 1 (all channels), 1 QUICK 2 (the chosen ones)
    int         uf8Ch[8] = {-1, -1, -1, -1, -1, -1, -1, -1};   // fader -> index into list
    // PLUG-IN mode: what each V-Pot shows (Surface::pluginView).
    int         pluginSlot = -1, paramPage = 0;
    std::string pluginSlotName;        // the open slot's plug-in, empty = overview
    std::string slotNames[8];          // the SEL channel's insert slots, "" = empty
    struct Pot { std::string label, value; double norm = 0.0; bool has = false, on = false; };
    Pot         pots[8];
    int         send = 0;              // the send SEND/PLUG-IN picked
    std::string selectedName;          // the SEL channel, empty = none
    bool        selectedPreamp = false;
    std::string names[8];              // the strips of the current bank, empty = none
    std::string sendNames[8];          // the sends of the first input (AUX 1, ...)
    // Every channel in Console order, for the Console picture under the UF8.
    struct Channel {
        std::string name, type;        // type: MIC, LINE, ADAT, VIRT, AUX, ...
        std::string id;                // uad::Model::stripId, for the QUICK 2 list
        bool   aux = false, stereo = false, preamp = false;
        bool   mute = false, solo = false, phantom = false;
        double faderDb = -144.0, faderTapered = 0.0, pan = 0.0, gainDb = 0.0;
        // The send SEND/PLUG-IN picked (Status::send), when the channel has it.
        bool   hasSend = false, sendBypass = false;
        double sendDb = -144.0, sendTapered = 0.0;
    };
    // The monitor output the CHANNEL encoder turns (first device that has one).
    bool   hasMonitor = false, monitorMute = false, monitorDim = false, monitorMono = false;
    bool   talkbackOn = false;
    double monitorDb = -96.0, monitorMinDb = -96.0, monitorMaxDb = 0.0;
    std::vector<Channel> list;
    std::uint32_t sent = 0;            // set commands sent to the engine so far
};

class Side {
public:
    Side() = default;
    ~Side() { enable(false); }

    // Start or stop the connection. On while any UF8 layer is a UAD Console layer.
    void enable(bool on, std::uint16_t port = 4710);

    // USB thread. True = the layer took the frame; the frames in `uf8` go to the left
    // unit. False = the caller's normal path (LAYER 1..3, 360, frames it does not know).
    bool fromUf8(const uf8frames::Frame& f, std::int64_t nowMs, std::vector<Frame>& uf8);

    // Timer. `shown`: the UF8 is on the UAD layer, paint it (frames into `uf8`).
    void tick(std::int64_t nowMs, bool shown, std::vector<Frame>& uf8);
    void invalidate();                           // entering the layer

    // Settings' virtual UF8 (main thread): inputs built byte for byte as the UF8
    // sends them (Uf8Sim) and run through the same surface, allowlist and write
    // gate. offlineTest: write while the interface is offline (never 48V).
    void virtualTouch(int strip, bool on, std::int64_t nowMs, bool offlineTest);
    void virtualFader(int strip, double tapered, std::int64_t nowMs, bool offlineTest);
    void virtualKey(std::uint8_t id, bool pressed, std::int64_t nowMs, bool offlineTest);
    void virtualPot(int pot, int detents, std::int64_t nowMs, bool offlineTest);
    std::uint32_t stripRgb(int s);
    Status status();
    // QUICK 2's channels (bindings.json); the timer passes them every tick.
    void setQuick2(const std::vector<std::string>& ids);

private:
    void virtualFrame_(const uf8frames::Frame& f, std::int64_t nowMs, bool offlineTest);

    std::mutex            mx_;
    uad::Client           client_;
    uad::Model            model_;
    uadsurf::Surface      surf_{model_};
    std::set<std::string> subscribed_;
    std::uint32_t         conn_ = 0;
    std::uint32_t         sent_ = 0;   // set commands sent (Settings shows it)
};

}  // namespace uadside
