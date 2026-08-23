#pragma once

#include <windows.h>

namespace lp {

struct PixelCache {
    HDC dc = nullptr; HBITMAP bmp = nullptr; HGDIOBJ old = nullptr;
    int w = 0, h = 0;
    bool Ensure(HDC ref, int cw, int ch) {
        if (cw <= 0 || ch <= 0) return false;
        if (dc && w == cw && h == ch) return true;
        Release();
        dc = CreateCompatibleDC(ref);
        if (!dc) return false;
        bmp = CreateCompatibleBitmap(ref, cw, ch);
        if (!bmp) { DeleteDC(dc); dc = nullptr; return false; }
        old = SelectObject(dc, bmp);
        w = cw; h = ch;
        return true;
    }
    void Release() {
        if (dc) { if (old) SelectObject(dc, old); DeleteDC(dc); dc = nullptr; }
        if (bmp) { DeleteObject(bmp); bmp = nullptr; }
        old = nullptr; w = h = 0;
    }
};

}
