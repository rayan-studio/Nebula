#pragma once
#include <Windows.h>
#include <string>
#include <vector>

// Show a simple custom popup menu at screen position. Commands will be posted
// to parent via WM_COMMAND with id = baseId + index. baseId should be > 0.
void ShowCustomPopup(HWND parent, const std::vector<std::wstring>& items, POINT screenPos, int baseId = 3000);
