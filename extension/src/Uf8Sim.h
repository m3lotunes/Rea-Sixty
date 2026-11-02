#pragma once
//
// Uf8Sim: a UF8 in software, for tests and the MCU simulator. It reads the frames
// Rea-Sixty sends to the device and keeps what the hardware would show (scribbles,
// value line, lamps, the motor's target, rings, meters), and it builds the frames
// the device sends (touch, fader, key, V-Pot) byte for byte as on the wire.
//
// It knows only what docs/protocol-notes.md and Protocol.cpp say; it is not a
// firmware model. What it cannot show: how the firmware renders a cell, motor
// timing, the two fader streams' offset. Those stay for the real unit.
//
#include "Protocol.h"
#include "Uf8Frames.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace uf8sim {

enum class Lamp : std::int8_t { Unknown = -1, Off = 0, Dim = 1, On = 2 };

inline const char* lampName(Lamp l)
{
    switch (l) {
        case Lamp::Off: return "off";
        case Lamp::Dim: return "dim";
        case Lamp::On:  return "ON";
        default:        return "?";
    }
}

// ---- frames the device sends -------------------------------------------------

inline uf8frames::Frame make_(std::initializer_list<std::uint8_t> b)
{
    uf8frames::Frame f;
    std::uint8_t sum = 0;
    std::size_t i = 0;
    for (std::uint8_t x : b) {
        f.b[i] = x;
        if (i > 0) sum = static_cast<std::uint8_t>(sum + x);
        ++i;
    }
    f.b[i++] = sum;
    f.n = static_cast<std::uint8_t>(i);
    return f;
}

inline uf8frames::Frame touch(int strip, bool on)
{
    return make_({0xFF, 0x20, 0x02, static_cast<std::uint8_t>(strip), static_cast<std::uint8_t>(on ? 1 : 0)});
}
// A fader position as the firmware streams it while the hand moves: bit 7 set on
// the low byte (the stream Rea-Sixty takes when it does not grab).
inline uf8frames::Frame fader(int strip, int pb, bool hiBit = true)
{
    return make_({0xFF, 0x21, 0x03, static_cast<std::uint8_t>(strip),
                  static_cast<std::uint8_t>((pb & 0x7F) | (hiBit ? 0x80 : 0)),
                  static_cast<std::uint8_t>((pb >> 7) & 0x7F)});
}
inline uf8frames::Frame key(std::uint8_t id, bool pressed)
{
    return make_({0xFF, 0x22, 0x03, id, 0x00, static_cast<std::uint8_t>(pressed ? 1 : 0)});
}
inline uf8frames::Frame vpot(int pot, int delta)
{
    return make_({0xFF, 0x24, 0x02, static_cast<std::uint8_t>(pot),
                  static_cast<std::uint8_t>(delta & 0x3F)});
}

// ---- what the device shows ----------------------------------------------------

struct Uf8 {
    std::array<std::string, 8> upper, lower, valueLine, slotLabel, csType, chanNum, faderDb;
    std::array<int, 8>          motorPb;        // last position the motor was sent, -1 none
    std::array<int, 8>          echoPb;         // last bit-7 echo (target buffer), -1 none
    std::array<bool, 8>         motorOn{};
    std::array<std::uint16_t, 8> bar{};
    std::array<std::uint8_t, 8>  barMode{};
    std::array<std::uint8_t, 16> vu{};
    int framesIn = 0, badChecksum = 0, unknown = 0;

    Uf8() { motorPb.fill(-1); echoPb.fill(-1); }

    // Per LED cell: the FF 38 / FF 39 pair and the legacy mono state.
    struct Cell { int a38 = -1, b38 = -1, a39 = -1, b39 = -1, legacy = -1; };
    std::map<std::uint8_t, Cell> cells;

    Lamp lamp(std::uint8_t cell) const
    {
        auto it = cells.find(cell);
        if (it == cells.end() || it->second.a38 < 0) return Lamp::Unknown;
        const Cell& c = it->second;
        if (c.legacy == 0) return Lamp::Off;
        const bool base38 = c.a38 == 0x00 && c.b38 == 0xF0;
        const bool base39 = c.a39 == 0x00 && c.b39 == 0xF0;
        if (base38 && base39) return Lamp::Off;
        if (base39) return Lamp::On;
        return Lamp::Dim;
    }
    std::pair<int, int> colour(std::uint8_t cell) const
    {
        auto it = cells.find(cell);
        return it == cells.end() ? std::pair<int, int>{-1, -1}
                                 : std::pair<int, int>{it->second.a38, it->second.b38};
    }
    // Per-strip cells (Protocol.cpp, buildLedColourPair / buildTopSoftKeyLed).
    static std::uint8_t stripCell(int strip, uf8::LedClass cls)
    {
        return static_cast<std::uint8_t>(0x17 - 3 * strip - static_cast<int>(cls));
    }
    Lamp solo(int s) const   { return lamp(stripCell(s, uf8::LedClass::Solo)); }
    Lamp cut(int s) const    { return lamp(stripCell(s, uf8::LedClass::Cut)); }
    Lamp sel(int s) const    { return lamp(stripCell(s, uf8::LedClass::Sel)); }
    Lamp topKey(int s) const { return lamp(static_cast<std::uint8_t>(0x1F - s)); }
    // A global lamp, by asking Protocol which cell it is.
    Lamp global(uf8::Uf8GlobalLed led) const
    {
        return lamp(uf8::buildUf8GlobalLed(led, uf8::GlobalLedState::Off).ff38[3]);
    }

