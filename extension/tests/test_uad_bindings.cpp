// The UAD Console layer in bindings.json (docs/uad-console-plan.md, Baustein E): the
// kind survives a save and a reload, uadLayerActive follows the layer the surface
// is on, anyUadLayer says whether the engine connection is needed, the keys a UAD
// layer passes on resolve on Layer 1, and the CC layer is unaffected. Runs the real
// bindings engine on a temp directory, like test_midi_cc_bindings.

#include "Bindings.h"
#include "BindingsHost.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace bnd = uf8::bindings;

static int g_fail = 0;
#define EXPECT(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static std::string g_dir;

static std::string fileText()
{
    std::ifstream f(g_dir + "/bindings.json");
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

int main()
{
    char tmpl[] = "/tmp/rs_uadXXXXXX";
    g_dir = mkdtemp(tmpl);
    bnd::Host h;
    h.configDir  = [] { return g_dir; };
    h.configFile = [] { return std::string("bindings.json"); };
    bnd::setHost(std::move(h));
    bnd::load();

    // Factory: no UAD layer, nothing written, no connection wanted.
    EXPECT(!bnd::uadLayerActive() && bnd::uadLayerActiveIndex() == -1 && !bnd::anyUadLayer());
    EXPECT(fileText().find("\"uad\"") == std::string::npos);

    // Layer 3 becomes the UAD Console layer.
    bnd::setLayerKind(2, bnd::LayerKind::Uad);
    EXPECT(bnd::layerKind(2) == bnd::LayerKind::Uad);
    EXPECT(bnd::anyUadLayer() && !bnd::uadLayerActive());
    EXPECT(fileText().find("\"kind\": \"uad\"") != std::string::npos);
    bnd::setActiveLayer(2);
    EXPECT(bnd::uadLayerActive() && bnd::uadLayerActiveIndex() == 2 && !bnd::ccLayerActive());
    // the keys it passes on (LAYER 1..3, 360) resolve on Layer 1
    EXPECT(bnd::applyLayerOption(2, bnd::ButtonId::Layer1) == 0);
    bnd::setActiveLayer(0);
    EXPECT(!bnd::uadLayerActive() && bnd::anyUadLayer());
    EXPECT(bnd::applyLayerOption(0, bnd::ButtonId::Layer1) == 0);

    // Reload from disk.
    bnd::setActiveLayer(2);
    bnd::load();
    bnd::refreshCcLayerActive();
    EXPECT(bnd::layerKind(2) == bnd::LayerKind::Uad && bnd::layerKind(0) == bnd::LayerKind::Reaper);
    EXPECT(bnd::uadLayerActive() && bnd::uadLayerActiveIndex() == 2 && bnd::anyUadLayer());

    // A CC layer next to it: each its own.
    bnd::setLayerKind(1, bnd::LayerKind::MidiCc);
    bnd::setActiveLayer(1);
    EXPECT(bnd::ccLayerActive() && !bnd::uadLayerActive() && bnd::anyUadLayer());
    bnd::load();
    bnd::refreshCcLayerActive();
    EXPECT(bnd::layerKind(1) == bnd::LayerKind::MidiCc && bnd::layerKind(2) == bnd::LayerKind::Uad);

    // Back to Reaper: no connection wanted any more.
    bnd::setLayerKind(2, bnd::LayerKind::Reaper);
    EXPECT(!bnd::anyUadLayer());
    bnd::load();
    bnd::refreshCcLayerActive();
    EXPECT(bnd::layerKind(2) == bnd::LayerKind::Reaper && !bnd::anyUadLayer());
    EXPECT(fileText().find("\"kind\": \"uad\"") == std::string::npos);

    // A kind this version does not know loads as Reaper (what an older Rea-Sixty
    // does with "uad").
    {
        std::string t = fileText();
        const std::string key = "\"kind\": \"midi_cc\"";
        const std::size_t at = t.find(key);
        EXPECT(at != std::string::npos);
        if (at != std::string::npos) {
            t.replace(at, key.size(), "\"kind\": \"from_the_future\"");
            std::ofstream(g_dir + "/bindings.json") << t;
            bnd::load();
            bnd::refreshCcLayerActive();
            EXPECT(bnd::layerKind(1) == bnd::LayerKind::Reaper);
        }
    }

    std::system(("rm -rf " + g_dir).c_str());
    if (g_fail) std::printf("%d failure(s)\n", g_fail);
    else        std::printf("uad_bindings: all passed\n");
    return g_fail ? 1 : 0;
}
