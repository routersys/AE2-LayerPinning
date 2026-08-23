#pragma once

namespace lp {

bool DrawHookSupported();
bool DrawHookActive();

void KeepDrawHookArmed();
void ReleaseDrawHook();

void SetRemapBase(int value);
void ClearRemapBase();

}
