#pragma once

namespace lp {

void SetRefreshWaker(void (*proc)());

void RequestRefresh();
void DeferRefreshRequest();

bool TakeRefreshRequest();

}
