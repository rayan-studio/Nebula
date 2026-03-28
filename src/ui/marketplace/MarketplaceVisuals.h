#pragma once

#include <windows.h>
#include <d2d1.h>

#include <string>

D2D1_COLOR_F MarketplaceCategoryColor(const std::wstring& category);
std::wstring MarketplaceInitials(const std::wstring& name);

void MarketplaceEnsureAvatarAsync(const std::wstring& url, HWND hwnd);
ID2D1Bitmap* MarketplaceLoadAvatarBitmap(ID2D1RenderTarget* ctx, const std::wstring& url);
