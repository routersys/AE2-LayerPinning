#pragma once

#include <windows.h>

#include "PixelCache.h"

namespace lp {

class CoverWindow {
public:
    void Create(HWND parent);
    void Destroy();
    HWND Hwnd() const { return hwnd_; }

    bool Capture(HDC hostDc, const RECT& lower);
    void Place(const RECT& lower);
    void Restore(HDC hostDc, const RECT& lower);
    void Hide();
    void ReleaseCache();

    bool CoverLowerRegion();
    void UncoverLowerRegion();

private:
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp);

    HWND hwnd_ = nullptr;
    PixelCache cache_;
};

CoverWindow& Cover();

}
