// The UAD Console side-car between a virtual UF8 (Uf8Sim.h) and a virtual UA Mixer
// Engine (UadSim.h) holding the user's Apollo Twin MkII as measured on 02.11.2026.
// The acceptance test of docs/uad-console-plan.md without the studio: names, dB and
// meters on the UF8, faders and keys writing the Console, the Console moving the
// motors, BANK, sends, preamp, monitor, 48V only on hold, nothing while offline,
// nothing outside the allowlist.

#include "UadModel.h"
#include "UadProto.h"
#include "UadSim.h"
#include "UadSurface.h"
#include "Uf8Sim.h"

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <deque>
#include <set>
#include <string>

static int g_fail = 0;
#define EXPECT(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

std::string noNul(std::string c) { if (!c.empty() && c.back() == '\0') c.pop_back(); return c; }

struct Rig {
    uadsim::Engine     eng = uadsim::twinMkII();
    uad::Model         model;
    uadsurf::Surface   surf{model};
    uf8sim::Uf8        uf8;
    std::set<std::string> subscribed;
    std::int64_t       now = 1000;

    Rig()
    {
        std::deque<std::string> todo{"/devices"};
        while (!todo.empty()) {
            const std::string p = todo.front();
            todo.pop_front();
            for (const auto& r : eng.handle("get " + p))
                for (auto& n : model.onMessage(r)) todo.push_back(n);
        }
        paint(0);
    }
    void toEngine(const std::vector<std::string>& cmds)
    {
        for (const auto& c : cmds)
            for (const auto& r : eng.handle(noNul(c))) model.onMessage(r);
    }
    void take(uadsurf::Out& o)
    {
        for (auto& f : o.uf8) uf8.feed(f);
        toEngine(o.cmds);
    }
    // What the client does: keep the subscriptions the surface wants.
    void resubscribe()
    {
        for (const auto& p : surf.wantedSubscriptions())
            if (subscribed.insert(p).second)
                for (const auto& r : eng.handle(noNul(uad::subscribe(p)))) model.onMessage(r);
    }
    void paint(int dt = 33)
    {
        now += dt;
        uadsurf::Out o;
        surf.paint(now, o);
        take(o);
        resubscribe();
    }
    bool press(const uf8frames::Frame& f)
    {
        uadsurf::Out o;
        const bool mine = surf.fromUf8(f, now, o);
        take(o);
        return mine;
    }
    void key(std::uint8_t id) { press(uf8sim::key(id, true)); press(uf8sim::key(id, false)); }
    void console(const std::string& prop, const std::string& v)     // a change in UAD Console
    {
        for (const auto& r : eng.change(prop, v)) model.onMessage(r);
    }
    bool wrote(const std::string& s) const
    {
        for (const auto& x : eng.sets) if (x == s) return true;
        return false;
    }
    std::string lastSet() const { return eng.sets.empty() ? "" : eng.sets.back(); }
};

// The value of the last set, as a number.
double lastNum(const Rig& r)
{
    const std::string s = r.lastSet();
    const std::size_t sp = s.rfind(' ');
    return sp == std::string::npos ? -1.0 : std::strtod(s.c_str() + sp + 1, nullptr);
}

}  // namespace

