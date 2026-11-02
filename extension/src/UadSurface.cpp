#include "UadSurface.h"

#include "Protocol.h"
#include "TrackName.h"     // utf8ToLatin1: the UF8 shows one byte per glyph
#include "UadProto.h"
#include "Uf1Text.h"       // uf8ValueLine, dbToVuByte_
#include "Uf8Fader.h"
#include "Uf8Strips.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace uadsurf {

namespace {

using G = uf8::Uf8GlobalLed;
const std::string kUnknown = "\x01";   // a cache value no view has: the next paint sends

void push(Out& out, const uf8::LedColourFrames& f)
{
    out.uf8.push_back(f.ff38);
    out.uf8.push_back(f.ff39);
    if (!f.legacy.empty()) out.uf8.push_back(f.legacy);
}

std::vector<std::string> split(const std::string& p)
{
    std::vector<std::string> out;
    std::string cur;
    for (const char c : p) {
        if (c == '/') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool oneOf(const std::string& s, std::initializer_list<const char*> l)
{
    for (const char* x : l) if (s == x) return true;
    return false;
}

std::string num(double v)
{
    char b[32];
    std::snprintf(b, sizeof(b), "%.6g", v);
    return b;
}

std::string fmtDb(double db)
{
    if (db <= -143.9) return "-inf";
    char b[16];
    std::snprintf(b, sizeof(b), "%.1f", db);
    return b;
}

std::string panText(double p)
{
    const int pct = static_cast<int>(std::lround(std::fabs(p) * 100.0));
    if (pct == 0) return "<C>";
    return std::string(p < 0 ? "L " : "R ") + std::to_string(pct);
}

std::string csType(const std::string& io)
{
    if (io == "Mic") return "MIC";
    if (io == "Line") return "LINE";
    if (io == "ADAT") return "ADAT";
    if (io == "Virtual") return "VIRT";
    if (io == "S/PDIF") return "SPDF";
    if (io == "HiZ") return "HI-Z";
    return io.substr(0, 4);
}

}  // namespace

bool allowedProperty(const std::string& p)
{
    const auto s = split(p);
    const std::size_t n = s.size();
    if (n < 3 || s[0] != "devices") return false;
    const std::string& name = s.back();
    if (n == 3) return oneOf(name, {"AltMonSelection", "TalkbackMaster"});
    if (n == 5 && s[2] == "inputs")  return oneOf(name, {"FaderLevelTapered", "Mute", "Solo", "Pan", "Pan2"});
    if (n == 5 && s[2] == "auxs")    return oneOf(name, {"FaderLevelTapered", "Mute"});
    if (n == 5 && s[2] == "outputs") return oneOf(name, {"CRMonitorLevel", "Mute", "DimOn", "MixToMono", "MixInSource"});
    if (n == 7 && s[2] == "inputs" && s[4] == "sends")   return oneOf(name, {"GainTapered", "Bypass"});
    if (n == 7 && s[2] == "inputs" && s[4] == "preamps") return oneOf(name, {"Gain", "48V", "Pad", "Phase", "LowCut", "HiZ"});
    return false;
}

void Surface::invalidate()
{
    for (auto& c : cache_) {
        c = Cache{};
        c.upper = c.lower = c.csType = c.chanNum = c.faderText = c.faderUnit = c.valueLine =
            c.slotLabel = kUnknown;
    }
    lastBar_.fill(0xFFFF);
    lastMode_.fill(0xFF);
    ringsInit_ = false;
    vuInit_ = false;
    lastGlobal_.fill(-1);
    fresh_ = true;
}

int Surface::stripAt_(int s) const
{
    const int idx = bank_ * 8 + s;
    return (s >= 0 && s < 8 && idx < static_cast<int>(strips_.size())) ? idx : -1;
}

std::uint32_t Surface::stripRgb(int s) const
{
    const int idx = stripAt_(s);
    if (idx < 0) return 0;
    const uad::Strip& st = strips_[static_cast<std::size_t>(idx)];
    if (st.kind == uad::Strip::Kind::Aux) return 0x30C060;           // green
    const uad::Input* in = m_.input(st);
    const std::string io = in ? in->ioType : "";
    if (io == "Mic" || io == "HiZ") return 0xE04040;                   // red
    if (io == "Line") return 0xE09030;                                 // orange
    if (io == "Virtual") return 0x9050E0;                              // violet
    return 0x3080E0;                                                   // blue: ADAT, S/PDIF
}

void Surface::write_(const std::string& prop, const std::string& json, std::int64_t now, Out& out)
{
    if (!allowedProperty(prop)) return;
    // Nothing while the device is gone: the engine keeps its old tree (measured).
    const auto seg = split(prop);
    const int d = std::atoi(seg[1].c_str());
    if (d < 0 || d >= static_cast<int>(m_.devices.size()) || !m_.devices[static_cast<std::size_t>(d)].online) return;
    Pending& p = pending_[prop];
    p.json = json;
    if (now - p.lastSentMs >= kWriteMs) {
        out.cmds.push_back(uad::set(prop, json));
        p.lastSentMs = now;
        p.waiting = false;
    } else {
        p.waiting = true;
    }
}

void Surface::flush_(std::int64_t now, Out& out)
{
    for (auto& [prop, p] : pending_)
        if (p.waiting && now - p.lastSentMs >= kWriteMs) {
            out.cmds.push_back(uad::set(prop, p.json));
            p.lastSentMs = now;
            p.waiting = false;
        }
}

void Surface::toggle_(const std::string& prop, bool current, std::int64_t now, Out& out)
{
    write_(prop, current ? "false" : "true", now, out);
}

bool Surface::fromUf8(const uf8frames::Frame& f, std::int64_t now, Out& out)
{
    if (f.n < 6) return false;
    if (strips_.empty()) strips_ = m_.strips();
    const std::uint8_t cmd = f.b[1];
    if (cmd == 0x20 && f.b[2] == 0x02) {                        // touch
        const int s = f.b[3];
        if (s > 7) return false;
        Touch& t = touch_[static_cast<std::size_t>(s)];
        if (f.b[4] != 0) {
            t.releasePending = false;
            if (!t.down) {
                t.down = true;
                out.uf8.push_back(uf8fader::limp(static_cast<std::uint8_t>(s)));
                cache_[static_cast<std::size_t>(s)].engaged = false;
            }
        } else if (t.down) {
            t.releasePending = true;
            t.releaseMs = now;
        }
        return true;
    }
    if (cmd == 0x21 && f.b[2] == 0x03) {                        // position
        const int s = f.b[3];
        if (s > 7) return false;
        if (!touch_[static_cast<std::size_t>(s)].down) return true;   // the motor's own echo
        const std::uint8_t a = f.b[4];
        const auto pb14 = static_cast<std::uint16_t>((a & 0x7F) | ((f.b[5] & 0x7F) << 7));
        const uf8fader::Read rd = uf8fader::read(pb14, (a & 0x80) != 0, false);
        if (!rd.usable) return true;
        out.uf8.push_back(uf8fader::echo(static_cast<std::uint8_t>(s), rd.pbOut));
        const int idx = stripAt_(s);
        if (idx < 0) return true;
        // The UF8 fader stands where the Console fader stands: FaderLevelTapered is
        // the Console's own fader position, so the map is linear.
        const double t = static_cast<double>(rd.pbOut) / uf8fader::kPbMax;
        handUntil_[static_cast<std::size_t>(s)] = now + kHandHoldMs;
        handTapered_[static_cast<std::size_t>(s)] = t;
        write_(uad::Model::propPath(strips_[static_cast<std::size_t>(idx)], "FaderLevelTapered"), num(t), now, out);
        return true;
    }
    if (cmd == 0x24 && f.b[2] == 0x02) {                        // V-Pot / channel encoder
        const int pot = f.b[3];
        const int d = uf8frames::vpotDelta(f.b[4]);
        if (d == 0) return true;
        if (pot < 8) { pot_(pot, d, now, out); return true; }
        if (pot != 8) return false;
        // The channel encoder is the monitor level: one step per detent (1 dB, FINE
        // 0.5 dB), never more than four per frame, never past the engine's range.
        for (std::size_t di = 0; di < m_.devices.size(); ++di) {
            const uad::Device& D = m_.devices[di];
            const int o = D.monitorOutput();
            if (!D.online || o < 0) continue;
            const uad::Output& mon = D.outputs[static_cast<std::size_t>(o)];
            const std::string prop = uad::Model::devicePath(static_cast<int>(di)) + "/outputs/" +
                                     std::to_string(o) + "/CRMonitorLevel";
            double base = mon.monitorDb;
            auto it = pending_.find(prop);
            if (it != pending_.end() && now - it->second.lastSentMs < kHandHoldMs)
                base = std::strtod(it->second.json.c_str(), nullptr);
            const int steps = std::clamp(d, -4, 4);
            const double db = std::clamp(base + steps * (fine_ ? 0.5 : 1.0), mon.monitorMinDb, mon.monitorMaxDb);
            write_(prop, num(db), now, out);
            break;
        }
        return true;
    }
    if (cmd == 0x22 && f.b[2] == 0x03 && f.n >= 7) {            // key
        const std::uint8_t id = f.b[3];
        if (id == 0x40 || id == 0x41 || id == 0x42 || id == 0x46) return false;   // LAYER 1..3, 360
        key_(id, f.b[5] == 0x01, now, out);
        return true;
    }
    return false;
}

void Surface::key_(std::uint8_t id, bool pressed, std::int64_t now, Out& out)
{
    if (id == 0x6F) { fine_ = pressed; return; }                // FINE
    if (id >= 0x18 && id <= 0x1F) { soft_(id - 0x18, pressed, now, out); return; }
    if (!pressed) return;
    if (id >= 0x20 && id <= 0x37) {                             // SOLO / CUT / SEL
        const int s = (id - 0x20) / 3, type = (id - 0x20) % 3;
        const int idx = stripAt_(s);
        if (idx < 0) return;
        const uad::Strip& st = strips_[static_cast<std::size_t>(idx)];
        if (type == 2) { selected_ = idx; return; }
        if (const uad::Input* in = m_.input(st)) {
            if (type == 0) toggle_(uad::Model::propPath(st, "Solo"), in->solo, now, out);
            else           toggle_(uad::Model::propPath(st, "Mute"), in->mute, now, out);
        } else if (const uad::Aux* a = m_.aux(st)) {
            if (type == 1) toggle_(uad::Model::propPath(st, "Mute"), a->mute, now, out);
        }
        return;
    }
    if (id >= 0x08 && id <= 0x0F) {                             // V-Pot push
        const int idx = stripAt_(id - 0x08);
        if (idx < 0) return;
        const uad::Strip& st = strips_[static_cast<std::size_t>(idx)];
        const uad::Input* in = m_.input(st);
        if (!in) return;
        if (mode_ == PotMode::Pan) write_(uad::Model::propPath(st, "Pan"), "0", now, out);
        else if (mode_ == PotMode::Send && send_ < static_cast<int>(in->sends.size()))
            toggle_(uad::Model::propPath(st, "sends/") + std::to_string(send_) + "/Bypass",
                    in->sends[static_cast<std::size_t>(send_)].bypass, now, out);
        return;
    }
    if (id >= 0x68 && id < 0x68 + kSoftBanks) { softBank_ = id - 0x68; return; }
    if (id >= 0x48 && id <= 0x4F) {                             // SEND/PLUG-IN 1..8 = the send
        int count = 0;
        for (const auto& st : strips_) if (const uad::Input* in = m_.input(st)) { count = static_cast<int>(in->sends.size()); break; }
        if (id - 0x48 < count) { send_ = id - 0x48; mode_ = PotMode::Send; }
        return;
    }
    if (id == 0x78 || id == 0x79) {                             // BANK < >
        const int maxBank = strips_.empty() ? 0 : (static_cast<int>(strips_.size()) - 1) / 8;
        bank_ = std::clamp(bank_ + (id == 0x79 ? 1 : -1), 0, maxBank);
        return;
    }
    if (id == 0x6E) { mode_ = PotMode::Pan; return; }           // PAN
    if (id == 0x76) {                                           // encoder push: monitor mute
        for (std::size_t di = 0; di < m_.devices.size(); ++di) {
            const uad::Device& D = m_.devices[di];
            const int o = D.monitorOutput();
            if (!D.online || o < 0) continue;
            toggle_(uad::Model::devicePath(static_cast<int>(di)) + "/outputs/" + std::to_string(o) + "/Mute",
                    D.outputs[static_cast<std::size_t>(o)].mute, now, out);
            break;
        }
    }
}

void Surface::soft_(int key, bool pressed, std::int64_t now, Out& out)
{
    if (softBank_ == 0) {                                       // the V-Pot modes
        if (!pressed) return;
        if (key == 0) mode_ = PotMode::Pan;
        if (key == 1) mode_ = PotMode::Send;
        if (key == 2) mode_ = PotMode::Preamp;
        return;
    }
    if (softBank_ == 1) {                                       // the monitor
        if (!pressed) return;
        for (std::size_t di = 0; di < m_.devices.size(); ++di) {
            const uad::Device& D = m_.devices[di];
            const int o = D.monitorOutput();
            if (!D.online || o < 0) continue;
            const std::string dev = uad::Model::devicePath(static_cast<int>(di));
            const std::string outp = dev + "/outputs/" + std::to_string(o) + "/";
            const uad::Output& mon = D.outputs[static_cast<std::size_t>(o)];
            if (key == 0 && D.altMonMax >= 1) write_(dev + "/AltMonSelection", D.altMon ? "0" : "1", now, out);
            if (key == 1) toggle_(outp + "DimOn", mon.dimOn, now, out);
            if (key == 2) toggle_(outp + "MixToMono", mon.mixToMono, now, out);
            if (key == 3) toggle_(outp + "Mute", mon.mute, now, out);
            break;
        }
        return;
    }
    // softBank_ == 2: the preamp of the selected channel.
    if (selected_ < 0 || selected_ >= static_cast<int>(strips_.size())) return;
    const uad::Strip& st = strips_[static_cast<std::size_t>(selected_)];
    const uad::Input* in = m_.input(st);
    if (!in || !in->hasPreamp) return;
    const std::string pre = uad::Model::propPath(st, "preamps/0/");
    if (key == 0) {
        // 48V only after a hold (paint fires it): phantom power can damage ribbon mics.
        if (pressed) { phantomKey_ = 0; phantomDownMs_ = now; phantomFired_ = false; }
        else phantomKey_ = -1;
        return;
    }
    if (!pressed) return;
    if (key == 1) toggle_(pre + "Pad", in->preamp.pad, now, out);
    if (key == 2) toggle_(pre + "Phase", in->preamp.phase, now, out);
    if (key == 3) toggle_(pre + "LowCut", in->preamp.lowCut, now, out);
    if (key == 4) toggle_(pre + "HiZ", in->preamp.hiZ, now, out);
}

void Surface::pot_(int s, int d, std::int64_t now, Out& out)
{
    const int idx = stripAt_(s);
    if (idx < 0) return;
    const uad::Strip& st = strips_[static_cast<std::size_t>(idx)];
    const uad::Input* in = m_.input(st);
    if (!in) return;
    // While own writes are in flight the model lags: continue from what was sent.
    auto base = [&](const std::string& prop, double model) {
        auto it = pending_.find(prop);
        if (it != pending_.end() && now - it->second.lastSentMs < kHandHoldMs)
            return std::strtod(it->second.json.c_str(), nullptr);
        return model;
    };
    if (mode_ == PotMode::Pan) {
        const std::string prop = uad::Model::propPath(st, "Pan");
        const double v = std::clamp(base(prop, in->pan) + d * (fine_ ? 0.005 : 0.02), -1.0, 1.0);
        write_(prop, num(v), now, out);
    } else if (mode_ == PotMode::Send) {
        if (send_ >= static_cast<int>(in->sends.size())) return;
        const std::string prop = uad::Model::propPath(st, "sends/") + std::to_string(send_) + "/GainTapered";
        const double v = std::clamp(base(prop, in->sends[static_cast<std::size_t>(send_)].gainTapered)
                                    + d * (fine_ ? 0.0025 : 0.01), 0.0, 1.0);
        write_(prop, num(v), now, out);
    } else if (in->hasPreamp) {
        // Whole dB, as the hardware knob steps (the engine snaps to its own steps).
        const std::string prop = uad::Model::propPath(st, "preamps/0/Gain");
        const double v = std::clamp(std::round(base(prop, in->preamp.gainDb)) + d, 0.0, 65.0);
        write_(prop, num(v), now, out);
    }
}

std::string Surface::potText_(int idx, bool full) const
{
    const uad::Strip& st = strips_[static_cast<std::size_t>(idx)];
    const uad::Input* in = m_.input(st);
    if (!in) return full ? "" : "";
    if (mode_ == PotMode::Pan) return panText(in->pan);
    if (mode_ == PotMode::Send) {
        if (send_ >= static_cast<int>(in->sends.size())) return "";
        const uad::Send& s = in->sends[static_cast<std::size_t>(send_)];
        return s.bypass ? std::string("off") : fmtDb(s.gainDb);
    }
    if (!in->hasPreamp) return "";
    char b[24];
    std::snprintf(b, sizeof(b), full ? "%.0f dB%s" : "%.0f%s", in->preamp.gainDb, in->preamp.phantom ? " 48V" : "");
    return b;
}

void Surface::paint(std::int64_t now, Out& out)
{
    strips_ = m_.strips();
    const int maxBank = strips_.empty() ? 0 : (static_cast<int>(strips_.size()) - 1) / 8;
    bank_ = std::clamp(bank_, 0, maxBank);
    if (selected_ >= static_cast<int>(strips_.size())) selected_ = -1;

    for (int s = 0; s < 8; ++s) {
        Touch& t = touch_[static_cast<std::size_t>(s)];
        if (t.down && t.releasePending && now - t.releaseMs >= kTouchQuietMs) {
            t.down = false;
            t.releasePending = false;
        }
    }
    if (phantomKey_ == 0 && !phantomFired_ && now - phantomDownMs_ >= kPhantomHoldMs) {
        phantomFired_ = true;
        if (selected_ >= 0) {
            const uad::Strip& st = strips_[static_cast<std::size_t>(selected_)];
            if (const uad::Input* in = m_.input(st); in && in->hasPreamp)
                toggle_(uad::Model::propPath(st, "preamps/0/48V"), in->preamp.phantom, now, out);
        }
    }
    flush_(now, out);
    if (fresh_) {
        out.uf8.push_back(uf8::buildSelectedStripBitmask(0));
        fresh_ = false;
    }

    std::array<std::uint16_t, 8> bar{};
    std::array<std::uint8_t, 8>  mode{};
    std::array<std::uint8_t, 16> vu{};
    for (int s = 0; s < 8; ++s) {
        const auto i = static_cast<std::size_t>(s);
        const auto u = static_cast<std::uint8_t>(s);
        Cache& c = cache_[i];
        const int idx = stripAt_(s);
        uf8strips::View v;
        std::string lower;
        mode[i] = 0x03;
        bar[i] = uf8::vpotPosUnipolar(0.0);
        if (idx >= 0) {
            const uad::Strip& st = strips_[static_cast<std::size_t>(idx)];
            const uad::Input* in = m_.input(st);
            const uad::Aux* ax = m_.aux(st);
            const bool online = st.device < static_cast<int>(m_.devices.size())
                             && m_.devices[static_cast<std::size_t>(st.device)].online;
            const double tapered = in ? in->faderTapered : ax ? ax->faderTapered : 0.0;
            const double db = in ? in->faderDb : ax ? ax->faderDb : -144.0;
            v.upper     = utf8ToLatin1(m_.name(st));
            v.csType    = ax ? "AUX" : csType(in ? in->ioType : "");
            v.chanNum   = std::to_string(idx + 1);
            v.faderText = online ? fmtDb(db) : "";
            v.faderUnit = online ? "dB" : "";
            const char* label = mode_ == PotMode::Pan ? "Pan" : mode_ == PotMode::Preamp ? "Preamp" : "";
            std::string sendName;
            if (mode_ == PotMode::Send && in && send_ < static_cast<int>(in->sends.size()))
                sendName = in->sends[static_cast<std::size_t>(send_)].name;
            v.valueLine = !online ? uf8ValueLine("Console", "offline")
                        : in ? uf8ValueLine(mode_ == PotMode::Send ? sendName : label, potText_(idx, true))
                             : std::string();
            lower       = online ? potText_(idx, false) : "offline";
            v.sel       = idx == selected_;
            v.selRgb    = 0xFFFFFF;
            v.cut       = in ? in->mute : ax ? ax->mute : false;
            v.solo      = in ? in->solo : false;
            v.drive     = online;
            const double shown = handUntil_[i] > now ? handTapered_[i] : tapered;
            v.faderPb   = static_cast<std::uint16_t>(std::lround(std::clamp(shown, 0.0, 1.0) * uf8fader::kPbMax));
            if (online && in) {
                if (mode_ == PotMode::Pan) {
                    mode[i] = 0x01;
                    bar[i] = uf8::vpotPosPan(in->pan);
                } else if (mode_ == PotMode::Send && send_ < static_cast<int>(in->sends.size())) {
                    mode[i] = 0x01;
                    bar[i] = uf8::vpotPosUnipolar(in->sends[static_cast<std::size_t>(send_)].gainTapered);
                } else if (mode_ == PotMode::Preamp && in->hasPreamp) {
                    mode[i] = 0x01;
                    bar[i] = uf8::vpotPosUnipolar(std::clamp((in->preamp.gainDb - 10.0) / 55.0, 0.0, 1.0));
                }
            }
            const uad::Meter* mt = in ? in->meter : ax ? ax->meter : nullptr;
            if (online && mt) {
                vu[i * 2]     = mt[0].level <= -77.0 ? 0 : dbToVuByte_(mt[0].clip ? 0.0 : mt[0].level);
                vu[i * 2 + 1] = mt[1].level <= -77.0 ? 0 : dbToVuByte_(mt[1].clip ? 0.0 : mt[1].level);
            }
        } else {
            // An empty strip: dark, its fader down, as Rea-Sixty's own blank strips.
            v.chanNum = " ";
            v.drive   = true;
            v.faderPb = 0;
        }
        // The soft key above the strip: its function in the current soft-key bank.
        static const char* const kModes[3] = {"PAN", "SEND", "INPUTS"};
        static const char* const kMon[4]   = {"ALT", "DIM", "MONO", "MUTE"};
        static const char* const kPre[5]   = {"48V", "PAD", "PHASE", "LOW CUT", "HI-Z"};
        int lit = -1;
        if (softBank_ == 0 && s < 3) {
            v.slotLabel = kModes[s];
            lit = static_cast<int>(mode_) == s ? 2 : 1;
        } else if (softBank_ == 1 && s < 4) {
            v.slotLabel = kMon[s];
            lit = 1;
            for (const auto& D : m_.devices) {
                const int o = D.monitorOutput();
                if (!D.online || o < 0) continue;
                const uad::Output& mon = D.outputs[static_cast<std::size_t>(o)];
                const bool on = s == 0 ? D.altMon != 0 : s == 1 ? mon.dimOn : s == 2 ? mon.mixToMono : mon.mute;
                lit = on ? 2 : 1;
                break;
            }
        } else if (softBank_ == 2 && s < 5 && selected_ >= 0) {
            const uad::Input* in = m_.input(strips_[static_cast<std::size_t>(selected_)]);
            if (in && in->hasPreamp) {
                v.slotLabel = kPre[s];
                const uad::Preamp& p = in->preamp;
                const bool on = s == 0 ? p.phantom : s == 1 ? p.pad : s == 2 ? p.phase : s == 3 ? p.lowCut : p.hiZ;
                lit = on ? 2 : 1;
            }
        }
        v.topKey    = lit < 0 ? 0 : lit;
        v.topKeyRgb = softBank_ == 2 && s == 0 ? 0xFF2020 : 0xFFFFFF;    // 48V in red

        uf8strips::paintStrip(
            uf8strips::Cells{c.upper, c.csType, c.chanNum, c.faderText, c.faderUnit, c.valueLine,
                             c.slotLabel, c.topKey, c.sel, c.cut, c.solo, c.faderPb},
            v, u, touch_[i].down, c.engaged,
            [&](Frame f) { out.uf8.push_back(std::move(f)); },
            [&](uf8::LedColourFrames f) { push(out, f); });
        if (c.lower != lower) {
            c.lower = lower;
            out.uf8.push_back(uf8::buildStripTextLower(u, lower));
        }
    }
    uf8strips::paintRings(lastBar_.data(), lastMode_.data(), ringsInit_, bar, mode,
                          [&](Frame f) { out.uf8.push_back(std::move(f)); });
    ringsInit_ = true;
    if (!vuInit_ || vu != lastVu_) {
        lastVu_ = vu;
        vuInit_ = true;
        for (auto& f : uf8::buildVuMeter(vu)) out.uf8.push_back(f);
    }

    // The global lamps: lit = active, dim = does something here, off = nothing.
    int sendCount = 0;
    for (const auto& st : strips_) if (const uad::Input* in = m_.input(st)) { sendCount = static_cast<int>(in->sends.size()); break; }
    const int maxB = strips_.empty() ? 0 : (static_cast<int>(strips_.size()) - 1) / 8;
    auto lamp = [&](G led, int state) {
        std::int8_t& last = lastGlobal_[static_cast<std::size_t>(led)];
        if (last == state) return;
        last = static_cast<std::int8_t>(state);
        push(out, uf8::buildUf8GlobalLed(led, state == 2 ? uf8::GlobalLedState::Bright
                                            : state == 1 ? uf8::GlobalLedState::Dim
                                                         : uf8::GlobalLedState::Off));
    };
    const G banks[6] = {G::VPotBank, G::Soft1, G::Soft2, G::Soft3, G::Soft4, G::Soft5};
    for (int b = 0; b < 6; ++b) lamp(banks[b], b >= kSoftBanks ? 0 : b == softBank_ ? 2 : 1);
    const G sp[8] = {G::SendPlugin1, G::SendPlugin2, G::SendPlugin3, G::SendPlugin4,
                     G::SendPlugin5, G::SendPlugin6, G::SendPlugin7, G::SendPlugin8};
    for (int k = 0; k < 8; ++k) lamp(sp[k], k >= sendCount ? 0 : (mode_ == PotMode::Send && k == send_) ? 2 : 1);
    lamp(G::BankLeft, bank_ > 0 ? 1 : 0);
    lamp(G::BankRight, bank_ < maxB ? 1 : 0);
    lamp(G::Pan, mode_ == PotMode::Pan ? 2 : 1);
    lamp(G::Fine, fine_ ? 2 : 1);
    for (G g : {G::Quick1, G::Quick2, G::Quick3, G::Channel, G::Plugin, G::PageLeft, G::PageRight,
                G::Flip, G::AutoOff, G::AutoRead, G::AutoWrite, G::AutoTrim, G::AutoLatch, G::AutoTouch,
                G::Norm, G::Rec, G::Auto, G::Nav, G::Nudge, G::Focus,
                G::ZoomUp, G::ZoomLeft, G::ZoomCenter, G::ZoomRight, G::ZoomDown})
        lamp(g, 0);
}

std::vector<std::string> Surface::wantedSubscriptions() const
{
    std::vector<std::string> w;
    for (std::size_t di = 0; di < m_.devices.size(); ++di) {
        const uad::Device& D = m_.devices[di];
        const std::string dev = uad::Model::devicePath(static_cast<int>(di));
        w.push_back(dev + "/DeviceOnline");
        w.push_back(dev + "/AltMonSelection");
        const int o = D.monitorOutput();
        if (o >= 0)
            for (const char* p : {"CRMonitorLevel", "Mute", "DimOn", "MixToMono"})
                w.push_back(dev + "/outputs/" + std::to_string(o) + "/" + p);
        // The channel list follows names and visibility on every input.
        for (std::size_t i = 0; i < D.inputs.size(); ++i)
            for (const char* p : {"Name", "StereoName", "EnabledByUser", "ChannelHidden"})
                w.push_back(dev + "/inputs/" + std::to_string(i) + "/" + p);
    }
    for (int s = 0; s < 8; ++s) {
        const int idx = stripAt_(s);
        if (idx < 0) continue;
        const uad::Strip& st = strips_[static_cast<std::size_t>(idx)];
        for (const char* p : {"FaderLevelTapered", "FaderLevel", "Mute", "meters/0/MeterLevel", "meters/1/MeterLevel",
                              "meters/0/MeterClip", "meters/1/MeterClip"})
            w.push_back(uad::Model::propPath(st, p));
        if (const uad::Input* in = m_.input(st)) {
            for (const char* p : {"Solo", "Pan"}) w.push_back(uad::Model::propPath(st, p));
            if (send_ < static_cast<int>(in->sends.size()))
                for (const char* p : {"GainTapered", "Gain", "Bypass"})
                    w.push_back(uad::Model::propPath(st, "sends/") + std::to_string(send_) + "/" + p);
            if (in->hasPreamp)
                for (const char* p : {"Gain", "48V", "Pad", "Phase", "LowCut", "HiZ"})
                    w.push_back(uad::Model::propPath(st, "preamps/0/") + p);
        }
    }
    return w;
}

}  // namespace uadsurf
