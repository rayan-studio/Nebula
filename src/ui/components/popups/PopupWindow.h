#pragma once
#include <Windows.h>
#include <string>

// Creates a movable, non-resizable popup window with Nebula styling.
// Returns the HWND for further customization if needed.
HWND ShowPopupWindow(HWND owner, int x, int y, int width, int height,
                     const std::wstring &title, const std::wstring &message = L"");
HWND GetActivePopupWindow();
bool CloseActivePopupWindow();
bool PopupHitTestClose(POINT screenPt);
bool PopupHitTestHeader(POINT screenPt);
void PopupSetHoverClose(bool hovered);
void PopupStartDrag(POINT screenPt);
void PopupDragTo(POINT screenPt);
void PopupEndDrag();
bool PopupIsDragging();
