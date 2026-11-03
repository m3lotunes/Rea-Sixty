#pragma once
//
// One UF8 strip as a controller surface: what each cell shows, and the frames
// that put it there, sent only when a cell changed. Pulled out of main.cpp's
// MIDI CC layer painter (ccPaintSurface_, 29.10.2026, docs/orc-uf8-plan.md stage
// 5) so that ORC's TotalMix side-car paints the UF8 with the same cells, the
// same builders and the same order, rather than a second painter.
//
// ⛔ THE CACHE STAYS WITH THE CALLER. `Cells` are references into wherever the
// caller keeps "what the device shows": main.cpp hands in its g_last* arrays,
// which its other painters and the device reopen invalidate as before; ORC hands
// in its own. A cache of its own in here would be a second writer for the same
// cells (the Hue lesson of 27.08.2026).
//
// The cells, top to bottom (Protocol.h has the field map):
//   colour bar        not here: ColorSync in main.cpp, buildColorCommand in ORC
//   CS-type cell      csType (12 chars)         buildChannelStripType
//   channel number    chanNum                   buildChannelNumber
//   upper scribble    upper (8 chars, Latin-1)  buildStripTextUpper
//   fader readout     faderText + faderUnit     buildFaderDbReadout
//   value line        valueLine (19 chars)      buildValueLine
//   soft-key label    slotLabel                 buildPluginSlotName
//   soft-key lamp     topKey 0 off, 1 dim, 2 on buildTopSoftKeyLed
//   SEL / CUT / SOLO  sel / cut / solo          buildLedColourPair
//   motor             drive + faderPb           uf8fader::drive (not while touched)
//   ring              ring + ringMode           per unit: buildVPotReadoutBar, buildVPotMode
// Test: tests/test_uf8_strips.cpp.

#include "Protocol.h"
#include "Uf8Fader.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace uf8strips {

struct View {
    std::string   upper;
    std::string   csType;
    std::string   chanNum   = " ";
    std::string   faderText;
    std::string   faderUnit;
    std::string   valueLine;
    std::string   slotLabel;
    int           topKey    = 0;          // 0 off, 1 dim, 2 on
    std::uint32_t topKeyRgb = 0xFFFFFF;
    bool          sel       = false;
    std::uint32_t selRgb    = 0xFFFFFF;
    bool          cut       = false;
    bool          solo      = false;
    std::uint32_t cutRgb    = 0;          // 0 = the lamp's own colour
    std::uint32_t soloRgb   = 0;
    // LedLook brightness (0 off, 1 dim, 2 bright) with the Rgb above, instead of
    // the on/off flags; -1 = the flags as before.
    std::int8_t   selBri    = -1;
    std::int8_t   cutBri    = -1;
    std::int8_t   soloBri   = -1;
    bool          drive     = false;      // the fader has a value to go to
    std::uint16_t faderPb   = 0;
    std::uint16_t ring      = 0;          // uf8::vpotPos*
    std::uint8_t  ringMode  = 0x03;       // 0x01 unipolar, 0x02 bipolar, 0x03 none
};

// What the device shows, the caller's storage. -1 / sentinel values mean
// "unknown", so the next paint sends the cell.
struct Cells {
    std::string&   upper;
    std::string&   csType;
    std::string&   chanNum;
    std::string&   faderText;
    std::string&   faderUnit;
    std::string&   valueLine;
    std::string&   slotLabel;
    std::int32_t&  topKey;
    std::int8_t&   sel;
    std::int8_t&   cut;
    std::int8_t&   solo;
    std::uint16_t& faderPb;
};

