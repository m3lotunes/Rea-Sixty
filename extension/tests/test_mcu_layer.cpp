// McuLayer: faders and V-Pots out, the byte stream in (split SysEx included), the
// LCD, rings, meters, LEDs, the handshake answers and the UF8 key table.

#include "McuLayer.h"

#include <cstdio>
#include <cstdlib>

static int g_fail = 0;
#define EXPECT(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

using namespace mcu;

static std::vector<Bytes> parse(Parser& p, const Bytes& in)
{
    std::vector<Bytes> out;
    p.feed(in.data(), in.size(), out);
    return out;
}

static void feedAll(Shadow& S, const Bytes& m, int64_t now, std::vector<Bytes>* reply = nullptr)
{
    std::vector<Bytes> r;
    apply(S, m, now, Identity{}, reply ? *reply : r);
}

int main()
{
    // ---- fader <-> pitchbend ------------------------------------------------------
    EXPECT(toPitchbend(0) == 0);
    EXPECT(toPitchbend(kUf8PbMax) == kPbMax);
    EXPECT(toPitchbend(kUf8PbMax + 100) == kPbMax);
    EXPECT(toPitchbend(-5) == 0);
    EXPECT(toUf8Fader(kPbMax) == kUf8PbMax);
    EXPECT(toUf8Fader(0) == 0);
    for (int pb = 0; pb <= kUf8PbMax; pb += 37)
        EXPECT(std::abs(toUf8Fader(toPitchbend(pb)) - pb) <= 1);
    {
        const Bytes m = faderMsg(2, kUf8PbMax);
        EXPECT(m.size() == 3 && m[0] == 0xE2 && m[1] == 0x7F && m[2] == 0x7F);
        const Bytes z = faderMsg(0, 0);
        EXPECT(z == (Bytes{0xE0, 0x00, 0x00}));
    }
    EXPECT(touchMsg(0, true) == (Bytes{0x90, 0x68, 0x7F}));
    EXPECT(touchMsg(7, false) == (Bytes{0x90, 0x6F, 0x00}));

    // ---- relative encoding ------------------------------------------------------------
    EXPECT(relByte(1) == 0x01);
    EXPECT(relByte(-1) == 0x41);
    EXPECT(relByte(31) == 0x1F);
    EXPECT(relByte(-32) == 0x60);
    EXPECT(relByte(200) == 0x3F);                  // clamped to 63 ticks
    EXPECT(relByte(0) == 0x00);
    for (int d = -63; d <= 63; ++d) if (d) EXPECT(relValue(relByte(d)) == d);
    EXPECT(vpotMsg(3, -2) == (Bytes{0xB0, 0x13, 0x42}));

    // ---- the channel encoder ---------------------------------------------------------
    {
        Local L;
        auto ch = encoderTurn(2, L);
        EXPECT(ch.size() == 4);
        EXPECT(ch[0] == (Bytes{0x90, 0x31, 0x7F}) && ch[1] == (Bytes{0x90, 0x31, 0x00}));
        EXPECT(encoderTurn(-1, L)[0] == (Bytes{0x90, 0x30, 0x7F}));
        EXPECT(encoderTurn(30, L).size() == 16);   // capped at 8 presses
        EXPECT(encoderTurn(0, L).empty());
        L.enc = EncMode::Nav;
        auto j = encoderTurn(-3, L);
        EXPECT(j.size() == 1 && j[0] == (Bytes{0xB0, 0x3C, 0x43}));
    }

    // ---- the parser ---------------------------------------------------------------
    {
        Parser p;
        auto out = parse(p, {0x90, 0x08, 0x7F, 0xE1, 0x10, 0x20});
        EXPECT(out.size() == 2 && out[0] == (Bytes{0x90, 0x08, 0x7F}) && out[1] == (Bytes{0xE1, 0x10, 0x20}));
        // running status
        out = parse(p, {0x90, 0x10, 0x7F, 0x11, 0x00});
        EXPECT(out.size() == 2 && out[1] == (Bytes{0x90, 0x11, 0x00}));
        // a SysEx split over three packets, a clock byte in the middle
        out = parse(p, {0xF0, 0x00, 0x00});
        EXPECT(out.empty());
        out = parse(p, {0x66, 0x14, 0xF8, 0x12});
        EXPECT(out.empty());
        out = parse(p, {0x00, 'A', 0xF7, 0xD0, 0x2C});
        EXPECT(out.size() == 2);
        EXPECT(out[0] == (Bytes{0xF0, 0x00, 0x00, 0x66, 0x14, 0x12, 0x00, 'A', 0xF7}));
        EXPECT(out[1] == (Bytes{0xD0, 0x2C}));    // one data byte
        // a SysEx cut by a status byte is dropped, the status byte is not
        out = parse(p, {0xF0, 0x00, 0x00, 0x66, 0xB0, 0x30, 0x05});
        EXPECT(out.size() == 1 && out[0] == (Bytes{0xB0, 0x30, 0x05}));
        // data without a status after system common: ignored
        p.reset();
        out = parse(p, {0xF2, 0x01, 0x02, 0x33});
        EXPECT(out.empty());
        // a runaway SysEx is dropped, the next message comes through
        Bytes big = {0xF0};
        big.insert(big.end(), kMaxSysex + 10, 0x20);
        big.push_back(0xF7);
        big.insert(big.end(), {0x90, 0x00, 0x7F});
        out = parse(p, big);
        EXPECT(out.size() == 1 && out[0] == (Bytes{0x90, 0x00, 0x7F}));
    }

    // ---- LCD ---------------------------------------------------------------------
    {
        Lcd l;
        EXPECT(l.cell(0, 0) == "       ");
        const uint8_t t[] = {'U', 'A', 'D', ' ', '1'};
        l.write(0, t, 5);
        EXPECT(l.cell(0, 0) == "UAD 1  ");
        EXPECT(trimRight(l.cell(0, 0)) == "UAD 1");
        const uint8_t v[] = {'-', '1', '2', '.', '5'};
        l.write(0x38 + 7, v, 5);                   // row 2, strip 2
        EXPECT(trimRight(l.cell(1, 1)) == "-12.5");
        // a write past the end of row 1 runs on into row 2
        const uint8_t w[] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I'};
        l.write(0x37 - 1, w, 9);
        EXPECT(l.cell(0, 7)[5] == 'A' && l.cell(0, 7)[6] == 'B');
        EXPECT(l.cell(1, 0).substr(0, 7) == "CDEFGHI");
        // past the end of row 2: clipped, nothing wraps to row 1
        l.write(0x6E, w, 9);
        EXPECT(l.cell(1, 7)[5] == 'A' && l.cell(1, 7)[6] == 'B');
        EXPECT(l.cell(0, 0) == "UAD 1  ");
        // control characters show as spaces
        const uint8_t c[] = {'X', 0x01, 'Y'};
        l.write(14, c, 3);
        EXPECT(l.cell(0, 2).substr(0, 3) == "X Y");
        EXPECT(l.cell(2, 0).empty() && l.cell(0, 8).empty());
    }

    // ---- rings --------------------------------------------------------------------
    {
        EXPECT(ringView(0x00).uf8Mode == 0x03);                         // position 0 = off
        EXPECT(ringView(0x0C).uf8Mode == 0x03);                         // 12 is not a position
        EXPECT(ringView(0x01).uf8Mode == 0x01 && ringView(0x01).value == 0.0);
        EXPECT(ringView(0x0B).uf8Mode == 0x01 && ringView(0x0B).value == 1.0);
        EXPECT(ringView(0x16).uf8Mode == 0x02 && ringView(0x16).value == 0.0);   // boost/cut centre
        EXPECT(ringView(0x11).value == -1.0 && ringView(0x1B).value == 1.0);
        EXPECT(ringView(0x56).uf8Mode == 0x02);                         // centre LED bit ignored
        EXPECT(ringView(0x21).uf8Mode == 0x01);                         // wrap -> unipolar
        EXPECT(ringView(0x36).uf8Mode == 0x01 && ringView(0x36).value == 1.0);   // spread
    }

    // ---- meters -------------------------------------------------------------------
    {
        Meter m;
        meterSet(m, 0x0C, 1000);
        EXPECT(meterLevel(m, 1000) == 12);
        EXPECT(meterLevel(m, 1000 + kMeterLevelMs) == 11);
        EXPECT(meterLevel(m, 1000 + 12 * kMeterLevelMs) == 0);
        EXPECT(meterLevel(m, 999999) == 0);
        meterSet(m, 0x0E, 2000);
        EXPECT(m.over && m.level == 12);           // overload leaves the level alone
        meterSet(m, 0x0F, 2000);
        EXPECT(!m.over);
        EXPECT(meterDb(12) == 0.0 && meterDb(1) == -60.0 && meterDb(0) < -100.0);
        EXPECT(meterDb(-3) == meterDb(0) && meterDb(99) == 0.0);
    }

    // ---- the shadow -----------------------------------------------------------------
    {
        Shadow S;
        EXPECT(!S.online && S.gen == 0);
        feedAll(S, {0x90, 0x08, 0x7F}, 10);
        EXPECT(S.online && S.gen == 1 && soloLed(S, 0) == Led::On);
        feedAll(S, {0x90, 0x10, 0x01}, 10);
        EXPECT(cutLed(S, 0) == Led::Blink);
        feedAll(S, {0x90, 0x10, 0x00}, 10);
        EXPECT(cutLed(S, 0) == Led::Off);
        feedAll(S, {0x90, 0x1B, 0x7F}, 10);
        feedAll(S, {0x90, 0x03, 0x7F}, 10);
        EXPECT(selLed(S, 3, SelMode::Norm) == Led::On && selLed(S, 3, SelMode::Rec) == Led::On);
        feedAll(S, {0x80, 0x1B, 0x40}, 10);
        EXPECT(selLed(S, 3, SelMode::Norm) == Led::Off);
        // faders, master included
        feedAll(S, {0xE5, 0x7F, 0x7F}, 10);
        EXPECT(S.faderKnown[5] && S.fader[5] == kPbMax && !S.faderKnown[4]);
        feedAll(S, {0xE8, 0x00, 0x40}, 10);
        EXPECT(S.faderKnown[8] && S.fader[8] == 0x2000);
        feedAll(S, {0xE9, 0x00, 0x40}, 10);                     // no channel 10
        EXPECT(!S.faderKnown[0]);
        // ring, assignment, timecode ignored
        const uint32_t g = S.gen;
        feedAll(S, {0xB0, 0x32, 0x16}, 10);
        EXPECT(S.ring[2] == 0x16 && S.gen == g + 1);
        feedAll(S, {0xB0, 0x4B, 0x10}, 10);
        feedAll(S, {0xB0, 0x4A, 0x0E}, 10);
        EXPECT(S.assign[0] == 'P' && S.assign[1] == 'N');
        feedAll(S, {0xB0, 0x40, 0x31}, 10);
        EXPECT(S.gen == g + 3);
        // meters: D0 per strip, D1 master
        feedAll(S, {0xD0, 0x7C}, 500);
        EXPECT(S.meter[7].level == 12 && S.meter[7].setMs == 500);
        feedAll(S, {0xD1, 0x1A}, 500);
        EXPECT(S.master[1].level == 10 && S.meter[1].level == 0);
        // LCD
        feedAll(S, {0xF0, 0x00, 0x00, 0x66, 0x14, 0x12, 0x07, 'A', 'U', 'X', ' ', '1', 0xF7}, 10);
        EXPECT(trimRight(S.lcd.cell(0, 1)) == "AUX 1");
        feedAll(S, {0xF0, 0x00, 0x00, 0x66, 0x10, 0x12, 0x38, 'L', 0xF7}, 10);    // Logic Control id
        EXPECT(S.lcd.cell(1, 0)[0] == 'L');
        feedAll(S, {0xF0, 0x00, 0x00, 0x66, 0x16, 0x12, 0x38, 'Z', 0xF7}, 10);    // not ours
        EXPECT(S.lcd.cell(1, 0)[0] == 'L');
        feedAll(S, {0xF0, 0x00, 0x20, 0x29, 0x14, 0x12, 0x38, 'Z', 0xF7}, 10);    // other maker
        EXPECT(S.lcd.cell(1, 0)[0] == 'L');
        // utilities
        feedAll(S, {0xF0, 0x00, 0x00, 0x66, 0x14, 0x62, 0xF7}, 10);
        EXPECT(soloLed(S, 0) == Led::Off);
        feedAll(S, {0xF0, 0x00, 0x00, 0x66, 0x14, 0x61, 0xF7}, 10);
        EXPECT(S.fader[5] == 0 && S.faderKnown[0]);
        feedAll(S, {0xF0, 0x00, 0x00, 0x66, 0x14, 0x0F, 0x7F, 0xF7}, 10);
        EXPECT(!S.online);
        feedAll(S, {0xF0, 0x00, 0x00, 0x66, 0x14, 0x63, 0xF7}, 20);
        EXPECT(!S.faderKnown[0] && S.lcd.cell(0, 1) == "       " && S.lastRxMs == 20);
    }

    // ---- handshake --------------------------------------------------------------------
    {
        Shadow S;
        std::vector<Bytes> r;
        feedAll(S, {0xF0, 0x00, 0x00, 0x66, 0x14, 0x00, 0xF7}, 1, &r);
        EXPECT(r.size() == 1);
        EXPECT(r[0].size() == 18 && r[0][4] == 0x14 && r[0][5] == 0x01 && r[0][6] == 'R'
               && r[0][17] == 0xF7);
        for (uint8_t b : r[0]) if (b != 0xF0 && b != 0xF7) EXPECT(b < 0x80);
        r.clear();
        feedAll(S, {0xF0, 0x00, 0x00, 0x66, 0x15, 0x02, 'x', 'x', 'x', 'x', 'x', 'x', 'x',
                    1, 2, 3, 4, 0xF7}, 1, &r);
        EXPECT(r.size() == 1 && r[0][4] == 0x15 && r[0][5] == 0x03 && r[0].size() == 14);
        r.clear();
        feedAll(S, {0xF0, 0x00, 0x00, 0x66, 0x14, 0x13, 0x00, 0xF7}, 1, &r);
        EXPECT(r.size() == 1 && r[0][5] == 0x14 && r[0][6] == 'V' && r[0].size() == 12);
        r.clear();
        feedAll(S, {0x90, 0x00, 0x7F}, 1, &r);
        EXPECT(r.empty());
    }

    // ---- blink --------------------------------------------------------------------
    EXPECT(ledLit(Led::On, 123) && !ledLit(Led::Off, 123));
    EXPECT(ledLit(Led::Blink, 0) && !ledLit(Led::Blink, kBlinkHalfMs)
           && ledLit(Led::Blink, 2 * kBlinkHalfMs));

    // ---- UF8 keys ---------------------------------------------------------------------
    {
        Local L;
        auto is = [&](uint8_t id, uint8_t n) {
            const KeyAction a = keyAction(id, L);
            return a.kind == KeyKind::Note && a.value == n;
        };
        auto kind = [&](uint8_t id) { return keyAction(id, L).kind; };
        // strips: SOLO 0x20+3n, CUT 0x21+3n, SEL 0x22+3n
        EXPECT(is(0x20, 0x08) && is(0x35, 0x0F));
        EXPECT(is(0x21, 0x10) && is(0x36, 0x17));
        EXPECT(is(0x22, 0x18) && is(0x37, 0x1F));
        L.sel = SelMode::Rec;
        EXPECT(is(0x22, 0x00) && is(0x37, 0x07));   // SEL arms in REC
        L.sel = SelMode::Norm;
        EXPECT(is(0x08, 0x20) && is(0x0F, 0x27));   // V-Pot push = V-SELECT
        // soft keys: the user's layout
        EXPECT(is(0x18, note::Pan) && is(0x1B, note::Eq) && is(0x1C, note::Instrument));
        EXPECT(kind(0x1D) == KeyKind::None);
        EXPECT(std::string(softKey(0, 3).label) == "INPUTS" && std::string(softKey(0, 4).label) == "GAIN");
        L.softBank = 1;
        EXPECT(is(0x18, 70));                       // "ALT": the note UA Midi Control listens for
        EXPECT(std::string(softKey(1, 0).label) == "ALT");
        EXPECT(kind(0x19) == KeyKind::None);
        L.softBank = 4;
        EXPECT(is(0x18, 0x36) && is(0x1F, 0x3D));   // F1..F8
        L.softBank = 0;
        EXPECT(softKey(6, 0).note == note::None && softKey(0, 8).note == note::None);
        // local keys
        EXPECT(kind(0x68) == KeyKind::SoftBank && keyAction(0x6D, L).value == 5);
        EXPECT(kind(0x70) == KeyKind::SelMode && keyAction(0x71, L).value == uint8_t(SelMode::Rec));
        EXPECT(kind(0x73) == KeyKind::EncNav && kind(0x76) == KeyKind::EncPush);
        // stays Rea-Sixty's
        EXPECT(kind(0x40) == KeyKind::Rea && kind(0x42) == KeyKind::Rea && kind(0x46) == KeyKind::Rea);
        // globals
        EXPECT(is(0x78, note::BankLeft) && is(0x79, note::BankRight));
        EXPECT(is(0x52, note::ChannelLeft) && is(0x53, note::ChannelRight));
        EXPECT(is(0x54, note::Flip) && is(0x6E, note::Pan) && is(0x50, note::Plugin));
        EXPECT(is(0x6F, note::Shift));              // FINE/SHIFT as in SSL's Bitwig profile
        EXPECT(is(0x59, note::Read) && is(0x5A, note::Write) && is(0x5B, note::Trim)
               && is(0x5C, note::Latch) && is(0x5D, note::Touch));
        EXPECT(kind(0x58) == KeyKind::None);        // OFF: no MCU note
        EXPECT(is(0x43, note::Global) && is(0x44, note::GvAux) && kind(0x45) == KeyKind::None);
        EXPECT(is(0x00, note::UserA) && is(0x01, note::UserB));
        for (uint8_t id = 0x48; id <= 0x4F; ++id) EXPECT(kind(id) == KeyKind::None);
        EXPECT(kind(0x51) == KeyKind::None && kind(0x72) == KeyKind::None
               && kind(0x74) == KeyKind::None && kind(0x75) == KeyKind::None);
        // the zoom pad: cursor + zoom, or transport while CHANNEL is held
        EXPECT(is(0x7A, note::Up) && is(0x7E, note::Down) && is(0x7B, note::Left)
               && is(0x7D, note::Right) && is(0x7C, note::Zoom));
        L.transport = true;
        EXPECT(is(0x7A, note::Play) && is(0x7E, note::Stop) && is(0x7B, note::Rewind)
               && is(0x7D, note::Forward) && is(0x7C, note::Record));
        EXPECT(is(0x78, note::BankLeft));           // the rest is unchanged
        L.transport = false;
        EXPECT(kind(0x02) == KeyKind::None && kind(0x7F) == KeyKind::None);
    }

    // ---- key lamps ------------------------------------------------------------------
    {
        Shadow S;
        Local L;
        feedAll(S, {0x90, note::Flip, 0x01}, 0);
        EXPECT(keyLed(S, 0x54, L) == Led::Blink);
        EXPECT(keyLed(S, 0x70, L) == Led::On && keyLed(S, 0x71, L) == Led::Off);
        EXPECT(keyLed(S, 0x68, L) == Led::On && keyLed(S, 0x69, L) == Led::Off);
        EXPECT(keyLed(S, 0x73, L) == Led::Off);
        L.enc = EncMode::Nav;
        EXPECT(keyLed(S, 0x73, L) == Led::On);
        EXPECT(keyLed(S, 0x40, L) == Led::Off);     // Rea-Sixty's keys light themselves
    }

    if (g_fail) std::printf("%d failure(s)\n", g_fail);
    else        std::printf("mcu_layer: all passed\n");
    return g_fail ? 1 : 0;
}
