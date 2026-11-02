//
// mcu_port_probe: what does a Mackie Control host send to a surface? Measurement
// B2 of docs/mcu-layer-plan.md, standalone, without REAPER and without the UF8.
//
// It creates a virtual CoreMIDI source + destination (default name "Rea-Sixty MCU
// Probe", fixed unique id) and logs every message the host sends, decoded with the
// same parser and shadow McuLayer.h gives the MCU layer. Point UA Midi Control (or
// any MCU host) at it in Mackie mode, use the host for a minute, and read the log.
//
// It sends NOTHING on its own: no fader, no key, nothing that could move a level on
// the host. With --answer it answers the MCU handshake (device query, host reply,
// version request), exactly as the layer will; without it the host gets silence,
// which shows whether the host needs the handshake at all.
//
// Usage:
//   mcu_port_probe [--name "Rea-Sixty MCU Probe"] [--uid 1381126001] [--secs 120]
//                  [--answer] [--log mcu_probe.log]
//
// Build on the Mac (no CMake needed, from the repository root):
//   clang++ -std=c++20 -O1 -Iextension/src extension/tools/mcu_port_probe.cpp
//           -framework CoreMIDI -framework CoreFoundation -o mcu_port_probe
// (one line)
// The CMake target mcu_port_probe builds the same on Apple only.
//
// See docs/mcu-probe-runbook.md for the measurement itself.

#include "McuLayer.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#if __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#endif

