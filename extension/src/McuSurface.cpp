#include "McuSurface.h"

#include "Protocol.h"
#include "Uf1Text.h"      // dbToVuByte_: the UF8 meter scale
#include "Uf8Fader.h"
#include "Uf8Strips.h"

namespace mcusurf {

namespace {

// UF8 key id -> its lamp. LAYER 1..3 and 360 are left out: Rea-Sixty's own
// global-LED pusher keeps lighting them on every layer.
struct KeyLamp {
    std::uint8_t      id;
    uf8::Uf8GlobalLed led;
};
using G = uf8::Uf8GlobalLed;
constexpr KeyLamp kLamps[] = {
    {0x43, G::Quick1}, {0x44, G::Quick2}, {0x45, G::Quick3}, {0x51, G::Channel},
    {0x48, G::SendPlugin1}, {0x49, G::SendPlugin2}, {0x4A, G::SendPlugin3}, {0x4B, G::SendPlugin4},
    {0x4C, G::SendPlugin5}, {0x4D, G::SendPlugin6}, {0x4E, G::SendPlugin7}, {0x4F, G::SendPlugin8},
    {0x50, G::Plugin}, {0x52, G::PageLeft}, {0x53, G::PageRight}, {0x54, G::Flip},
    {0x58, G::AutoOff}, {0x59, G::AutoRead}, {0x5A, G::AutoWrite}, {0x5B, G::AutoTrim},
    {0x5C, G::AutoLatch}, {0x5D, G::AutoTouch},
    {0x68, G::VPotBank}, {0x69, G::Soft1}, {0x6A, G::Soft2}, {0x6B, G::Soft3},
    {0x6C, G::Soft4}, {0x6D, G::Soft5},
    {0x6E, G::Pan}, {0x6F, G::Fine}, {0x70, G::Norm}, {0x71, G::Rec}, {0x72, G::Auto},
    {0x73, G::Nav}, {0x74, G::Nudge}, {0x75, G::Focus},
    {0x78, G::BankLeft}, {0x79, G::BankRight},
    {0x7A, G::ZoomUp}, {0x7B, G::ZoomLeft}, {0x7C, G::ZoomCenter}, {0x7D, G::ZoomRight},
    {0x7E, G::ZoomDown},
};

// A cache value no view ever has, so the next paint sends the cell.
const std::string kUnknown = "\x01";

void push(Out& out, const uf8::LedColourFrames& f)
{
    out.uf8.push_back(f.ff38);
    out.uf8.push_back(f.ff39);
    if (!f.legacy.empty()) out.uf8.push_back(f.legacy);
}

}  // namespace

void Surface::invalidate()
{
    for (auto& c : cache_) {
        c = StripCache{};
        c.upper = c.lower = c.csType = c.chanNum = c.faderText = c.faderUnit =
            c.valueLine = c.slotLabel = kUnknown;
    }
    lastBar_.fill(0xFFFF);
    lastMode_.fill(0xFF);
    ringsInit_ = false;
    vuInit_ = false;
    lastGlobal_.fill(-1);
    lastPbSent_.fill(-1);
    fresh_ = true;
}

std::uint8_t Surface::meterByte(const mcu::Meter& m, std::int64_t nowMs)
{
    const int level = mcu::meterLevel(m, nowMs);
    return level <= 0 ? 0 : dbToVuByte_(mcu::meterDb(level));
}

void Surface::fromHost(const std::uint8_t* p, std::size_t n, std::int64_t nowMs, Out& out)
{
    std::vector<mcu::Bytes> msgs;
    parser_.feed(p, n, msgs);
    for (const auto& m : msgs) {
        // While a fader is under the hand the host's echo must not pull the motor:
        // the shadow keeps it (the painter does not drive a touched strip).
        mcu::apply(shadow_, m, nowMs, id_, out.midi);
    }
}

bool Surface::fromUf8(const uf8frames::Frame& f, std::int64_t nowMs, Out& out)
{
    if (f.n < 6) return false;
    const std::uint8_t cmd = f.b[1];
    if (cmd == 0x20 && f.b[2] == 0x02) {                     // touch: FF 20 02 strip state
        const int s = f.b[3];
        if (s > 7) return false;
        Touch& t = touch_[static_cast<size_t>(s)];
        if (f.b[4] != 0) {
            t.releasePending = false;
            if (!t.down) {
                t.down = true;
                out.midi.push_back(mcu::touchMsg(s, true));
                out.uf8.push_back(uf8fader::limp(static_cast<std::uint8_t>(s)));
                cache_[static_cast<size_t>(s)].engaged = false;
            }
        } else if (t.down) {
            // Let go only after the quiet time (paint): the sensor bounces.
            t.releasePending = true;
            t.releaseMs = nowMs;
        }
        return true;
    }
    if (cmd == 0x21 && f.b[2] == 0x03) {                     // position: FF 21 03 strip A B ck
        const int s = f.b[3];
        if (s > 7) return false;
        if (!touch_[static_cast<size_t>(s)].down) return true;   // motor echo: never a move
        const std::uint8_t a = f.b[4];
        const auto pb14 = static_cast<std::uint16_t>((a & 0x7F) | ((f.b[5] & 0x7F) << 7));
        // No grab on this layer (as the CC layer and ORC): the bit-7 stream and the
        // stops only.
        const uf8fader::Read rd = uf8fader::read(pb14, (a & 0x80) != 0, false);
        if (!rd.usable) return true;
        out.uf8.push_back(uf8fader::echo(static_cast<std::uint8_t>(s), rd.pbOut));
        const mcu::Bytes m = mcu::faderMsg(s, rd.pbOut);
        const int pb = m[1] | (m[2] << 7);
        if (pb != lastPbSent_[static_cast<size_t>(s)]) {
            lastPbSent_[static_cast<size_t>(s)] = pb;
            out.midi.push_back(m);
            // The hand is the value until the host says otherwise; a host that does
            // not echo must not see the motor jump back on release.
            shadow_.fader[static_cast<size_t>(s)] = static_cast<std::uint16_t>(pb);
            shadow_.faderKnown[static_cast<size_t>(s)] = true;
        }
        return true;
    }
    if (cmd == 0x24 && f.b[2] == 0x02) {                     // V-Pot: FF 24 02 pot raw
        const int pot = f.b[3];
        const int d = uf8frames::vpotDelta(f.b[4]);
        if (d == 0) return true;
        if (pot < 8) out.midi.push_back(mcu::vpotMsg(pot, d));
        else if (pot == 8)
            for (auto& m : mcu::encoderTurn(d, local_)) out.midi.push_back(std::move(m));
        else return false;
        return true;
    }
    if (cmd == 0x22 && f.b[2] == 0x03 && f.n >= 7) {         // key: FF 22 03 id 00 state
        const std::uint8_t id = f.b[3];
        if (mcu::keyAction(id, local_).kind == mcu::KeyKind::Rea) return false;
        key_(id, f.b[5] == 0x01, nowMs, out);
        return true;
    }
    return false;
}

void Surface::key_(std::uint8_t id, bool pressed, std::int64_t nowMs, Out& out)
{
    const mcu::KeyAction a = mcu::keyAction(id, local_);
    switch (a.kind) {
        case mcu::KeyKind::Note:
            out.midi.push_back(mcu::noteMsg(a.value, pressed));
            break;
        case mcu::KeyKind::SelMode:
            if (pressed) local_.sel = static_cast<mcu::SelMode>(a.value);
            break;
        case mcu::KeyKind::SoftBank:
            if (pressed) local_.softBank = a.value;
            break;
        case mcu::KeyKind::EncNav:
            if (pressed)
                local_.enc = local_.enc == mcu::EncMode::Nav ? mcu::EncMode::Channel
                                                             : mcu::EncMode::Nav;
            break;
        case mcu::KeyKind::EncPush:
            if (pressed) {
                encDown_ = true;
                encHoldFired_ = false;
                encDownMs_ = nowMs;
            } else {
                // A tap goes back to CHANNEL mode; a hold already did its work.
                if (encDown_ && !encHoldFired_) local_.enc = mcu::EncMode::Channel;
                encDown_ = false;
            }
            break;
        default:
            break;
    }
}

void Surface::paint(std::int64_t nowMs, Out& out)
{
    // Touches let go after the quiet time: the host hears the release, and the
    // painter below may drive the motor again.
    for (int s = 0; s < 8; ++s) {
        Touch& t = touch_[static_cast<size_t>(s)];
        if (t.down && t.releasePending && nowMs - t.releaseMs >= kTouchQuietMs) {
            t.down = false;
            t.releasePending = false;
            out.midi.push_back(mcu::touchMsg(s, false));
            lastPbSent_[static_cast<size_t>(s)] = -1;
        }
    }
    if (encDown_ && !encHoldFired_ && opt_.transportHold && nowMs - encDownMs_ >= kEncHoldMs) {
        local_.transport = !local_.transport;
        encHoldFired_ = true;
    }
    if (fresh_) {
        // No strip is "the selected one" for the firmware's SEL colouring; the SEL
        // lamps come from the host (the CC layer does the same).
        out.uf8.push_back(uf8::buildSelectedStripBitmask(0));
        fresh_ = false;
    }

    std::array<std::uint16_t, 8> bar{};
    std::array<std::uint8_t, 8>  mode{};
    std::array<std::uint8_t, 16> vu{};
    for (int s = 0; s < 8; ++s) {
        const auto st = static_cast<std::uint8_t>(s);
        const auto i = static_cast<size_t>(s);
        StripCache& c = cache_[i];
        const mcu::SoftKey& sk = mcu::softKey(local_.softBank, s);
        const bool rec = local_.sel == mcu::SelMode::Rec;

        uf8strips::View v;
        v.upper     = mcu::trimRight(shadow_.lcd.cell(0, s));
        v.csType    = "";
        v.chanNum   = " ";
        v.faderText = "";
        v.faderUnit = "";
        v.valueLine = "";
        v.slotLabel = sk.label;
        v.topKey    = sk.note == mcu::note::None ? 0
                    : (mcu::ledLit(shadow_.led[sk.note], nowMs) ? 2 : 1);
        v.topKeyRgb = opt_.rgb;
        v.sel       = mcu::ledLit(mcu::selLed(shadow_, s, local_.sel), nowMs);
        v.selRgb    = rec ? 0xFF0000 : opt_.rgb;
        v.cut       = mcu::ledLit(mcu::cutLed(shadow_, s), nowMs);
        v.solo      = mcu::ledLit(mcu::soloLed(shadow_, s), nowMs);
        v.drive     = shadow_.faderKnown[i];
        v.faderPb   = static_cast<std::uint16_t>(mcu::toUf8Fader(shadow_.fader[i]));
        // A SEL colour change (NORM <-> REC) is a new lamp even if on/off is not.
        if (c.rec != rec) { c.rec = rec; c.sel = -1; }

        uf8strips::paintStrip(
            uf8strips::Cells{c.upper, c.csType, c.chanNum, c.faderText, c.faderUnit,
                             c.valueLine, c.slotLabel, c.topKey, c.sel, c.cut, c.solo, c.faderPb},
            v, st, touch_[i].down, c.engaged,
            [&](Frame f) { out.uf8.push_back(std::move(f)); },
            [&](uf8::LedColourFrames f) { push(out, f); });

        // The lower scribble row (the V-Pot value line in MCU), not part of the
        // shared strip cells.
        const std::string lower = mcu::trimRight(shadow_.lcd.cell(1, s));
        if (c.lower != lower) {
            c.lower = lower;
            out.uf8.push_back(uf8::buildStripTextLower(st, lower));
        }

        const mcu::RingView r = mcu::ringView(shadow_.ring[i]);
        mode[i] = r.uf8Mode;
        bar[i]  = r.uf8Mode == 0x02 ? uf8::vpotPosBipolar(r.value)
                                    : uf8::vpotPosUnipolar(r.uf8Mode == 0x01 ? r.value : 0.0);
        // MCU has one meter per strip; the UF8 shows two columns. Both get it.
        vu[i * 2] = vu[i * 2 + 1] = meterByte(shadow_.meter[i], nowMs);
    }
    uf8strips::paintRings(lastBar_.data(), lastMode_.data(), ringsInit_, bar, mode,
                          [&](Frame f) { out.uf8.push_back(std::move(f)); });
    ringsInit_ = true;
    if (!vuInit_ || vu != lastVu_) {
        lastVu_ = vu;
        vuInit_ = true;
        for (auto& f : uf8::buildVuMeter(vu)) out.uf8.push_back(f);
    }

    // The global lamps: lit = the host lights the note the key sends, dim = the key
    // does something here, off = it does nothing on this layer.
    for (const KeyLamp& k : kLamps) {
        const mcu::KeyAction a = mcu::keyAction(k.id, local_);
        const std::int8_t state =
            a.kind == mcu::KeyKind::None ? 0
            : mcu::ledLit(mcu::keyLed(shadow_, k.id, local_), nowMs) ? 2 : 1;
        std::int8_t& last = lastGlobal_[static_cast<size_t>(k.led)];
        if (last == state) continue;
        last = state;
        const uf8::GlobalLedState gs = state == 2 ? uf8::GlobalLedState::Bright
                                     : state == 1 ? uf8::GlobalLedState::Dim
                                                  : uf8::GlobalLedState::Off;
        push(out, uf8::buildUf8GlobalLed(k.led, gs));
    }
}

}  // namespace mcusurf
