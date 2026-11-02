// McuSurface between a virtual UF8 (Uf8Sim.h) and a scripted MCU host that behaves
// as UA Midi Control is documented to (docs/mcu-layer-plan.md 2.1). The scenarios
// are the manual acceptance test of the task, run without the studio:
// fader 1 moves channel 1 and the motor follows the console, names on the
// scribbles, mute/solo lamps, BANK, the layer keys stay Rea-Sixty's, and the state
// is back at once after a layer switch. Plus the user's own keys (ALT, INPUTS,
// GAIN), REC mode, transport by holding the encoder, blink, meters, handshake.

#include "McuSurface.h"
#include "Uf8Sim.h"

#include <cstdio>
#include <cstdlib>
#include <string>

static int g_fail = 0;
#define EXPECT(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

using mcu::Bytes;

namespace {

// The whole rig: the surface, the UF8 it paints, the MIDI the host received.
struct Rig {
    mcusurf::Surface  surf;
    uf8sim::Uf8       uf8;
    std::vector<Bytes> toHost;
    std::int64_t      now = 1000;

    void take(mcusurf::Out& o)
    {
        for (auto& f : o.uf8) uf8.feed(f);
        for (auto& m : o.midi) toHost.push_back(m);
    }
    bool press(const uf8frames::Frame& f)
    {
        mcusurf::Out o;
        const bool mine = surf.fromUf8(f, now, o);
        take(o);
        return mine;
    }
    void host(const Bytes& m)
    {
        mcusurf::Out o;
        surf.fromHost(m.data(), m.size(), now, o);
        take(o);
    }
    void paint(std::int64_t advanceMs = 33)
    {
        now += advanceMs;
        mcusurf::Out o;
        surf.paint(now, o);
        take(o);
    }
    bool sent(const Bytes& m) const
    {
        for (const auto& x : toHost) if (x == m) return true;
        return false;
    }
    int count(std::uint8_t status) const
    {
        int n = 0;
        for (const auto& x : toHost) if (!x.empty() && x[0] == status) ++n;
        return n;
    }
};

// What UA Midi Control sends: a whole LCD row of names (7 per strip).
Bytes lcdRow(int row, const char* const names[8])
{
    Bytes m = {0xF0, 0x00, 0x00, 0x66, 0x14, 0x12, static_cast<std::uint8_t>(row ? 0x38 : 0x00)};
    for (int s = 0; s < 8; ++s) {
        std::string n = names[s];
        n.resize(7, ' ');
        m.insert(m.end(), n.begin(), n.end());
    }
    m.push_back(0xF7);
    return m;
}

Bytes pb(int ch, int v) { return {static_cast<std::uint8_t>(0xE0 | ch), static_cast<std::uint8_t>(v & 0x7F), static_cast<std::uint8_t>((v >> 7) & 0x7F)}; }
Bytes led(std::uint8_t n, std::uint8_t v) { return {0x90, n, v}; }

const char* const kBank1[8] = {"UAD 1", "UAD 2", "AVALON", "ADAT 2", "ADAT 3", "ADAT 4", "ADAT 5", "ADAT 6"};
const char* const kBank2[8] = {"ADAT 7", "ADAT 8", "REAPER", "VoX Ret", "AUX 1", "AUX 2", "TALKBCK", "MONITOR"};
const char* const kPans[8]  = {"  <C>", "  <C>", "L 20", "R 20", "  <C>", "  <C>", "  <C>", "  <C>"};

}  // namespace

