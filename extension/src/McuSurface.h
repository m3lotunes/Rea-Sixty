#pragma once
//
// McuSurface: the UF8 on an MCU layer, without REAPER (plan docs/mcu-layer-plan.md,
// Bausteine E and F). UF8 frames in, MCU messages out; MCU messages in, UF8 frames
// out. McuLayer.h has the protocol, this is the loop around it: the touch state,
// the encoder push, the caches that keep the painter to what changed.
//
// ⛔ NOTHING HERE IS A SECOND COPY. The frames are the extension's: framing
// (Uf8Frames.h), the fader read and the motor (Uf8Fader.h), the strip cells
// (Uf8Strips.h), the builders and the LED table (Protocol), the meter scale
// (Uf1Text.h dbToVuByte_). main.cpp plugs it between the USB thread, the CoreMIDI
// port and the timer; tests/test_mcu_surface.cpp plugs it between a virtual UF8
// (Uf8Sim.h) and a scripted host. ORC could host it the same way.
//
// Threads: none of its own and no lock. The caller serialises: main.cpp holds one
// mutex around fromUf8 (USB thread), fromHost (drained on the timer) and paint.
//
#include "McuLayer.h"
#include "Uf8Frames.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace mcusurf {

using Frame = std::vector<std::uint8_t>;

// What leaves the surface in one call, in order.
struct Out {
    std::vector<mcu::Bytes> midi;    // to the host (the "Rea-Sixty MCU" source)
    std::vector<Frame>      uf8;     // to the UF8 (the left unit)
};

struct Options {
    // CHANNEL encoder held = the zoom pad becomes transport (SSL, Hb p. 127).
    bool     transportHold = true;
    // Colour of SEL (NORM), the soft keys and the colour bar. MCU has no colours.
    std::uint32_t rgb = 0xFFFFFF;
};

// The quiet time before a touch counts as let go: the sensor bounces under a held
// finger. main.cpp's kTouchDebounceQuiet, same value.
constexpr int kTouchQuietMs = 150;
// Holding the encoder this long switches transport mode, once per hold.
constexpr int kEncHoldMs = 600;

class Surface {
public:
    explicit Surface(Options o = {}) : opt_(o) { invalidate(); }

    // Host -> shadow. The handshake answers land in out.midi. Kept whether the
    // layer is shown or not, so entering it shows the host's state at once.
    void fromHost(const std::uint8_t* p, std::size_t n, std::int64_t nowMs, Out& out);

    // UF8 -> host, one whole frame of the left unit. False = not the layer's (LAYER
    // keys, 360, frames it does not know): the caller's normal path takes it.
    bool fromUf8(const uf8frames::Frame& f, std::int64_t nowMs, Out& out);

    // Shadow -> UF8: what changed since the last paint, everything after
    // invalidate(). Also lets touches go after their quiet time and fires the
    // encoder hold. Call it every tick while the layer is shown.
    void paint(std::int64_t nowMs, Out& out);

    // Entering the layer or a reopened device: every cell again on the next paint.
    void invalidate();

    const mcu::Shadow& shadow() const { return shadow_; }
    const mcu::Local&  local() const  { return local_; }
    Options&           options()      { return opt_; }
    bool touched(int strip) const { return strip >= 0 && strip < 8 && touch_[strip].down; }

    // The UF8 meter byte (0..0x1F) for an MCU meter now.
    static std::uint8_t meterByte(const mcu::Meter& m, std::int64_t nowMs);

private:
    struct Touch {
        bool         down = false;
        bool         releasePending = false;
        std::int64_t releaseMs = 0;
    };
    struct StripCache {
        std::string   upper, lower, csType, chanNum, faderText, faderUnit, valueLine, slotLabel;
        std::int32_t  topKey  = -1;
        std::int8_t   sel = -1, cut = -1, solo = -1;
        std::uint16_t faderPb = 0xFFFF;
        bool          engaged = false;
        bool          rec     = false;   // SEL showed REC (red) when last painted
    };

    void key_(std::uint8_t id, bool pressed, std::int64_t nowMs, Out& out);

    Options                        opt_;
    mcu::Parser                    parser_;
    mcu::Shadow                    shadow_;
    mcu::Identity                  id_;
    mcu::Local                     local_;
    std::array<Touch, 8>           touch_{};
    std::array<int, 8>             lastPbSent_{};
    std::array<StripCache, 8>      cache_{};
    std::array<std::uint16_t, 8>   lastBar_{};
    std::array<std::uint8_t, 8>    lastMode_{};
    bool                           ringsInit_ = false;
    std::array<std::uint8_t, 16>   lastVu_{};
    bool                           vuInit_ = false;
    std::array<std::int8_t, 64>    lastGlobal_{};   // per Uf8GlobalLed, -1 = unknown
    bool                           encDown_ = false;
    bool                           encHoldFired_ = false;
    std::int64_t                   encDownMs_ = 0;
    bool                           fresh_ = true;   // invalidate() pending
};

}  // namespace mcusurf