namespace {

std::atomic<bool> g_stop{false};
[[maybe_unused]] void onSignal(int) { g_stop.store(true); }

int64_t nowMs()
{
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return duration_cast<milliseconds>(steady_clock::now() - t0).count();
}

std::string hex(const mcu::Bytes& b)
{
    std::string s;
    char t[4];
    for (size_t i = 0; i < b.size(); ++i) {
        std::snprintf(t, sizeof(t), i ? " %02X" : "%02X", b[i]);
        s += t;
    }
    return s;
}

// MCU's name for a note, for the log.
std::string noteName(uint8_t n)
{
    static const char* const kGlobal[] = {
        "TRACK", "SEND", "PAN", "PLUGIN", "EQ", "INSTRUMENT", "BANK<", "BANK>",
        "CHANNEL<", "CHANNEL>", "FLIP", "GLOBAL", "NAME/VALUE", "SMPTE/BEATS",
        "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8",
        "GV MIDI", "GV INPUTS", "GV AUDIO", "GV INSTR", "GV AUX", "GV BUSSES", "GV OUTPUTS", "GV USER",
        "SHIFT", "OPTION", "CONTROL", "ALT/CMD",
        "READ", "WRITE", "TRIM", "TOUCH", "LATCH", "GROUP",
        "SAVE", "UNDO", "CANCEL", "ENTER",
        "MARKER", "NUDGE", "CYCLE", "DROP", "REPLACE", "CLICK", "SOLO(key)",
        "REW", "FF", "STOP", "PLAY", "REC",
        "UP", "DOWN", "LEFT", "RIGHT", "ZOOM", "SCRUB", "USER A", "USER B" };
    char b[32];
    if (n < 0x08)       std::snprintf(b, sizeof(b), "REC %d", n + 1);
    else if (n < 0x10)  std::snprintf(b, sizeof(b), "SOLO %d", n - 0x08 + 1);
    else if (n < 0x18)  std::snprintf(b, sizeof(b), "MUTE %d", n - 0x10 + 1);
    else if (n < 0x20)  std::snprintf(b, sizeof(b), "SELECT %d", n - 0x18 + 1);
    else if (n < 0x28)  std::snprintf(b, sizeof(b), "V-SELECT %d", n - 0x20 + 1);
    else if (n < 0x68)  return kGlobal[n - 0x28];
    else if (n < 0x70)  std::snprintf(b, sizeof(b), "TOUCH %d", n - 0x68 + 1);
    else if (n == 0x70) return "TOUCH MASTER";
    else if (n == 0x71) return "SMPTE led";
    else if (n == 0x72) return "BEATS led";
    else if (n == 0x73) return "RUDE SOLO led";
    else                std::snprintf(b, sizeof(b), "note 0x%02X", n);
    return b;
}

std::string describe(const mcu::Bytes& m)
{
    char b[160];
    if (m.empty()) return "";
    const uint8_t st = m[0], type = st & 0xF0, ch = st & 0x0F;
    if (st == 0xF0) {
        if (m.size() >= 7 && m[1] == 0 && m[2] == 0 && m[3] == 0x66) {
            const uint8_t cmd = m[5];
            const char* what =
                cmd == 0x00 ? "DEVICE QUERY" : cmd == 0x02 ? "HOST CONNECTION REPLY" :
                cmd == 0x0A ? "transport click" : cmd == 0x0B ? "LCD backlight" :
                cmd == 0x0C ? "touchless faders" : cmd == 0x0E ? "touch sensitivity" :
                cmd == 0x0F ? "GO OFFLINE" : cmd == 0x10 ? "timecode digits" :
                cmd == 0x11 ? "assignment digits" : cmd == 0x12 ? "LCD" :
                cmd == 0x13 ? "VERSION REQUEST" : cmd == 0x20 ? "meter mode (strip)" :
                cmd == 0x21 ? "meter mode (global)" : cmd == 0x61 ? "faders to minimum" :
                cmd == 0x62 ? "all LEDs off" : cmd == 0x63 ? "RESET" :
                cmd == 0x72 ? "scribble colours (X-Touch)" : "?";
            if (cmd == 0x12 && m.size() >= 8) {
                const std::string text(m.begin() + 7, m.end() - (m.back() == 0xF7 ? 1 : 0));
                std::snprintf(b, sizeof(b), "SYSEX model %02X: LCD @0x%02X (row %d, strip %d) \"%s\"",
                              m[4], m[6], m[6] >= 0x38 ? 2 : 1, (m[6] % 0x38) / 7 + 1, text.c_str());
            } else {
                std::snprintf(b, sizeof(b), "SYSEX model %02X: cmd %02X %s (%zu bytes)",
                              m[4], cmd, what, m.size());
            }
        } else {
            std::snprintf(b, sizeof(b), "SYSEX (not Mackie, %zu bytes)", m.size());
        }
        return b;
    }
    switch (type) {
        case 0x90:
        case 0x80: {
            const int v = m.size() > 2 ? m[2] : 0;
            const char* s = (type == 0x80 || v == 0) ? "off" : v == 1 ? "BLINK" : v == 0x7F ? "on" : "on(?)";
            std::snprintf(b, sizeof(b), "LED %-12s = %s (vel %d, ch %d)", noteName(m[1]).c_str(), s, v, ch + 1);
            return b;
        }
        case 0xE0:
            std::snprintf(b, sizeof(b), "FADER %s = %d", ch == 8 ? "MASTER" : std::to_string(ch + 1).c_str(),
                          m.size() > 2 ? ((m[1] & 0x7F) | ((m[2] & 0x7F) << 7)) : -1);
            return b;
        case 0xB0: {
            const int cc = m[1], v = m.size() > 2 ? m[2] : 0;
            if (cc >= 0x30 && cc <= 0x37)
                std::snprintf(b, sizeof(b), "RING %d = mode %d pos %d%s (ch %d)", cc - 0x30 + 1,
                              (v >> 4) & 3, v & 0x0F, (v & 0x40) ? " +centre" : "", ch + 1);
            else if (cc >= 0x40 && cc <= 0x49)
                std::snprintf(b, sizeof(b), "TIMECODE digit %d = 0x%02X (ch %d)", cc - 0x40, v, ch + 1);
            else if (cc == 0x4A || cc == 0x4B)
                std::snprintf(b, sizeof(b), "ASSIGNMENT %s = '%c' (ch %d)", cc == 0x4B ? "left" : "right",
                              mcu::assignChar(static_cast<uint8_t>(v)), ch + 1);
            else
                std::snprintf(b, sizeof(b), "CC %d = %d (ch %d)", cc, v, ch + 1);
            return b;
        }
        case 0xD0:
            if (m.size() < 2) return "METER (short)";
            std::snprintf(b, sizeof(b), "METER %s %d level 0x%X", ch == 0 ? "strip" : "MASTER(D1)",
                          (m[1] >> 4) + 1, m[1] & 0x0F);
            return b;
        default:
            std::snprintf(b, sizeof(b), "other (status %02X)", st);
            return b;
    }
}

// What the summary counts by: the kind of message, keyed so that one fader moving
// is one line and not a thousand.
std::string category(const mcu::Bytes& m)
{
    char b[48];
    const uint8_t st = m[0], type = st & 0xF0, ch = st & 0x0F;
    if (st == 0xF0) {
        if (m.size() >= 7 && m[1] == 0 && m[2] == 0 && m[3] == 0x66)
            std::snprintf(b, sizeof(b), "sysex model %02X cmd %02X", m[4], m[5]);
        else
            std::snprintf(b, sizeof(b), "sysex other");
        return b;
    }
    switch (type) {
        case 0x80: case 0x90: return "LED " + noteName(m.size() > 1 ? m[1] : 0);
        case 0xE0: std::snprintf(b, sizeof(b), "FADER ch %d", ch + 1); return b;
        case 0xB0:
            if (m.size() > 1 && m[1] >= 0x30 && m[1] <= 0x37) return "RING";
            if (m.size() > 1 && m[1] >= 0x40 && m[1] <= 0x49) return "TIMECODE";
            if (m.size() > 1 && (m[1] == 0x4A || m[1] == 0x4B)) return "ASSIGNMENT";
            std::snprintf(b, sizeof(b), "CC %d", m.size() > 1 ? m[1] : -1);
            return b;
        case 0xD0: std::snprintf(b, sizeof(b), "METER D%X", ch); return b;
        default: break;
    }
    std::snprintf(b, sizeof(b), "status %02X", st);
    return b;
}

struct Probe {
    std::mutex             mx;
    mcu::Parser            parser;
    mcu::Shadow            shadow;
    mcu::Identity          id;
    bool                   answer = false;
    FILE*                  log    = nullptr;
    std::map<std::string, int> counts;
    std::vector<mcu::Bytes> pendingReplies;

