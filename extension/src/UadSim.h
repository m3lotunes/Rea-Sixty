#pragma once
//
// UadSim: a UA Mixer Engine in software, for tests and the simulator. It answers
// get / set / subscribe the way the real engine does (docs/uad-mixer-engine-
// protocol.md): node replies with properties and children, property replies,
// "<prop>/value" for subscribe answers, pushes and set acks, and an error for a
// path it does not know. Values are clamped to min/max as the engine does.
//
// twinMkII() builds the tree measured on the user's Mac on 02.11.2026 (Apollo
// Twin MkII, UAD Console; docs/uad-console-plan.md section 3): the same inputs,
// names, stereo pairs, visibility, preamps, sends, auxes, outputs and meters, the
// properties without a value, a preset tree in every input. Plug-in, ASIO and
// preset contents are left out or kept small.
//
// It is not the engine: no step tables, no metering of its own, no network.
//
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace uadsim {

struct Prop {
    std::string type;               // string, bool, int, int64, float
    std::string value;              // JSON text; empty = the property has no value
    std::string def;                // JSON text of "default", empty = none
    std::string values;             // JSON array text of "values", empty = none
    bool   hasRange = false;
    double min = 0, max = 0;
    bool   readonly = false;
    std::string raw;                // a whole JSON object instead (the preset tree)
};

struct Node {
    std::vector<std::pair<std::string, Prop>> props;
    std::vector<std::string> children;
    Prop* find(const std::string& n)
    {
        for (auto& p : props) if (p.first == n) return &p.second;
        return nullptr;
    }
};

inline std::string q(const std::string& s)
{
    std::string o = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') o.push_back('\\');
        o.push_back(c);
    }
    return o + "\"";
}

inline std::string num(double v)
{
    char b[32];
    std::snprintf(b, sizeof(b), "%.7g", v);
    std::string s = b;
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos) s += ".0";
    return s;
}

class Engine {
public:
    std::map<std::string, Node> nodes;     // node path -> node
    std::vector<std::string> sets;         // every set as received ("set <path> <value>")
    std::set<std::string> subs;            // subscribed property paths (no "/value")
    int gets = 0;

    Node& node(const std::string& path) { return nodes[path]; }
    void link(const std::string& parent, const std::string& key)
    {
        auto& c = nodes[parent].children;
        if (std::find(c.begin(), c.end(), key) == c.end()) c.push_back(key);
        nodes[parent + "/" + key];
    }
    Prop& prop(const std::string& nodePath, const std::string& name, const std::string& type,
               const std::string& jsonValue)
    {
        Node& n = nodes[nodePath];
        if (Prop* p = n.find(name)) { p->type = type; p->value = jsonValue; return *p; }
        n.props.push_back({name, Prop{type, jsonValue, "", "", false, 0, 0, false, ""}});
        return n.props.back().second;
    }

    // The current JSON value of a property path, "" when unknown or valueless.
    std::string value(const std::string& propPath)
    {
        auto [n, p] = split_(propPath);
        if (!n) return {};
        Prop* pr = n->find(p);
        return pr ? pr->value : std::string();
    }

    // One command, without its NUL. Returns the engine's messages, without NULs.
    std::vector<std::string> handle(const std::string& cmd)
    {
        std::vector<std::string> out;
        const std::size_t sp = cmd.find(' ');
        if (sp == std::string::npos) return out;
        const std::string method = cmd.substr(0, sp);
        std::string rest = cmd.substr(sp + 1);
        std::string path = rest, val;
        if (method == "set") {
            const std::size_t sp2 = rest.find(' ');
            if (sp2 == std::string::npos) return out;
            path = rest.substr(0, sp2);
            val = rest.substr(sp2 + 1);
        }
        while (path.size() > 1 && path.back() == '/') path.pop_back();
        if (method == "get") {
            ++gets;
            auto it = nodes.find(path);
            if (it != nodes.end()) { out.push_back(nodeReply_(path, it->second)); return out; }
            auto [n, p] = split_(path);
            Prop* pr = n ? n->find(p) : nullptr;
            if (pr) out.push_back("{\"path\":" + q(path) + ",\"data\":" + propJson_(*pr) + "}");
            else out.push_back(error_(path, "get"));
            return out;
        }
        if (method == "subscribe") {
            std::string pp = stripValue_(path);
            auto [n, p] = split_(pp);
            Prop* pr = n ? n->find(p) : nullptr;
            if (!pr) { out.push_back(error_(path, "subscribe")); return out; }
            subs.insert(pp);
            out.push_back(valueMsg_(pp, *pr));
            return out;
        }
        if (method == "unsubscribe") {
            subs.erase(stripValue_(path));
            return out;
        }
        if (method == "set") {
            sets.push_back(cmd);
            const std::string pp = stripValue_(path);
            auto [n, p] = split_(pp);
            Prop* pr = n ? n->find(p) : nullptr;
            if (!pr || pp == path) { out.push_back(error_(path, "set")); return out; }
            pr->value = clamp_(*pr, val);
            out.push_back(valueMsg_(pp, *pr));       // the ack
            return out;
        }
        return out;                                   // unknown verbs: no reply
    }

