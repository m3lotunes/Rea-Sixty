// SettingsScreen_MidiCc.cpp: the MIDI CC layer (and the UAD Console layer's block)
// in Settings > Bindings > UF8
// (docs/midi-cc-layer-plan.md, Baustein I, as in Frank's approved mockup of
// 27.10.2026): the layer type, the layer block (outputs, input, follow, default
// channel, presets), the five pages, the grid of controls and the editor of the
// one selected.
//
// ⛔ ONLY CALL SHAPES SettingsScreen.cpp ALREADY USES. ReaImGui tears the window
// down on a call shape it objects to, with no crash report, even when the
// signature is fine ([[reaimgui-new-call-shape-teardown]]).

#include "SettingsScreen.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "Bindings.h"
#include "MidiCcLayer.h"
#include "Protocol.h"
#include "UadClient.h"
#include "reaper_imgui_functions.h"
#include "reaper_plugin_functions.h"

int reasixty_ccInputStatus();   // main.cpp: 0 none, 1 open, 2 not found, 3 busy
uadside::Status reasixty_uadStatus();   // main.cpp: the UAD Console layer's connection
bool reasixty_drawLedLook(ImGui_Context* ctx, ledlook::Look& l, const char* tag);   // below, shared with the UAD page

namespace {

namespace bnd = uf8::bindings;
using midicc::Kind;

int  s_kind  = static_cast<int>(Kind::Fader);
int  s_index = 0;
int  s_presetSel = -1;
char s_presetName[64] = {0};

inline void tip_(ImGui_Context* ctx, const char* text)
{
    if (ImGui_IsItemHovered(ctx, nullptr)) ImGui_SetTooltip(ctx, text);
}

const char* kindName_(Kind k)
{
    switch (k) {
        case Kind::Fader:      return "Fader";
        case Kind::VPot:       return "V-Pot";
        case Kind::Push:       return "V-Pot push";
        case Kind::Soft:       return "Soft-key";
        case Kind::SendPlugin: return "Send/plug-in key";
        case Kind::Foot:       return "Foot switch";
    }
    return "";
}

int rgba_(uint32_t rgb, int alpha) { return static_cast<int>((rgb << 8) | uint32_t(alpha)); }

std::vector<std::string> outputNames_()
{
    std::vector<std::string> v;
    const int n = GetNumMIDIOutputs();
    for (int i = 0; i < n; ++i) {
        char nm[256] = {0};
        if (GetMIDIOutputName(i, nm, sizeof(nm)) && *nm) v.push_back(nm);
    }
    return v;
}

std::vector<std::string> inputNames_()
{
    std::vector<std::string> v;
    const int n = GetNumMIDIInputs();
    for (int i = 0; i < n; ++i) {
        char nm[256] = {0};
        // REAPER's own keyboard is not a device the layer can open (probe 27.10.).
        if (GetMIDIInputName(i, nm, sizeof(nm)) && *nm
            && std::string(nm) != "Virtual MIDI Keyboard")
            v.push_back(nm);
    }
    return v;
}

bool hasOutput_(const midicc::Layer& L, const std::string& name)
{
    return std::find(L.outputs.begin(), L.outputs.end(), name) != L.outputs.end();
}

void setOutput_(midicc::Layer& L, const std::string& name, bool on)
{
    auto it = std::find(L.outputs.begin(), L.outputs.end(), name);
    if (on && it == L.outputs.end()) L.outputs.push_back(name);
    if (!on && it != L.outputs.end()) L.outputs.erase(it);
}

// The layer block: where the MIDI goes, where it comes back from, presets.
bool drawConnections_(ImGui_Context* ctx, int layer, midicc::Layer& L)
{
    bool dirty = false;
    constexpr double kLabelW = 170.0, kFieldW = 360.0;
    auto row = [&](const char* label, const char* tip) {
        ImGui_AlignTextToFramePadding(ctx);
        ImGui_Text(ctx, label);
        tip_(ctx, tip);
        double x = kLabelW;
        ImGui_SameLine(ctx, &x, nullptr);
        ImGui_SetNextItemWidth(ctx, kFieldW);
    };

    // Send to: several outputs, picked in one dropdown.
    const std::vector<std::string> outs = outputNames_();
    std::string preview;
    for (const auto& o : L.outputs) {
        const bool off = !o.empty() && std::find(outs.begin(), outs.end(), o) == outs.end();
        preview += (preview.empty() ? "" : ", ") + (o.empty() ? std::string("REAPER") : o) + (off ? " (offline)" : "");
    }
    if (preview.empty()) preview = "(nothing)";
    row("Send to", "Where the layer's MIDI goes. Pick one or more.");
    if (ImGui_BeginCombo(ctx, "##cc_out_combo", preview.c_str(), nullptr)) {
        bool v = hasOutput_(L, "");
        if (ImGui_Checkbox(ctx, "REAPER (virtual MIDI keyboard: tracks record it)##cc_out_vkb", &v)) {
            setOutput_(L, "", v);
            dirty = true;
        }
        for (size_t i = 0; i < outs.size(); ++i) {
            char id[300];
            snprintf(id, sizeof(id), "%s##cc_out_%d", outs[i].c_str(), int(i));
            bool on = hasOutput_(L, outs[i]);
            if (ImGui_Checkbox(ctx, id, &on)) { setOutput_(L, outs[i], on); dirty = true; }
        }
        for (size_t i = 0; i < L.outputs.size(); ++i) {   // chosen, but not plugged in
            const std::string& o = L.outputs[i];
            if (o.empty() || std::find(outs.begin(), outs.end(), o) != outs.end()) continue;
            char id[320];
            snprintf(id, sizeof(id), "%s (offline)##cc_out_off_%d", o.c_str(), int(i));
            bool on = true;
            if (ImGui_Checkbox(ctx, id, &on) && !on) { setOutput_(L, o, false); dirty = true; break; }
        }
        ImGui_EndCombo(ctx);
    }

    // Values from a device: one input.
    row("Values from device", "A synth or controller that reports its knobs back. The faders follow it.");
    {
        const std::string cur = L.inputDevice.empty() ? std::string("(none)") : L.inputDevice;
        if (ImGui_BeginCombo(ctx, "##cc_input", cur.c_str(), nullptr)) {
            bool sel = L.inputDevice.empty();
            if (ImGui_Selectable(ctx, "(none)##cc_in_none", &sel, nullptr, nullptr, nullptr)) { L.inputDevice.clear(); dirty = true; }
            const std::vector<std::string> ins = inputNames_();
            for (size_t i = 0; i < ins.size(); ++i) {
                char id[300];
                snprintf(id, sizeof(id), "%s##cc_in_%d", ins[i].c_str(), int(i));
                bool on = (L.inputDevice == ins[i]);
                if (ImGui_Selectable(ctx, id, &on, nullptr, nullptr, nullptr)) { L.inputDevice = ins[i]; dirty = true; }
            }
            ImGui_EndCombo(ctx);
        }
        if (!L.inputDevice.empty() && layer == bnd::ccLayerActiveIndex()) {
            const int st = reasixty_ccInputStatus();
            if (st == 2 || st == 3) {
                ImGui_SameLine(ctx, nullptr, nullptr);
                ImGui_TextDisabled(ctx, st == 2 ? "not connected"
                                                : "busy: disable it as input in REAPER's MIDI preferences");
            }
        }
    }

    // Values from REAPER.
    ImGui_AlignTextToFramePadding(ctx);
    ImGui_Text(ctx, "Values from REAPER");
    tip_(ctx, "While REAPER plays, the faders follow the CC lanes in the selected track's MIDI items.");
    {
        double x = kLabelW;
        ImGui_SameLine(ctx, &x, nullptr);
        bool v = L.followItems;
        if (ImGui_Checkbox(ctx, "follow the CC lanes of the selected track##cc_follow", &v)) { L.followItems = v; dirty = true; }
    }

    // Default channel.
    row("Default channel", "Every control sends on this channel unless it has its own.");
    ImGui_SetNextItemWidth(ctx, 110.0);
    {
        int ch = L.defaultChannel;
        if (ImGui_InputInt(ctx, "##cc_defch", &ch, nullptr, nullptr, nullptr)) { L.defaultChannel = std::clamp(ch, 1, 16); dirty = true; }
    }
    return dirty;
}

// Preset row: one place for CC sets (load, save, save as, delete, factory).
bool drawPresets_(ImGui_Context* ctx, int layer, midicc::Layer& L)
{
    bool dirty = false;
    const std::vector<std::string> names = bnd::ccPresetNames();
    if (s_presetSel >= static_cast<int>(names.size())) s_presetSel = -1;
    ImGui_Text(ctx, "Preset");
    ImGui_SameLine(ctx, nullptr, nullptr);
    ImGui_SetNextItemWidth(ctx, 200.0);
    if (ImGui_BeginCombo(ctx, "##cc_preset",
                         s_presetSel >= 0 ? names[s_presetSel].c_str() : "(choose)", nullptr)) {
        for (size_t i = 0; i < names.size(); ++i) {
            char id[96];
            snprintf(id, sizeof(id), "%s##cc_pre_%d", names[i].c_str(), int(i));
            bool s = (s_presetSel == int(i));
            if (ImGui_Selectable(ctx, id, &s, nullptr, nullptr, nullptr)) s_presetSel = int(i);
        }
        ImGui_EndCombo(ctx);
    }
    if (s_presetSel >= 0) {
        ImGui_SameLine(ctx, nullptr, nullptr);
        if (ImGui_Button(ctx, "Load##cc_pre_load", nullptr, nullptr))
            bnd::loadCcPreset(names[s_presetSel], layer);
        tip_(ctx, "Replaces this layer's CC set with the preset.");
        ImGui_SameLine(ctx, nullptr, nullptr);
        if (ImGui_Button(ctx, "Save##cc_pre_save", nullptr, nullptr))
            bnd::saveCcPreset(names[s_presetSel], layer);
        tip_(ctx, "Overwrites the preset with this layer's CC set.");
    }
    ImGui_SameLine(ctx, nullptr, nullptr);
    if (ImGui_Button(ctx, "Save as##cc_pre_saveas", nullptr, nullptr)) {
        s_presetName[0] = 0;
        ImGui_OpenPopup(ctx, "Save CC preset###cc_pre_modal", nullptr);
    }
    if (s_presetSel >= 0) {
        ImGui_SameLine(ctx, nullptr, nullptr);
        if (ImGui_Button(ctx, "Delete##cc_pre_del", nullptr, nullptr)) {
            bnd::deleteCcPreset(names[s_presetSel]);
            s_presetSel = -1;
        }
    }
    // Two factory sets: Instrument (the CCs synths and libraries answer to) and
    // Controller (free CCs only, one channel per page). Connections stay.
    ImGui_SameLine(ctx, nullptr, nullptr);
    ImGui_SetNextItemWidth(ctx, 190.0);
    if (ImGui_BeginCombo(ctx, "##cc_pre_factory", "Factory set...", nullptr)) {
        auto apply = [&](midicc::Layer f) {
            f.outputs = L.outputs;
            f.inputDevice = L.inputDevice;
            f.followItems = L.followItems;
            L = f;
            dirty = true;
        };
        bool a = false, b = false;
        if (ImGui_Selectable(ctx, "Instrument: Mod, Expression, Volume, Cutoff ...##cc_fac_i", &a, nullptr, nullptr, nullptr))
            apply(midicc::factoryLayer());
        tip_(ctx, "Faders on the CCs synths and sample libraries answer to; only the V-POT page is set.");
        if (ImGui_Selectable(ctx, "Controller: free CCs, one channel per page##cc_fac_c", &b, nullptr, nullptr, nullptr))
            apply(midicc::controllerLayer());
        tip_(ctx, "Every control on a CC MIDI leaves free. Page V-POT = channel 1 ... page 5 = channel 6, "
                  "same CC numbers on each. SOLO, CUT, SEL, the other keys and the encoder on channel 16.");
        ImGui_EndCombo(ctx);
    }
    tip_(ctx, "Replaces this layer's CC set with a factory one. Connections stay.");

    if (ImGui_BeginPopupModal(ctx, "Save CC preset###cc_pre_modal", nullptr, nullptr)) {
        ImGui_PushItemWidth(ctx, 280);
        ImGui_InputTextWithHint(ctx, "Name##cc_pre_name", "e.g. 'Strings'",
                                s_presetName, static_cast<int>(sizeof(s_presetName)),
                                nullptr, nullptr);
        ImGui_PopItemWidth(ctx);
        std::string nm = s_presetName;
        while (!nm.empty() && nm.back() == ' ') nm.pop_back();
        while (!nm.empty() && nm.front() == ' ') nm.erase(nm.begin());
        if (!nm.empty()) {
            if (ImGui_Button(ctx, "Save##cc_pre_modal_ok", nullptr, nullptr)) {
                if (dirty) bnd::setCcLayer(layer, L);   // save what is on screen
                dirty = false;
                bnd::saveCcPreset(nm, layer);
                ImGui_CloseCurrentPopup(ctx);
            }
        } else {
            ImGui_TextDisabled(ctx, "Save");
        }
        ImGui_SameLine(ctx, nullptr, nullptr);
        if (ImGui_Button(ctx, "Cancel##cc_pre_modal_cancel", nullptr, nullptr))
            ImGui_CloseCurrentPopup(ctx);
        ImGui_EndPopup(ctx);
    }
    return dirty;
}


// Channel: the layer's default, or its own (Novation's "Default Channel" pattern).
bool channelRow_(ImGui_Context* ctx, midicc::Control& c, int defaultChannel)
{
    bool dirty = false;
    ImGui_Text(ctx, "Channel");
    ImGui_SameLine(ctx, nullptr, nullptr);
    char dl[48];
    snprintf(dl, sizeof(dl), "Default (%d)##cc_ch_def", defaultChannel);
    int own = c.channel >= 1 ? 1 : 0;
    if (ImGui_RadioButtonEx(ctx, dl, &own, 0)) { c.channel = 0; dirty = true; }
    ImGui_SameLine(ctx, nullptr, nullptr);
    if (ImGui_RadioButtonEx(ctx, "Own##cc_ch_own", &own, 1) && c.channel < 1) { c.channel = defaultChannel; dirty = true; }
    if (c.channel >= 1) {
        ImGui_SameLine(ctx, nullptr, nullptr);
        ImGui_SetNextItemWidth(ctx, 100.0);
        int ch = c.channel;
        if (ImGui_InputInt(ctx, "##cc_ch_own_n", &ch, nullptr, nullptr, nullptr)) { c.channel = std::clamp(ch, 1, 16); dirty = true; }
    }
    return dirty;
}


// The editor of one of the other keys (s_kind 100, s_index = UF8 id) or the
// channel encoder (s_kind 101): the same on every page.
constexpr int kSelKey = 100, kSelEncoder = 101;
bool drawExtraEditor_(ImGui_Context* ctx, midicc::Layer& L)
{
    bool dirty = false;
    const bool enc = s_kind == kSelEncoder;
    midicc::Control& c = enc ? L.encoder : L.keys[s_index];
    char hdr[96];
    snprintf(hdr, sizeof(hdr), "%s, all pages", enc ? "Channel encoder" : midicc::keyName(s_index).c_str());
    ImGui_Text(ctx, hdr);
    ImGui_TextDisabled(ctx, enc ? "Without a CC the encoder keeps its normal job on this layer."
                                : "Without a CC (-1) the key keeps its normal job on this layer.");
    dirty |= channelRow_(ctx, c, L.defaultChannel);
    ImGui_Text(ctx, "CC number");
    ImGui_SameLine(ctx, nullptr, nullptr);
    ImGui_SetNextItemWidth(ctx, 110.0);
    {
        int n = c.number;
        if (ImGui_InputInt(ctx, "##cc_x_num", &n, nullptr, nullptr, nullptr)) { c.number = std::clamp(n, -1, 127); dirty = true; }
        tip_(ctx, "0 to 127. -1 = no CC, the normal job.");
    }
    if (!enc) {
        ImGui_Text(ctx, "Behaviour");
        ImGui_SameLine(ctx, nullptr, nullptr);
        ImGui_SetNextItemWidth(ctx, 140.0);
        const bool hold = c.behaviour == midicc::Behaviour::Hold;
        if (ImGui_BeginCombo(ctx, "##cc_x_beh", hold ? "Hold" : "Toggle", nullptr)) {
            bool sT = !hold, sH = hold;
            if (ImGui_Selectable(ctx, "Toggle##cc_x_t", &sT, nullptr, nullptr, nullptr)) { c.behaviour = midicc::Behaviour::Toggle; dirty = true; }
            if (ImGui_Selectable(ctx, "Hold##cc_x_h", &sH, nullptr, nullptr, nullptr)) { c.behaviour = midicc::Behaviour::Hold; dirty = true; }
            ImGui_EndCombo(ctx);
        }
        tip_(ctx, hold ? "127 while held, 0 on release." : "0 and 127 in turn.");
    }
    ImGui_Text(ctx, "Name");
    ImGui_SameLine(ctx, nullptr, nullptr);
    {
        char buf[16];
        snprintf(buf, 9, "%s", c.name.c_str());
        int flags = 0;
        ImGui_PushItemWidth(ctx, 120);
        if (ImGui_InputText(ctx, "##cc_x_name", buf, 9, &flags, nullptr)) { c.name = buf; dirty = true; }
        ImGui_PopItemWidth(ctx);
        tip_(ctx, "8 characters, for Settings. Empty = the CC number.");
    }
    if (!enc && midicc::keyHasLamp(s_index)) {
        ImGui_Spacing(ctx);
        dirty |= reasixty_drawLedLook(ctx, c.led, "ccx");
        ImGui_TextDisabled(ctx, "Active = value on, Inactive = value off.");
    } else if (!enc) {
        ImGui_TextDisabled(ctx, "LED: this key has no lamp on the UF8.");
    }
    if (!enc && !midicc::assigned(c) && c.name.empty() && c.led == ledlook::fromColour(0xFF0000))
        L.keys.erase(s_index);
    return dirty;
}

// The editor of the selected control.
bool drawControlEditor_(ImGui_Context* ctx, midicc::Layer& L, int page)
{
    if (s_kind == kSelKey || s_kind == kSelEncoder) return drawExtraEditor_(ctx, L);
    bool dirty = false;
    const Kind k = static_cast<Kind>(s_kind);
    if (s_index >= midicc::countOf(k)) s_index = 0;
    midicc::Control& c = L.pages[page].at(k, s_index);

    char hdr[96];
    snprintf(hdr, sizeof(hdr), "%s %d  -  page %s%s%s", kindName_(k), s_index + 1, midicc::pageKey(page),
             L.pages[page].name.empty() ? "" : " ", L.pages[page].name.c_str());
    ImGui_Text(ctx, hdr);

    dirty |= channelRow_(ctx, c, L.defaultChannel);

    ImGui_Text(ctx, k == Kind::SendPlugin ? "Program" : "CC number");
    ImGui_SameLine(ctx, nullptr, nullptr);
    ImGui_SetNextItemWidth(ctx, 110.0);
    {
        int n = c.number;
        if (ImGui_InputInt(ctx, "##cc_ed_num", &n, nullptr, nullptr, nullptr)) {
            c.number = std::clamp(n, -1, 127);
            if (c.number > 31) c.hiRes = false;
            dirty = true;
        }
        tip_(ctx, "0 to 127. -1 = this control does nothing.");
    }

    if (k == Kind::Fader) {
        if (c.number >= 0 && c.number <= 31) {
            bool hr = c.hiRes;
            char lbl[80];
            snprintf(lbl, sizeof(lbl), "14 bit (CC %d and CC %d)##cc_ed_hr",
                          c.number, c.number + 32);
            if (ImGui_Checkbox(ctx, lbl, &hr)) { c.hiRes = hr; dirty = true; }
            tip_(ctx, "16384 steps instead of 128. The receiver has to read the pair.");
        } else {
            ImGui_TextDisabled(ctx, "14 bit: only CC 0 to 31");
        }
    }

    if (midicc::isKey(k)) {
        ImGui_Text(ctx, "Behaviour");
        ImGui_SameLine(ctx, nullptr, nullptr);
        ImGui_SetNextItemWidth(ctx, 140.0);
        const bool hold = c.behaviour == midicc::Behaviour::Hold;
        if (ImGui_BeginCombo(ctx, "##cc_ed_beh", hold ? "Hold" : "Toggle", nullptr)) {
            bool sT = !hold, sH = hold;
            if (ImGui_Selectable(ctx, "Toggle##cc_beh_t", &sT, nullptr, nullptr, nullptr)) {
                c.behaviour = midicc::Behaviour::Toggle;
                dirty = true;
            }
            if (ImGui_Selectable(ctx, "Hold##cc_beh_h", &sH, nullptr, nullptr, nullptr)) {
                c.behaviour = midicc::Behaviour::Hold;
                dirty = true;
            }
            ImGui_EndCombo(ctx);
        }
        tip_(ctx, hold ? "127 while held, 0 on release." : "0 and 127 in turn.");
    }

    ImGui_Text(ctx, "Name");
    ImGui_SameLine(ctx, nullptr, nullptr);
    {
        char buf[16];
        snprintf(buf, 9, "%s", c.name.c_str());   // the field holds 8
        int flags = 0;
        ImGui_PushItemWidth(ctx, 120);
        if (ImGui_InputText(ctx, "##cc_ed_name", buf, 9, &flags, nullptr)) {
            c.name = buf;
            dirty = true;
        }
        ImGui_PopItemWidth(ctx);
        tip_(ctx, "8 characters, as the UF8 shows them. Empty = the CC number.");
    }

    // Only where the UF8 has a lamp for it (midicc::hasColour).
    if (!midicc::hasColour(k)) {
        ImGui_TextDisabled(ctx, "LED: no lamp on the UF8 for this control.");
        return dirty;
    }
    ImGui_Spacing(ctx);
    dirty |= reasixty_drawLedLook(ctx, c.led, "ccp");
    ImGui_TextDisabled(ctx, k == Kind::Fader ? "Shown on SEL and the colour bar (Active colour)."
                          : k == Kind::VPot ? "Shown on SEL and the colour bar while FLIP puts the V-Pot on the fader."
                                            : "Active = value on, Inactive = value off.");
    return dirty;
}

}  // namespace

