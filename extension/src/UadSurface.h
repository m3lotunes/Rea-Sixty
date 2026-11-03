#pragma once
//
// UadSurface: the UF8 as a UAD Console mixer (docs/uad-console-plan.md, Baustein C,
// controls in section 4, safety in section 5). UF8 frames in, engine commands out;
// the model (UadModel) in, UF8 frames out. No REAPER, no socket, no device: the
// caller passes the commands to the engine and the engine's messages to the model.
// tests/test_uad_surface.cpp runs it between a virtual UF8 (Uf8Sim.h) and a
// virtual engine (UadSim.h).
//
// ⛔ NOTHING HERE IS A SECOND COPY. Framing (Uf8Frames.h), the fader read and the
// motor (Uf8Fader.h), the strip cells (Uf8Strips.h), the builders and the LED
// table (Protocol), the value line and the meter scale (Uf1Text.h) are the
// extension's own.
//
// ⛔ EVERY WRITE GOES THROUGH write_(): the allowlist (allowedProperty), nothing
// while no device is online, at most one value per path every kWriteMs with the
// newest winning. The engine does not enforce its own readonly flags.
//
// Threads: none of its own and no lock; the caller serialises (as McuSurface).
//
#include "UadModel.h"
#include "Uf8Frames.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace uadsurf {

using Frame = std::vector<std::uint8_t>;

struct Out {
    std::vector<std::string> cmds;   // to the engine, each a whole command with its NUL
    std::vector<Frame>       uf8;    // to the UF8 (the left unit)
};

// What the V-Pots do (the V-POT soft-key bank, the user's old Layer 3 labels).
enum class PotMode : std::uint8_t { Pan, Send, Preamp };

constexpr int kTouchQuietMs = 150;   // main.cpp's kTouchDebounceQuiet
constexpr int kWriteMs      = 16;    // one write per path per window [Ref: open-apollo]
constexpr int kHandHoldMs   = 350;   // the hand wins over the engine's echo this long
constexpr int kPhantomHoldMs = 500;  // 48V only after holding the key this long
constexpr int kSoftBanks    = 3;     // V-POT (modes), 1 Monitor, 2 Preamp

// Properties the surface may write, by name. Anything else is refused.
bool allowedProperty(const std::string& propPath);

class Surface {
public:
    explicit Surface(uad::Model& m) : m_(m) { invalidate(); }

    // UF8 -> engine. False = not the side-car's (LAYER 1..3, 360, frames it does
    // not know): the caller's normal path takes it.
    bool fromUf8(const uf8frames::Frame& f, std::int64_t nowMs, Out& out);
    // Model -> UF8, plus what time does: touch releases, the 48V hold, the writes
    // waiting for their window. Every tick while the side-car is shown.
    void paint(std::int64_t nowMs, Out& out);
    void invalidate();

    // Take the channel list from the model now (paint does it too). Without a UF8
    // nothing paints, and the subscriptions below would stay empty.
    void refresh();

    // What time does without painting: touch releases, the 48V hold, the writes
    // waiting for their window. paint calls it; without a UF8 the owner does.
    void service(std::int64_t nowMs, Out& out);

    // Offline test, for Settings' virtual UF8 only: writes go out while the
    // interface is offline (the engine reports changes then, measured 03.11.2026;
    // whether it takes writes is what this test shows). Never
    // 48V. The owner sets it around one input and clears it again.
    void setOfflineWrites(bool on) { offlineWrites_ = on; }

    // What the client should be subscribed to for what is on the surface now.
    std::vector<std::string> wantedSubscriptions() const;

    int     bank() const     { return bank_; }
    PotMode mode() const     { return mode_; }
    int     send() const     { return send_; }
    int     softBank() const { return softBank_; }
    int     selected() const { return selected_; }    // index into Model::strips(), -1 none
    bool    touched(int s) const { return s >= 0 && s < 8 && touch_[s].down; }
    // The colour of a strip for the colour bar, by channel type (the Console has none).
    std::uint32_t stripRgb(int s) const;

private:
    struct Touch { bool down = false, releasePending = false; std::int64_t releaseMs = 0; };
    struct Cache {
        std::string   upper, lower, csType, chanNum, faderText, faderUnit, valueLine, slotLabel;
        std::int32_t  topKey = -1;
        std::int8_t   sel = -1, cut = -1, solo = -1;
        std::uint16_t faderPb = 0xFFFF;
        bool          engaged = false;
    };
    struct Pending { std::string json; std::int64_t lastSentMs = -1000000; bool waiting = false; };

    bool devOk_(const uad::Device& D) const { return D.online || offlineWrites_; }
    bool offlineWrites_ = false;
    int  stripAt_(int s) const;            // surface strip 0..7 -> index into strips, -1
    void write_(const std::string& prop, const std::string& json, std::int64_t now, Out& out);
    void flush_(std::int64_t now, Out& out);
    void key_(std::uint8_t id, bool pressed, std::int64_t now, Out& out);
    void pot_(int s, int detents, std::int64_t now, Out& out);
    void soft_(int key, bool pressed, std::int64_t now, Out& out);
    void toggle_(const std::string& prop, bool current, std::int64_t now, Out& out);
    std::string potText_(int idx, bool full) const;

    uad::Model&                 m_;
    std::vector<uad::Strip>     strips_;       // refreshed each paint
    int     bank_ = 0, send_ = 0, softBank_ = 0, selected_ = -1;
    PotMode mode_ = PotMode::Pan;
    bool    fine_ = false;
    std::array<Touch, 8>        touch_{};
    std::array<std::int64_t, 8> handUntil_{};
    std::array<double, 8>       handTapered_{};
    std::array<Cache, 8>        cache_{};
    std::array<std::uint16_t, 8> lastBar_{};
    std::array<std::uint8_t, 8>  lastMode_{};
    bool    ringsInit_ = false;
    std::array<std::uint8_t, 16> lastVu_{};
    bool    vuInit_ = false;
    std::array<std::int8_t, 64> lastGlobal_{};
    std::map<std::string, Pending> pending_;
    int          phantomKey_ = -1;             // soft key held for 48V
    std::int64_t phantomDownMs_ = 0;
    bool         phantomFired_ = false;
    bool         fresh_ = true;
};

}  // namespace uadsurf
