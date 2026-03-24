#include "DropdownMenu.h"
#include "ui/components/popups/CustomPopup.h"

#include "helpers/window_helpers.h"
#include "ui/theme/Theme.h"
#include "utils/logger/Logger.h"

#include <algorithm>
#include <cmath>

namespace
{
std::vector<MenuItem> g_menuItems;
MenuDropdown g_activeDropdown = {-1, std::vector<std::wstring>(), std::vector<std::wstring>(), std::vector<wchar_t>(),
    std::vector<bool>(), std::vector<bool>(), D2D1::RectF(), -1, false, 0, std::vector<bool>()};
MenuDropdown g_subDropdown = {-1, std::vector<std::wstring>(), std::vector<std::wstring>(), std::vector<wchar_t>(),
    std::vector<bool>(), std::vector<bool>(), D2D1::RectF(), -1, false, 0, std::vector<bool>()};

// ---------------------------------------------------------------------------
// Segoe MDL2 Assets glyphs — rendered as text with DirectWrite, no SVG loading.
// Full list: https://learn.microsoft.com/en-us/windows/apps/design/style/segoe-ui-symbol-font
// ---------------------------------------------------------------------------
static std::wstring ContextMenuIconGlyph(const std::wstring &label)
{
    // Explorer context menu
    if (label.find(L"Ouvrir le dossier")      != std::wstring::npos) return L"\uE8DA"; // FolderOpen
    if (label.find(L"Ouvrir le fichier")       != std::wstring::npos) return L"\uE7C3"; // Page
    if (label.find(L"Ouvrir dans")             != std::wstring::npos) return L"\uEC50"; // FolderOpen2
    if (label.find(L"Copier le chemin")        != std::wstring::npos) return L"\uE71B"; // Link
    if (label.find(L"Ajouter")                 != std::wstring::npos) return L"\uE710"; // Add
    if (label.find(L"Nouveau fichier")         != std::wstring::npos) return L"\uE7C3"; // Page
    if (label.find(L"Nouveau dossier")         != std::wstring::npos) return L"\uE8F4"; // NewFolder
    if (label.find(L"Class Header")            != std::wstring::npos) return L"\uE943"; // Code (h)
    if (label.find(L"Class Source")            != std::wstring::npos) return L"\uE943"; // Code (cpp)
    if (label.find(L"Duplicate")               != std::wstring::npos) return L"\uE8C8"; // Copy
    if (label.find(L"Rename")                  != std::wstring::npos) return L"\uE8D6"; // Rename
    if (label.find(L"Delete")                  != std::wstring::npos) return L"\uE74D"; // Delete
    // Editor context menu
    if (label.find(L"Couper")                  != std::wstring::npos) return L"\uE8C6"; // Cut
    if (label.find(L"Copier")                  != std::wstring::npos) return L"\uE8C8"; // Copy
    if (label.find(L"Coller")                  != std::wstring::npos) return L"\uE77F"; // Paste
    if (label.find(L"Aller")                   != std::wstring::npos) return L"\uE8A7"; // GoToStart
    if (label.find(L"ggwave")                  != std::wstring::npos) return L"\uE720"; // Microphone
    if (label.find(L"Deplacer")                != std::wstring::npos) return L"\uE8DE"; // MoveToFolder
    return L"";
}

static std::wstring MenuDropdownIconGlyph(const std::wstring &label)
{
    // File
    if (label == L"New")                   return L"\uE7C3"; // Page
    if (label == L"New Window")            return L"\uE8A7"; // OpenWith
    if (label == L"Open..." ||
        label == L"Open Project")          return L"\uE8DA"; // FolderOpen
    if (label == L"Open Recent")           return L"\uE8B1"; // History
    if (label == L"Close")                 return L"\uE8BB"; // ClosePane
    // Edit
    if (label == L"Undo")                  return L"\uE7A7"; // Undo
    if (label == L"Redo")                  return L"\uE7A6"; // Redo
    if (label == L"Cut")                   return L"\uE8C6"; // Cut
    if (label == L"Copy")                  return L"\uE8C8"; // Copy
    if (label == L"Paste")                 return L"\uE77F"; // Paste
    if (label == L"Delete")                return L"\uE74D"; // Delete
    if (label == L"Select All")            return L"\uE8B3"; // SelectAll
    // Selection
    if (label == L"Expand Selection")      return L"\uE8C4"; // ResizeMouseLarge
    if (label == L"Shrink Selection")      return L"\uE8C5"; // ResizeMouseSmall
    if (label == L"Select Line")           return L"\uE8FD"; // Line
    // View
    if (label == L"New Terminal")          return L"\uE756"; // CommandPrompt
    if (label == L"Command Palette")       return L"\uE952"; // CommandBar
    if (label == L"Open View")             return L"\uE8A9"; // OpenPane
    if (label == L"Toggle Sidebar")        return L"\uE8A0"; // SidePanel
    if (label == L"Show Extensions")       return L"\uE74C"; // Plugin
    if (label == L"Keyboard Shortcuts")    return L"\uE92E"; // Keyboard
    // Go
    if (label == L"Go to File")            return L"\uE8E5"; // OpenFile
    if (label == L"Go to Line")            return L"\uE8FD"; // Line
    if (label == L"Go to Symbol")          return L"\uE8A1"; // Symbol
    if (label == L"Go to Definition")      return L"\uE71C"; // Forward
    // Debug
    if (label == L"Start Debugging" ||
        label == L"Run")                   return L"\uE768"; // Play
    if (label == L"Stop")                  return L"\uE71A"; // Stop
    if (label == L"Restart Debugging")     return L"\uE72C"; // Refresh
    if (label == L"Step Over")             return L"\uE8EF"; // Next
    if (label == L"Step Into")             return L"\uEBE8"; // Bug (debug step)
    // Help
    if (label == L"Welcome")               return L"\uE82D"; // Home
    if (label == L"Documentation")         return L"\uE736"; // Library
    if (label == L"About")                 return L"\uE946"; // Info
    if (label == L"Release Notes")         return L"\uE8BD"; // Document
    if (label == L"Report Issue")          return L"\uEBE8"; // Bug
    return L"";
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

// ---------------------------------------------------------------------------
// Cached dropdown resources (created once per render-target, reused every frame)
// ---------------------------------------------------------------------------
namespace DropdownCache
{
    static ID2D1RenderTarget   *ctx_          = nullptr;
    static UI::Theme::Mode      themeMode_    = UI::Theme::Mode::Dark;
    static ID2D1SolidColorBrush *shadow_      = nullptr;
    static ID2D1SolidColorBrush *bg_          = nullptr;
    static ID2D1SolidColorBrush *border_      = nullptr;
    static ID2D1SolidColorBrush *hover_       = nullptr;
    static ID2D1SolidColorBrush *text_        = nullptr;
    static ID2D1SolidColorBrush *disabled_    = nullptr;
    static ID2D1SolidColorBrush *separator_   = nullptr;
    static ID2D1SolidColorBrush *icon_        = nullptr; // muted icon tint
    static ID2D1SolidColorBrush *iconDanger_  = nullptr; // red for Delete
    static IDWriteTextFormat    *format_      = nullptr;
    static IDWriteTextFormat    *iconFmt_     = nullptr; // Segoe MDL2 Assets

    static void Release()
    {
        auto sr = [](auto*& p){ if(p){ p->Release(); p=nullptr; } };
        sr(shadow_); sr(bg_); sr(border_); sr(hover_);
        sr(text_); sr(disabled_); sr(separator_);
        sr(icon_); sr(iconDanger_);
        sr(format_); sr(iconFmt_);
    }

    static void Ensure(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite)
    {
        UI::Theme::Mode mode = UI::Theme::GetMode();
        if (ctx_ == ctx && themeMode_ == mode && shadow_) return;
        Release();
        ctx_ = ctx; themeMode_ = mode;

        const UI::Theme::Palette &p = UI::Theme::GetPalette();
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.30f), &shadow_);
        ctx->CreateSolidColorBrush(p.inputBackground, &bg_);
        ctx->CreateSolidColorBrush(p.inputBorder, &border_);
        D2D1_COLOR_F hov = p.explorerToolbarHover;
        hov.a = (mode == UI::Theme::Mode::Light) ? 0.90f : 1.0f;
        ctx->CreateSolidColorBrush(hov, &hover_);
        ctx->CreateSolidColorBrush(UI::Theme::PrimaryText(), &text_);
        ctx->CreateSolidColorBrush(UI::Theme::MutedText(), &disabled_);
        ctx->CreateSolidColorBrush(UI::Theme::ChromeBorder(), &separator_);

        // Icon brush: slightly muted version of primary text
        D2D1_COLOR_F ic = UI::Theme::PrimaryText();
        ic.a *= 0.55f;
        ctx->CreateSolidColorBrush(ic, &icon_);
        // Danger brush for Delete
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.90f, 0.35f, 0.35f, 0.85f), &iconDanger_);

        if (dwrite)
        {
            dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 13.5f, L"en-us", &format_);
            if (!format_)
                dwrite->CreateTextFormat(L"Segoe UI", nullptr,
                    DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL, 13.5f, L"en-us", &format_);
            if (format_)
            {
                format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }

            // Icon font: Segoe MDL2 Assets (Windows 10/11 system font)
            dwrite->CreateTextFormat(L"Segoe MDL2 Assets", nullptr,
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-us", &iconFmt_);
            if (iconFmt_)
            {
                iconFmt_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                iconFmt_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        }
    }
} // namespace DropdownCache