    void feed(const std::vector<std::uint8_t>& f)
    {
        ++framesIn;
        if (f.size() < 4 || f[0] != 0xFF) { ++unknown; return; }
        std::uint8_t sum = 0;
        for (std::size_t i = 1; i + 1 < f.size(); ++i) sum = static_cast<std::uint8_t>(sum + f[i]);
        if (sum != f.back()) { ++badChecksum; return; }
        const std::uint8_t cmd = f[1];
        auto text = [&](std::size_t from, std::size_t to) {
            std::string s;
            for (std::size_t i = from; i < to && i + 1 < f.size(); ++i)
                if (f[i]) s.push_back(static_cast<char>(f[i]));
            return s;
        };
        auto rtrim = [](std::string s) {
            while (!s.empty() && s.back() == ' ') s.pop_back();
            return s;
        };
        switch (cmd) {
            case 0x66: {
                const std::size_t len = f[2];
                const std::uint8_t sub = f[3];
                const std::size_t end = 3 + len;                  // index after the payload
                const int s = f.size() > 4 ? f[4] : 0;
                if (sub == 0x0B && s < 8)                    upper[s] = text(5, end);
                else if (sub == 0x0E && len == 0x09 && s < 8) lower[s] = rtrim(text(5, end));
                else if (sub == 0x0E && s < 8)               valueLine[s] = rtrim(text(5, end));
                else if (sub == 0x04 && s < 8)               slotLabel[s] = text(5, end);
                else if (sub == 0x17 && s < 8)               csType[s] = text(5, end);
                else if (sub == 0x14 && s < 8)               chanNum[s] = text(5, end);
                else if (sub == 0x0C && s < 8)               faderDb[s] = rtrim(text(5, end));
                else if (sub == 0x0F && len == 0x11)
                    for (int i = 0; i < 8; ++i)
                        bar[static_cast<std::size_t>(i)] = static_cast<std::uint16_t>(
                            f[4 + 2 * static_cast<std::size_t>(i)] |
                            (f[5 + 2 * static_cast<std::size_t>(i)] << 8));
                else if (sub == 0x0D && len == 0x09)
                    for (std::size_t i = 0; i < 8; ++i) barMode[i] = f[4 + i];
                else if (len == 0x21 && (sub == 0x09 || sub == 0x0A))
                    for (std::size_t i = 0; i < 16; ++i) vu[i] = f[4 + i];
                break;
            }
            case 0x1E: {                                          // motor position
                const int s = f[3];
                if (s >= 8) break;
                const int pb = (f[4] & 0x7F) | ((f[5] & 0x7F) << 7);
                if (f[4] & 0x80) echoPb[static_cast<std::size_t>(s)] = pb;
                else             motorPb[static_cast<std::size_t>(s)] = pb;
                break;
            }
            case 0x1D:                                            // motor on / limp
                if (f[3] < 8) motorOn[f[3]] = f[4] != 0;
                break;
            case 0x38: cells[f[3]].a38 = f[5]; cells[f[3]].b38 = f[6]; break;
            case 0x39: cells[f[3]].a39 = f[5]; cells[f[3]].b39 = f[6]; break;
            case 0x3B: cells[f[3]].legacy = f[5]; break;
            default: break;                                       // brightness, keepalive …
        }
    }

    // A picture of the eight strips, for the simulator and failing tests.
    std::string render() const
    {
        auto col = [](const std::string& s) {
            char b[16];
            std::snprintf(b, sizeof(b), "%-9.8s", s.c_str());
            return std::string(b);
        };
        auto lampCh = [](Lamp l) { return l == Lamp::On ? "##" : l == Lamp::Dim ? "--" : "  "; };
        std::string o;
        auto row = [&](const char* name, auto cell) {
            o += name;
            for (int s = 0; s < 8; ++s) o += col(cell(s));
            o += "\n";
        };
        row("key   |", [&](int s) { return "[" + std::string(lampCh(topKey(s))) + "]" + slotLabel[static_cast<std::size_t>(s)].substr(0, 4); });
        row("name  |", [&](int s) { return upper[static_cast<std::size_t>(s)]; });
        row("value |", [&](int s) { return lower[static_cast<std::size_t>(s)]; });
        row("ring  |", [&](int s) {
            char b[16];
            const auto i = static_cast<std::size_t>(s);
            if (barMode[i] == 0x03 || barMode[i] == 0) return std::string("");
            if (barMode[i] == 0x02) std::snprintf(b, sizeof(b), "%+d", static_cast<std::int8_t>(bar[i] & 0xFF));
            else                    std::snprintf(b, sizeof(b), "%d%%", bar[i] & 0xFF);
            return std::string(b);
        });
        row("meter |", [&](int s) {
            const int v = vu[static_cast<std::size_t>(s) * 2];
            return std::string(static_cast<std::size_t>(v / 4), '|');
        });
        row("SOLO  |", [&](int s) { return std::string(lampCh(solo(s))); });
        row("CUT   |", [&](int s) { return std::string(lampCh(cut(s))); });
        row("SEL   |", [&](int s) { return std::string(lampCh(sel(s))); });
        row("fader |", [&](int s) {
            const int pb = motorPb[static_cast<std::size_t>(s)];
            return pb < 0 ? std::string("?") : std::to_string(pb * 100 / 15583) + "%";
        });
        return o;
    }
};

}  // namespace uf8sim
