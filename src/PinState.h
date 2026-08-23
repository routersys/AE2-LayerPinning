#pragma once

namespace lp {

int PinCount();
int SceneId();
void SelectScene(int sceneId);
bool ApplyPin(int layer, bool wantPin);
bool IsMenuEligible(int layer);

}