// One strip. `st` is the strip on its unit (0..7). `send(frame)` writes a
// frame, `leds(LedColourFrames)` an LED pair. `touched`: the cap is under a
// finger, the motor stays off it. `engaged` is the caller's motor flag. Returns
// true when it drove the motor (the caller records the target).
template <class Send, class Leds>
bool paintStrip(Cells c, const View& v, std::uint8_t st, bool touched, bool& engaged,
                Send&& send, Leds&& leds)
{
    if (c.upper != v.upper) {
        c.upper = v.upper;
        send(uf8::buildStripTextUpper(st, v.upper));
    }
    if (c.csType != v.csType) {
        c.csType = v.csType;
        send(uf8::buildChannelStripType(st, v.csType));
    }
    if (c.chanNum != v.chanNum) {
        c.chanNum = v.chanNum;
        send(uf8::buildChannelNumber(st, v.chanNum));
    }
    if (c.faderText != v.faderText || c.faderUnit != v.faderUnit) {
        c.faderText = v.faderText;
        c.faderUnit = v.faderUnit;
        send(uf8::buildFaderDbReadout(st, v.faderText, v.faderUnit));
    }
    if (c.valueLine != v.valueLine) {
        c.valueLine = v.valueLine;
        send(uf8::buildValueLine(st, v.valueLine));
    }
    if (c.slotLabel != v.slotLabel) {
        c.slotLabel = v.slotLabel;
        send(uf8::buildPluginSlotName(st, v.slotLabel));
    }
    if (c.topKey != v.topKey) {
        c.topKey = v.topKey;
        leds(uf8::buildTopSoftKeyLed(st,
            v.topKey == 2 ? uf8::TopSoftKeyState::On
                          : v.topKey == 1 ? uf8::TopSoftKeyState::Dim : uf8::TopSoftKeyState::Off,
            uf8::ledColourForTrackRgb(v.topKeyRgb)));
    }
    // The cache holds on/off and a 6-bit hash of the colour, so a new colour
    // is sent too, not only a new state.
    auto key = [](bool on, std::uint32_t rgb) {
        const std::uint32_t h = rgb ? (((rgb >> 18) ^ (rgb >> 12) ^ (rgb >> 6) ^ rgb) & 0x3F) : 0;
        return static_cast<std::int8_t>((on ? 1 : 0) | (h << 1));
    };
    // A brightness from LedLook: bright = on, dim = off in the colour's dim
    // shade, off = the dim bytes zeroed.
    auto briKey = [](int bri, std::uint32_t rgb) {
        const std::uint32_t h = (((rgb >> 18) ^ (rgb >> 12) ^ (rgb >> 6) ^ rgb) & 0x1F);
        return static_cast<std::int8_t>(0x40 | (bri & 3) | (h << 2));
    };
    auto briFrames = [&](uf8::LedClass cls, int bri, std::uint32_t rgb) {
        uf8::LedColour col = uf8::ledColourForTrackRgb(rgb);
        if (bri <= 0) { col.aDim = 0; col.bDim = 0; }
        return uf8::buildLedColourPair(st, cls, bri >= 2, col);
    };
    const std::int8_t sel = v.selBri >= 0 ? briKey(v.selBri, v.selRgb) : key(v.sel, v.selRgb);
    if (c.sel != sel && v.selBri >= 0) {
        c.sel = sel;
        leds(briFrames(uf8::LedClass::Sel, v.selBri, v.selRgb));
    } else if (c.sel != sel) {
        c.sel = sel;
        leds(uf8::buildLedColourPair(st, uf8::LedClass::Sel, v.sel,
                                     uf8::ledColourForTrackRgb(v.selRgb)));
    }
    const std::int8_t cut = v.cutBri >= 0 ? briKey(v.cutBri, v.cutRgb) : key(v.cut, v.cutRgb);
    if (c.cut != cut && v.cutBri >= 0) {
        c.cut = cut;
        leds(briFrames(uf8::LedClass::Cut, v.cutBri, v.cutRgb));
    } else if (c.cut != cut) {
        c.cut = cut;
        leds(v.cutRgb ? uf8::buildLedColourPair(st, uf8::LedClass::Cut, v.cut, uf8::ledColourForTrackRgb(v.cutRgb))
                      : uf8::buildLedColourPair(st, uf8::LedClass::Cut, v.cut));
    }
    const std::int8_t solo = v.soloBri >= 0 ? briKey(v.soloBri, v.soloRgb) : key(v.solo, v.soloRgb);
    if (c.solo != solo && v.soloBri >= 0) {
        c.solo = solo;
        leds(briFrames(uf8::LedClass::Solo, v.soloBri, v.soloRgb));
    } else if (c.solo != solo) {
        c.solo = solo;
        leds(v.soloRgb ? uf8::buildLedColourPair(st, uf8::LedClass::Solo, v.solo, uf8::ledColourForTrackRgb(v.soloRgb))
                       : uf8::buildLedColourPair(st, uf8::LedClass::Solo, v.solo));
    }
    // Motor: a limp strip takes the bit-7 echo first, then the enable, or the
    // firmware drops the position (uf8fader::drive).
    if (v.drive && !touched && (!engaged || c.faderPb != v.faderPb)) {
        c.faderPb = v.faderPb;
        for (auto& f : uf8fader::drive(st, v.faderPb, engaged)) send(std::move(f));
        return true;
    }
    return false;
}

// The rings of one unit: positions first, then the modes (cap37 ordering), each
// only when it changed or `init` is false. `lastBar` / `lastMode` point at the
// unit's eight cached entries.
template <class Send>
void paintRings(std::uint16_t* lastBar, std::uint8_t* lastMode, bool init,
                const std::array<std::uint16_t, 8>& bar, const std::array<std::uint8_t, 8>& mode,
                Send&& send)
{
    bool barChanged = !init, modeChanged = !init;
    for (int i = 0; i < 8; ++i) {
        if (lastBar[i] != bar[i])   barChanged  = true;
        if (lastMode[i] != mode[i]) modeChanged = true;
    }
    if (barChanged) {
        for (int i = 0; i < 8; ++i) lastBar[i] = bar[i];
        send(uf8::buildVPotReadoutBar(bar));
    }
    if (modeChanged) {
        for (int i = 0; i < 8; ++i) lastMode[i] = mode[i];
        send(uf8::buildVPotMode(mode));
    }
}

} // namespace uf8strips
