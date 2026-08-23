#include "PinState.h"

#include <unordered_map>

#include "HostContext.h"

namespace lp {

static std::unordered_map<int, int> g_pinByScene;
static int g_sceneId  = 0;
static int g_pinCount = 0;

int PinCount() { return g_pinCount; }
int SceneId() { return g_sceneId; }

void SelectScene(int sceneId) {
    g_sceneId = sceneId;
    auto it = g_pinByScene.find(g_sceneId);
    g_pinCount = (it == g_pinByScene.end()) ? 0 : it->second;
}

bool ApplyPin(int layer, bool wantPin) {
    if (wantPin) {
        if (layer != g_pinCount) return false;
        g_pinCount = layer + 1;
    } else {
        if (layer >= g_pinCount) return false;
        g_pinCount = layer;
    }
    g_pinByScene[g_sceneId] = g_pinCount;
    return true;
}

bool IsMenuEligible(int layer) {
    if (layer < 0 || !Edit()) return false;
    if (layer < g_pinCount) return true;
    if (layer != g_pinCount) return false;
    return layer + 1 < EditInfo().display_layer_num;
}

}
