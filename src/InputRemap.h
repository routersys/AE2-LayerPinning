#pragma once

#include <windows.h>

namespace lp {

bool Remapping();

LRESULT HandleMouse(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool* handled);
LRESULT HandleContextMenu(HWND hwnd, WPARAM wp, LPARAM lp, bool* handled);

void ExpireMenuRemap();
void EndMenuRemap();

bool CoverHeld();
void ExpireCover();

int ApplyScrollHold(int v);
void ReassertScrollHold();
void CancelScrollHold();

}