    // The engine changes a value itself (Console's GUI, a knob, audio): the push
    // every subscriber gets. Empty when nobody subscribed.
    std::vector<std::string> change(const std::string& propPath, const std::string& jsonValue)
    {
        auto [n, p] = split_(propPath);
        Prop* pr = n ? n->find(p) : nullptr;
        if (!pr) return {};
        pr->value = clamp_(*pr, jsonValue);
        if (!subs.count(propPath)) return {};
        return {valueMsg_(propPath, *pr)};
    }

private:
    static std::string stripValue_(std::string p)
    {
        const std::string leaf = "/value";
        if (p.size() > leaf.size() && p.compare(p.size() - leaf.size(), leaf.size(), leaf) == 0)
            p.resize(p.size() - leaf.size());
        return p;
    }
    std::pair<Node*, std::string> split_(const std::string& propPath)
    {
        const std::size_t s = propPath.rfind('/');
        if (s == std::string::npos || s == 0) return {nullptr, {}};
        auto it = nodes.find(propPath.substr(0, s));
        if (it == nodes.end()) return {nullptr, {}};
        return {&it->second, propPath.substr(s + 1)};
    }
    static std::string clamp_(const Prop& p, const std::string& v)
    {
        if (!p.hasRange || p.type == "string" || p.type == "bool") return v;
        const double d = std::clamp(std::strtod(v.c_str(), nullptr), p.min, p.max);
        if (p.type == "int" || p.type == "int64") return std::to_string(static_cast<long long>(d));
        return num(d);
    }
    static std::string propJson_(const Prop& p)
    {
        if (!p.raw.empty()) return p.raw;
        std::string s = "{\"type\":" + q(p.type);
        if (p.readonly) s += ",\"readonly\":true";
        if (p.hasRange) s += ",\"min\":" + num(p.min) + ",\"max\":" + num(p.max);
        if (!p.def.empty()) s += ",\"default\":" + p.def;
        if (!p.values.empty()) s += ",\"values\":" + p.values;
        if (!p.value.empty()) s += ",\"value\":" + p.value;
        return s + "}";
    }
    static std::string nodeReply_(const std::string& path, const Node& n)
    {
        std::string s = "{\"path\":" + q(path) + ",\"data\":{\"properties\":{";
        bool first = true;
        for (const auto& [name, p] : n.props) {
            if (!first) s += ",";
            first = false;
            s += q(name) + ":" + propJson_(p);
        }
        s += "}";
        if (!n.children.empty()) {
            s += ",\"children\":{";
            for (std::size_t i = 0; i < n.children.size(); ++i) s += (i ? "," : "") + q(n.children[i]) + ":{}";
            s += "}";
        }
        return s + "}}";
    }
    static std::string valueMsg_(const std::string& propPath, const Prop& p)
    {
        return "{\"path\":" + q(propPath + "/value") + ",\"data\":" + (p.value.empty() ? "null" : p.value) + "}";
    }
    static std::string error_(const std::string& path, const char* verb)
    {
        return "{\"path\":" + q(path) + ",\"error\":\"Unable to resolve path for " + verb + ".\"}";
    }
};

// ---- the user's interface, as measured --------------------------------------------