// The UF8 picture of a CC layer (SettingsScreen.cpp, drawUf8CcVirtual) picks the
// control the editor below shows: kind 0..5 = midicc::Kind on the page, 100 = one
// of the other keys (index = UF8 id), 101 = the channel encoder.
void reasixty_ccSelect(int kind, int index) { s_kind = kind; s_index = index; }

// The REAPER layer's LED block (SettingsScreen.cpp, drawLedRow) for a LedLook:
// Active and Inactive, each a colour from the SSL palette (click the swatch)
// and Off / Dim / Bright; then "Show LED even when no action is assigned".
bool reasixty_drawLedLook(ImGui_Context* ctx, ledlook::Look& l, const char* tag)
{
    bool dirty = false;
    auto row = [&](const char* label, std::uint32_t& rgb, int& bri, const char* part) {
        // From where the row starts (a group beside the UF8 starts far right):
        // SetCursorPosX counts from the window's edge, not the group's.
        const double x0 = ImGui_GetCursorPosX(ctx);
        ImGui_AlignTextToFramePadding(ctx);
        ImGui_Text(ctx, label);
        ImGui_SameLine(ctx, nullptr, nullptr);
        ImGui_SetCursorPosX(ctx, x0 + 80.0);
        char btnId[48], popId[48];
        snprintf(btnId, sizeof(btnId), "##ledcur_%s_%s", tag, part);
        snprintf(popId, sizeof(popId), "ledpal_%s_%s", tag, part);
        int flags = 0;
        double bw = 56.0, bh = 22.0;
        if (ImGui_ColorButton(ctx, btnId, static_cast<int>((rgb << 8) | 0xFF), &flags, &bw, &bh))
            ImGui_OpenPopup(ctx, popId, nullptr);
        if (ImGui_BeginPopup(ctx, popId, nullptr)) {
            int n = 0;
            const uf8::PaletteRgb* pal = uf8::selPaletteRgb(&n);
            for (int i = 0; i < n; ++i) {
                const std::uint32_t c = (std::uint32_t(pal[i].r) << 16) | (std::uint32_t(pal[i].g) << 8) | pal[i].b;
                char sw[48];
                snprintf(sw, sizeof(sw), "##ledsw_%s_%s_%d", tag, part, i);
                int f = 0;
                double w = 26.0, h = 26.0;
                if (ImGui_ColorButton(ctx, sw, static_cast<int>((c << 8) | 0xFF), &f, &w, &h)) {
                    rgb = c;
                    dirty = true;
                    ImGui_CloseCurrentPopup(ctx);
                }
                if ((i % 5) != 4 && i != n - 1) ImGui_SameLine(ctx, nullptr, nullptr);
            }
            ImGui_EndPopup(ctx);
        }
        ImGui_SameLine(ctx, nullptr, nullptr);
        ImGui_Text(ctx, "  ");
        static const char* const names[3] = {"Off", "Dim", "Bright"};
        for (int i = 0; i < 3; ++i) {
            ImGui_SameLine(ctx, nullptr, nullptr);
            char id[48];
            snprintf(id, sizeof(id), "%s##ledb_%s_%s_%d", names[i], tag, part, i);
            if (ImGui_RadioButton(ctx, id, bri == i)) { bri = i; dirty = true; }
        }
    };
    ImGui_Text(ctx, "LED");
    row("Active", l.activeRgb, l.activeBri, "a");
    row("Inactive", l.inactiveRgb, l.inactiveBri, "i");
    char cb[64];
    snprintf(cb, sizeof(cb), "Show LED even when no action is assigned##ledforce_%s", tag);
    bool en = l.showWhenEmpty;
    if (ImGui_Checkbox(ctx, cb, &en)) { l.showWhenEmpty = en; dirty = true; }
    return dirty;
}
bool reasixty_ccIsSelected(int kind, int index) { return s_kind == kind && s_index == index; }

