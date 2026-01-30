#include "core/window/Window.h"
#include <d2d1.h>
#include <dwrite.h>

void Window::DrawNewProjectOverlay(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, const RECT &clientRect)
{
    newProjOverlay_.Draw(*this, ctx, dwrite, clientRect);
}

bool Window::HandleNewProjectMouseDown(HWND hwnd, POINT pt)
{
    return newProjOverlay_.HandleMouseDown(*this, hwnd, pt);
}

bool Window::HandleNewProjectMouseUp(HWND hwnd, POINT pt)
{
    return newProjOverlay_.HandleMouseUp(*this, hwnd, pt);
}

bool Window::HandleNewProjectMouseMove(HWND hwnd, POINT pt)
{
    return newProjOverlay_.HandleMouseMove(*this, hwnd, pt);
}

bool Window::HandleNewProjectChar(wchar_t ch)
{
    return newProjOverlay_.HandleChar(*this, ch);
}

bool Window::HandleNewProjectKeyDown(WPARAM key)
{
    return newProjOverlay_.HandleKeyDown(*this, key);
}

void Window::ShowNewProjectOverlay()
{
    newProjectVisible_ = true;
    newProjPage_ = NewProjectPage::Home;
    hoveredButton_ = Hovered_None;
    newProjNameFocused_ = false;
    newProjLocationFocused_ = false;
    newProjNameInput_.SetFocused(false);
    newProjLocationInput_.SetFocused(false);
    newProjNameInput_.SetPlaceholder(L"Nom du projet");
    newProjLocationInput_.SetPlaceholder(L"Rechercher recent (Alt+E)");
    newProjLocationInput_.SetIcon(L"\uE721");
    newProjLocationInput_.SetIconFont(L"Segoe Fluent Icons");
    newProjNameInput_.SetText(L"");
    newProjLocationInput_.SetText(L"");
    newProjTemplateIndex_ = 0;
    newProjTemplateHover_ = -1;
    LoadRecentProjects();
}

void Window::HideNewProjectOverlay()
{
    newProjectVisible_ = false;
    newProjPage_ = NewProjectPage::Home;
    newProjNameFocused_ = false;
    newProjLocationFocused_ = false;
    newProjNameInput_.SetFocused(false);
    newProjLocationInput_.SetFocused(false);
}
