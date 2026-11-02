#pragma once
//
// McuLayer: the UF8 as a Mackie Control Unit for an external app on a layer of
// kind MCU (plan docs/mcu-layer-plan.md, Baustein B). Pure logic: no REAPER, no
// CoreMIDI, no device. main.cpp owns the ports, the threads and the painter;
// tests/test_mcu_layer.cpp is a table of what is below.
//
// The first client is UA Midi Control (Fonoflow) in its Mackie mode, driving a UA
// Apollo through UAD Console. With SSL 360 the user ran it on UF8 Layer 3 /
// "SSL V-MIDI Port 9" with SSL's Bitwig MCU profile; Rea-Sixty owns the UF8 now,
// so it has to be the MCU itself, on its own virtual ports.
//
// What MCU puts on the wire (Logic Control manual, appendix B; cross-checked with
// Ardour's libs/surfaces/mackie):
//
//   Fader          En ll mm       n = strip 0..7, 8 = master; 14 bit, 0..16383
//   Key / LED      90 nn vv       key: 7F press, 00 release; LED: 7F on, 01 blink, 00 off
//   V-Pot turn     B0 1n dd       dd relative: bit 6 = counter-clockwise, bits 0..5 = ticks
//   V-Pot ring     B0 3n rr       rr = 0 c mm pppp: c centre, mm mode, pppp position 0..11
//   Jog            B0 3C dd       relative as the V-Pots
//   Assignment     B0 4A/4B cc    two 7-segment characters (the UF8 has none)
//   Meter          D0 sv          s = strip, v = 0..C level, E overload on, F off
//   SysEx          F0 00 00 66 id cmd … F7, id 14 MCU, 15 XT, 10/11 Logic Control
//
// Everything that is our choice and not the protocol's (which UF8 key sends which
// note, how a ring looks on the UF8's readout bar, the meter scale) says so where
// it is decided, and the plan lists it under "vermutet" until it is measured.
//
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace mcu {

constexpr int kStrips = 8;
// The UF8 fader's top position. Mirrors kUf8FaderPbMax in main.cpp (15583, the
// highest value the firmware reports); main.cpp static_asserts the two are equal.
constexpr int kUf8PbMax = 15583;
constexpr int kPbMax    = 16383;
constexpr int kLcdCols  = 56;            // per row; two rows
constexpr int kLcdCell  = 7;             // characters per strip
// Rea-Sixty's own blink rate (main.cpp, the surface-wait animation): an LED the
// host asks to blink (velocity 01) blinks with the rest of the surface.
constexpr int kBlinkHalfMs = 350;
// The device decays its meters itself; the host only sends new peaks. The LC
// manual gives "approximately 300 ms per meter division (1.8 seconds to fall
// from 100% to 0%)". Its LCD meter has six divisions; in MCU levels (0..12) the
// same 1.8 s are 150 ms per level.
constexpr int kMeterLevels = 12;
constexpr int kMeterLevelMs = 150;
// A SysEx longer than this is not MCU (the longest, a full LCD write, is 120
// bytes). Dropped, so a runaway stream cannot grow the buffer.
constexpr size_t kMaxSysex = 512;

using Bytes = std::vector<uint8_t>;

// ---- MCU note numbers (Logic Control manual, appendix B) ---------------------
namespace note {
constexpr uint8_t Rec = 0x00, Solo = 0x08, Mute = 0x10, Select = 0x18, VSelect = 0x20;
constexpr uint8_t Track = 0x28, Send = 0x29, Pan = 0x2A, Plugin = 0x2B, Eq = 0x2C,
                  Instrument = 0x2D;
constexpr uint8_t BankLeft = 0x2E, BankRight = 0x2F, ChannelLeft = 0x30, ChannelRight = 0x31;
constexpr uint8_t Flip = 0x32, Global = 0x33, NameValue = 0x34, SmpteBeats = 0x35;
constexpr uint8_t F1 = 0x36;             // F1..F8 = 0x36..0x3D
constexpr uint8_t GvMidi = 0x3E, GvInputs = 0x3F, GvAudio = 0x40, GvInstr = 0x41,
                  GvAux = 0x42, GvBusses = 0x43, GvOutputs = 0x44, GvUser = 0x45;
constexpr uint8_t Shift = 0x46, Option = 0x47, Control = 0x48, Alt = 0x49;
constexpr uint8_t Read = 0x4A, Write = 0x4B, Trim = 0x4C, Touch = 0x4D, Latch = 0x4E,
                  Group = 0x4F;
constexpr uint8_t Save = 0x50, Undo = 0x51, Cancel = 0x52, Enter = 0x53;
constexpr uint8_t Marker = 0x54, Nudge = 0x55, Cycle = 0x56, Drop = 0x57, Replace = 0x58,
                  Click = 0x59, SoloKey = 0x5A;
constexpr uint8_t Rewind = 0x5B, Forward = 0x5C, Stop = 0x5D, Play = 0x5E, Record = 0x5F;
constexpr uint8_t Up = 0x60, Down = 0x61, Left = 0x62, Right = 0x63, Zoom = 0x64, Scrub = 0x65;
constexpr uint8_t UserA = 0x66, UserB = 0x67, FaderTouch = 0x68, MasterTouch = 0x70;
constexpr uint8_t None = 0xFF;           // ours: "this key sends nothing"
}  // namespace note

// ---- UF8 key ids (docs/buttons-leds-quickref.md) ------------------------------
namespace uf8key {
constexpr uint8_t Foot1 = 0x00, Foot2 = 0x01;
constexpr uint8_t VPotPush = 0x08;       // + strip
constexpr uint8_t TopSoft = 0x18;        // + strip, the key above each scribble
constexpr uint8_t StripFirst = 0x20, StripLast = 0x37;  // SOLO/CUT/SEL = 0x20 + 3·strip + 0/1/2
constexpr uint8_t Layer1 = 0x40, Layer2 = 0x41, Layer3 = 0x42;
constexpr uint8_t Quick1 = 0x43, Quick2 = 0x44, Quick3 = 0x45, Btn360 = 0x46;
constexpr uint8_t SendPlugin1 = 0x48;    // ..0x4F
constexpr uint8_t Plugin = 0x50, Channel = 0x51, PageLeft = 0x52, PageRight = 0x53, Flip = 0x54;
constexpr uint8_t AutoOff = 0x58, AutoRead = 0x59, AutoWrite = 0x5A, AutoTrim = 0x5B,
                  AutoLatch = 0x5C, AutoTouch = 0x5D;
constexpr uint8_t VPotBank = 0x68;       // Soft bank 1..5 = 0x69..0x6D
constexpr uint8_t Pan = 0x6E, Fine = 0x6F, Norm = 0x70, Rec = 0x71, Auto = 0x72;
constexpr uint8_t Nav = 0x73, NudgeKey = 0x74, Focus = 0x75, EncPush = 0x76;
constexpr uint8_t BankLeft = 0x78, BankRight = 0x79;
constexpr uint8_t ZoomUp = 0x7A, ZoomLeft = 0x7B, ZoomCentre = 0x7C, ZoomRight = 0x7D,
                  ZoomDown = 0x7E;
}  // namespace uf8key

// ---- UF8 -> MCU: faders, V-Pots, the channel encoder ---------------------------

// The UF8 reports 0..15583, MCU means 0..16383. Linear both ways: the host owns the
// fader law, the surface only says where the cap is.
inline uint16_t toPitchbend(int uf8Pb)
{
    uf8Pb = std::clamp(uf8Pb, 0, kUf8PbMax);
    return static_cast<uint16_t>((static_cast<int64_t>(uf8Pb) * kPbMax + kUf8PbMax / 2) / kUf8PbMax);
}

inline int toUf8Fader(int pb)
{
    pb = std::clamp(pb, 0, kPbMax);
    return static_cast<int>((static_cast<int64_t>(pb) * kUf8PbMax + kPbMax / 2) / kPbMax);
}

inline Bytes faderMsg(int strip, int uf8Pb)
{
    const uint16_t pb = toPitchbend(uf8Pb);
    return {static_cast<uint8_t>(0xE0 | (strip & 0x0F)), static_cast<uint8_t>(pb & 0x7F),
            static_cast<uint8_t>((pb >> 7) & 0x7F)};
}

inline Bytes noteMsg(uint8_t n, bool on)
{
    return {0x90, static_cast<uint8_t>(n & 0x7F), static_cast<uint8_t>(on ? 0x7F : 0x00)};
}

inline Bytes touchMsg(int strip, bool touched)
{
    return noteMsg(static_cast<uint8_t>(note::FaderTouch + (strip & 0x07)), touched);
}

// Relative encoding of the V-Pots and the jog wheel: bit 6 set = counter-clockwise,
// bits 0..5 = how many ticks. 0 is not a move.
inline uint8_t relByte(int delta)
{
    if (delta == 0) return 0;
    const int m = std::min(std::abs(delta), 63);
    return static_cast<uint8_t>(delta > 0 ? m : (0x40 | m));
}

inline int relValue(uint8_t v)
{
    const int m = v & 0x3F;
    return (v & 0x40) ? -m : m;
}

// One V-Pot frame from the UF8 carries -32..+31 detents; MCU hosts accelerate on
// their own side, so the detents go out as they come.
inline Bytes vpotMsg(int strip, int detents)
{
    return {0xB0, static_cast<uint8_t>(0x10 + (strip & 0x07)), relByte(detents)};
}

// ---- local state of the layer (what MCU does not know about) ------------------

// SEL is select (NORM) or record arm (REC), as on every SSL profile. Kept by the
// layer itself, so switching it does not touch REAPER's selection mode.
enum class SelMode : uint8_t { Norm, Rec };
// The channel encoder: CHANNEL < > per detent (the default, as SSL's profiles), or
// the jog wheel (NAV).
enum class EncMode : uint8_t { Channel, Nav };

struct Local {
    SelMode sel       = SelMode::Norm;
    int     softBank  = 0;           // 0 = V-POT, 1..5 = soft-key banks 1..5
    bool    transport = false;       // zoom pad = transport (CHANNEL encoder held)
    EncMode enc       = EncMode::Channel;
};

constexpr int kSoftBanks = 6;

// The encoder turn as MCU messages. CHANNEL mode is a key press + release per
// detent (MCU has no relative message for it); a fast spin is capped so one frame
// cannot flood the host.
inline std::vector<Bytes> encoderTurn(int detents, const Local& L)
{
    std::vector<Bytes> out;
    if (detents == 0) return out;
    if (L.enc == EncMode::Nav) {
        out.push_back({0xB0, 0x3C, relByte(detents)});
        return out;
    }
    const uint8_t n = detents > 0 ? note::ChannelRight : note::ChannelLeft;
    const int count = std::min(std::abs(detents), 8);
    for (int i = 0; i < count; ++i) {
        out.push_back(noteMsg(n, true));
        out.push_back(noteMsg(n, false));
    }
    return out;
}

// ---- UF8 keys -> what they do on the MCU layer ---------------------------------

// The soft keys above the scribbles: a note and the label the top zone shows. The
// defaults follow the user's own Layer 3 in SSL 360 (plan 2.5, V-POT bank and bank
// 1); banks 2..5 fill in the rest of the MCU key set, as SSL's Logic profile does.
struct SoftKey {
    uint8_t     note  = note::None;
    const char* label = "";
};

inline const SoftKey& softKey(int bank, int key)
{
    static const SoftKey kNone{};
    static const SoftKey k[kSoftBanks][kStrips] = {
        // V-POT: the user's layout. INPUTS is MCU EQ (UA Midi Control: preamp +
        // Unison), GAIN is INSTRUMENT (UA Midi Control: gains).
        {{note::Pan, "PAN"}, {note::Send, "SEND"}, {note::Plugin, "PLUGIN"},
         {note::Eq, "INPUTS"}, {note::Instrument, "GAIN"}, {}, {}, {}},
        // 1: the user's "ALT" key. UA Midi Control's one custom mapping (Alt monitor)
        // listens for note 70 = 0x46 from the old port; this key sends exactly that.
        {{note::Shift, "ALT"}, {}, {}, {}, {}, {}, {}, {}},
        // 2: transport and friends (UA Midi Control repurposes transport for monitor
        // functions, so the labels stay MCU's own words).
        {{note::Rewind, "REW"}, {note::Forward, "FF"}, {note::Stop, "STOP"},
         {note::Play, "PLAY"}, {note::Record, "REC"}, {note::Cycle, "CYCLE"},
         {note::Marker, "MARKER"}, {note::Nudge, "NUDGE"}},
        // 3: Global View (UA Midi Control: Inputs, Aux, Outputs ... as channel views).
        {{note::GvMidi, "MIDI"}, {note::GvInputs, "INPUTS"}, {note::GvAudio, "AUDIO"},
         {note::GvInstr, "INSTR"}, {note::GvAux, "AUX"}, {note::GvBusses, "BUSSES"},
         {note::GvOutputs, "OUTPUTS"}, {note::GvUser, "USER"}},
        // 4: F1..F8 (UA Midi Control: F1 = DAW mode).
        {{0x36, "F1"}, {0x37, "F2"}, {0x38, "F3"}, {0x39, "F4"},
         {0x3A, "F5"}, {0x3B, "F6"}, {0x3C, "F7"}, {0x3D, "F8"}},
        // 5: modifiers and utilities. 0x49 is labelled CMD, not ALT: "ALT" is the
        // user's monitor key in bank 1.
        {{note::Shift, "SHIFT"}, {note::Option, "OPTION"}, {note::Control, "CTRL"},
         {note::Alt, "CMD"}, {note::Save, "SAVE"}, {note::Undo, "UNDO"},
         {note::Cancel, "CANCEL"}, {note::Enter, "ENTER"}},
    };
    if (bank < 0 || bank >= kSoftBanks || key < 0 || key >= kStrips) return kNone;
    return k[bank][key];
}

enum class KeyKind : uint8_t {
    None,      // no function on the MCU layer (lamp dark)
    Note,      // sends MCU note `value` (press 7F, release 00)
    Rea,       // stays Rea-Sixty's: LAYER 1..3, 360
    SelMode,   // NORM / REC: value = SelMode
    SoftBank,  // V-POT, 1..5: value = bank
    EncNav,    // NAV: the encoder jogs
    EncPush,   // channel encoder push: tap = CHANNEL mode, hold = transport mode
};

struct KeyAction {
    KeyKind kind  = KeyKind::None;
    uint8_t value = 0;
};

// Strip keys. SEL sends SELECT in NORM and REC (arm) in REC, as SSL's profiles do.
inline KeyAction stripKey(uint8_t id, SelMode sel)
{
    if (id < uf8key::StripFirst || id > uf8key::StripLast) return {};
    const int strip = (id - uf8key::StripFirst) / 3;
    switch ((id - uf8key::StripFirst) % 3) {
        case 0:  return {KeyKind::Note, static_cast<uint8_t>(note::Solo + strip)};
        case 1:  return {KeyKind::Note, static_cast<uint8_t>(note::Mute + strip)};
        default: break;
    }
    return {KeyKind::Note, static_cast<uint8_t>((sel == SelMode::Rec ? note::Rec : note::Select) + strip)};
}

// Every UF8 key. The global part follows SSL's Bitwig profile where it has an MCU
// equivalent (FINE/SHIFT = SHIFT, PAGE < > = CHANNEL < >, automation, the zoom pad
// and its transport mode, Hb p. 124-133) and the user's Layer 3 where SSL 360 had
// it customised (QUICK 1 = GLOBAL, QUICK 2 "AUX"). Keys without an MCU equivalent
// on that layer have no function: SEND/PLUG-IN 1..8 and the automation keys were in
// user mode with nothing assigned, CHANNEL and OFF have no MCU note.
inline KeyAction keyAction(uint8_t id, const Local& L)
{
    using namespace uf8key;
    if (id >= StripFirst && id <= StripLast) return stripKey(id, L.sel);
    if (id >= VPotPush && id < VPotPush + kStrips)
        return {KeyKind::Note, static_cast<uint8_t>(note::VSelect + (id - VPotPush))};
    if (id >= TopSoft && id < TopSoft + kStrips) {
        const SoftKey& k = softKey(L.softBank, id - TopSoft);
        return k.note == note::None ? KeyAction{} : KeyAction{KeyKind::Note, k.note};
    }
    if (id >= VPotBank && id <= VPotBank + 5)
        return {KeyKind::SoftBank, static_cast<uint8_t>(id - VPotBank)};
    if (L.transport) {
        switch (id) {
            case ZoomUp:     return {KeyKind::Note, note::Play};
            case ZoomDown:   return {KeyKind::Note, note::Stop};
            case ZoomLeft:   return {KeyKind::Note, note::Rewind};
            case ZoomRight:  return {KeyKind::Note, note::Forward};
            case ZoomCentre: return {KeyKind::Note, note::Record};
            default: break;
        }
    }
    switch (id) {
        case Layer1: case Layer2: case Layer3: case Btn360:
            return {KeyKind::Rea, 0};
        case Norm:      return {KeyKind::SelMode, static_cast<uint8_t>(SelMode::Norm)};
        case Rec:       return {KeyKind::SelMode, static_cast<uint8_t>(SelMode::Rec)};
        case Nav:       return {KeyKind::EncNav, 0};
        case EncPush:   return {KeyKind::EncPush, 0};
        case BankLeft:  return {KeyKind::Note, note::BankLeft};
        case BankRight: return {KeyKind::Note, note::BankRight};
        case PageLeft:  return {KeyKind::Note, note::ChannelLeft};
        case PageRight: return {KeyKind::Note, note::ChannelRight};
        case uf8key::Flip:   return {KeyKind::Note, note::Flip};
        case uf8key::Pan:    return {KeyKind::Note, note::Pan};
        case uf8key::Plugin: return {KeyKind::Note, note::Plugin};
        case Fine:      return {KeyKind::Note, note::Shift};
        case AutoRead:  return {KeyKind::Note, note::Read};
        case AutoWrite: return {KeyKind::Note, note::Write};
        case AutoTrim:  return {KeyKind::Note, note::Trim};
        case AutoLatch: return {KeyKind::Note, note::Latch};
        case AutoTouch: return {KeyKind::Note, note::Touch};
        case ZoomUp:    return {KeyKind::Note, note::Up};
        case ZoomDown:  return {KeyKind::Note, note::Down};
        case ZoomLeft:  return {KeyKind::Note, note::Left};
        case ZoomRight: return {KeyKind::Note, note::Right};
        case ZoomCentre: return {KeyKind::Note, note::Zoom};
        case Quick1:    return {KeyKind::Note, note::Global};
        case Quick2:    return {KeyKind::Note, note::GvAux};
        case Foot1:     return {KeyKind::Note, note::UserA};
        case Foot2:     return {KeyKind::Note, note::UserB};
        default: break;
    }
    return {};
}

// ---- MCU -> UF8: the incoming byte stream ---------------------------------------

// CoreMIDI hands over packets, and a SysEx may span several of them (SSL 360's log
// on the user's Mac holds thousands of "ran out of data reading sys. ex." errors:
// split or cut SysEx do arrive). The parser turns any split of the stream into
// whole messages: running status kept, real-time bytes skipped even inside a
// SysEx, a SysEx cut by a new status byte or grown past kMaxSysex dropped.
class Parser {
public:
    void feed(const uint8_t* p, size_t n, std::vector<Bytes>& out)
    {
        for (size_t i = 0; i < n; ++i) byte_(p[i], out);
    }
    void reset() { cur_.clear(); status_ = 0; sysex_ = false; }

private:
    static int dataBytes_(uint8_t status)
    {
        const uint8_t t = status & 0xF0;
        return (t == 0xC0 || t == 0xD0) ? 1 : 2;
    }

    void byte_(uint8_t b, std::vector<Bytes>& out)
    {
        if (b >= 0xF8) return;                       // real-time: timing, active sensing
        if (b == 0xF0) {
            cur_.assign(1, b);
            sysex_ = true;
            status_ = 0;
            return;
        }
        if (b == 0xF7) {
            if (sysex_) {
                cur_.push_back(b);
                out.push_back(cur_);
            }
            cur_.clear();
            sysex_ = false;
            return;
        }
        if (b & 0x80) {                              // any other status
            sysex_ = false;                          // a cut SysEx is dropped
            cur_.clear();
            if (b >= 0xF1) { status_ = 0; return; }  // system common: not MCU, no running status
            status_ = b;
            cur_.push_back(b);
            return;
        }
        if (sysex_) {
            if (cur_.size() >= kMaxSysex) { sysex_ = false; cur_.clear(); return; }
            cur_.push_back(b);
            return;
        }
        if (!status_) return;                        // data without a status
        if (cur_.empty()) cur_.push_back(status_);   // running status
        cur_.push_back(b);
        if (static_cast<int>(cur_.size()) == 1 + dataBytes_(status_)) {
            out.push_back(cur_);
            cur_.clear();
        }
    }

    Bytes   cur_;
    uint8_t status_ = 0;
    bool    sysex_  = false;
};

// ---- the shadow: everything the host has said, layer shown or not --------------

// The LCD as MCU sees it: 2 rows x 56 characters, 7 per strip, written in pieces
// from any offset (row 1 at 0x00..0x37, row 2 at 0x38..0x6F; a write runs on from
// the end of row 1 into row 2).
struct Lcd {
    std::array<char, 2 * kLcdCols> c;
    Lcd() { clear(); }
    void clear() { c.fill(' '); }

    void write(int offset, const uint8_t* s, size_t n)
    {
        for (size_t i = 0; i < n; ++i) {
            const size_t pos = static_cast<size_t>(offset) + i;
            if (offset < 0 || pos >= c.size()) return;
            const uint8_t ch = s[i];
            c[pos] = (ch >= 0x20 && ch < 0x7F) ? static_cast<char>(ch) : ' ';
        }
    }

    std::string cell(int row, int strip) const
    {
        if (row < 0 || row > 1 || strip < 0 || strip >= kStrips) return {};
        return std::string(c.data() + row * kLcdCols + strip * kLcdCell, kLcdCell);
    }
};

// Hosts pad cells with spaces; the UF8's scribble shows the text unpadded, as SSL
// 360 sends it (main.cpp, onMidiFromReaper).
inline std::string trimRight(std::string s)
{
    const size_t e = s.find_last_not_of(' ');
    if (e == std::string::npos) return {};
    s.resize(e + 1);
    return s;
}

enum class Led : uint8_t { Off, On, Blink };

inline bool ledLit(Led l, int64_t nowMs)
{
    if (l == Led::On) return true;
    if (l == Led::Blink) return ((nowMs / kBlinkHalfMs) & 1) == 0;
    return false;
}

struct Meter {
    int     level = 0;               // 0..12, as last sent
    int64_t setMs = 0;
    bool    over  = false;
};

inline void meterSet(Meter& m, uint8_t v, int64_t nowMs)
{
    v &= 0x0F;
    if (v <= kMeterLevels) { m.level = v; m.setMs = nowMs; }
    else if (v == 0x0E) m.over = true;
    else if (v == 0x0F) m.over = false;
}

// The level now, decayed since it was sent.
inline int meterLevel(const Meter& m, int64_t nowMs)
{
    const int64_t gone = nowMs > m.setMs ? (nowMs - m.setMs) / kMeterLevelMs : 0;
    return static_cast<int>(std::max<int64_t>(0, m.level - gone));
}

// The level as dB, for the UF8's own meter scale (main.cpp, dbToVuByte_). The
// steps are the Logic Control's meter LED labels; that a host sends level n for
// "at least that many dB" is the usual reading, not measured.
inline double meterDb(int level)
{
    static const double k[kMeterLevels + 1] = {
        -120.0, -60.0, -50.0, -40.0, -30.0, -20.0, -14.0, -10.0, -8.0, -6.0, -4.0, -2.0, 0.0 };
    return k[std::clamp(level, 0, kMeterLevels)];
}

// The V-Pot ring on the UF8's readout bar. The bar knows unipolar (one line, as
// Rea-Sixty draws pan), bipolar (a fill from the centre) and off (Protocol.h,
// buildVPotMode). MCU's dot and boost/cut map straight; wrap (a fill from the left)
// and spread (a fill out from the centre) have no exact twin and take the nearest.
struct RingView {
    uint8_t uf8Mode = 0x03;          // 0x01 unipolar, 0x02 bipolar, 0x03 off
    double  value   = 0.0;           // unipolar 0..1, bipolar -1..1
};

inline RingView ringView(uint8_t cc)
{
    const int pos  = cc & 0x0F;
    const int mode = (cc >> 4) & 0x03;
    if (pos < 1 || pos > 11) return {};
    switch (mode) {
        case 1:  return {0x02, (pos - 6) / 5.0};                         // boost/cut
        case 3:  return {0x01, std::min(1.0, (pos - 1) / 5.0)};          // spread
        default: return {0x01, (pos - 1) / 10.0};                        // dot, wrap
    }
}

// Who we say we are. Serial and challenge are 7-bit; any host reply is accepted.
struct Identity {
    std::array<uint8_t, 7> serial    = {'R', 'S', '6', '0', 'M', 'C', 'U'};
    std::array<uint8_t, 4> challenge = {0x52, 0x53, 0x36, 0x30};
    std::array<uint8_t, 5> version   = {'V', '1', '.', '0', '0'};
};

inline Bytes sysex(uint8_t model, uint8_t cmd, const uint8_t* data = nullptr, size_t n = 0)
{
    Bytes b = {0xF0, 0x00, 0x00, 0x66, model, cmd};
    if (data) b.insert(b.end(), data, data + n);
    b.push_back(0xF7);
    return b;
}

inline bool isMackieModel(uint8_t m) { return m == 0x14 || m == 0x15 || m == 0x10 || m == 0x11; }

struct Shadow {
    std::array<uint16_t, kStrips + 1> fader{};        // [8] = master
    std::array<bool, kStrips + 1>     faderKnown{};
    std::array<Led, 128>              led{};
    Lcd                               lcd;
    std::array<uint8_t, kStrips>      ring{};
    std::array<Meter, kStrips>        meter{};
    std::array<Meter, 2>              master{};       // D1, left / right
    std::array<char, 2>               assign{{' ', ' '}};
    uint32_t gen      = 0;           // bumped by every change; the painter compares
    bool     online   = false;       // a host has spoken since start or reset
    int64_t  lastRxMs = 0;

    void reset() { *this = Shadow{}; }
};

// The assignment display's 7-segment code: 0x00..0x1F = '@'..'_', 0x20..0x3F as
// ASCII, bit 6 = the dot (dropped, the UF8 cell shows the character).
inline char assignChar(uint8_t v)
{
    const uint8_t c = v & 0x3F;
    return static_cast<char>(c < 0x20 ? c + 0x40 : c);
}

// One whole message from the host into the shadow. Handshake and version answers,
// if any, land in `reply` (the caller sends them back at once).
inline void apply(Shadow& S, const Bytes& m, int64_t nowMs, const Identity& id,
                  std::vector<Bytes>& reply)
{
    if (m.empty()) return;
    S.lastRxMs = nowMs;
    const uint8_t st = m[0];
    if (st == 0xF0) {
        if (m.size() < 7 || m[1] != 0x00 || m[2] != 0x00 || m[3] != 0x66 || !isMackieModel(m[4]))
            return;
        const uint8_t model = m[4], cmd = m[5];
        const size_t bodyEnd = m.back() == 0xF7 ? m.size() - 1 : m.size();
        switch (cmd) {
            case 0x00: {                 // device query: host connection query
                Bytes d(id.serial.begin(), id.serial.end());
                d.insert(d.end(), id.challenge.begin(), id.challenge.end());
                reply.push_back(sysex(model, 0x01, d.data(), d.size()));
                break;
            }
            case 0x02:                   // host connection reply: confirm, any answer
                reply.push_back(sysex(model, 0x03, id.serial.data(), id.serial.size()));
                break;
            case 0x13:                   // version request
                reply.push_back(sysex(model, 0x14, id.version.data(), id.version.size()));
                break;
            case 0x12:                   // LCD
                if (m.size() >= 8)
                    S.lcd.write(m[6], m.data() + 7, bodyEnd > 7 ? bodyEnd - 7 : 0);
                break;
            case 0x0F:                   // go offline
                S.online = false;
                ++S.gen;
                return;
            case 0x61:                   // all faders to minimum
                S.fader.fill(0);
                S.faderKnown.fill(true);
                break;
            case 0x62:                   // all LEDs off
                S.led.fill(Led::Off);
                break;
            case 0x63:                   // reset
                S.reset();
                S.lastRxMs = nowMs;
                ++S.gen;
                return;
            default:
                return;                  // config and meter-mode SysEx: nothing to show
        }
        S.online = true;
        ++S.gen;
        return;
    }
    const uint8_t type = st & 0xF0, ch = st & 0x0F;
    switch (type) {
        case 0x90:
            if (m.size() < 3) return;
            S.led[m[1] & 0x7F] = m[2] == 0 ? Led::Off : m[2] == 1 ? Led::Blink : Led::On;
            break;
        case 0x80:
            if (m.size() < 2) return;
            S.led[m[1] & 0x7F] = Led::Off;
            break;
        case 0xE0:
            if (m.size() < 3 || ch > kStrips) return;
            S.fader[ch] = static_cast<uint16_t>((m[1] & 0x7F) | ((m[2] & 0x7F) << 7));
            S.faderKnown[ch] = true;
            break;
        case 0xB0:
            if (m.size() < 3) return;
            if (m[1] >= 0x30 && m[1] < 0x30 + kStrips) S.ring[m[1] - 0x30] = m[2];
            else if (m[1] == 0x4A) S.assign[1] = assignChar(m[2]);    // right digit
            else if (m[1] == 0x4B) S.assign[0] = assignChar(m[2]);    // left digit
            else return;                                              // timecode and the rest
            break;
        case 0xD0: {
            if (m.size() < 2) return;
            const size_t s = static_cast<size_t>((m[1] >> 4) & 0x07);
            if (ch == 0) meterSet(S.meter[s], m[1], nowMs);
            else if (ch == 1) meterSet(S.master[s & 1], m[1], nowMs);  // master, never strip 1
            else return;
            break;
        }
        default:
            return;
    }
    S.online = true;
    ++S.gen;
}

// What a strip's keys show. SEL shows select in NORM and arm in REC (the UF8 draws
// it red then, as SSL does).
inline Led soloLed(const Shadow& S, int strip) { return S.led[note::Solo + (strip & 7)]; }
inline Led cutLed(const Shadow& S, int strip)  { return S.led[note::Mute + (strip & 7)]; }
inline Led selLed(const Shadow& S, int strip, SelMode sel)
{
    return S.led[(sel == SelMode::Rec ? note::Rec : note::Select) + (strip & 7)];
}

// The lamp of a UF8 key that sends a note: that note's LED. Local keys light from
// the layer's own state; keys without a function stay dark.
inline Led keyLed(const Shadow& S, uint8_t id, const Local& L)
{
    const KeyAction a = keyAction(id, L);
    switch (a.kind) {
        case KeyKind::Note:     return S.led[a.value & 0x7F];
        case KeyKind::SelMode:  return static_cast<SelMode>(a.value) == L.sel ? Led::On : Led::Off;
        case KeyKind::SoftBank: return a.value == L.softBank ? Led::On : Led::Off;
        case KeyKind::EncNav:   return L.enc == EncMode::Nav ? Led::On : Led::Off;
        default: break;
    }
    return Led::Off;
}

}  // namespace mcu
