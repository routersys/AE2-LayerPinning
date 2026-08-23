#pragma once

#include <windows.h>
#include <vector>

#include "PixelCache.h"

namespace lp {

inline constexpr UINT kTickIntervalMs = 33;

class StripImage {
public:
    bool Refresh(HDC hostDc, const RECT& strip);
    void Paint(HDC dc, const RECT& rc);
    void Release();

    bool Valid() const { return valid_; }
    bool Changed() const { return changed_; }
    int IdleStreak() const { return idleStreak_; }
    void ResetIdleStreak() { idleStreak_ = 0; }
    void Invalidate() { valid_ = false; }

private:
    unsigned long long Hash();

    PixelCache cache_;
    std::vector<BYTE> hashBuf_;
    unsigned long long hash_ = 0;
    int idleStreak_ = 0;
    bool valid_ = false;
    bool changed_ = false;
};

class OverlayWindow {
public:
    void Create(HWND parent);
    void StartTicking(void (*proc)());
    void Destroy();

    HWND Hwnd() const { return hwnd_; }
    bool Visible() const;
    void Hide();
    void ShowRepainted();
    void RepaintIfVisible();
    void UpdatePlacement(bool shouldShow);

    StripImage& Image() { return image_; }

private:
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp);

    HWND hwnd_ = nullptr;
    StripImage image_;
    void (*tick_)() = nullptr;
};

OverlayWindow& Overlay();

}