int main()
{
    // ---- 1. bank 1 on the UF8, as the Console has it -----------------------------
    {
        Rig r;
        r.paint();
        EXPECT(r.uf8.badChecksum == 0);
        EXPECT(r.uf8.upper[0] == "UAD 1" && r.uf8.upper[2] == "AVALON" && r.uf8.upper[7] == "ADAT 6");
        EXPECT(r.uf8.faderDb[0] == "0.0dB" && r.uf8.faderDb[2] == "-6.0dB");
        EXPECT(r.uf8.csType[0] == "MIC" && r.uf8.csType[1] == "LINE" && r.uf8.csType[3] == "ADAT");
        EXPECT(r.uf8.chanNum[0] == "1" && r.uf8.chanNum[7] == "8");
        EXPECT(r.uf8.motorPb[0] == static_cast<int>(std::lround(0.7818182 * 15583)));
        EXPECT(r.uf8.solo(0) == uf8sim::Lamp::On && r.uf8.cut(0) == uf8sim::Lamp::On);   // UAD 1: mute+solo
        EXPECT(r.uf8.solo(1) != uf8sim::Lamp::On && r.uf8.cut(1) != uf8sim::Lamp::On);
        EXPECT(r.uf8.lower[0] == "<C>" && r.uf8.barMode[0] == 0x01);                    // PAN mode
        EXPECT(r.uf8.slotLabel[0] == "PAN" && r.uf8.slotLabel[2] == "INPUTS" && r.uf8.topKey(0) == uf8sim::Lamp::On);
        EXPECT(r.uf8.global(uf8::Uf8GlobalLed::BankRight) == uf8sim::Lamp::Dim
               && r.uf8.global(uf8::Uf8GlobalLed::BankLeft) == uf8sim::Lamp::Off);
        EXPECT(r.uf8.global(uf8::Uf8GlobalLed::SendPlugin6) == uf8sim::Lamp::Dim
               && r.uf8.global(uf8::Uf8GlobalLed::SendPlugin7) == uf8sim::Lamp::Off);   // six sends
        EXPECT(r.eng.sets.empty());                // showing writes nothing
        const int before = r.uf8.framesIn;
        r.paint();
        EXPECT(r.uf8.framesIn == before);          // nothing new, nothing sent
        if (g_fail) std::printf("%s", r.uf8.render().c_str());
    }

    // ---- 2. fader 3 (AVALON) by hand: the Console follows, the motor never fights ----
    {
        Rig r;
        r.paint();
        const int start = r.uf8.motorPb[2];
        r.press(uf8sim::touch(2, true));
        EXPECT(!r.uf8.motorOn[2]);
        r.press(uf8sim::fader(2, 15583 / 2));
        EXPECT(r.lastSet().rfind("set /devices/0/inputs/2/FaderLevelTapered/value ", 0) == 0
               && std::fabs(lastNum(r) - 0.5) < 0.001);
        // two more frames inside one write window: one write, the newest value
        const std::size_t n = r.eng.sets.size();
        r.press(uf8sim::fader(2, 15583 / 4));
        r.press(uf8sim::fader(2, 15583 / 5));
        EXPECT(r.eng.sets.size() == n);
        r.paint(20);
        EXPECT(r.eng.sets.size() == n + 1 && std::fabs(lastNum(r) - 0.2) < 0.001);
        EXPECT(r.model.devices[0].inputs[2].faderTapered > 0.19 && r.model.devices[0].inputs[2].faderTapered < 0.21);
        EXPECT(r.uf8.motorPb[2] == start);         // not driven under the hand
        r.press(uf8sim::touch(2, false));
        r.paint(uadsurf::kTouchQuietMs + 10);
        r.paint(uadsurf::kHandHoldMs);
        EXPECT(r.uf8.motorOn[2] && std::abs(r.uf8.motorPb[2] - 15583 / 5) <= 2);
        // the motor's own position frames, untouched, write nothing
        const std::size_t m = r.eng.sets.size();
        r.press(uf8sim::fader(2, 9000));
        EXPECT(r.eng.sets.size() == m);
    }

    // ---- 3. the Console moves a fader: the motor follows ------------------------------
    {
        Rig r;
        r.paint();
        r.console("/devices/0/inputs/3/FaderLevelTapered", "0.25");
        r.console("/devices/0/inputs/3/FaderLevel", "-29.25");
        r.paint();
        EXPECT(r.uf8.motorPb[3] == static_cast<int>(std::lround(0.25 * 15583)));
        EXPECT(r.uf8.faderDb[3] == "-29.2dB" || r.uf8.faderDb[3] == "-29.3dB");
        r.console("/devices/0/inputs/1/Mute", "true");
        r.paint();
        EXPECT(r.uf8.cut(1) == uf8sim::Lamp::On);
        r.console("/devices/0/inputs/3/Name", uadsim::q("Bass DI"));
        r.paint();
        EXPECT(r.uf8.upper[3] == "Bass DI");
    }

    // ---- 4. BANK > : the second page, auxes included ---------------------------------
    {
        Rig r;
        r.paint();
        r.key(0x79);
        r.paint();
        EXPECT(r.uf8.upper[0] == "ADAT 7" && r.uf8.upper[2] == "REAPER" && r.uf8.upper[3] == "VoX Retu");
        EXPECT(r.uf8.upper[4] == "AUX 1" && r.uf8.upper[5] == "AUX 2" && r.uf8.upper[6].empty());
        EXPECT(r.uf8.csType[4] == "AUX" && r.uf8.chanNum[4] == "13" && r.uf8.chanNum[6] == " ");
        EXPECT(r.uf8.motorPb[6] == 0 && r.uf8.motorPb[7] == 0);          // empty strips: fader down
        EXPECT(r.uf8.global(uf8::Uf8GlobalLed::BankRight) == uf8sim::Lamp::Off
               && r.uf8.global(uf8::Uf8GlobalLed::BankLeft) == uf8sim::Lamp::Dim);
        // an aux fader writes the aux
        r.press(uf8sim::touch(4, true));
        r.press(uf8sim::fader(4, 15583));
        EXPECT(r.lastSet() == "set /devices/0/auxs/0/FaderLevelTapered/value 1");
        // CUT on an aux mutes it; SOLO on an aux does nothing
        r.key(0x21 + 3 * 5);
        EXPECT(r.lastSet() == "set /devices/0/auxs/1/Mute/value true");
        const std::size_t n = r.eng.sets.size();
        r.key(0x20 + 3 * 5);
        EXPECT(r.eng.sets.size() == n);
        // an empty strip does nothing
        r.press(uf8sim::touch(7, true));
        r.press(uf8sim::fader(7, 5000));
        EXPECT(r.eng.sets.size() == n);
        r.key(0x79);                                // already the last bank
        r.paint();
        EXPECT(r.surf.bank() == 1);
        r.key(0x78);
        r.paint();
        EXPECT(r.uf8.upper[0] == "UAD 1");
    }

    // ---- 5. strip keys and V-Pots ------------------------------------------------------
    {
        Rig r;
        r.paint();
        r.key(0x21 + 3 * 2);                        // CUT 3
        EXPECT(r.lastSet() == "set /devices/0/inputs/2/Mute/value true");
        r.key(0x20 + 3 * 0);                        // SOLO 1 (on) -> off
        EXPECT(r.lastSet() == "set /devices/0/inputs/0/Solo/value false");
        r.paint();
        EXPECT(r.uf8.cut(2) == uf8sim::Lamp::On && r.uf8.solo(0) != uf8sim::Lamp::On);
        r.press(uf8sim::vpot(1, -5));               // pan UAD 2 left
        EXPECT(r.lastSet() == "set /devices/0/inputs/1/Pan/value -0.1");
        r.paint();
        EXPECT(r.uf8.lower[1] == "L 10");
        r.press(uf8sim::key(0x6F, true));           // FINE held
        r.press(uf8sim::vpot(1, 2));
        r.press(uf8sim::key(0x6F, false));
        r.paint(20);
        EXPECT(r.lastSet() == "set /devices/0/inputs/1/Pan/value -0.09");
        r.key(0x09);                                // push = centre
        EXPECT(r.lastSet() == "set /devices/0/inputs/1/Pan/value 0");
        r.key(0x22 + 3 * 1);                        // SEL 2
        r.paint();
        EXPECT(r.surf.selected() == 1 && r.uf8.sel(1) == uf8sim::Lamp::On && r.uf8.sel(0) != uf8sim::Lamp::On);
    }

    // ---- 6. sends: SEND, SEND/PLUG-IN picks the bus, push = off ------------------------
    {
        Rig r;
        r.paint();
        r.key(0x18 + 1);                            // V-POT bank, SEND
        r.key(0x49);                                // SEND/PLUG-IN 2 = aux2
        r.paint();
        EXPECT(r.surf.mode() == uadsurf::PotMode::Send && r.surf.send() == 1);
        EXPECT(r.uf8.global(uf8::Uf8GlobalLed::SendPlugin2) == uf8sim::Lamp::On);
        EXPECT(r.uf8.lower[3] == "-inf");
        r.press(uf8sim::vpot(3, 10));
        EXPECT(r.lastSet() == "set /devices/0/inputs/3/sends/1/GainTapered/value 0.1");
        r.key(0x0B);
        EXPECT(r.lastSet() == "set /devices/0/inputs/3/sends/1/Bypass/value true");
        r.key(0x4A);                                // SEND/PLUG-IN 3 = cue1 (HP)
        r.paint();
        EXPECT(r.surf.send() == 2 && r.uf8.valueLine[0].rfind("HP", 0) == 0);
        const std::size_t n = r.eng.sets.size();
        r.key(0x4E);                                // SEND/PLUG-IN 7: there is no 7th send
        EXPECT(r.surf.send() == 2 && r.eng.sets.size() == n);
    }

    // ---- 7. preamp: INPUTS, whole dB, only where there is one; 48V only on hold ---------
    {
        Rig r;
        r.paint();
        r.key(0x18 + 2);                            // INPUTS
        r.press(uf8sim::vpot(0, 3));
        EXPECT(r.lastSet() == "set /devices/0/inputs/0/preamps/0/Gain/value 13");
        const std::size_t n = r.eng.sets.size();
        r.press(uf8sim::vpot(2, 3));                // AVALON has no preamp
        EXPECT(r.eng.sets.size() == n);
        r.paint();
        EXPECT(r.uf8.lower[0] == "13" && r.uf8.barMode[2] == 0x03);
        // the preamp keys act on the selected channel
        r.key(0x22);                                // SEL 1
        r.key(0x6A);                                // soft-key bank 2: preamp
        r.paint();
        EXPECT(r.uf8.slotLabel[0] == "48V" && r.uf8.slotLabel[3] == "LOW CUT");
        r.key(0x18 + 1);                            // PAD
        EXPECT(r.lastSet() == "set /devices/0/inputs/0/preamps/0/Pad/value true");
        const std::size_t m = r.eng.sets.size();
        r.key(0x18);                                // 48V tapped: nothing
        r.paint(1000);
        EXPECT(r.eng.sets.size() == m);
        r.press(uf8sim::key(0x18, true));           // 48V held
        r.paint(uadsurf::kPhantomHoldMs - 100);
        EXPECT(r.eng.sets.size() == m);
        r.paint(200);
        EXPECT(r.lastSet() == "set /devices/0/inputs/0/preamps/0/48V/value true");
        r.paint(2000);                              // holding longer fires once
        EXPECT(r.eng.sets.size() == m + 1);
        r.press(uf8sim::key(0x18, false));
        r.paint();
        EXPECT(r.uf8.topKey(0) == uf8sim::Lamp::On && r.uf8.lower[0] == "13 48V");
        // a channel without preamp selected: the bank is dark and does nothing
        r.key(0x22 + 3 * 2);
        r.paint();
        EXPECT(r.uf8.slotLabel[1].empty());
        const std::size_t k = r.eng.sets.size();
        r.key(0x18 + 1);
        EXPECT(r.eng.sets.size() == k);
    }

    // ---- 8. monitor: encoder, ALT, DIM, MONO, MUTE ---------------------------------------
    {
        Rig r;
        r.paint();
        r.press(uf8sim::vpot(8, 2));
        EXPECT(r.lastSet() == "set /devices/0/outputs/4/CRMonitorLevel/value -18");
        r.press(uf8sim::vpot(8, 31));               // a fast spin: four steps at most
        r.paint(20);
        EXPECT(r.lastSet() == "set /devices/0/outputs/4/CRMonitorLevel/value -14");
        for (int i = 0; i < 10; ++i) { r.press(uf8sim::vpot(8, 4)); r.paint(20); }
        EXPECT(r.lastSet() == "set /devices/0/outputs/4/CRMonitorLevel/value 0");   // never past max
        r.key(0x76);                                // encoder push: monitor mute
        EXPECT(r.lastSet() == "set /devices/0/outputs/4/Mute/value true");
        r.key(0x69);                                // soft-key bank 1: monitor
        r.paint();
        EXPECT(r.uf8.slotLabel[0] == "ALT" && r.uf8.slotLabel[3] == "MUTE" && r.uf8.topKey(3) == uf8sim::Lamp::On);
        r.key(0x18);
        EXPECT(r.lastSet() == "set /devices/0/AltMonSelection/value 1");
        r.key(0x18 + 1);
        EXPECT(r.lastSet() == "set /devices/0/outputs/4/DimOn/value true");
        r.key(0x18 + 2);
        EXPECT(r.lastSet() == "set /devices/0/outputs/4/MixToMono/value true");
        r.paint();
        EXPECT(r.uf8.topKey(0) == uf8sim::Lamp::On && r.uf8.topKey(1) == uf8sim::Lamp::On);
    }

    // ---- 9. meters ----------------------------------------------------------------------
    {
        Rig r;
        r.paint();
        r.console("/devices/0/inputs/2/meters/0/MeterLevel", "-12.0");
        r.console("/devices/0/inputs/2/meters/1/MeterLevel", "-30.0");
        r.paint();
        EXPECT(r.uf8.vu[4] > r.uf8.vu[5] && r.uf8.vu[5] > 0 && r.uf8.vu[0] == 0);
        r.console("/devices/0/inputs/2/meters/0/MeterClip", "true");
        r.paint();
        EXPECT(r.uf8.vu[4] == 0x1F);
    }

    // ---- 10. offline: shown, nothing written ---------------------------------------------
    {
        Rig r;
        r.paint();
        r.console("/devices/0/DeviceOnline", "false");
        r.paint();
        EXPECT(r.uf8.lower[0] == "offline" && r.uf8.faderDb[0].empty());
        r.press(uf8sim::touch(0, true));
        r.press(uf8sim::fader(0, 1000));
        r.key(0x21);
        r.press(uf8sim::vpot(8, 3));
        r.paint(100);
        EXPECT(r.eng.sets.empty());
        r.console("/devices/0/DeviceOnline", "true");
        r.paint();
        EXPECT(r.uf8.lower[0] == "<C>");
    }

    // ---- 12. QUICK 2: the chosen channels on the faders; PLUG-IN -------------------------
    {
        Rig r;
        const std::vector<uad::Strip> all = r.surf.strips();
        EXPECT(r.surf.quick() == 0 && all.size() > 3);
        r.surf.setQuick2({uad::Model::stripId(all[2]), "0/in/999", "nonsense", uad::Model::stripId(all[0])});
        r.key(0x44);
        r.paint();
        EXPECT(r.surf.quick() == 1 && r.surf.strips().size() == 2);
        EXPECT(r.surf.strips().size() == 2 && r.surf.strips()[0] == all[2] && r.surf.strips()[1] == all[0]);
        r.eng.sets.clear();
        r.press(uf8sim::touch(0, true));
        r.press(uf8sim::fader(0, 9000, true));
        r.paint(100);
        bool wroteThird = false;
        for (const auto& s : r.eng.sets)
            wroteThird = wroteThird || s.find(uad::Model::propPath(all[2], "FaderLevelTapered")) != std::string::npos;
        EXPECT(wroteThird);
        r.press(uf8sim::touch(0, false));
        r.paint(300);
        r.key(0x43);
        r.paint();
        EXPECT(r.surf.quick() == 0 && r.surf.strips().size() == all.size());

        // PLUG-IN: SEL UAD 1, mode PLUGIN, the 1176 on V-Pot 3.
        r.key(0x22);
        r.key(0x1B);
        r.paint();
        EXPECT(r.surf.mode() == uadsurf::PotMode::Plugin && r.surf.selected() == 0);
        auto pv = r.surf.pluginView(2);
        EXPECT(pv.has && pv.label == "UA 1176LN Legacy" && pv.value == "on");
        EXPECT(!r.surf.pluginView(0).has);
        r.key(0x08 + 2);                                     // push = off
        r.paint(100);
        EXPECT(!r.model.devices[0].inputs[0].effects[2].power);
        r.key(0x48 + 0);                                     // an empty slot does not open
        EXPECT(r.surf.pluginSlot() == -1);
        r.key(0x48 + 2);                                     // the 1176's parameters
        r.paint();
        EXPECT(r.surf.pluginSlot() == 2);
        pv = r.surf.pluginView(0);
        EXPECT(pv.has && pv.label == "Input" && pv.value == "-34.92");
        const double before = r.model.devices[0].inputs[0].effects[2].params[0].norm;
        r.press(uf8sim::vpot(0, 5));
        r.paint(100);
        EXPECT(std::fabs(r.model.devices[0].inputs[0].effects[2].params[0].norm - (before + 0.05)) < 1e-6);
        r.key(0x48 + 2);                                     // again = back to the overview
        EXPECT(r.surf.pluginSlot() == -1);
        EXPECT(uadsurf::allowedProperty("/devices/0/inputs/0/effects/2/Power"));
        EXPECT(uadsurf::allowedProperty("/devices/0/inputs/0/effects/2/parameters/0/NormalizedValue"));
        EXPECT(!uadsurf::allowedProperty("/devices/0/inputs/0/effects/2/parameters/0/StringValue"));
        EXPECT(!uadsurf::allowedProperty("/devices/0/inputs/0/effects/2/Preset"));
        EXPECT(uadsurf::allowedProperty("/TalkbackOn") && !uadsurf::allowedProperty("/Dirty"));
        // a strip id survives the round trip
        uad::Strip back;
        EXPECT(uad::Model::parseStripId(uad::Model::stripId(all[1]), back) && back == all[1]);
    }

    // ---- 11. the layer keys stay Rea-Sixty's; the allowlist --------------------------------
    {
        Rig r;
        EXPECT(!r.press(uf8sim::key(0x40, true)) && !r.press(uf8sim::key(0x42, true)) && !r.press(uf8sim::key(0x46, true)));
        EXPECT(uadsurf::allowedProperty("/devices/0/inputs/3/FaderLevelTapered"));
        EXPECT(uadsurf::allowedProperty("/devices/0/inputs/3/sends/1/Bypass"));
        EXPECT(uadsurf::allowedProperty("/devices/0/outputs/4/CRMonitorLevel"));
        EXPECT(!uadsurf::allowedProperty("/devices/0/DeviceHwID"));
        EXPECT(!uadsurf::allowedProperty("/devices/0/inputs/3/Name"));
        EXPECT(!uadsurf::allowedProperty("/devices/0/inputs/3/preamps/0/GainTapered"));
        EXPECT(!uadsurf::allowedProperty("/devices/0/inputs/3/effects/0/EffectName"));
        EXPECT(!uadsurf::allowedProperty("/Session"));
        EXPECT(!uadsurf::allowedProperty("/devices/0/outputs/4/CRMonitorLevelTapered"));
        // everything any scenario above wrote was on the list (checked once more here
        // on a fresh rig driving every control)
        r.paint();
        for (int s = 0; s < 8; ++s) { r.press(uf8sim::touch(s, true)); r.press(uf8sim::fader(s, 4000 + s)); r.press(uf8sim::vpot(s, 1)); }
        for (std::uint8_t id = 0x00; id < 0x80; ++id) if (id != 0x40 && id != 0x41 && id != 0x42 && id != 0x46) r.key(id);
        r.press(uf8sim::vpot(8, 1));
        r.paint(1000);
        for (const auto& s : r.eng.sets) {
            const std::size_t a = s.find(' '), b = s.find(' ', a + 1);
            std::string path = s.substr(a + 1, b - a - 1);
            path.resize(path.size() - 6);           // "/value"
            EXPECT(uadsurf::allowedProperty(path));
        }
        EXPECT(!r.eng.sets.empty());
    }

    if (g_fail) std::printf("%d failure(s)\n", g_fail);
    else        std::printf("uad_surface: all passed\n");
    return g_fail ? 1 : 0;
}
