// UadModel against the virtual engine (UadSim.h) built from the tree measured on
// the user's Mac: the walk, the channel list, stereo pairs, preamps, sends,
// auxes, the monitor output, values from subscriptions and set acks, errors,
// missing values, a second (cascaded) device.

#include "UadModel.h"
#include "UadProto.h"
#include "UadSim.h"

#include <cstdio>
#include <deque>
#include <string>

static int g_fail = 0;
#define EXPECT(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

std::string strip0(std::string c) { if (!c.empty() && c.back() == '\0') c.pop_back(); return c; }

// The walk a client does: get /devices, then whatever the model asks for.
int walk(uad::Model& m, uadsim::Engine& e)
{
    std::deque<std::string> todo{"/devices"};
    int gets = 0;
    while (!todo.empty()) {
        const std::string p = todo.front();
        todo.pop_front();
        ++gets;
        for (const auto& reply : e.handle(strip0(uad::get(p))))
            for (auto& n : m.onMessage(reply)) todo.push_back(n);
    }
    return gets;
}

void feed(uad::Model& m, const std::vector<std::string>& msgs)
{
    for (const auto& x : msgs) m.onMessage(x);
}

}  // namespace

int main()
{
    // ---- the walk -----------------------------------------------------------------
    uadsim::Engine e = uadsim::twinMkII();
    uad::Model m;
    const int gets = walk(m, e);
    // devices, device, 3 lists, 22 inputs, their preamps/sends/meters lists and
    // nodes, 2 auxes, 7 outputs and meters: no plug-ins, no presets walked
    EXPECT(gets > 200 && gets < 500);
    EXPECT(m.devices.size() == 1);
    const uad::Device& D = m.devices[0];
    EXPECT(D.known && D.online && D.name == "Apollo Twin MkII" && D.typeId == "Apollo Twin MkII");
    EXPECT(D.altMon == 0 && D.altMonMax == 2 && D.dimAttenuation == 17);
    EXPECT(D.inputs.size() == 22 && D.auxs.size() == 2 && D.outputs.size() == 7);

    // ---- inputs -----------------------------------------------------------------
    EXPECT(D.inputs[0].name == "UAD 1" && D.inputs[0].defaultName == "ANALOG 1" && D.inputs[0].ioType == "Mic");
    EXPECT(D.inputs[0].mute && D.inputs[0].solo);
    EXPECT(D.inputs[0].faderDb == 0.0 && D.inputs[0].faderTapered > 0.78 && D.inputs[0].faderTapered < 0.79);
    EXPECT(D.inputs[2].name == "AVALON" && D.inputs[2].faderDb == -6.0);
    EXPECT(D.inputs[12].stereo && D.inputs[12].stereoName == "REAPER");
    EXPECT(D.inputs[21].ioType.empty());                 // no value in the reply: stays default
    EXPECT(D.inputs[0].hasPreamp && D.inputs[1].hasPreamp && !D.inputs[2].hasPreamp);
    EXPECT(D.inputs[0].preamp.gainDb == 10.0 && D.inputs[0].preamp.knobSteps == 54 && !D.inputs[0].preamp.phantom);
    EXPECT(D.inputs[3].sends.size() == 6 && D.inputs[3].sends[0].id == "aux1" && D.inputs[3].sends[5].id == "cue4");
    EXPECT(D.inputs[3].sends[2].name == "HP" && D.inputs[3].sends[2].gainDb == -144.0);
    EXPECT(D.inputs[3].meter[0].level == -77.0 && D.inputs[3].meter[1].peak == -77.0);

    // ---- the channel list -----------------------------------------------------------
    const auto strips = m.strips();
    std::string names;
    for (const auto& s : strips) names += m.name(s) + "|";
    EXPECT(strips.size() == 14);
    EXPECT(names == "UAD 1|UAD 2|AVALON|ADAT 2|ADAT 3|ADAT 4|ADAT 5|ADAT 6|ADAT 7|ADAT 8|REAPER|VoX Return|AUX 1|AUX 2|");
    if (strips.size() == 14) {
        EXPECT(strips[10].index == 12 && strips[11].index == 14);           // stereo pairs on their left half
        EXPECT(strips[12].kind == uad::Strip::Kind::Aux && strips[13].index == 1);
        EXPECT(uad::Model::propPath(strips[2], "FaderLevelTapered") == "/devices/0/inputs/2/FaderLevelTapered");
        EXPECT(uad::Model::stripPath(strips[13]) == "/devices/0/auxs/1");
    }
    EXPECT(m.aux({0, uad::Strip::Kind::Aux, 0})->faderDb == -10.0);

    // ---- the monitor ----------------------------------------------------------------
    EXPECT(D.monitorOutput() == 4);
    const uad::Output& mon = D.outputs[4];
    EXPECT(mon.monitor && mon.name == "MONITOR" && mon.monitorDb == -20.0);
    EXPECT(mon.monitorMinDb == -96.0 && mon.monitorMaxDb == 0.0);
    EXPECT(mon.mixInSource == "mon" && mon.mixInValues.size() == 5 && mon.mixInValues[1] == "cue1");
    EXPECT(D.outputs[5].mixInSource == "cue1" && D.outputs[5].mixInValues.size() == 6 && !D.outputs[5].monitor);

    // ---- values: subscribe answers, pushes, set acks -----------------------------------
    {
        const uint32_t g = m.gen;
        feed(m, e.handle(strip0(uad::subscribe("/devices/0/inputs/2/FaderLevelTapered"))));
        EXPECT(m.gen == g);                                 // same value: no change
        feed(m, e.change("/devices/0/inputs/2/FaderLevelTapered", "0.5"));
        EXPECT(m.devices[0].inputs[2].faderTapered == 0.5 && m.gen == g + 1);
        feed(m, e.change("/devices/0/inputs/3/FaderLevelTapered", "0.4"));   // not subscribed: no push
        EXPECT(m.devices[0].inputs[3].faderTapered != 0.4);
        // a set and its ack
        feed(m, e.handle(strip0(uad::setBool("/devices/0/inputs/4/Mute", true))));
        EXPECT(m.devices[0].inputs[4].mute);
        EXPECT(e.sets.size() == 1 && e.sets[0] == "set /devices/0/inputs/4/Mute/value true");
        // clamped by the engine, the model takes what the engine says
        feed(m, e.handle(strip0(uad::setNumber("/devices/0/outputs/4/CRMonitorLevel", 6.0))));
        EXPECT(m.devices[0].outputs[4].monitorDb == 0.0);
        // a meter push, a device going offline
        e.subs.insert("/devices/0/inputs/0/meters/1/MeterLevel");
        feed(m, e.change("/devices/0/inputs/0/meters/1/MeterLevel", "-12.5"));
        EXPECT(m.devices[0].inputs[0].meter[1].level == -12.5);
        e.subs.insert("/devices/0/DeviceOnline");
        feed(m, e.change("/devices/0/DeviceOnline", "false"));
        EXPECT(!m.devices[0].online && !m.online());
        // a property reply ("get <prop>")
        feed(m, e.handle("get /devices/0/AltMonSelection"));
        EXPECT(m.devices[0].altMon == 0);
        e.change("/devices/0/AltMonSelection", "1");
        feed(m, e.handle("get /devices/0/AltMonSelection"));
        EXPECT(m.devices[0].altMon == 1);
        // a name change in the Console shows up in the list
        e.subs.insert("/devices/0/inputs/3/Name");
        feed(m, e.change("/devices/0/inputs/3/Name", uadsim::q("Bass DI")));
        EXPECT(m.name(m.strips()[3]) == "Bass DI");
        // a channel switched off in the Console leaves the list
        e.subs.insert("/devices/0/inputs/9/EnabledByUser");
        feed(m, e.change("/devices/0/inputs/9/EnabledByUser", "false"));
        EXPECT(m.strips().size() == 13);
    }

    // ---- what is ignored ------------------------------------------------------------
    {
        const uint32_t g = m.gen;
        feed(m, e.handle("get /devices/0/nope"));                       // error reply
        feed(m, e.handle("set /devices/0/inputs/0/Mute true"));         // no /value: error
        m.onMessage("{not json");
        m.onMessage("");
        m.onMessage("{\"path\":\"/devices/0/inputs/99/Mute/value\",\"data\":true}");   // grows, harmless
        m.onMessage("{\"path\":\"/somewhere/else/value\",\"data\":1}");
        EXPECT(m.devices[0].inputs.size() == 100 && m.gen == g + 1);
        EXPECT(e.handle("frobnicate /devices").empty());                // unknown verb: no reply
    }

    // ---- two units (cascaded) -------------------------------------------------------------
    {
        uadsim::Engine e2 = uadsim::twinMkII();
        std::vector<std::pair<std::string, uadsim::Node>> copy;
        for (const auto& [p, n] : e2.nodes)
            if (p.rfind("/devices/0", 0) == 0) copy.push_back({"/devices/1" + p.substr(10), n});
        for (auto& [p, n] : copy) e2.nodes[p] = n;
        e2.link("/devices", "1");
        e2.prop("/devices/1", "DeviceName", "string", uadsim::q("Apollo x8"));
        e2.prop("/devices/1/inputs/0", "Name", "string", uadsim::q("KICK"));
        uad::Model m2;
        walk(m2, e2);
        EXPECT(m2.devices.size() == 2 && m2.devices[1].name == "Apollo x8");
        const auto s2 = m2.strips();
        EXPECT(s2.size() == 28 && s2[14].device == 1 && m2.name(s2[14]) == "KICK");
        EXPECT(uad::Model::propPath(s2[14], "Mute") == "/devices/1/inputs/0/Mute");
    }

    if (g_fail) std::printf("%d failure(s)\n", g_fail);
    else        std::printf("uad_model: all passed\n");
    return g_fail ? 1 : 0;
}