// The row above the UF8 schematic: which layer is edited, and what it drives.
// Returns true when the layer is a MIDI CC layer (the caller then draws the CC
// editor instead of the schematic).
bool drawUf8LayerKindRow(ImGui_Context* ctx, int layer)
{
    // All three layers, always, each with its type; the one the UF8 is on (and
    // this page edits) in blue. A click takes the UF8 there, like its LAYER key.
    // Before, the keys were only here on CC and UAD layers and lived in the
    // schematic on REAPER layers, so they seemed to vanish (user, 03.11.2026).
    ImGui_Text(ctx, "Layers");
    for (int l = 0; l < 3; ++l) {
        ImGui_SameLine(ctx, nullptr, nullptr);
        const bnd::LayerKind k = bnd::layerKind(l);
        char id[48];
        snprintf(id, sizeof(id), " Layer %d: %s ##lk_go_%d", l + 1,
                 k == bnd::LayerKind::MidiCc ? "MIDI CC"
                 : k == bnd::LayerKind::Uad  ? "UAD"
                                             : "REAPER", l);
        int pushed = 0;
        if (l == layer) { ImGui_PushStyleColor(ctx, ImGui_Col_Button, 0x4477CCFF); ++pushed; }
        if (ImGui_Button(ctx, id, nullptr, nullptr)) {
            const bnd::ButtonId b = l == 0 ? bnd::ButtonId::Layer1
                                  : l == 1 ? bnd::ButtonId::Layer2 : bnd::ButtonId::Layer3;
            bnd::dispatch(b, /*pressed*/ true);
            bnd::dispatch(b, /*pressed*/ false);
        }
        if (pushed) ImGui_PopStyleColor(ctx, &pushed);
    }
    ImGui_SameLine(ctx, nullptr, nullptr);
    ImGui_TextDisabled(ctx, "   blue = the layer the UF8 is on and this page edits");

    char hdr[48];
    snprintf(hdr, sizeof(hdr), "Editing: Layer %d", layer + 1);
    ImGui_Text(ctx, hdr);
    ImGui_SameLine(ctx, nullptr, nullptr);
    ImGui_Text(ctx, "   Layer type");
    ImGui_SameLine(ctx, nullptr, nullptr);
    ImGui_SetNextItemWidth(ctx, 140.0);
    const bnd::LayerKind kind = bnd::layerKind(layer);
    const char* kindName = kind == bnd::LayerKind::MidiCc ? "MIDI CC"
                         : kind == bnd::LayerKind::Uad    ? "UAD Console"
                                                          : "REAPER";
    if (ImGui_BeginCombo(ctx, "##uf8_layer_kind", kindName, nullptr)) {
        bool sR = kind == bnd::LayerKind::Reaper, sC = kind == bnd::LayerKind::MidiCc,
             sU = kind == bnd::LayerKind::Uad;
        if (ImGui_Selectable(ctx, "REAPER##lk_reaper", &sR, nullptr, nullptr, nullptr))
            bnd::setLayerKind(layer, bnd::LayerKind::Reaper);
        if (ImGui_Selectable(ctx, "MIDI CC##lk_cc", &sC, nullptr, nullptr, nullptr))
            bnd::setLayerKind(layer, bnd::LayerKind::MidiCc);
        if (ImGui_Selectable(ctx, "UAD Console##lk_uad", &sU, nullptr, nullptr, nullptr))
            bnd::setLayerKind(layer, bnd::LayerKind::Uad);
        ImGui_EndCombo(ctx);
    }
    tip_(ctx, "MIDI CC: faders, V-Pots and keys send MIDI on this layer.\n"
              "UAD Console: the UF8 is a mixer for UAD Console (Universal Audio).");
    return bnd::layerKind(layer) != bnd::LayerKind::Reaper;
}

