#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>

#include "ui/components/input/TextInput.h"

class SettingsTabView
{
public:
    SettingsTabView();
    void UpdateLayout(HWND hwnd, float left, float top, float right, float bottom);
    void Draw(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd);
    void OnMouseMove(HWND hwnd, POINT clientPoint);
    void OnLeftButtonDown(HWND hwnd, POINT clientPoint);
    void OnLeftButtonUp(HWND hwnd);
    bool OnChar(wchar_t ch);
    bool OnKeyDown(WPARAM key);
    bool IsPointInView(POINT clientPoint) const;

private:
    D2D1_RECT_F bounds_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F rowRect_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F toggleRect_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F themeRowRect_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F themeToggleRect_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F tamponSectionRect_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F tamponInputRect_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F githubSectionRect_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F githubSignInRect_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F githubDisconnectRect_ = D2D1::RectF(0, 0, 0, 0);

    bool rowHovered_ = false;
    bool themeRowHovered_ = false;
    bool githubSignInHovered_ = false;
    bool githubDisconnectHovered_ = false;

    std::wstring githubStatusMessage_;
    TextInput tamponInput_;

    void RefreshInputTheme();
    void BeginGitHubSignIn();
    void ClearGitHubToken();
};