inline Prop& range(Prop& p, double lo, double hi) { p.hasRange = true; p.min = lo; p.max = hi; return p; }

inline Engine twinMkII(bool online = true)
{
    Engine e;
    const std::string D = "/devices/0";
    e.link("/devices", "0");
    e.prop(D, "Type", "string", q("device"));
    e.prop(D, "DeviceName", "string", q("Apollo Twin MkII"));
    e.prop(D, "DeviceTypeID", "string", q("Apollo Twin MkII"));
    e.prop(D, "DeviceOnline", "bool", online ? "true" : "false");
    e.prop(D, "DeviceHwID", "int64", "1234567").readonly = true;
    e.prop(D, "CueBusCount", "int", "2");
    range(e.prop(D, "AltMonSelection", "int", "0"), 0, 2);
    range(e.prop(D, "DimAttenuation", "int", "17"), 0, 60).def = "17";
    e.prop(D, "TalkbackMaster", "int", "-1");
    e.prop(D, "ActiveSpans", "int", "1095909712");         // uninitialised, as measured ("PARA")
    for (const char* c : {"inputs", "auxs", "outputs"}) e.link(D, c);

    struct In { const char* def; const char* name; const char* io; bool stereo; const char* stereoName; bool active; bool enabled; };
    const In ins[22] = {
        {"ANALOG 1", "UAD 1", "Mic", false, "ANALOG 1/2", true, true},
        {"ANALOG 2", "UAD 2", "Line", false, "ANALOG 2", true, true},
        {"ADAT 1", "AVALON", "ADAT", false, "ADAT 1/2", true, true},
        {"ADAT 2", "ADAT 2", "ADAT", false, "ADAT 2", true, true},
        {"ADAT 3", "ADAT 3", "ADAT", false, "ADAT 3/4", true, true},
        {"ADAT 4", "ADAT 4", "ADAT", false, "ADAT 4", true, true},
        {"ADAT 5", "ADAT 5", "ADAT", false, "ADAT 5/6", true, true},
        {"ADAT 6", "ADAT 6", "ADAT", false, "ADAT 6", true, true},
        {"ADAT 7", "ADAT 7", "ADAT", false, "ADAT 7/8", true, true},
        {"ADAT 8", "ADAT 8", "ADAT", false, "ADAT 8", true, true},
        {"S/PDIF L", "S/PDIF L", "S/PDIF", true, "S/PDIF", false, true},
        {"S/PDIF R", "S/PDIF R", "S/PDIF", true, "S/PDIF", false, true},
        {"VIRTUAL 1/2", "VIRTUAL 1", "Virtual", true, "REAPER", true, true},
        {"VIRTUAL 2", "VIRTUAL 2", "Virtual", true, "VIRTUAL 2", false, true},
        {"VIRTUAL 3/4", "VIRTUAL Return", "Virtual", true, "VoX Return", true, true},
        {"VIRTUAL 4", "VIRTUAL 4", "Virtual", true, "VIRTUAL 4", false, true},
        {"VIRTUAL 5", "VIRTUAL 5", "Virtual", false, "VIRTUAL 5/6", false, false},
        {"VIRTUAL 6", "VIRTUAL 6", "Virtual", false, "VIRTUAL 6", false, false},
        {"VIRTUAL 7", "VIRTUAL 7", "Virtual", false, "VIRTUAL 7/8", false, false},
        {"VIRTUAL 8", "VIRTUAL 8", "Virtual", false, "VIRTUAL 8", false, false},
        {"TALKBACK", "TALKBACK", "TalkbackMic", false, "TALKBACK", false, true},
        {"N/A 1", "N/A 1", "", false, "N/A 1/2", false, false},
    };
    const char* sendIds[6] = {"aux1", "aux2", "cue1", "cue2", "cue3", "cue4"};
    const char* sendNames[6] = {"AUX 1", "AUX 2", "HP", "LINE 3/4", "CUE 3", "CUE 4"};
    // A small stand-in for the 124-node Channel Strip preset tree every input carries.
    const std::string preset = "{\"type\":\"string\",\"plugin\":{\"name\":\"Channel Strip\",\"params\":[1,2,3]},"
                               "\"value\":{\"nodes\":{\"a\":{\"b\":[{\"c\":1}]}}}}";
    for (int i = 0; i < 22; ++i) {
        const In& x = ins[i];
        const std::string I = D + "/inputs/" + std::to_string(i);
        e.link(D + "/inputs", std::to_string(i));
        e.prop(I, "Type", "string", q("input"));
        e.prop(I, "Name", "string", q(x.name)).def = q(x.def);
        e.prop(I, "StereoName", "string", q(x.stereoName));
        // input 21's IOType comes without a value (measured)
        e.prop(I, "IOType", "string", *x.io ? q(x.io) : "");
        e.prop(I, "Stereo", "bool", x.stereo ? "true" : "false");
        e.prop(I, "Active", "bool", x.active ? "true" : "false").readonly = true;
        e.prop(I, "EnabledByUser", "bool", x.enabled ? "true" : "false");
        e.prop(I, "ChannelHidden", "bool", "false");
        range(e.prop(I, "FaderLevel", "float", num(i == 0 ? 0.0 : -6.0)), -144, 12);
        range(e.prop(I, "FaderLevelTapered", "float", num(i == 0 ? 0.7818182 : 0.6727273)), 0, 1);
        e.prop(I, "Mute", "bool", i == 0 ? "true" : "false");          // input 0: mute and solo on (measured)
        e.prop(I, "Solo", "bool", i == 0 ? "true" : "false");
        range(e.prop(I, "Pan", "float", "0.0"), -1, 1);
        range(e.prop(I, "Pan2", "float", "0.0"), -1, 1);
        e.prop(I, "SoloSafe", "bool", "false");
        e.prop(I, "Preset", "string", "").raw = preset;
        if (i < 2) {
            e.link(I, "preamps");
            e.link(I + "/preamps", "0");
            const std::string P = I + "/preamps/0";
            range(e.prop(P, "Gain", "float", "10.0"), -144, 65).def = "10.0";
            e.prop(P, "48V", "bool", "false");
            e.prop(P, "Pad", "bool", "false");
            e.prop(P, "HiZ", "bool", "false");
            e.prop(P, "LowCut", "bool", "false");
            e.prop(P, "Phase", "bool", "false");
            range(e.prop(P, "GainTapered", "float", i == 0 ? "1.0" : "0.0"), 0, 1);  // offline mismatch, measured
            e.prop(P, "PreampInputKnobSteps", "int", "54");
        }
        e.link(I, "sends");
        for (int s = 0; s < 6; ++s) {
            const std::string S = I + "/sends/" + std::to_string(s);
            e.link(I + "/sends", std::to_string(s));
            e.prop(S, "ID", "string", q(sendIds[s]));
            e.prop(S, "Name", "string", q(sendNames[s]));
            range(e.prop(S, "Gain", "float", "-144.0"), -144, 12);
            range(e.prop(S, "GainTapered", "float", "0.0"), 0, 1);
            e.prop(S, "Bypass", "bool", "false");
            range(e.prop(S, "Pan", "float", "0.0"), -1, 1);
        }
        e.link(I, "meters");
        for (int k = 0; k < 2; ++k) {
            const std::string M = I + "/meters/" + std::to_string(k);
            e.link(I + "/meters", std::to_string(k));
            range(e.prop(M, "MeterLevel", "float", "-77.0"), -77, 0);
            range(e.prop(M, "MeterPeakLevel", "float", "-77.0"), -77, 0);
            e.prop(M, "MeterClip", "bool", "false");
        }
        e.link(I, "effects");                     // eight slots
        for (int k = 0; k < 8; ++k) {
            const std::string F = I + "/effects/" + std::to_string(k);
            e.link(I + "/effects", std::to_string(k));
            // UAD 1 slot 2: a 1176 with two of its parameters (measured names and
            // values, 03.11.2026); every other slot empty.
            const bool loaded = i == 0 && k == 2;
            e.prop(F, "EffectName", "string", q(loaded ? "UAD UA 1176LN Legacy" : ""));
            e.prop(F, "Power", "bool", loaded ? "true" : "false");
            if (loaded) {
                e.link(F, "parameters");
                const char* pn[2] = {"Input", "Output"};
                const char* pv[2] = {"0.272552490234375", "0.670074462890625"};
                for (int p = 0; p < 2; ++p) {
                    const std::string Pp = F + "/parameters/" + std::to_string(p);
                    e.link(F + "/parameters", std::to_string(p));
                    e.prop(Pp, "Name", "string", q(pn[p]));
                    e.prop(Pp, "StringValue", "string", q(p ? "-15.84" : "-34.92"));
                    range(e.prop(Pp, "NormalizedValue", "float", pv[p]), 0, 1);
                }
            }
        }
    }
    for (int a = 0; a < 2; ++a) {
        const std::string A = D + "/auxs/" + std::to_string(a);
        e.link(D + "/auxs", std::to_string(a));
        e.prop(A, "Name", "string", q(a ? "AUX 2" : "AUX 1"));
        e.prop(A, "Gain", "float", "");               // no value (measured)
        e.prop(A, "GainTapered", "float", "");
        range(e.prop(A, "FaderLevel", "float", "-10.0"), -144, 12);
        range(e.prop(A, "FaderLevelTapered", "float", num((-10.0 + 43) / 55)), 0, 1);
        e.prop(A, "Mute", "bool", "false");
        e.prop(A, "MixToMono", "bool", "false");
        e.prop(A, "Active", "bool", "true").readonly = true;
        e.prop(A, "EnabledByUser", "bool", "true");
        e.link(A, "meters");
        for (int k = 0; k < 2; ++k) {
            const std::string M = A + "/meters/" + std::to_string(k);
            e.link(A + "/meters", std::to_string(k));
            range(e.prop(M, "MeterLevel", "float", "-77.0"), -77, 0);
            range(e.prop(M, "MeterPeakLevel", "float", "-77.0"), -77, 0);
            e.prop(M, "MeterClip", "bool", "false");
        }
    }
    struct Out { const char* name; const char* io; };
    const Out outs[7] = {{"HP", "Cue"}, {"LINE 3/4", "Cue"}, {"CUE 3", "Cue"}, {"CUE 4", "Cue"},
                         {"MONITOR", "Monitor"}, {"HP", "Headphone"}, {"LINE 3/4", "Line"}};
    for (int o = 0; o < 7; ++o) {
        const std::string O = D + "/outputs/" + std::to_string(o);
        e.link(D + "/outputs", std::to_string(o));
        e.prop(O, "Name", "string", q(outs[o].name));
        e.prop(O, "IOType", "string", q(outs[o].io));
        if (o == 4) {
            e.prop(O, "MixInSource", "string", q("mon")).values = "[\"mon\",\"cue1\",\"cue2\",\"cue3\",\"cue4\"]";
            e.prop(O, "MixToMono", "bool", "false");
            e.prop(O, "Mute", "bool", "false");
            e.prop(O, "DimOn", "bool", "false");
            range(e.prop(O, "CRMonitorLevel", "float", "-20.0"), -96, 0);
            range(e.prop(O, "CRMonitorLevelTapered", "float", num(41.0 / 54)), 0, 1);
            e.prop(O, "MirrorsToDigital", "bool", "false");
        } else {
            e.prop(O, "MixInSource", "string", q(o == 5 ? "cue1" : "default"))
                .values = "[\"default\",\"cue1\",\"cue2\",\"cue3\",\"cue4\",\"none\"]";
            e.prop(O, "Mute", "bool", "false");
            e.prop(O, "Stereo", "bool", "true");
            if (o == 6) {
                e.prop(O, "AltMonEnabled", "bool", "true");
                e.prop(O, "AltMonTrim", "float", "0.0");
            }
        }
        e.link(O, "meters");
        for (int k = 0; k < 2; ++k) {
            const std::string M = O + "/meters/" + std::to_string(k);
            e.link(O + "/meters", std::to_string(k));
            range(e.prop(M, "MeterLevel", "float", "-77.0"), -77, 0);
            range(e.prop(M, "MeterPeakLevel", "float", "-77.0"), -77, 0);
            e.prop(M, "MeterClip", "bool", "false");
        }
    }
    return e;
}

}  // namespace uadsim
