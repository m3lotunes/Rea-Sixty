#pragma once
//
// UadModel: what UAD Console's mixer looks like, read from the UA Mixer Engine's
// replies (docs/uad-console-plan.md, Baustein B). Pure: JSON text in, a model out;
// no socket, no REAPER. Not tied to one interface: any number of devices
// (cascaded units are /devices/0, /devices/1, ...), inputs, preamps, sends and
// outputs, all taken from the tree. The monitor output is the output that has
// CRMonitorLevel, found by that property and never by its index.
//
// Two kinds of message feed it:
//   - a node reply to `get <node>`: {"path": p, "data": {"properties": {...},
//     "children": {...}}}. onMessage reads the properties it knows and returns the
//     child nodes worth asking for next (the walk skips plug-ins, ASIO ports and
//     the preset trees, which are most of the 1 MB).
//   - a value: {"path": "<node>/<Property>/value", "data": v}, the answer to a
//     subscribe, every push after it, and the acknowledgement of a set.
//
// Properties a node does not report keep their defaults (the engine leaves out
// "value" on some, measured 02.11.2026 on the user's Mac).
//
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace uad {

struct Meter {
    double level = -77.0;    // dBFS, -77 = silence (the engine's floor)
    double peak  = -77.0;
    bool   clip  = false;
};

struct Send {
    std::string id;          // aux1, aux2, cue1..cue4
    std::string name;
    double gainDb = -144.0;
    double gainTapered = 0.0;
    bool   bypass = false;
};

struct Preamp {
    double gainDb = 10.0;
    bool   phantom = false, pad = false, hiZ = false, lowCut = false, phase = false;
    int    knobSteps = 0;    // PreampInputKnobSteps, 0 = not reported
};

// A plug-in parameter (effects/K/parameters/N): only while a plug-in is loaded.
struct Param {
    std::string name, text;  // Name, StringValue ("-34.92")
    double norm = 0.0;       // NormalizedValue 0..1
};

// One insert slot (effects/K). EffectName "" = empty.
struct Effect {
    std::string name;
    bool   power = false;
    std::vector<Param> params;
};

struct Input {
    std::string name, defaultName, stereoName, ioType;
    bool   stereo = false, active = false, enabledByUser = false, hidden = false;
    double faderDb = -144.0, faderTapered = 0.0;
    bool   mute = false, solo = false;
    double pan = 0.0, pan2 = 0.0;
    bool   hasPreamp = false;
    Preamp preamp;
    std::vector<Send> sends;
    std::vector<Effect> effects;
    Meter  meter[2];
    bool   known = false;    // the node itself has been read
};

struct Aux {
    std::string name;
    double faderDb = -144.0, faderTapered = 0.0;
    bool   mute = false, mixToMono = false, active = true, enabledByUser = true;
    Meter  meter[2];
    bool   known = false;
};

struct Output {
    std::string name, ioType, mixInSource;
    std::vector<std::string> mixInValues;
    bool   mute = false, dimOn = false, mixToMono = false;
    bool   monitor = false;  // has CRMonitorLevel
    double monitorDb = -96.0, monitorTapered = 0.0, monitorMinDb = -96.0, monitorMaxDb = 0.0;
    Meter  meter[2];
};

struct Device {
    std::string name, typeId;
    bool   online = false;
    int    altMon = 0, altMonMax = 0;
    int    dimAttenuation = 17;
    std::vector<Input>  inputs;
    std::vector<Aux>    auxs;
    std::vector<Output> outputs;
    bool   known = false;
    int monitorOutput() const;   // index into outputs, -1 none
};

// One channel on the surface. A stereo pair is one strip, on its left (even) input,
// which carries the controls (plan section 3).
struct Strip {
    enum class Kind : std::uint8_t { Input, Aux };
    int  device = 0;
    Kind kind   = Kind::Input;
    int  index  = 0;
    bool operator==(const Strip& o) const { return device == o.device && kind == o.kind && index == o.index; }
};

class Model {
public:
    // Any message from the engine. Returns the nodes to `get` next (a walk starts
    // with get /devices and feeds every reply here). Errors and unknown paths are
    // ignored.
    std::vector<std::string> onMessage(const std::string& json);

    std::vector<Device> devices;
    bool talkbackOn = false;         // /TalkbackOn, the Console's TALKBACK button
    // Plug-in windows the engine relayed as opened ("/OpenWindow") and not yet as
    // closed ("/CloseWindow"), by effect path. Measured 03.11.2026: the engine
    // passes every client's post on to all clients, Console opens the window.
    std::vector<std::string> openWindows;
    bool windowOpen(const std::string& effectPath) const;    std::uint32_t gen = 0;           // bumped by every change

    // The channels in Console order: per device the visible inputs (Active, enabled
    // by the user, not hidden; a stereo pair once), then its auxes.
    std::vector<Strip> strips() const;
    std::string        name(const Strip& s) const;
    const Input*       input(const Strip& s) const;
    const Aux*         aux(const Strip& s) const;
    bool               online() const;   // any device online

    // Paths, for subscribing and writing. Property paths, without "/value".
    static std::string devicePath(int d);
    static std::string stripPath(const Strip& s);
    static std::string propPath(const Strip& s, const char* prop);
    // A strip as stable text for bindings.json: "0/in/2", "0/aux/1". parseStripId
    // reads it back (false = not one).
    static std::string stripId(const Strip& s);
    static bool        parseStripId(const std::string& id, Strip& out);

private:
    std::vector<std::string> node_(const std::vector<std::string>& seg, const void* data);
    bool value_(const std::vector<std::string>& seg, const std::string& prop, const void* v);
    Device& dev_(int d);
};

}  // namespace uad