int main()
{
    // ---- 1. the console's state arrives, the UF8 shows it -------------------------
    {
        Rig r;
        r.host(lcdRow(0, kBank1));
        r.host(lcdRow(1, kPans));
        r.host(pb(0, 12000));                      // channel 1 fader
        r.host(pb(1, 0));
        r.host(led(0x08, 0x7F));                   // solo 1
        r.host(led(0x11, 0x7F));                   // mute 2
        r.host(led(0x18, 0x7F));                   // select 1
        r.host(led(0x2A, 0x7F));                   // PAN assign lit
        r.host({0xB0, 0x30, 0x16});                // ring 1: boost/cut centre
        r.host({0xB0, 0x32, 0x03});                // ring 3: dot, position 3
        r.host({0xD0, 0x0A});                      // meter strip 1 level 10
        r.paint();
        EXPECT(r.uf8.badChecksum == 0 && r.uf8.unknown == 0);
        EXPECT(r.uf8.upper[0] == "UAD 1" && r.uf8.upper[2] == "AVALON" && r.uf8.upper[7] == "ADAT 6");
        EXPECT(r.uf8.lower[2] == "L 20" && r.uf8.lower[0] == "  <C>");   // leading blanks kept
        EXPECT(r.uf8.motorPb[0] == mcu::toUf8Fader(12000) && r.uf8.motorOn[0]);
        EXPECT(r.uf8.motorPb[1] == 0);
        EXPECT(r.uf8.motorPb[2] == -1);            // the host said nothing: the motor stays
        EXPECT(r.uf8.solo(0) == uf8sim::Lamp::On && r.uf8.solo(1) == uf8sim::Lamp::Dim);   // unlit strip lamps glow dim, as on the REAPER layer
        EXPECT(r.uf8.cut(1) == uf8sim::Lamp::On && r.uf8.cut(0) == uf8sim::Lamp::Dim);
        EXPECT(r.uf8.sel(0) == uf8sim::Lamp::On);
        EXPECT(r.uf8.global(uf8::Uf8GlobalLed::Pan) == uf8sim::Lamp::On);
        EXPECT(r.uf8.global(uf8::Uf8GlobalLed::Flip) == uf8sim::Lamp::Dim);   // works, not lit
        EXPECT(r.uf8.global(uf8::Uf8GlobalLed::SendPlugin1) == uf8sim::Lamp::Off);   // no function
        EXPECT(r.uf8.barMode[0] == 0x02 && r.uf8.bar[0] == uf8::vpotPosBipolar(0.0));
        EXPECT(r.uf8.barMode[2] == 0x01 && r.uf8.bar[2] == uf8::vpotPosUnipolar(0.2));
        EXPECT(r.uf8.barMode[1] == 0x03);
        EXPECT(r.uf8.vu[0] > 0 && r.uf8.vu[0] == r.uf8.vu[1] && r.uf8.vu[2] == 0);
        // the user's soft keys on the V-POT bank
        EXPECT(r.uf8.slotLabel[0] == "PAN" && r.uf8.slotLabel[3] == "INPUTS" && r.uf8.slotLabel[4] == "GAIN");
        EXPECT(r.uf8.topKey(0) == uf8sim::Lamp::On);        // PAN is lit by the host
        EXPECT(r.uf8.topKey(1) == uf8sim::Lamp::Dim && r.uf8.topKey(5) == uf8sim::Lamp::Off);
        // a second paint with nothing new sends nothing
        const int before = r.uf8.framesIn;
        r.paint();
        EXPECT(r.uf8.framesIn == before);
        if (g_fail) std::printf("%s", r.uf8.render().c_str());
    }

    // ---- 2. fader 1 by hand: touch, move, let go; the motor never fights ----------
    {
        Rig r;
        r.host(pb(0, 8000));
        r.paint();
        const int start = r.uf8.motorPb[0];
        EXPECT(r.press(uf8sim::touch(0, true)));
        EXPECT(r.sent({0x90, 0x68, 0x7F}) && !r.uf8.motorOn[0]);
        EXPECT(r.press(uf8sim::fader(0, 15583)));
        EXPECT(r.sent(pb(0, 16383)));
        EXPECT(r.uf8.echoPb[0] == 15583);          // the target follows the hand (bit 7)
        r.host(pb(0, 16383));                       // the console echoes
        r.paint();
        EXPECT(r.uf8.motorPb[0] == start);          // ...and the motor is not driven
        // a frame without bit 7 before any grab: not used (Uf8Fader.h)
        const std::size_t n = r.toHost.size();
        EXPECT(r.press(uf8sim::fader(0, 9000, false)));
        EXPECT(r.toHost.size() == n);
        // let go: the release waits for the quiet time
        EXPECT(r.press(uf8sim::touch(0, false)));
        r.paint(50);
        EXPECT(!r.sent({0x90, 0x68, 0x00}));
        r.paint(mcusurf::kTouchQuietMs);
        EXPECT(r.sent({0x90, 0x68, 0x00}));
        r.paint();
        EXPECT(r.uf8.motorOn[0] && r.uf8.motorPb[0] == 15583);   // stays where the hand left it
        // a bounce inside the quiet time keeps the touch
        r.press(uf8sim::touch(1, true));
        r.press(uf8sim::touch(1, false));
        r.paint(40);
        r.press(uf8sim::touch(1, true));
        r.paint(400);
        EXPECT(!r.sent({0x90, 0x69, 0x00}) && r.surf.touched(1));
    }

    // ---- 3. the console moves channel 1: the motor follows --------------------------
    {
        Rig r;
        r.host(pb(0, 4000));
        r.paint();
        r.host(pb(0, 10000));
        r.paint();
        EXPECT(r.uf8.motorPb[0] == mcu::toUf8Fader(10000));
        // without a touch, position frames are the motor's own echo: never sent on
        EXPECT(r.press(uf8sim::fader(0, 3000)));
        EXPECT(r.count(0xE0) == 0);
    }

    // ---- 4. BANK > : the host pages, new names arrive -----------------------------
    {
        Rig r;
        r.host(lcdRow(0, kBank1));
        r.paint();
        EXPECT(r.press(uf8sim::key(0x79, true)) && r.press(uf8sim::key(0x79, false)));
        EXPECT(r.sent({0x90, 0x2F, 0x7F}) && r.sent({0x90, 0x2F, 0x00}));
        r.host(lcdRow(0, kBank2));
        r.paint();
        EXPECT(r.uf8.upper[2] == "REAPER" && r.uf8.upper[7] == "MONITOR");
        // PAGE < > = CHANNEL < >
        r.press(uf8sim::key(0x52, true));
        EXPECT(r.sent({0x90, 0x30, 0x7F}));
    }

    // ---- 5. LAYER 1 / 360 stay Rea-Sixty's; back on the layer, all is there ------------
    {
        Rig r;
        EXPECT(!r.press(uf8sim::key(0x40, true)));    // LAYER 1: the caller's path
        EXPECT(!r.press(uf8sim::key(0x42, true)));
        EXPECT(!r.press(uf8sim::key(0x46, true)));    // 360
        EXPECT(r.toHost.empty());
        // away on REAPER: the host keeps talking, the shadow keeps up
        r.host(lcdRow(0, kBank2));
        r.host(pb(3, 9000));
        r.host(led(0x0B, 0x7F));
        // REAPER painted the UF8 meanwhile; coming back repaints every cell
        r.uf8 = uf8sim::Uf8{};
        r.uf8.upper[2] = "Kick";
        r.surf.invalidate();
        r.paint();
        EXPECT(r.uf8.upper[2] == "REAPER" && r.uf8.motorPb[3] == mcu::toUf8Fader(9000));
        EXPECT(r.uf8.solo(3) == uf8sim::Lamp::On && r.uf8.solo(0) == uf8sim::Lamp::Dim);
        EXPECT(r.uf8.lower[0].empty() && r.uf8.valueLine[0].empty() && r.uf8.faderDb[0].empty());
        EXPECT(r.uf8.global(uf8::Uf8GlobalLed::BankRight) == uf8sim::Lamp::Dim);
    }

    // ---- 6. the user's keys: ALT, INPUTS, GAIN, QUICK; strips; V-Pots ----------------
    {
        Rig r;
        r.paint();
        r.press(uf8sim::key(0x18 + 3, true));          // V-POT bank, key 4: INPUTS = EQ
        r.press(uf8sim::key(0x18 + 4, true));          // key 5: GAIN = INSTRUMENT
        EXPECT(r.sent({0x90, 0x2C, 0x7F}) && r.sent({0x90, 0x2D, 0x7F}));
        r.press(uf8sim::key(0x69, true));              // soft-key bank 1
        r.paint();
        EXPECT(r.uf8.slotLabel[0] == "ALT" && r.uf8.slotLabel[1].empty());
        EXPECT(r.uf8.global(uf8::Uf8GlobalLed::Soft1) == uf8sim::Lamp::On
               && r.uf8.global(uf8::Uf8GlobalLed::VPotBank) == uf8sim::Lamp::Dim);
        r.press(uf8sim::key(0x18, true));
        EXPECT(r.sent({0x90, 70, 0x7F}));              // UA Midi Control's Alt-monitor note
        r.press(uf8sim::key(0x43, true));
        r.press(uf8sim::key(0x44, true));
        EXPECT(r.sent({0x90, 0x33, 0x7F}) && r.sent({0x90, 0x42, 0x7F}));
        r.press(uf8sim::key(0x20 + 3 * 2, true));      // SOLO 3
        r.press(uf8sim::key(0x21 + 3 * 2, true));      // CUT 3
        r.press(uf8sim::key(0x0A, true));              // V-Pot 3 push
        EXPECT(r.sent({0x90, 0x0A, 0x7F}) && r.sent({0x90, 0x12, 0x7F}) && r.sent({0x90, 0x22, 0x7F}));
        r.press(uf8sim::vpot(0, -1));
        r.press(uf8sim::vpot(7, 3));
        EXPECT(r.sent({0xB0, 0x10, 0x41}) && r.sent({0xB0, 0x17, 0x03}));
        r.press(uf8sim::vpot(8, 2));                   // channel encoder: CHANNEL > twice
        int right = 0;
        for (const auto& m : r.toHost) if (m == Bytes{0x90, 0x31, 0x7F}) ++right;
        EXPECT(right == 2);
        r.press(uf8sim::key(0x6F, true));              // FINE = SHIFT
        EXPECT(r.sent({0x90, 0x46, 0x7F}));
        // a key with no function sends nothing and is still the layer's
        const std::size_t n = r.toHost.size();
        EXPECT(r.press(uf8sim::key(0x48, true)) && r.toHost.size() == n);
        // frames the layer does not know go back to the caller
        uf8frames::Frame poll = uf8sim::make_({0xFF, 0x04, 0x02, 0x9D, 0x01});
        EXPECT(!r.press(poll));
    }

    // ---- 7. REC mode: SEL arms, its lamp shows arm in red -----------------------------
    {
        Rig r;
        r.host(led(0x18, 0x7F));                       // select 1
        r.paint();
        EXPECT(r.uf8.sel(0) == uf8sim::Lamp::On);
        const auto white = r.uf8.colour(uf8sim::Uf8::stripCell(0, uf8::LedClass::Sel));
        r.press(uf8sim::key(0x71, true));              // REC
        r.paint();
        EXPECT(r.uf8.sel(0) == uf8sim::Lamp::Dim);     // not armed
        EXPECT(r.uf8.global(uf8::Uf8GlobalLed::Rec) == uf8sim::Lamp::On
               && r.uf8.global(uf8::Uf8GlobalLed::Norm) == uf8sim::Lamp::Dim);
        r.press(uf8sim::key(0x22, true));              // SEL 1
        EXPECT(r.sent({0x90, 0x00, 0x7F}));
        r.host(led(0x00, 0x7F));                       // armed
        r.paint();
        EXPECT(r.uf8.sel(0) == uf8sim::Lamp::On);
        EXPECT(r.uf8.colour(uf8sim::Uf8::stripCell(0, uf8::LedClass::Sel)) != white);
    }

    // ---- 8. hold the encoder: the zoom pad is transport ---------------------------------
    {
        Rig r;
        r.press(uf8sim::key(0x7A, true));
        EXPECT(r.sent({0x90, 0x60, 0x7F}));            // UP
        r.press(uf8sim::key(0x76, true));
        r.paint(mcusurf::kEncHoldMs + 10);
        r.press(uf8sim::key(0x76, false));
        EXPECT(r.surf.local().transport);
        r.press(uf8sim::key(0x7A, true));
        r.press(uf8sim::key(0x7E, true));
        r.press(uf8sim::key(0x7C, true));
        EXPECT(r.sent({0x90, 0x5E, 0x7F}) && r.sent({0x90, 0x5D, 0x7F}) && r.sent({0x90, 0x5F, 0x7F}));
        r.host(led(0x5E, 0x7F));                       // PLAY lit
        r.paint();
        EXPECT(r.uf8.global(uf8::Uf8GlobalLed::ZoomUp) == uf8sim::Lamp::On);
        // a tap leaves transport alone and returns the encoder to CHANNEL
        r.press(uf8sim::key(0x73, true));              // NAV
        EXPECT(r.surf.local().enc == mcu::EncMode::Nav);
        r.press(uf8sim::vpot(8, -2));
        EXPECT(r.sent({0xB0, 0x3C, 0x42}));            // jog
        r.press(uf8sim::key(0x76, true));
        r.paint(100);
        r.press(uf8sim::key(0x76, false));
        EXPECT(r.surf.local().transport && r.surf.local().enc == mcu::EncMode::Channel);
        // the option off: holding does nothing
        r.surf.options().transportHold = false;
        r.press(uf8sim::key(0x76, true));
        r.paint(2000);
        r.press(uf8sim::key(0x76, false));
        EXPECT(r.surf.local().transport);
    }

    // ---- 9. blink, meters fall, handshake, split SysEx ---------------------------------
    {
        Rig r;
        r.now = 0;
        r.host(led(0x32, 0x01));                       // FLIP blinks
        r.paint(10);
        const auto a = r.uf8.global(uf8::Uf8GlobalLed::Flip);
        r.paint(mcu::kBlinkHalfMs);
        const auto b = r.uf8.global(uf8::Uf8GlobalLed::Flip);
        EXPECT(a != b && (a == uf8sim::Lamp::On || b == uf8sim::Lamp::On));

        r.host({0xD0, 0x0C});
        r.paint();
        EXPECT(r.uf8.vu[0] == 0x1F);
        r.paint(6 * mcu::kMeterLevelMs);
        EXPECT(r.uf8.vu[0] > 0 && r.uf8.vu[0] < 0x1F);
        r.paint(7 * mcu::kMeterLevelMs);
        EXPECT(r.uf8.vu[0] == 0);

        r.host({0xF0, 0x00, 0x00, 0x66, 0x14, 0x00, 0xF7});
        EXPECT(r.toHost.size() == 1 && r.toHost[0][5] == 0x01);

        // an LCD write in three pieces, as CoreMIDI may deliver it
        const Bytes whole = lcdRow(0, kBank1);
        mcusurf::Out o;
        r.surf.fromHost(whole.data(), 10, r.now, o);
        r.surf.fromHost(whole.data() + 10, 30, r.now, o);
        r.paint();
        EXPECT(r.uf8.upper[0].empty());                // nothing whole yet, nothing shown
        r.surf.fromHost(whole.data() + 40, whole.size() - 40, r.now, o);
        r.paint();
        EXPECT(r.uf8.upper[0] == "UAD 1" && r.uf8.upper[7] == "ADAT 6");
        if (g_fail) std::printf("%s", r.uf8.render().c_str());
    }

    if (g_fail) std::printf("%d failure(s)\n", g_fail);
    else        std::printf("mcu_surface: all passed\n");
    return g_fail ? 1 : 0;
}
