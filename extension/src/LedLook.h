#pragma once
//
// LedLook: how a key's lamp looks, the REAPER layer's LED model (Settings >
// Bindings > LED: Active colour + Off/Dim/Bright, Inactive colour + Off/Dim/
// Bright, "Show LED even when no action is assigned") for the MIDI CC and the
// UAD Console layer (Frank 03.11.2026: "die Farb- und LED-Logik wie auf dem
// REAPER-Layer"). Pure data and the state -> lamp rule; no device, no ImGui.
//
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace ledlook {

enum Bri : int { Off = 0, Dim = 1, Bright = 2 };

struct Look {
    std::uint32_t activeRgb   = 0xFFFFFF;   // 0xRRGGBB
    int           activeBri   = Bright;
    std::uint32_t inactiveRgb = 0xFFFFFF;
    int           inactiveBri = Dim;
    bool          showWhenEmpty = false;    // a key with nothing to do still shows Inactive
    bool operator==(const Look& o) const
    {
        return activeRgb == o.activeRgb && activeBri == o.activeBri && inactiveRgb == o.inactiveRgb
            && inactiveBri == o.inactiveBri && showWhenEmpty == o.showWhenEmpty;
    }
    bool operator!=(const Look& o) const { return !(*this == o); }
};

// One colour for both states, bright while active and dim while not: what the
// layers did before the model (and what a palette index from an old file means).
inline Look fromColour(std::uint32_t rgb)
{
    Look l;
    l.activeRgb = l.inactiveRgb = rgb;
    return l;
}

// state: 2 = active (on), 1 = inactive (does something, now off), 0 = nothing
// assigned. Out: the colour and brightness to show (Off = dark).
inline void resolve(const Look& l, int state, std::uint32_t& rgb, int& bri)
{
    if (state >= 2)                         { rgb = l.activeRgb;   bri = l.activeBri; }
    else if (state == 1 || l.showWhenEmpty) { rgb = l.inactiveRgb; bri = l.inactiveBri; }
    else                                    { rgb = 0;             bri = Off; }
}

// bindings.json: {"a": "RRGGBB", "ab": 2, "i": "RRGGBB", "ib": 1, "e": false}
inline std::string toJson(const Look& l)
{
    char b[128];
    // Unqualified on purpose: on MSVC a WDL header included before this one
    // turns snprintf into a macro (WDL_snprintf), and std::snprintf then fails
    // on the Windows build alone (CI 2026-11-08; see HueClient.cpp).
    snprintf(b, sizeof(b), "{\"a\": \"%06X\", \"ab\": %d, \"i\": \"%06X\", \"ib\": %d, \"e\": %s}",
                  static_cast<unsigned>(l.activeRgb & 0xFFFFFF), l.activeBri,
                  static_cast<unsigned>(l.inactiveRgb & 0xFFFFFF), l.inactiveBri,
                  l.showWhenEmpty ? "true" : "false");
    return b;
}

inline std::uint32_t hexRgb(const char* s, std::uint32_t def)
{
    if (!s || !*s) return def;
    char* end = nullptr;
    const unsigned long v = std::strtoul(s, &end, 16);
    return (end && end != s) ? static_cast<std::uint32_t>(v & 0xFFFFFF) : def;
}

inline int clampBri(int b) { return b < Off ? Off : b > Bright ? Bright : b; }

}  // namespace ledlook