    void line(const char* fmt, const std::string& a, const std::string& b)
    {
        char t[1024];
        std::snprintf(t, sizeof(t), fmt, a.c_str(), b.c_str());
        std::printf("%s\n", t);
        if (log) { std::fprintf(log, "%s\n", t); std::fflush(log); }
    }

    // Core MIDI thread.
    void onBytes(const uint8_t* p, size_t n)
    {
        std::lock_guard<std::mutex> lk(mx);
        std::vector<mcu::Bytes> msgs;
        parser.feed(p, n, msgs);
        const int64_t t = nowMs();
        for (const auto& m : msgs) {
            const std::string d = describe(m);
            char ts[32];
            std::snprintf(ts, sizeof(ts), "%8lld ms  IN ", static_cast<long long>(t));
            line("%s%s", ts, hex(m) + "   " + d);
            ++counts[category(m)];
            std::vector<mcu::Bytes> reply;
            mcu::apply(shadow, m, t, id, reply);
            if (answer)
                for (auto& r : reply) pendingReplies.push_back(std::move(r));
        }
    }

    void summary()
    {
        std::lock_guard<std::mutex> lk(mx);
        line("%s%s", "\n==== summary ====", "");
        for (const auto& kv : counts) {
            char c[16];
            std::snprintf(c, sizeof(c), "%6d  ", kv.second);
            line("%s%s", c, kv.first);
        }
        line("%s%s", "\nLCD row 1: |", std::string(shadow.lcd.c.data(), mcu::kLcdCols) + "|");
        line("%s%s", "LCD row 2: |", std::string(shadow.lcd.c.data() + mcu::kLcdCols, mcu::kLcdCols) + "|");
        std::string f, r, l;
        for (int i = 0; i <= mcu::kStrips; ++i) {
            f += shadow.faderKnown[static_cast<size_t>(i)]
                 ? std::to_string(shadow.fader[static_cast<size_t>(i)]) + " " : "-- ";
        }
        for (int i = 0; i < mcu::kStrips; ++i) {
            char t[8];
            std::snprintf(t, sizeof(t), "%02X ", shadow.ring[static_cast<size_t>(i)]);
            r += t;
        }
        for (int n = 0; n < 128; ++n)
            if (shadow.led[static_cast<size_t>(n)] != mcu::Led::Off)
                l += noteName(static_cast<uint8_t>(n)) +
                     (shadow.led[static_cast<size_t>(n)] == mcu::Led::Blink ? "(blink)" : "") + ", ";
        line("%s%s", "faders 1-8, master: ", f);
        line("%s%s", "rings 1-8: ", r);
        line("%s%s", "LEDs on: ", l.empty() ? std::string("none") : l);
        line("%s%s", "assignment: ", std::string(shadow.assign.data(), 2));
    }
};

Probe g_probe;

}  // namespace

#if __APPLE__

static void readProc(const MIDIPacketList* list, void* /*refCon*/, void* /*srcConnRefCon*/)
{
    const MIDIPacket* pkt = &list->packet[0];
    for (UInt32 i = 0; i < list->numPackets; ++i) {
        g_probe.onBytes(pkt->data, pkt->length);
        pkt = MIDIPacketNext(pkt);
    }
}