// The UAD Console layer's block, in place of the schematic (docs/uad-console-plan.md).
// Read only in this version: the connection, and what each control does. The
// Console-like editor comes later (plan, Baustein G).
void drawUf8UadLayer(ImGui_Context* ctx, int layer)
{
    (void)layer;
    const uadside::Status st = reasixty_uadStatus();
    ImGui_Spacing(ctx);
    char line[160];
    if (!st.running)
        snprintf(line, sizeof(line), "Connection: starting...");
    else if (!st.connected)
        snprintf(line, sizeof(line), "Connection: UAD Console not found (is it running on this computer?)");
    else
        snprintf(line, sizeof(line), "Connection: UAD Console, %s, %s, %d channels",
                 st.device.empty() ? "no interface" : st.device.c_str(),
                 st.online ? "online" : "OFFLINE", st.channels);
    ImGui_Text(ctx, line);
    tip_(ctx, "Rea-Sixty talks to UAD Console's mixer engine on this computer (127.0.0.1:4710).\n"
              "Nothing is written while the interface is offline.");
    ImGui_TextDisabled(ctx, "Green keys work on this layer, grey ones do nothing here. "
                            "Hover or click for what they do.");
    ImGui_Spacing(ctx);
    // The picture of the UF8 and the Console follows (SettingsScreen.cpp,
    // drawUf8UadVector).
}

