#pragma once

#include <windows.h>

#include "HostContext.h"

namespace lp {

bool GeometryValid();
int ContentRight();
int DisplayLayerNum();
void SetDisplayLayerNum(int layerNum);

void ProbeGeometry(EDIT_SECTION* edit);
void RequestProbeGeometry();

bool LayerAreaRect(RECT* out);
bool PinnedStripRect(RECT* out);
RECT LowerRegionRect(const RECT& area, const RECT& strip);
bool LowerRegionRect(RECT* out);

bool PointInPinnedStrip(POINT ptClient);
bool CursorInPinnedStrip(LPARAM lp);
int LayerUnderCursor(int screenX, int screenY);

}
