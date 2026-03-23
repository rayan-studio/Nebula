#include "DropdownMenu.h"

#include "helpers/window_helpers.h"
#include "core/explorer/Explorer.h"
#include "ui/theme/Theme.h"
#include "utils/logger/Logger.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace
{
std::vector<MenuItem> g_menuItems;
MenuDropdown g_activeDropdown = {-1, std::vector<std::wstring>(), std::vector<std::wstring>(), std::vector<wchar_t>(),
    std::vector<bool>(), std::vector<bool>(), D2D1::RectF(), -1, false, 0, std::vector<bool>()};
MenuDropdown g_subDropdown = {-1, std::vector<std::wstring>(), std::vector<std::wstring>(), std::vector<wchar_t>(),
    std::vector<bool>(), std::vector<bool>(), D2D1::RectF(), -1, false, 0, std::vector<bool>()};
std::unordered_map<std::string, ID2D1Bitmap *> g_contextIconCache;
ID2D1RenderTarget *g_contextIconCtx = nullptr;

void ClearContextIconCache()
{
    for (auto &pair : g_contextIconCache)
    {
        if (pair.second)
            pair.second->Release();
    }
    g_contextIconCache.clear();
}

std::string ContextMenuIconPathForLabel(const std::wstring &label)
{
    if (label.find(L"Ouvrir le dossier") != std::wstring::npos || label.find(L"Open Folder") != std::wstring::npos)
        return "assets\\ressource\\icons\\folder-open.svg";
    if (label.find(L"Ouvrir le fichier") != std::wstring::npos || label.find(L"Open File") != std::wstring::npos)
        return "assets\\ressource\\icons\\document.svg";
    if (label.find(L"Ouvrir dans l'explorateur") != std::wstring::npos || label.find(L"Open in Explorer") != std::wstring::npos)
        return "assets\\ressource\\icons\\folder-open.svg";
    if (label.find(L"Copier le chemin") != std::wstring::npos || label.find(L"Copy Path") != std::wstring::npos)
        return "assets\\ressource\\icons\\folder-link-open.svg";
    if (label.find(L"Ajouter") != std::wstring::npos || label.find(L"Add") != std::wstring::npos)
        return "assets\\ressource\\icons\\folder-open.svg";
    if (label.find(L"Nouveau fichier") != std::wstring::npos || label.find(L"New File") != std::wstring::npos)
        return "assets\\ressource\\icons\\document.svg";
    if (label.find(L"Nouveau dossier") != std::wstring::npos || label.find(L"New Folder") != std::wstring::npos)
        return "assets\\ressource\\icons\\folder.svg";
    if (label.find(L"Class Header") != std::wstring::npos || label.find(L"Header") != std::wstring::npos)
        return "assets\\ressource\\icons\\h.svg";
    if (label.find(L"Class Source") != std::wstring::npos || label.find(L"Source") != std::wstring::npos)
        return "assets\\ressource\\icons\\cpp.svg";
    if (label.find(L"Duplicate") != std::wstring::npos)
        return "assets\\ressource\\icons\\folder-template-open.svg";
    if (label.find(L"Rename") != std::wstring::npos || label.find(L"Renommer") != std::wstring::npos)
        return "assets\\ressource\\icons\\folder-tools-open.svg";
    if (label.find(L"Delete") != std::wstring::npos || label.find(L"Supprimer") != std::wstring::npos)
        return "assets\\ressource\\icons\\folder-trash.svg";
    return {};
}

std::string MenuDropdownIconPathForLabel(const std::wstring &label)
{
    if (label == L"New") return "assets/ressource/icons/document.svg";
    if (label == L"New Window") return "assets/ressource/icons/folder-desktop-open.svg";
    if (label == L"Open..." || label == L"Open Project") return "assets/ressource/icons/folder-open.svg";
    if (label == L"Open Recent") return "assets/ressource/icons/folder-log-open.svg";
    if (label == L"Close") return "assets/ressource/icons/folder-archive-open.svg";
    if (label == L"Undo" || label == L"Redo") return "assets/ressource/icons/folder-backup-open.svg";
    if (label == L"Cut" || label == L"Copy" || label == L"Paste" || label == L"Paste Without Formatting")
        return "assets/ressource/icons/folder-content-open.svg";
    if (label == L"Delete") return "assets/ressource/icons/folder-trash.svg";
    if (label == L"Select All") return "assets/ressource/icons/folder-keys-open.svg";
    if (label == L"Expand Selection") return "assets/ressource/icons/folder-link-open.svg";
    if (label == L"Shrink Selection") return "assets/ressource/icons/folder-backup-open.svg";
    if (label == L"Select Line") return "assets/ressource/icons/folder-content-open.svg";
    if (label == L"Find" || label == L"Find in Files") return "assets/ressource/icons/search.svg";
    if (label == L"Replace" || label == L"Replace in Files") return "assets/ressource/icons/folder-tools-open.svg";
    if (label == L"Toggle Comment" || label == L"Format Document") return "assets/ressource/icons/folder-tools-open.svg";
    if (label == L"New Terminal") return "assets/ressource/icons/folder-console-open.svg";
    if (label == L"Command Palette") return "assets/ressource/icons/folder-command-open.svg";
    if (label == L"Open View") return "assets/ressource/icons/folder-views-open.svg";
    if (label == L"Toggle Sidebar") return "assets/ressource/icons/folder-layout-open.svg";
    if (label == L"Show Extensions") return "assets/ressource/icons/folder-plugin-open.svg";
    if (label == L"Keyboard Shortcuts") return "assets/ressource/icons/folder-keys-open.svg";
    if (label == L"Go to File" || label == L"Go to Line" || label == L"Go to Symbol" || label == L"Go to Definition")
        return "assets/ressource/icons/folder-link-open.svg";
    if (label == L"Start Debugging" || label == L"Run" || label == L"Restart Debugging")
        return "assets/ressource/icons/playwright.svg";
    if (label == L"Stop") return "assets/ressource/icons/folder-stop-open.svg";
    if (label == L"Step Over" || label == L"Step Into") return "assets/ressource/icons/folder-debug-open.svg";
    if (label == L"Welcome" || label == L"Documentation" || label == L"About" || label == L"Release Notes" || label == L"Report Issue")
        return "assets/ressource/icons/folder-helper-open.svg";
    return {};
}

ID2D1Bitmap *GetContextMenuIconBitmap(ID2D1RenderTarget *ctx, const std::string &path, int pxSize, UINT dpi)
{
    if (!ctx || path.empty())
        return nullptr;

    if (g_contextIconCtx != ctx)
    {
        ClearContextIconCache();
        g_contextIconCtx = ctx;
    }

    std::string key = path + "|" + std::to_string(pxSize) + "|" + std::to_string(dpi);
    auto it = g_contextIconCache.find(key);
    if (it != g_contextIconCache.end())
        return it->second;

    ID2D1Bitmap *bmp = GetExplorerManager().LoadSvgIconPublic(ctx, path, pxSize, dpi);
    if (bmp)
        g_contextIconCache[key] = bmp;
    return bmp;
}

void RedrawMenuOwner(HWND hwnd, bool wholeWindow)
{
    if (!hwnd)
        return;

    if (wholeWindow)
    {
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    RECT tb = win32_titlebar_rect(hwnd);
    RedrawWindow(hwnd, &tb, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
}

bool DropdownHasShortcuts(const MenuDropdown &dd)
{
    for (const auto &shortcut : dd.shortcuts)
    {
        if (!shortcut.empty())
            return true;
    }
    return false;
}

void DrawDropdownPanel(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, MenuDropdown &dd)
{
    if (!ctx)
        return;

    const UI::Theme::Palette &palette = UI::Theme::GetPalette();
    const D2D1_COLOR_F dropdownBg = palette.inputBackground;
    const D2D1_COLOR_F dropdownBorder = palette.inputBorder;
    const D2D1_COLOR_F dropdownHover = palette.explorerToolbarHover;
    const D2D1_COLOR_F dropdownText = UI::Theme::PrimaryText();
    const D2D1_COLOR_F dropdownDisabled = UI::Theme::MutedText();
    const D2D1_COLOR_F dropdownSeparator = UI::Theme::ChromeBorder();

    D2D1_RECT_F r = dd.rect;
    r = D2D1::RectF(std::round(r.left), std::round(r.top), std::round(r.right), std::round(r.bottom));
    dd.rect = r;

    ID2D1SolidColorBrush *shadowBrush = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.14f), &shadowBrush);
    ID2D1SolidColorBrush *bgBrush = nullptr;
    ctx->CreateSolidColorBrush(dropdownBg, &bgBrush);
    ID2D1SolidColorBrush *borderBrush = nullptr;
    ctx->CreateSolidColorBrush(dropdownBorder, &borderBrush);
    ID2D1SolidColorBrush *hoverBrush = nullptr;
    D2D1_COLOR_F hoverColor = dropdownHover;
    hoverColor.a = (UI::Theme::GetMode() == UI::Theme::Mode::Light) ? 0.90f : 1.0f;
    ctx->CreateSolidColorBrush(hoverColor, &hoverBrush);
    ID2D1SolidColorBrush *textBrush = nullptr;
    ctx->CreateSolidColorBrush(dropdownText, &textBrush);
    ID2D1SolidColorBrush *disabledBrush = nullptr;
    ctx->CreateSolidColorBrush(dropdownDisabled, &disabledBrush);

    IDWriteTextFormat *textFormat = nullptr;
    if (dwrite)
    {
        dwrite->CreateTextFormat(
            L"Segoe UI Variable Text",
            nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            13.0f,
            L"en-us",
            &textFormat);
        if (!textFormat)
        {
            dwrite->CreateTextFormat(
                L"Segoe UI",
                nullptr,
                DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                13.0f,
                L"en-us",
                &textFormat);
        }

        if (textFormat)
        {
            textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }

    D2D1_ROUNDED_RECT shadowRounded = D2D1::RoundedRect(
        D2D1::RectF(r.left + 1.0f, r.top + 2.0f, r.right + 1.0f, r.bottom + 2.0f),
        6.0f, 6.0f);
    if (shadowBrush)
        ctx->FillRoundedRectangle(shadowRounded, shadowBrush);

    D2D1_ROUNDED_RECT bgRounded = D2D1::RoundedRect(r, 6.0f, 6.0f);
    if (bgBrush)
        ctx->FillRoundedRectangle(bgRounded, bgBrush);
    if (borderBrush)
    {
        D2D1_ROUNDED_RECT borderRounded = D2D1::RoundedRect(
            D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f),
            5.5f, 5.5f);
        ctx->DrawRoundedRectangle(borderRounded, borderBrush, 1.0f);
    }

    const float itemHeight = (r.bottom - r.top) / (dd.items.empty() ? 1.0f : (float)dd.items.size());
    const float panelPad = 4.0f;
    const float iconSize = 15.0f;
    const float iconPad = 8.0f;
    const float iconColumnWidth = 24.0f;
    const float textPadLeft = 8.0f;
    const float rightPad = 12.0f;
    const float submenuChevronWidth = 16.0f;
    const bool drawShortcuts = DropdownHasShortcuts(dd) && dd.menuIndex >= 0;

    float shortcutColWidth = 0.0f;
    if (drawShortcuts)
    {
        shortcutColWidth = 66.0f;
        for (const auto &shortcut : dd.shortcuts)
        {
            if (!shortcut.empty())
            {
                float approx = 10.0f + (float)shortcut.size() * 6.0f;
                shortcutColWidth = (std::max)(shortcutColWidth, (std::min)(approx, 150.0f));
            }
        }
    }

    for (size_t i = 0; i < dd.items.size(); ++i)
    {
        D2D1_RECT_F itemRect = D2D1::RectF(
            r.left + panelPad,
            r.top + i * itemHeight,
            r.right - panelPad,
            r.top + (i + 1) * itemHeight);

        bool isEnabled = true;
        if (!dd.enabled.empty() && i < dd.enabled.size())
            isEnabled = dd.enabled[i];
        bool isSeparator = (!dd.separators.empty() && i < dd.separators.size() && dd.separators[i]);
        bool hasSub = (!dd.hasSubmenu.empty() && i < dd.hasSubmenu.size() && dd.hasSubmenu[i]);

        if (isSeparator)
        {
            float y = std::floor(itemRect.top + itemHeight * 0.5f) + 0.5f;
            ID2D1SolidColorBrush *sepBrush = nullptr;
            ctx->CreateSolidColorBrush(dropdownSeparator, &sepBrush);
            if (sepBrush)
            {
                ctx->DrawLine(D2D1::Point2F(itemRect.left + 12.0f, y),
                              D2D1::Point2F(itemRect.right - 12.0f, y), sepBrush, 1.0f);
                sepBrush->Release();
            }
            continue;
        }

        if (isEnabled && (int)i == dd.hoveredItem)
        {
            D2D1_RECT_F hoverRect = D2D1::RectF(
                itemRect.left + 2.0f, itemRect.top + 2.0f,
                itemRect.right - 2.0f, itemRect.bottom - 2.0f);
            D2D1_ROUNDED_RECT hoverRounded = D2D1::RoundedRect(hoverRect, 4.0f, 4.0f);
            if (hoverBrush)
                ctx->FillRoundedRectangle(hoverRounded, hoverBrush);
        }

        if (!textFormat || !dwrite)
            continue;

        ID2D1SolidColorBrush *brushToUse = isEnabled ? textBrush : disabledBrush;
        float shortcutWidth = drawShortcuts ? shortcutColWidth : 0.0f;
        float shortcutGap = drawShortcuts ? 14.0f : 0.0f;

        D2D1_RECT_F shortcutRect = D2D1::RectF(
            itemRect.right - (shortcutWidth + rightPad),
            itemRect.top,
            itemRect.right - rightPad,
            itemRect.bottom);
        D2D1_RECT_F textRect = D2D1::RectF(
            itemRect.left + iconPad + iconColumnWidth + textPadLeft,
            itemRect.top,
            itemRect.right - rightPad - shortcutWidth - shortcutGap - (hasSub ? submenuChevronWidth : 0.0f),
            itemRect.bottom);

        const std::wstring &label = dd.items[i];
        std::string iconPath = (dd.menuIndex == -1)
            ? ContextMenuIconPathForLabel(label)
            : MenuDropdownIconPathForLabel(label);
        if (!iconPath.empty())
        {
            FLOAT dpiX = 96.0f, dpiY = 96.0f;
            ctx->GetDpi(&dpiX, &dpiY);
            ID2D1Bitmap *iconBmp = GetContextMenuIconBitmap(ctx, iconPath, (int)iconSize, (UINT)dpiX);
            if (iconBmp)
            {
                float iconLeft = itemRect.left + iconPad;
                float iconTop = std::round(itemRect.top + (itemHeight - iconSize) * 0.5f);
                D2D1_RECT_F iconRect = D2D1::RectF(
                    std::round(iconLeft),
                    iconTop,
                    std::round(iconLeft + iconSize),
                    iconTop + iconSize);
                ctx->DrawBitmap(iconBmp, iconRect, isEnabled ? 1.0f : 0.55f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
        }

        IDWriteTextLayout *labelLayout = nullptr;
        dwrite->CreateTextLayout(label.c_str(), (UINT32)label.size(),
                                 textFormat, textRect.right - textRect.left, itemHeight, &labelLayout);
        if (labelLayout)
        {
            labelLayout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING trimming = {};
            trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
            IDWriteInlineObject *ellipsis = nullptr;
            if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(textFormat, &ellipsis)))
            {
                labelLayout->SetTrimming(&trimming, ellipsis);
                ellipsis->Release();
            }
            ctx->DrawTextLayout(D2D1::Point2F(textRect.left, textRect.top), labelLayout, brushToUse);
            labelLayout->Release();
        }

        if (drawShortcuts && i < dd.shortcuts.size() && !dd.shortcuts[i].empty())
        {
            IDWriteTextLayout *shortcutLayout = nullptr;
            const std::wstring &shortcut = dd.shortcuts[i];
            dwrite->CreateTextLayout(shortcut.c_str(), (UINT32)shortcut.size(),
                                     textFormat, shortcutRect.right - shortcutRect.left, itemHeight, &shortcutLayout);
            if (shortcutLayout)
            {
                shortcutLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                shortcutLayout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                ctx->DrawTextLayout(D2D1::Point2F(shortcutRect.left, shortcutRect.top), shortcutLayout, brushToUse);
                shortcutLayout->Release();
            }
        }

        if (hasSub)
        {
            D2D1_RECT_F chevronRect = D2D1::RectF(
                itemRect.right - 20.0f,
                itemRect.top,
                itemRect.right - 8.0f,
                itemRect.bottom);
            ctx->DrawTextW(L"\u203A", 1, textFormat, chevronRect, brushToUse,
                           D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
        }
    }

    if (textFormat)
        textFormat->Release();
    if (textBrush)
        textBrush->Release();
    if (disabledBrush)
        disabledBrush->Release();
    if (hoverBrush)
        hoverBrush->Release();
    if (borderBrush)
        borderBrush->Release();
    if (bgBrush)
        bgBrush->Release();
    if (shadowBrush)
        shadowBrush->Release();
}
}

int GetHoveredMenuItem([[maybe_unused]] HWND hwnd, POINT pt)
{
    (void)hwnd;
    for (size_t i = 0; i < g_menuItems.size(); ++i)
    {
        auto &item = g_menuItems[i];
        if (pt.x >= item.rect.left && pt.x <= item.rect.right &&
            pt.y >= item.rect.top && pt.y <= item.rect.bottom)
        {
            return (int)i;
        }
    }
    return -1;
}

void SetMenuItemHovered(int index, bool hovered)
{
    if (index >= 0 && index < (int)g_menuItems.size())
        g_menuItems[index].hovered = hovered;
}

std::vector<MenuItem> &GetMenuItems()
{
    return g_menuItems;
}

void ShowMenuDropdown(HWND hwnd, int menuIndex, D2D1_RECT_F menuRect)
{
    g_activeDropdown.menuIndex = menuIndex;
    g_activeDropdown.baseId = 0;
    g_activeDropdown.visible = true;
    g_activeDropdown.hoveredItem = -1;
    g_activeDropdown.shortcuts.clear();
    g_activeDropdown.icons.clear();
    g_activeDropdown.separators.clear();
    g_activeDropdown.hasSubmenu.clear();
    g_activeDropdown.enabled.clear();
    g_subDropdown.visible = false;
    g_subDropdown.hoveredItem = -1;

    switch (menuIndex)
    {
    case 0:
        g_activeDropdown.items = {L"New", L"New Window", L"Open...", L"Open Recent", L"Open Project", L"Close"};
        g_activeDropdown.shortcuts = {L"Ctrl+N", L"", L"Ctrl+O", L"", L"Ctrl+Shift+O", L"Ctrl+W"};
        g_activeDropdown.hasSubmenu = {false, false, false, true, false, false};
        break;
    case 1:
        g_activeDropdown.items = {L"Undo", L"Cut", L"Copy", L"Paste", L"Delete", L"Select All"};
        g_activeDropdown.shortcuts = {L"Ctrl+Z", L"Ctrl+X", L"Ctrl+C", L"Ctrl+V", L"Del", L"Ctrl+A"};
        g_activeDropdown.hasSubmenu.assign(g_activeDropdown.items.size(), false);
        break;
    case 2:
        g_activeDropdown.items = {L"Definir Tampon...", L"Effacer Tampon", L"Voir Tampon"};
        g_activeDropdown.shortcuts.assign(g_activeDropdown.items.size(), L"");
        g_activeDropdown.hasSubmenu.assign(g_activeDropdown.items.size(), false);
        break;
    case 3:
        g_activeDropdown.items = {L"Select All", L"Expand Selection", L"Shrink Selection", L"Select Line"};
        g_activeDropdown.shortcuts = {L"Ctrl+A", L"Shift+Alt+Right", L"Shift+Alt+Left", L"Ctrl+L"};
        g_activeDropdown.hasSubmenu.assign(g_activeDropdown.items.size(), false);
        break;
    case 4:
        g_activeDropdown.items = {L"New Terminal", L"Command Palette", L"Open View", L"Toggle Sidebar", L"Show Extensions", L"Keyboard Shortcuts"};
        g_activeDropdown.shortcuts = {L"Ctrl+`", L"Ctrl+Shift+P", L"", L"Ctrl+B", L"Ctrl+Shift+X", L"Ctrl+K, Ctrl+S"};
        g_activeDropdown.hasSubmenu.assign(g_activeDropdown.items.size(), false);
        break;
    case 5:
        g_activeDropdown.items = {L"Go to File", L"Go to Line", L"Go to Symbol", L"Go to Definition"};
        g_activeDropdown.shortcuts = {L"Ctrl+P", L"Ctrl+G", L"Ctrl+Shift+O", L"F12"};
        g_activeDropdown.hasSubmenu.assign(g_activeDropdown.items.size(), false);
        break;
    case 6:
        g_activeDropdown.items = {L"Start Debugging", L"Run", L"Stop", L"Restart Debugging", L"Step Over", L"Step Into"};
        g_activeDropdown.shortcuts = {L"F5", L"Ctrl+F5", L"Shift+F5", L"Ctrl+Shift+F5", L"F10", L"F11"};
        g_activeDropdown.hasSubmenu.assign(g_activeDropdown.items.size(), false);
        break;
    case 7:
        g_activeDropdown.items = {L"Welcome", L"Documentation", L"About", L"Release Notes", L"Report Issue"};
        g_activeDropdown.shortcuts.assign(g_activeDropdown.items.size(), L"");
        g_activeDropdown.hasSubmenu.assign(g_activeDropdown.items.size(), false);
        break;
    default:
        g_activeDropdown.items = {L"Item 1", L"Item 2"};
        g_activeDropdown.shortcuts.assign(g_activeDropdown.items.size(), L"");
        g_activeDropdown.hasSubmenu.assign(g_activeDropdown.items.size(), false);
        break;
    }

    if (g_activeDropdown.shortcuts.size() != g_activeDropdown.items.size())
        g_activeDropdown.shortcuts.resize(g_activeDropdown.items.size());
    if (g_activeDropdown.hasSubmenu.size() != g_activeDropdown.items.size())
        g_activeDropdown.hasSubmenu.assign(g_activeDropdown.items.size(), false);
    g_activeDropdown.enabled.assign(g_activeDropdown.items.size(), true);

    float itemHeight = 28.0f;
    float width = 210.0f;
    bool hasLongShortcut = false;
    for (const auto &shortcut : g_activeDropdown.shortcuts)
    {
        if (shortcut.size() >= 10)
        {
            hasLongShortcut = true;
            break;
        }
    }
    if (menuIndex == 3)
        width = 320.0f;
    else if (hasLongShortcut)
        width = 290.0f;
    else if (g_activeDropdown.items.size() > 6)
        width = 260.0f;

    float height = itemHeight * (float)g_activeDropdown.items.size();
    g_activeDropdown.rect = D2D1::RectF(
        menuRect.left,
        menuRect.bottom + 4.0f,
        menuRect.left + width,
        menuRect.bottom + 4.0f + height);

    for (size_t i = 0; i < g_menuItems.size(); ++i)
        SetMenuItemHovered((int)i, (int)i == menuIndex);

    RedrawMenuOwner(hwnd, false);
    Logger::Instance().Log(std::wstring(L"DropdownMenu::ShowMenuDropdown - index = ") + std::to_wstring(menuIndex));
}

void HideMenuDropdown(HWND hwnd)
{
    g_activeDropdown.visible = false;
    for (size_t i = 0; i < g_menuItems.size(); ++i)
        SetMenuItemHovered((int)i, false);
    g_subDropdown.visible = false;
    g_subDropdown.hoveredItem = -1;
    RedrawMenuOwner(hwnd, false);
}

bool IsMenuDropdownVisible()
{
    return g_activeDropdown.visible;
}

void DrawMenuDropdown(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite)
{
    if (!g_activeDropdown.visible)
        return;
    DrawDropdownPanel(ctx, dwrite, g_activeDropdown);
}

int GetDropdownHoveredItem(POINT pt)
{
    if (!g_activeDropdown.visible)
        return -1;

    D2D1_RECT_F r = g_activeDropdown.rect;
    if (pt.x < r.left || pt.x > r.right || pt.y < r.top || pt.y > r.bottom)
        return -1;

    float itemHeight = (r.bottom - r.top) / (g_activeDropdown.items.empty() ? 1.0f : (float)g_activeDropdown.items.size());
    int index = (int)((pt.y - r.top) / itemHeight);
    if (index >= 0 && index < (int)g_activeDropdown.items.size())
    {
        if (!g_activeDropdown.separators.empty() && index < (int)g_activeDropdown.separators.size() &&
            g_activeDropdown.separators[index])
            return -1;
        if (!g_activeDropdown.enabled.empty() && index < (int)g_activeDropdown.enabled.size() &&
            !g_activeDropdown.enabled[index])
            return -1;
        return index;
    }
    return -1;
}

void SetDropdownHoveredItem(int index)
{
    g_activeDropdown.hoveredItem = index;
}

bool IsPointInDropdown(POINT pt)
{
    if (!g_activeDropdown.visible)
        return false;
    D2D1_RECT_F r = g_activeDropdown.rect;
    return pt.x >= r.left && pt.x <= r.right && pt.y >= r.top && pt.y <= r.bottom;
}

MenuDropdown &GetActiveDropdown()
{
    return g_activeDropdown;
}

void ShowContextMenuDropdown(HWND hwnd, const std::vector<std::wstring> &items, D2D1_POINT_2F position, int baseId)
{
    g_subDropdown.visible = false;
    g_subDropdown.hoveredItem = -1;
    g_activeDropdown.menuIndex = -1;
    g_activeDropdown.visible = true;
    g_activeDropdown.hoveredItem = -1;
    g_activeDropdown.items = items;
    g_activeDropdown.shortcuts.clear();
    g_activeDropdown.icons.clear();
    g_activeDropdown.separators.clear();
    g_activeDropdown.hasSubmenu.clear();
    g_activeDropdown.baseId = baseId;
    g_activeDropdown.enabled.assign(items.size(), true);

    float itemHeight = 30.0f;
    float width = 232.0f;
    float height = itemHeight * (float)items.size();
    g_activeDropdown.rect = D2D1::RectF(position.x, position.y, position.x + width, position.y + height);

    RedrawMenuOwner(hwnd, true);
}

void ShowSubmenuDropdown(HWND hwnd, const std::vector<std::wstring> &items, D2D1_POINT_2F position, int baseId)
{
    g_subDropdown.menuIndex = (baseId >= 9000 && baseId < 9100) ? -1 : -2;
    g_subDropdown.visible = true;
    g_subDropdown.hoveredItem = -1;
    g_subDropdown.items = items;
    g_subDropdown.shortcuts.clear();
    g_subDropdown.icons.clear();
    g_subDropdown.separators.clear();
    g_subDropdown.hasSubmenu.clear();
    g_subDropdown.baseId = baseId;
    g_subDropdown.enabled.assign(items.size(), true);

    float itemHeight = 30.0f;
    float width = (g_subDropdown.menuIndex == -1) ? 236.0f : 260.0f;
    float height = itemHeight * (float)items.size();
    g_subDropdown.rect = D2D1::RectF(position.x, position.y, position.x + width, position.y + height);

    RedrawMenuOwner(hwnd, true);
}

void HideSubmenuDropdown(HWND hwnd)
{
    g_subDropdown.visible = false;
    g_subDropdown.hoveredItem = -1;
    RedrawMenuOwner(hwnd, true);
}

bool IsSubmenuDropdownVisible()
{
    return g_subDropdown.visible;
}

void DrawSubmenuDropdown(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite)
{
    if (!g_subDropdown.visible)
        return;
    DrawDropdownPanel(ctx, dwrite, g_subDropdown);
}

int GetSubmenuHoveredItem(POINT pt)
{
    if (!g_subDropdown.visible)
        return -1;
    D2D1_RECT_F r = g_subDropdown.rect;
    if (pt.x < r.left || pt.x > r.right || pt.y < r.top || pt.y > r.bottom)
        return -1;
    float itemHeight = (r.bottom - r.top) / (g_subDropdown.items.empty() ? 1.0f : (float)g_subDropdown.items.size());
    int index = (int)((pt.y - r.top) / itemHeight);
    if (index >= 0 && index < (int)g_subDropdown.items.size())
    {
        if (!g_subDropdown.enabled.empty() && index < (int)g_subDropdown.enabled.size() &&
            !g_subDropdown.enabled[index])
            return -1;
        return index;
    }
    return -1;
}

void SetSubmenuHoveredItem(int index)
{
    g_subDropdown.hoveredItem = index;
}

bool IsPointInSubmenu(POINT pt)
{
    if (!g_subDropdown.visible)
        return false;
    D2D1_RECT_F r = g_subDropdown.rect;
    return pt.x >= r.left && pt.x <= r.right && pt.y >= r.top && pt.y <= r.bottom;
}

MenuDropdown &GetSubmenuDropdown()
{
    return g_subDropdown;
}
