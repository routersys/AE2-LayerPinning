#pragma once

namespace lp {

const wchar_t* MenuTextPin();
const wchar_t* MenuTextUnpin();

void* InstallLayerMenuHook();
void UninstallLayerMenuHook();

int MenuTargetLayer();

}
