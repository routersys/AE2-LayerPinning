#pragma once

#include <windows.h>

#include "plugin2.h"

namespace lp {

void DestroyOverlay();
void ReleaseCaches();

void OnPinMenu(EDIT_SECTION* edit);
void OnUnpinMenu(EDIT_SECTION* edit);
void OnChangeScene(EDIT_SECTION* edit);
void OnAnyEvent(void* param);

}