static CFStringRef cfs(const std::string& s)
{
    return CFStringCreateWithCString(nullptr, s.c_str(), kCFStringEncodingUTF8);
}

int main(int argc, char** argv)
{
    std::string name = "Rea-Sixty MCU Probe", logPath = "mcu_probe.log";
    SInt32 uid = 1381126001;     // 'RS' + digits: any fixed value, not SSL's
    int secs = 120;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](void) -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--name") name = next();
        else if (a == "--uid") uid = static_cast<SInt32>(std::atol(next()));
        else if (a == "--secs") secs = std::atoi(next());
        else if (a == "--answer") g_probe.answer = true;
        else if (a == "--log") logPath = next();
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    g_probe.log = std::fopen(logPath.c_str(), "w");
    std::signal(SIGINT, onSignal);

    MIDIClientRef client = 0;
    MIDIEndpointRef src = 0, dst = 0;
    CFStringRef cn = cfs("mcu_port_probe");
    if (MIDIClientCreate(cn, nullptr, nullptr, &client) != noErr) { std::fprintf(stderr, "MIDIClientCreate failed\n"); return 1; }
    CFRelease(cn);
    CFStringRef n = cfs(name);
    // The destination is where the host sends to; the source is where it listens.
    // Same name on both, as hardware surfaces show up.
    if (MIDIDestinationCreate(client, n, readProc, nullptr, &dst) != noErr
        || MIDISourceCreate(client, n, &src) != noErr) {
        std::fprintf(stderr, "creating the endpoints failed\n");
        return 1;
    }
    CFRelease(n);
    // A fixed unique id, so a host that remembers ports by id finds this one again.
    // The destination gets uid + 1 (ids are per object).
    const OSStatus u1 = MIDIObjectSetIntegerProperty(src, kMIDIPropertyUniqueID, uid);
    const OSStatus u2 = MIDIObjectSetIntegerProperty(dst, kMIDIPropertyUniqueID, uid + 1);
    CFStringRef man = cfs("Rea-Sixty"), mod = cfs("MCU probe");
    MIDIObjectSetStringProperty(src, kMIDIPropertyManufacturer, man);
    MIDIObjectSetStringProperty(dst, kMIDIPropertyManufacturer, man);
    MIDIObjectSetStringProperty(src, kMIDIPropertyModel, mod);
    MIDIObjectSetStringProperty(dst, kMIDIPropertyModel, mod);
    CFRelease(man);
    CFRelease(mod);

    std::printf("\"%s\" is up (source uid %d%s, destination uid %d%s). Handshake answers: %s.\n"
                "Listening for %d s (Ctrl+C stops early). Log: %s\n\n",
                name.c_str(), static_cast<int>(uid), u1 == noErr ? "" : " NOT SET",
                static_cast<int>(uid + 1), u2 == noErr ? "" : " NOT SET",
                g_probe.answer ? "on" : "off", secs, logPath.c_str());

    const int64_t end = nowMs() + int64_t(secs) * 1000;
    while (!g_stop.load() && nowMs() < end) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
        std::vector<mcu::Bytes> out;
        {
            std::lock_guard<std::mutex> lk(g_probe.mx);
            out.swap(g_probe.pendingReplies);
        }
        for (const auto& r : out) {
            Byte buf[256];
            MIDIPacketList* list = reinterpret_cast<MIDIPacketList*>(buf);
            MIDIPacket* p = MIDIPacketListInit(list);
            p = MIDIPacketListAdd(list, sizeof(buf), p, 0, r.size(), r.data());
            if (p) MIDIReceived(src, list);
            std::lock_guard<std::mutex> lk(g_probe.mx);
            g_probe.line("%s%s", "             OUT ", hex(r) + "   (handshake answer)");
        }
    }
    g_probe.summary();
    MIDIEndpointDispose(src);
    MIDIEndpointDispose(dst);
    MIDIClientDispose(client);
    if (g_probe.log) std::fclose(g_probe.log);
    return 0;
}

#else

int main()
{
    std::fprintf(stderr, "mcu_port_probe: macOS only (CoreMIDI virtual ports)\n");
    // Keep the decoder honest on other platforms: one message through it.
    const uint8_t b[] = {0xF0, 0x00, 0x00, 0x66, 0x14, 0x12, 0x00, 'O', 'K', 0xF7};
    g_probe.onBytes(b, sizeof(b));
    g_probe.summary();
    return 1;
}

#endif