static constexpr float k_itemH    = kDropdownItemH;
static constexpr float k_sepH     = kDropdownSepH;
static constexpr float k_innerPad = kDropdownInnerPad;

// Compute total pixel height of a dropdown given its items + separators.
static float ComputeDropdownHeight(const MenuDropdown &dd)
{
    float h = 2.0f * k_innerPad; // top + bottom
    for (size_t i = 0; i < dd.items.size(); ++i)
    {
        if (!dd.separators.empty() && i < dd.separators.size() && dd.separators[i])
            h += k_sepH;
        h += k_itemH;
    }
    return h;
}

void DrawDropdownPanel(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, MenuDropdown &dd)
{
    if (!ctx) return;

    DropdownCache::Ensure(ctx, dwrite);

    D2D1_RECT_F r = dd.rect;
    r = D2D1::RectF(std::round(r.left), std::round(r.top), std::round(r.right), std::round(r.bottom));
    dd.rect = r;

    // ---- Shadow (two layers for depth) ----
    if (DropdownCache::shadow_)
    {
        DropdownCache::shadow_->SetOpacity(0.18f);
        ctx->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(r.left+3.0f, r.top+5.0f, r.right+3.0f, r.bottom+5.0f), 7.0f, 7.0f),
            DropdownCache::shadow_);
        DropdownCache::shadow_->SetOpacity(0.10f);
        ctx->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(r.left+1.0f, r.top+1.0f, r.right+1.0f, r.bottom+1.0f), 7.0f, 7.0f),
            DropdownCache::shadow_);
        DropdownCache::shadow_->SetOpacity(1.0f);
    }

    // ---- Background + border ----
    if (DropdownCache::bg_)
        ctx->FillRoundedRectangle(D2D1::RoundedRect(r, 6.0f, 6.0f), DropdownCache::bg_);
    if (DropdownCache::border_)
        ctx->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(r.left+0.5f, r.top+0.5f, r.right-0.5f, r.bottom-0.5f), 5.5f, 5.5f),
            DropdownCache::border_, 1.0f);

    const float panelPad          =  5.0f; // horizontal inset for hover rect
    const float iconPad           = 12.0f; // left margin before icon
    const float iconColumnWidth   = 22.0f; // icon cell width
    const float textPadLeft       =  8.0f; // gap between icon and label
    const float rightPad          = 14.0f;
    const float submenuChevronW   = 16.0f;
    const bool  drawShortcuts     = DropdownHasShortcuts(dd);

    float shortcutColWidth = 0.0f;
    if (drawShortcuts)
    {
        shortcutColWidth = 66.0f;
        for (const auto &sc : dd.shortcuts)
            if (!sc.empty())
                shortcutColWidth = (std::max)(shortcutColWidth, (std::min)(10.0f + (float)sc.size() * 6.0f, 150.0f));
    }

    float y = r.top + k_innerPad; // start after top inner padding

    for (size_t i = 0; i < dd.items.size(); ++i)
    {
        // ---- Separator before this item ----
        bool hasSepBefore = !dd.separators.empty() && i < dd.separators.size() && dd.separators[i];
        if (hasSepBefore && DropdownCache::separator_)
        {
            float sy = std::floor(y + k_sepH * 0.5f) + 0.5f;
            ctx->DrawLine(D2D1::Point2F(r.left + panelPad + 8.0f, sy),
                          D2D1::Point2F(r.right - panelPad - 8.0f, sy),
                          DropdownCache::separator_, 1.0f);
            y += k_sepH;
        }

        D2D1_RECT_F itemRect = D2D1::RectF(r.left + panelPad, y, r.right - panelPad, y + k_itemH);
        y += k_itemH;

        bool isEnabled = dd.enabled.empty() || i >= dd.enabled.size() || dd.enabled[i];
        bool hasSub    = !dd.hasSubmenu.empty() && i < dd.hasSubmenu.size() && dd.hasSubmenu[i];

        // ---- Hover highlight ----
        if (isEnabled && (int)i == dd.hoveredItem && DropdownCache::hover_)
        {
            ctx->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(itemRect.left+2.0f, itemRect.top+2.0f,
                                               itemRect.right-2.0f, itemRect.bottom-2.0f), 5.0f, 5.0f),
                DropdownCache::hover_);
        }

        if (!DropdownCache::format_ || !dwrite) continue;

        ID2D1SolidColorBrush *brush = isEnabled ? DropdownCache::text_ : DropdownCache::disabled_;

        // ---- Icon (Segoe MDL2 Assets glyph) ----
        const std::wstring &label = dd.items[i];
        std::wstring glyph = (dd.menuIndex == -1)
            ? ContextMenuIconGlyph(label)
            : MenuDropdownIconGlyph(label);
        if (!glyph.empty() && DropdownCache::iconFmt_)
        {
            // "Delete" gets a red accent; everything else gets muted icon color
            bool isDanger = (label.find(L"Delete") != std::wstring::npos ||
                             label.find(L"Supprimer") != std::wstring::npos);
            ID2D1SolidColorBrush *iconBrush = isEnabled
                ? (isDanger ? DropdownCache::iconDanger_ : DropdownCache::icon_)
                : DropdownCache::disabled_;

            D2D1_RECT_F iconRect = D2D1::RectF(
                itemRect.left + iconPad,
                itemRect.top,
                itemRect.left + iconPad + iconColumnWidth,
                itemRect.bottom);
            ctx->DrawTextW(glyph.c_str(), (UINT32)glyph.size(),
                           DropdownCache::iconFmt_, iconRect, iconBrush,
                           D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
        }

        float shortcutWidth = drawShortcuts ? shortcutColWidth : 0.0f;
        float shortcutGap   = drawShortcuts ? 14.0f : 0.0f;

        D2D1_RECT_F textRect = D2D1::RectF(
            itemRect.left + iconPad + iconColumnWidth + textPadLeft,
            itemRect.top,
            itemRect.right - rightPad - shortcutWidth - shortcutGap - (hasSub ? submenuChevronW : 0.0f),
            itemRect.bottom);

        // ---- Label ----
        IDWriteTextLayout *lay = nullptr;
        dwrite->CreateTextLayout(label.c_str(), (UINT32)label.size(),
            DropdownCache::format_, textRect.right - textRect.left, k_itemH, &lay);
        if (lay)
        {
            lay->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING trim = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            IDWriteInlineObject *ell = nullptr;
            if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(DropdownCache::format_, &ell)))
            { lay->SetTrimming(&trim, ell); ell->Release(); }
            ctx->DrawTextLayout(D2D1::Point2F(textRect.left, textRect.top), lay, brush);
            lay->Release();
        }

        // ---- Shortcut ----
        if (drawShortcuts && i < dd.shortcuts.size() && !dd.shortcuts[i].empty())
        {
            D2D1_RECT_F sr = D2D1::RectF(itemRect.right-(shortcutWidth+rightPad), itemRect.top,
                                          itemRect.right-rightPad, itemRect.bottom);
            IDWriteTextLayout *sl = nullptr;
            dwrite->CreateTextLayout(dd.shortcuts[i].c_str(), (UINT32)dd.shortcuts[i].size(),
                DropdownCache::format_, sr.right-sr.left, k_itemH, &sl);
            if (sl)
            {
                sl->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                sl->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                ctx->DrawTextLayout(D2D1::Point2F(sr.left, sr.top), sl, brush);
                sl->Release();
            }
        }

        // ---- Submenu chevron ----
        if (hasSub)
        {
            D2D1_RECT_F cr = D2D1::RectF(itemRect.right-20.0f, itemRect.top, itemRect.right-8.0f, itemRect.bottom);
            ctx->DrawTextW(L"\u203A", 1, DropdownCache::format_, cr, brush,
                           D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
        }
    }
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

    float width = 220.0f;
    bool hasLongShortcut = false;
    for (const auto &shortcut : g_activeDropdown.shortcuts)
        if (shortcut.size() >= 10) { hasLongShortcut = true; break; }
    if (menuIndex == 3)       width = 330.0f;
    else if (hasLongShortcut) width = 300.0f;
    else if (g_activeDropdown.items.size() > 6) width = 270.0f;

    g_activeDropdown.separators.assign(g_activeDropdown.items.size(), false);
    float height = ComputeDropdownHeight(g_activeDropdown);
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
    if (!g_activeDropdown.visible) return -1;
    const D2D1_RECT_F &r = g_activeDropdown.rect;
    if (pt.x < r.left || pt.x > r.right || pt.y < r.top || pt.y > r.bottom) return -1;

    float y = r.top + k_innerPad;
    for (int i = 0; i < (int)g_activeDropdown.items.size(); ++i)
    {
        if (!g_activeDropdown.separators.empty() && i < (int)g_activeDropdown.separators.size()
            && g_activeDropdown.separators[i])
            y += k_sepH;
        if (pt.y >= y && pt.y < y + k_itemH)
        {
            if (!g_activeDropdown.enabled.empty() && i < (int)g_activeDropdown.enabled.size()
                && !g_activeDropdown.enabled[i])
                return -1;
            return i;
        }
        y += k_itemH;
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

void ShowContextMenuDropdown(HWND hwnd, const std::vector<std::wstring> &items, D2D1_POINT_2F position, int baseId,
                             const std::vector<bool> &separators,
                             const std::vector<std::wstring> &shortcuts,
                             const std::vector<bool> &enabled)
{
    // Close any open titlebar dropdown
    g_subDropdown.visible = false;
    g_subDropdown.hoveredItem = -1;
    g_activeDropdown.visible = false;
    g_activeDropdown.hoveredItem = -1;
    for (size_t i = 0; i < g_menuItems.size(); ++i)
        SetMenuItemHovered((int)i, false);

    // Convert client coords → screen coords for the OS popup window
    POINT screenPt = { (LONG)position.x, (LONG)position.y };
    ClientToScreen(hwnd, &screenPt);

    // Show the real popup window — it manages its own rendering and events
    ShowCustomPopup(hwnd, items, screenPt, baseId, separators, shortcuts, enabled);
}

void ShowSubmenuDropdown(HWND hwnd, const std::vector<std::wstring> &items, D2D1_POINT_2F position, int baseId,
                         const std::vector<bool> &separators)
{
    g_subDropdown.menuIndex = (baseId >= 9000 && baseId < 9100) ? -1 : -2;
    g_subDropdown.visible = true;
    g_subDropdown.hoveredItem = -1;
    g_subDropdown.items = items;
    g_subDropdown.shortcuts.clear();
    g_subDropdown.icons.clear();
    g_subDropdown.separators = separators;
    if (g_subDropdown.separators.size() < items.size())
        g_subDropdown.separators.resize(items.size(), false);
    g_subDropdown.hasSubmenu.assign(items.size(), false);
    g_subDropdown.baseId = baseId;
    g_subDropdown.enabled.assign(items.size(), true);

    float width = (g_subDropdown.menuIndex == -1) ? 252.0f : 270.0f;
    float height = ComputeDropdownHeight(g_subDropdown);

    // ---- Screen edge clipping ----
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfo(mon, &mi);
    RECT work = mi.rcWork;

    float x = position.x, y = position.y;
    if (x + width  > (float)work.right)  x = position.x - width;   // flip left
    if (y + height > (float)work.bottom) y = (float)work.bottom - height;
    if (x < (float)work.left) x = (float)work.left;
    if (y < (float)work.top)  y = (float)work.top;

    g_subDropdown.rect = D2D1::RectF(x, y, x + width, y + height);
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
    if (!g_subDropdown.visible) return -1;
    const D2D1_RECT_F &r = g_subDropdown.rect;
    if (pt.x < r.left || pt.x > r.right || pt.y < r.top || pt.y > r.bottom) return -1;

    float y = r.top + k_innerPad;
    for (int i = 0; i < (int)g_subDropdown.items.size(); ++i)
    {
        if (!g_subDropdown.separators.empty() && i < (int)g_subDropdown.separators.size()
            && g_subDropdown.separators[i])
            y += k_sepH;
        if (pt.y >= y && pt.y < y + k_itemH)
        {
            if (!g_subDropdown.enabled.empty() && i < (int)g_subDropdown.enabled.size()
                && !g_subDropdown.enabled[i])
                return -1;
            return i;
        }
        y += k_itemH;
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
