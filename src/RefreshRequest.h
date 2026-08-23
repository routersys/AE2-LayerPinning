#pragma once

namespace lp {

void SetRefreshWaker(void (*proc)());

void RequestRefresh();
void RequestUrgentRefresh();
void DeferRefreshRequest();

bool TakeRefreshRequest();
bool TakeUrgentRefreshRequest();

}