// The CC layer's page in Settings, top to bottom (redesign 03.11.2026, after
// SSL 360, Novation Components, Electra One, Grid Editor):
//   drawUf8CcHeader     preset row, mode (Edit / Test), connections (folded,
//                       with a one-line summary), page tabs with names
//   drawUf8CcVirtual    the UF8 picture (SettingsScreen.cpp): the only way to
//                       pick a control
//   drawUf8CcInspector  the picked control, beside the picture when there is room
namespace { int s_editPage = 0; int s_lastLive = -1; bool s_test = false; }

void reasixty_ccSetEditPage(int p) { s_editPage = std::clamp(p, 0, midicc::kPages - 1); }

// The page Settings shows and the mode, before the picture is drawn: the page
// the UF8 is on, unless a tab or a page key in the picture picked another one.
int reasixty_ccEditPageFor(int livePage, bool* test)
{
    if (livePage != s_lastLive) { s_editPage = livePage; s_lastLive = livePage; }
    s_editPage = std::clamp(s_editPage, 0, midicc::kPages - 1);
    if (test) *test = s_test;
    return s_editPage;
}

int drawUf8CcHeader(ImGui_Context* ctx, int layer, int livePage, bool* test)
{
    midicc::Layer L = bnd::ccLayer(layer);
    bool dirty = false;
    reasixty_ccEditPageFor(livePage, nullptr);

    dirty |= drawPresets_(ctx, layer, L);

    ImGui_Text(ctx, "Mode");
    ImGui_SameLine(ctx, nullptr, nullptr);
    int mode = s_test ? 1 : 0;
    if (ImGui_RadioButtonEx(ctx, "Edit: click a control to set it up##cc_mode_e", &mode, 0)) s_test = false;
    ImGui_SameLine(ctx, nullptr, nullptr);
    if (ImGui_RadioButtonEx(ctx, "Test: clicks and drags send MIDI##cc_mode_t", &mode, 1)) s_test = true;
    *test = s_test;
    if (s_test && layer != bnd::ccLayerActiveIndex()) {
        ImGui_PushStyleColor(ctx, ImGui_Col_Text, 0xE0A040FF);
        ImGui_Text(ctx, "This layer is not the active one, so nothing is sent.");
        ImGui_PopStyleColor(ctx, nullptr);
        char b[48];
        snprintf(b, sizeof(b), "Switch to layer %d##cc_virt_go", layer + 1);
        if (ImGui_Button(ctx, b, nullptr, nullptr)) {
            const bnd::ButtonId ids[3] = {bnd::ButtonId::Layer1, bnd::ButtonId::Layer2, bnd::ButtonId::Layer3};
            bnd::dispatch(ids[layer], true);
            bnd::dispatch(ids[layer], false);
        }
    }

    // Connections: set once, folded away, summarised in the header.
    {
        std::string outs;
        for (const auto& o : L.outputs) outs += (outs.empty() ? "" : ", ") + (o.empty() ? std::string("REAPER") : o);
        char hdr[512];
        snprintf(hdr, sizeof(hdr), "Connections:  to %s  |  values from %s%s  |  default channel %d###cc_conn",
                 outs.empty() ? "nothing" : outs.c_str(),
                 L.inputDevice.empty() ? "no device" : L.inputDevice.c_str(),
                 L.followItems ? " + REAPER" : "", L.defaultChannel);
        if (ImGui_CollapsingHeader(ctx, hdr, nullptr, nullptr)) {
            dirty |= drawConnections_(ctx, layer, L);
            ImGui_Spacing(ctx);
        }
    }

    // Page tabs = the soft-key bank keys; the one the UF8 is on is marked.
    ImGui_Text(ctx, "Page");
    for (int p = 0; p < midicc::kPages; ++p) {
        ImGui_SameLine(ctx, nullptr, nullptr);
        std::string lbl = midicc::pageKey(p);
        if (!L.pages[p].name.empty()) lbl += " " + L.pages[p].name;
        if (p == livePage && layer == bnd::ccLayerActiveIndex()) lbl += " (UF8)";
        lbl += "##cc_tab_" + std::to_string(p);
        int pushed = 0;
        if (p == s_editPage) { ImGui_PushStyleColor(ctx, ImGui_Col_Button, 0x4477CCFF); ++pushed; }
        if (ImGui_Button(ctx, lbl.c_str(), nullptr, nullptr)) s_editPage = p;
        if (pushed) ImGui_PopStyleColor(ctx, &pushed);
    }
    ImGui_SameLine(ctx, nullptr, nullptr);
    ImGui_Text(ctx, "  Name");
    ImGui_SameLine(ctx, nullptr, nullptr);
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%s", L.pages[s_editPage].name.c_str());
        int flags = 0;
        ImGui_PushItemWidth(ctx, 140);
        if (ImGui_InputText(ctx, "##cc_page_name", buf, 17, &flags, nullptr)) { L.pages[s_editPage].name = buf; dirty = true; }
        ImGui_PopItemWidth(ctx);
        tip_(ctx, "A name for this page, e.g. 'Strings'. Settings only.");
    }
    ImGui_TextDisabled(ctx, "Coloured: sends a CC.  Dim: no CC, normal job.");
    ImGui_TextDisabled(ctx, "Grey: fixed (layers, 360, pages).");
    ImGui_TextDisabled(ctx, "Per page: soft-keys, V-Pots, SEND/PLUG-IN, faders, feet.");
    ImGui_TextDisabled(ctx, "All pages: every other key and the encoder.");
    if (dirty) bnd::setCcLayer(layer, L);
    return s_editPage;
}

void drawUf8CcInspector(ImGui_Context* ctx, int layer, int page)
{
    midicc::Layer L = bnd::ccLayer(layer);
    if (drawControlEditor_(ctx, L, page)) bnd::setCcLayer(layer, L);
}
