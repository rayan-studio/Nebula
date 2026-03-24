#pragma once
#include <windows.h>
#include <string>
#include <vector>

// Shows a D2D-rendered OS popup window (WS_POPUP + CS_DROPSHADOW) for context menus.
// The popup floats above all windows and can extend beyond the main window bounds.
// When the user selects item i, WM_COMMAND(wParam = baseId + i) is posted to parent.
// The popup closes on: item selection, right-click, Escape, or CloseCustomPopup().

void ShowCustomPopup(HWND parent,
                     const std::vector<std::wstring> &items,
                     POINT screenPos,              // screen coordinates
                     int baseId,
                     const std::vector<bool>         &separators = {},
                     const std::vector<std::wstring> &shortcuts  = {},
                     const std::vector<bool>         &enabled    = {});

void CloseCustomPopup();
bool IsCustomPopupVisible();
bool IsPointInCustomPopup(POINT screenPt);   // screenPt in screen coordinates
