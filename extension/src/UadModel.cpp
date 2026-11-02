#include "UadModel.h"

#include "JsonTree.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace uad {

namespace {

using E = wdl_json_element;

std::vector<std::string> split(const std::string& p)
{
    std::vector<std::string> out;
    std::string cur;
    for (const char c : p) {
        if (c == '/') {
            if (!cur.empty()) out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

bool isIndex(const std::string& s)
{
    if (s.empty() || s.size() > 4) return false;
    for (const char c : s) if (c < '0' || c > '9') return false;
    return true;
}

// A property object {type, value, ...} or a bare value: the value element, null
// when the property has none (measured: some do not).
const E* valueEl(const E* p)
{
    if (!p) return nullptr;
    if (p->is_object()) return p->get_item_by_name("value");
    return p;
}

bool getNum(const E* p, double& out)
{
    const E* v = valueEl(p);
    if (!v || v->is_object() || v->is_array()) return false;
    const char* s = v->get_string_value(true);
    if (!s || !*s) return false;
    if (!std::strcmp(s, "true"))  { out = 1.0; return true; }
    if (!std::strcmp(s, "false")) { out = 0.0; return true; }
    char* end = nullptr;
    out = std::strtod(s, &end);
    return end && end != s;
}

bool getBool(const E* p, bool& out)
{
    const E* v = valueEl(p);
    if (!v || v->is_object() || v->is_array()) return false;
    const char* s = v->get_string_value(true);
    if (!s) return false;
    if (!std::strcmp(s, "true") || !std::strcmp(s, "1"))  { out = true;  return true; }
    if (!std::strcmp(s, "false") || !std::strcmp(s, "0")) { out = false; return true; }
    return false;
}

bool getStr(const E* p, std::string& out)
{
    const E* v = valueEl(p);
    if (!v || v->is_object() || v->is_array()) return false;
    const char* s = v->get_string_value(false);     // quoted strings only
    if (!s) return false;
    out = s;
    return true;
}

double fieldNum(const E* p, const char* name, double fallback)
{
    if (!p || !p->is_object()) return fallback;
    double d = fallback;
    const E* f = p->get_item_by_name(name);
    if (f && !f->is_object() && !f->is_array()) {
        const char* s = f->get_string_value(true);
        if (s && *s) d = std::strtod(s, nullptr);
    }
    return d;
}

template <class T>
bool assign(T& field, const T& v)
{
    if (field == v) return false;
    field = v;
    return true;
}

bool setNum(double& f, const E* p)  { double v; return getNum(p, v) && assign(f, v); }
bool setBool(bool& f, const E* p)   { bool v;   return getBool(p, v) && assign(f, v); }
bool setStr(std::string& f, const E* p) { std::string v; return getStr(p, v) && assign(f, v); }
bool setInt(int& f, const E* p)     { double v; return getNum(p, v) && assign(f, static_cast<int>(v)); }

std::vector<std::string> childKeys(const E* data)
{
    std::vector<std::string> keys;
    const E* ch = data && data->is_object() ? data->get_item_by_name("children") : nullptr;
    if (!ch || !ch->is_object()) return keys;
    for (int i = 0; ch->enum_item(i); ++i)
        if (const char* k = ch->enum_item_name(i)) keys.emplace_back(k);
    return keys;
}

int maxIndex(const std::vector<std::string>& keys)
{
    int m = -1;
    for (const auto& k : keys) if (isIndex(k)) m = std::max(m, std::atoi(k.c_str()));
    return m;
}

std::string join(const std::vector<std::string>& seg, std::size_t n)
{
    std::string s;
    for (std::size_t i = 0; i < n && i < seg.size(); ++i) s += "/" + seg[i];
    return s;
}

bool has(const std::vector<std::string>& keys, const char* k)
{
    for (const auto& x : keys) if (x == k) return true;
    return false;
}

template <class T>
T& at(std::vector<T>& v, int i)
{
    if (static_cast<int>(v.size()) <= i) v.resize(static_cast<std::size_t>(i) + 1);
    return v[static_cast<std::size_t>(i)];
}

bool meterProp(Meter& m, const std::string& prop, const E* p)
{
    if (prop == "MeterLevel")     return setNum(m.level, p);
    if (prop == "MeterPeakLevel") return setNum(m.peak, p);
    if (prop == "MeterClip")      return setBool(m.clip, p);
    return false;
}

}  // namespace

int Device::monitorOutput() const
{
    for (std::size_t i = 0; i < outputs.size(); ++i)
        if (outputs[i].monitor) return static_cast<int>(i);
    return -1;
}

Device& Model::dev_(int d) { return at(devices, d); }

std::vector<std::string> Model::onMessage(const std::string& json)
{
    wdl_json_parser parser;
    const E* root = parser.parse(json.c_str(), static_cast<int>(json.size()));
    JsonTreeGuard guard{parser, root};
    if (!root || !root->is_object() || root->get_item_by_name("error")) return {};
    const char* path = root->get_string_by_name("path");
    const E* data = root->get_item_by_name("data");
    if (!path || !data) return {};
    const std::vector<std::string> seg = split(path);
    if (seg.empty()) return {};

    // "<node>/<Property>/value": a subscribe answer, a push or a set ack.
    if (seg.back() == "value" && seg.size() >= 3) {
        std::vector<std::string> node(seg.begin(), seg.end() - 2);
        if (value_(node, seg[seg.size() - 2], data)) ++gen;
        return {};
    }
    // "get <node>/<Property>": the property object itself.
    if (data->is_object() && data->get_item_by_name("type") && !data->get_item_by_name("properties")
        && !data->get_item_by_name("children") && seg.size() >= 2) {
        std::vector<std::string> node(seg.begin(), seg.end() - 1);
        if (value_(node, seg.back(), data)) ++gen;
        return {};
    }
    if (!data->is_object()) return {};
    return node_(seg, data);
}

std::vector<std::string> Model::node_(const std::vector<std::string>& seg, const void* dataV)
{
    const E* data = static_cast<const E*>(dataV);
    const E* props = data->get_item_by_name("properties");
    const std::vector<std::string> keys = childKeys(data);
    const std::string here = join(seg, seg.size());
    std::vector<std::string> next;
    bool changed = false;
    auto eachProp = [&]() {
        if (!props || !props->is_object()) return;
        for (int i = 0; props->enum_item(i); ++i)
            if (const char* n = props->enum_item_name(i))
                changed |= value_(seg, n, props->enum_item(i));
    };
    auto prop = [&](const char* n) -> const E* {
        return props && props->is_object() ? props->get_item_by_name(n) : nullptr;
    };
    auto list = [&](auto& vec) {
        const int m = maxIndex(keys);
        if (m >= 0 && static_cast<int>(vec.size()) <= m) { vec.resize(static_cast<std::size_t>(m) + 1); changed = true; }
        for (const auto& k : keys) if (isIndex(k)) next.push_back(here + "/" + k);
    };
    const std::size_t n = seg.size();

    if (n == 1 && seg[0] == "devices") {
        for (const auto& k : keys) if (isIndex(k)) { dev_(std::atoi(k.c_str())); next.push_back(here + "/" + k); }
    } else if (n >= 2 && seg[0] == "devices" && isIndex(seg[1])) {
        Device& D = dev_(std::atoi(seg[1].c_str()));
        if (n == 2) {
            eachProp();
            if (const E* a = prop("AltMonSelection")) changed |= assign(D.altMonMax, static_cast<int>(fieldNum(a, "max", 0)));
            D.known = true;
            for (const char* c : {"inputs", "auxs", "outputs"}) if (has(keys, c)) next.push_back(here + "/" + c);
        } else if (seg[2] == "inputs" && n == 3) {
            list(D.inputs);
        } else if (seg[2] == "inputs" && isIndex(seg[3])) {
            Input& in = at(D.inputs, std::atoi(seg[3].c_str()));
            if (n == 4) {
                eachProp();
                if (const E* nm = prop("Name")) {
                    const E* d = nm->is_object() ? nm->get_item_by_name("default") : nullptr;
                    if (d && d->get_string_value(false)) changed |= assign(in.defaultName, std::string(d->get_string_value(false)));
                }
                in.known = true;
                for (const char* c : {"preamps", "sends", "meters"}) if (has(keys, c)) next.push_back(here + "/" + c);
            } else if (n == 5 && seg[4] == "preamps") {
                if (has(keys, "0")) { changed |= assign(in.hasPreamp, true); next.push_back(here + "/0"); }
            } else if (n == 5 && seg[4] == "sends") {
                list(in.sends);
            } else if (n == 5 && seg[4] == "meters") {
                for (const char* c : {"0", "1"}) if (has(keys, c)) next.push_back(here + "/" + c);
            } else if (n == 6 && (seg[4] == "preamps" || seg[4] == "sends" || seg[4] == "meters")) {
                eachProp();
            }
        } else if (seg[2] == "auxs" && n == 3) {
            list(D.auxs);
        } else if (seg[2] == "auxs" && isIndex(seg[3])) {
            Aux& a = at(D.auxs, std::atoi(seg[3].c_str()));
            if (n == 4) {
                eachProp();
                a.known = true;
                if (has(keys, "meters")) next.push_back(here + "/meters");
            } else if (n == 5 && seg[4] == "meters") {
                for (const char* c : {"0", "1"}) if (has(keys, c)) next.push_back(here + "/" + c);
            } else if (n == 6 && seg[4] == "meters") {
                eachProp();
            }
        } else if (seg[2] == "outputs" && n == 3) {
            list(D.outputs);
        } else if (seg[2] == "outputs" && isIndex(seg[3])) {
            Output& o = at(D.outputs, std::atoi(seg[3].c_str()));
            if (n == 4) {
                eachProp();
                if (const E* src = prop("MixInSource")) {
                    const E* vals = src->is_object() ? src->get_item_by_name("values") : nullptr;
                    std::vector<std::string> v;
                    if (vals && vals->is_array())
                        for (int i = 0; vals->enum_item(i); ++i) {
                            const E* x = vals->enum_item(i);
                            // a plain list, or a list of {value, string, enabled} [Ref]
                            const char* s = x->is_object() ? x->get_string_by_name("value") : x->get_string_value(false);
                            if (s) v.emplace_back(s);
                        }
                    changed |= assign(o.mixInValues, v);
                }
                if (const E* cr = prop("CRMonitorLevel")) {
                    changed |= assign(o.monitor, true);
                    changed |= assign(o.monitorMinDb, fieldNum(cr, "min", -96.0));
                    changed |= assign(o.monitorMaxDb, fieldNum(cr, "max", 0.0));
                }
                if (has(keys, "meters")) next.push_back(here + "/meters");
            } else if (n == 5 && seg[4] == "meters") {
                for (const char* c : {"0", "1"}) if (has(keys, c)) next.push_back(here + "/" + c);
            } else if (n == 6 && seg[4] == "meters") {
                eachProp();
            }
        }
    }
    if (changed) ++gen;
    return next;
}

bool Model::value_(const std::vector<std::string>& seg, const std::string& prop, const void* vV)
{
    const E* v = static_cast<const E*>(vV);
    const std::size_t n = seg.size();
    if (n < 2 || seg[0] != "devices" || !isIndex(seg[1])) return false;
    Device& D = dev_(std::atoi(seg[1].c_str()));
    if (n == 2) {
        if (prop == "DeviceName")      return setStr(D.name, v);
        if (prop == "DeviceTypeID")    return setStr(D.typeId, v);
        if (prop == "DeviceOnline")    return setBool(D.online, v);
        if (prop == "AltMonSelection") return setInt(D.altMon, v);
        if (prop == "DimAttenuation")  return setInt(D.dimAttenuation, v);
        return false;
    }
    if (n < 4 || !isIndex(seg[3])) return false;
    const int i = std::atoi(seg[3].c_str());
    if (seg[2] == "inputs") {
        Input& in = at(D.inputs, i);
        if (n == 4) {
            if (prop == "Name")              return setStr(in.name, v);
            if (prop == "StereoName")        return setStr(in.stereoName, v);
            if (prop == "IOType")            return setStr(in.ioType, v);
            if (prop == "Stereo")            return setBool(in.stereo, v);
            if (prop == "Active")            return setBool(in.active, v);
            if (prop == "EnabledByUser")     return setBool(in.enabledByUser, v);
            if (prop == "ChannelHidden")     return setBool(in.hidden, v);
            if (prop == "FaderLevel")        return setNum(in.faderDb, v);
            if (prop == "FaderLevelTapered") return setNum(in.faderTapered, v);
            if (prop == "Mute")              return setBool(in.mute, v);
            if (prop == "Solo")              return setBool(in.solo, v);
            if (prop == "Pan")               return setNum(in.pan, v);
            if (prop == "Pan2")              return setNum(in.pan2, v);
            return false;
        }
        if (n == 6 && seg[4] == "preamps" && seg[5] == "0") {
            Preamp& p = in.preamp;
            in.hasPreamp = true;
            if (prop == "Gain")      return setNum(p.gainDb, v);
            if (prop == "48V")       return setBool(p.phantom, v);
            if (prop == "Pad")       return setBool(p.pad, v);
            if (prop == "HiZ")       return setBool(p.hiZ, v);
            if (prop == "LowCut")    return setBool(p.lowCut, v);
            if (prop == "Phase")     return setBool(p.phase, v);
            if (prop == "PreampInputKnobSteps") return setInt(p.knobSteps, v);
            return false;
        }
        if (n == 6 && seg[4] == "sends" && isIndex(seg[5])) {
            Send& s = at(in.sends, std::atoi(seg[5].c_str()));
            if (prop == "ID")          return setStr(s.id, v);
            if (prop == "Name")        return setStr(s.name, v);
            if (prop == "Gain")        return setNum(s.gainDb, v);
            if (prop == "GainTapered") return setNum(s.gainTapered, v);
            if (prop == "Bypass")      return setBool(s.bypass, v);
            return false;
        }
        if (n == 6 && seg[4] == "meters" && (seg[5] == "0" || seg[5] == "1"))
            return meterProp(in.meter[seg[5] == "1"], prop, v);
        return false;
    }
    if (seg[2] == "auxs") {
        Aux& a = at(D.auxs, i);
        if (n == 4) {
            if (prop == "Name")              return setStr(a.name, v);
            if (prop == "FaderLevel")        return setNum(a.faderDb, v);
            if (prop == "FaderLevelTapered") return setNum(a.faderTapered, v);
            if (prop == "Mute")              return setBool(a.mute, v);
            if (prop == "MixToMono")         return setBool(a.mixToMono, v);
            if (prop == "Active")            return setBool(a.active, v);
            if (prop == "EnabledByUser")     return setBool(a.enabledByUser, v);
            return false;
        }
        if (n == 6 && seg[4] == "meters" && (seg[5] == "0" || seg[5] == "1"))
            return meterProp(a.meter[seg[5] == "1"], prop, v);
        return false;
    }
    if (seg[2] == "outputs") {
        Output& o = at(D.outputs, i);
        if (n == 4) {
            if (prop == "Name")                  return setStr(o.name, v);
            if (prop == "IOType")                return setStr(o.ioType, v);
            if (prop == "MixInSource")           return setStr(o.mixInSource, v);
            if (prop == "Mute")                  return setBool(o.mute, v);
            if (prop == "DimOn")                 return setBool(o.dimOn, v);
            if (prop == "MixToMono")             return setBool(o.mixToMono, v);
            if (prop == "CRMonitorLevel")        { const bool c = assign(o.monitor, true); return setNum(o.monitorDb, v) || c; }
            if (prop == "CRMonitorLevelTapered") return setNum(o.monitorTapered, v);
            return false;
        }
        if (n == 6 && seg[4] == "meters" && (seg[5] == "0" || seg[5] == "1"))
            return meterProp(o.meter[seg[5] == "1"], prop, v);
        return false;
    }
    return false;
}

std::vector<Strip> Model::strips() const
{
    std::vector<Strip> out;
    for (std::size_t d = 0; d < devices.size(); ++d) {
        const Device& D = devices[d];
        for (std::size_t i = 0; i < D.inputs.size(); ++i) {
            const Input& in = D.inputs[i];
            if (!in.known || !in.active || !in.enabledByUser || in.hidden) continue;
            // The right half of a stereo pair: its left half has the controls.
            if (in.stereo && (i % 2) == 1 && D.inputs[i - 1].stereo) continue;
            out.push_back({static_cast<int>(d), Strip::Kind::Input, static_cast<int>(i)});
        }
        for (std::size_t a = 0; a < D.auxs.size(); ++a) {
            const Aux& x = D.auxs[a];
            if (!x.known || !x.active || !x.enabledByUser) continue;
            out.push_back({static_cast<int>(d), Strip::Kind::Aux, static_cast<int>(a)});
        }
    }
    return out;
}

const Input* Model::input(const Strip& s) const
{
    if (s.kind != Strip::Kind::Input || s.device < 0 || s.device >= static_cast<int>(devices.size())) return nullptr;
    const auto& v = devices[static_cast<std::size_t>(s.device)].inputs;
    return s.index >= 0 && s.index < static_cast<int>(v.size()) ? &v[static_cast<std::size_t>(s.index)] : nullptr;
}

const Aux* Model::aux(const Strip& s) const
{
    if (s.kind != Strip::Kind::Aux || s.device < 0 || s.device >= static_cast<int>(devices.size())) return nullptr;
    const auto& v = devices[static_cast<std::size_t>(s.device)].auxs;
    return s.index >= 0 && s.index < static_cast<int>(v.size()) ? &v[static_cast<std::size_t>(s.index)] : nullptr;
}

std::string Model::name(const Strip& s) const
{
    if (const Input* in = input(s)) {
        if (in->stereo && !in->stereoName.empty()) return in->stereoName;
        return in->name.empty() ? in->defaultName : in->name;
    }
    if (const Aux* a = aux(s)) return a->name;
    return {};
}

bool Model::online() const
{
    for (const auto& d : devices) if (d.online) return true;
    return false;
}

std::string Model::devicePath(int d) { return "/devices/" + std::to_string(d); }

std::string Model::stripPath(const Strip& s)
{
    return devicePath(s.device) + (s.kind == Strip::Kind::Aux ? "/auxs/" : "/inputs/") + std::to_string(s.index);
}

std::string Model::propPath(const Strip& s, const char* prop) { return stripPath(s) + "/" + prop; }

}  // namespace uad
