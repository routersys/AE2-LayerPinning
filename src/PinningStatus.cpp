#include "PinningStatus.h"

#include "LayerGeometry.h"
#include "OverlayWindow.h"
#include "PinState.h"
#include "ScrollAnchor.h"

namespace lp {

bool PinningActive() {
    return PinCount() > 0 && GeometryValid() && HasStartPointer();
}

bool OverlayShouldShow(int v) {
    return PinningActive() && Overlay().Image().Valid() && v > 0;
}

}
