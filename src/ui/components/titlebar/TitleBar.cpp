#include "TitleBar.h"
#include "helpers/window_helpers.h"
#include "utils/logger/Logger.h"
#include "core/window/Window.h"
#include "core/explorer/Explorer.h"
#include <windows.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>

// Cache global pour l'icône
static ID2D1Bitmap *g_iconBitmap = nullptr;

// État global des menus
static std::vector<MenuItem> g_menuItems;
static MenuDropdown g_activeDropdown = {-1, std::vector<std::wstring>(), D2D1::RectF(), -1, false, 0, std::vector<bool>()};

// Helper pour charger l'icône
static ID2D1Bitmap *LoadIconBitmap(ID2D1RenderTarget *ctx, const wchar_t *filename)
{
    if (g_iconBitmap)
        return g_iconBitmap;

    IWICImagingFactory *wicFactory = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory));
    if (!wicFactory)
        return nullptr;

    IWICBitmapDecoder *decoder = nullptr;
    wicFactory->CreateDecoderFromFilename(filename, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);

    if (decoder)
    {
        IWICBitmapFrameDecode *frame = nullptr;
        decoder->GetFrame(0, &frame);

        if (frame)
        {
            IWICFormatConverter *converter = nullptr;
            wicFactory->CreateFormatConverter(&converter);

            if (converter)
            {
                converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
                ctx->CreateBitmapFromWicBitmap(converter, NULL, &g_iconBitmap);
                converter->Release();
            }
            frame->Release();
        }
        decoder->Release();
    }

    wicFactory->Release();
    return g_iconBitmap;
}

// Fonction helper pour dessiner l'icône minimize (ligne horizontale)
static void DrawMinimizeIcon(ID2D1RenderTarget *ctx, ID2D1SolidColorBrush *brush, D2D1_RECT_F rect)
{
    float centerX = std::round((rect.left + rect.right) / 2.0f);
    float centerY = std::round((rect.top + rect.bottom) / 2.0f);
    float lineWidth = 10.0f;

    D2D1_RECT_F lineRect = D2D1::RectF(
        centerX - lineWidth / 2.0f,
        centerY - 0.5f,
        centerX + lineWidth / 2.0f,
        centerY + 0.5f);

    ctx->FillRectangle(lineRect, brush);
}

// Fonction helper pour dessiner l'icône maximize (carré simple)
static void DrawMaximizeIcon(ID2D1RenderTarget *ctx, ID2D1SolidColorBrush *brush, D2D1_RECT_F rect)
{
    float centerX = std::round((rect.left + rect.right) / 2.0f);
    float centerY = std::round((rect.top + rect.bottom) / 2.0f);
    float size = 10.0f;

    D2D1_RECT_F iconRect = D2D1::RectF(
        centerX - size / 2.0f,
        centerY - size / 2.0f,
        centerX + size / 2.0f,
        centerY + size / 2.0f);

    ctx->DrawRectangle(iconRect, brush, 1.0f);
}

// Fonction helper pour dessiner l'icône restore (style Windows classique)
static void DrawRestoreIcon(
    ID2D1RenderTarget *ctx,
    IDWriteFactory *dwrite,
    ID2D1SolidColorBrush *brush,
    D2D1_RECT_F rect)
{
    if (!dwrite || !ctx || !brush)
        return;

    float height = rect.bottom - rect.top;
    float fontSize = std::clamp(height * 0.3f, 10.0f, height - 2.0f);

    IDWriteTextFormat *iconFormat = nullptr;
    if (SUCCEEDED(dwrite->CreateTextFormat(
            L"Segoe MDL2 Assets", NULL,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            fontSize, L"en-us", &iconFormat)))
    {
        iconFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        iconFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        const wchar_t glyph[2] = {0xE923, 0};

        ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        ctx->DrawTextW(glyph, 1, iconFormat, rect, brush,
                       D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);

        iconFormat->Release();
    }
}

// Fonction helper pour dessiner l'icône close (X)
static void DrawCloseIcon(ID2D1RenderTarget *ctx, ID2D1SolidColorBrush *brush, D2D1_RECT_F rect)
{
    float centerX = std::round((rect.left + rect.right) / 2.0f);
    float centerY = std::round((rect.top + rect.bottom) / 2.0f);
    float size = 10.0f;

    D2D1_POINT_2F p1 = D2D1::Point2F(centerX - size / 2.0f, centerY - size / 2.0f);
    D2D1_POINT_2F p2 = D2D1::Point2F(centerX + size / 2.0f, centerY + size / 2.0f);
    D2D1_POINT_2F p3 = D2D1::Point2F(centerX + size / 2.0f, centerY - size / 2.0f);
    D2D1_POINT_2F p4 = D2D1::Point2F(centerX - size / 2.0f, centerY + size / 2.0f);

    ctx->DrawLine(p1, p2, brush, 1.0f);
    ctx->DrawLine(p3, p4, brush, 1.0f);
}

// Fonction helper pour dessiner l'icône play (triangle)
static void DrawPlayIcon(
    ID2D1RenderTarget *ctx,
    IDWriteFactory *dwrite,
    ID2D1SolidColorBrush *brush,
    D2D1_RECT_F rect)
{
    if (!dwrite || !ctx || !brush)
        return;

    float height = rect.bottom - rect.top;
    float fontSize = std::clamp(height * 0.45f, 12.0f, height - 2.0f);

    IDWriteTextFormat *iconFormat = nullptr;
    if (SUCCEEDED(dwrite->CreateTextFormat(
            L"Segoe MDL2 Assets", NULL,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            fontSize, L"en-us", &iconFormat)))
    {
        iconFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        iconFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        const wchar_t glyph[2] = {0xE768, 0}; // Play icon

        ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        ctx->DrawTextW(glyph, 1, iconFormat, rect, brush,
                       D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);

        iconFormat->Release();
    }
}

void DrawCustomTitleBarD2D(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd, int hoveredButton, bool hasFocus, const std::wstring &title)
{
    (void)hasFocus;
    (void)title;
    if (!ctx)
        return;

    RECT title_bar_rect = win32_titlebar_rect(hwnd);
    D2D1_RECT_F tb = D2D1::RectF((FLOAT)title_bar_rect.left, (FLOAT)title_bar_rect.top, (FLOAT)title_bar_rect.right, (FLOAT)title_bar_rect.bottom);

    D2D1_ANTIALIAS_MODE oldAA = ctx->GetAntialiasMode();
    D2D1_TEXT_ANTIALIAS_MODE oldTextAA = ctx->GetTextAntialiasMode();
    ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

    // Background - dark theme
    ID2D1SolidColorBrush *bgBrush = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(18.0f / 255.0f, 18.0f / 255.0f, 18.0f / 255.0f), &bgBrush);
    ctx->FillRectangle(tb, bgBrush);

    // Bottom border
    ID2D1SolidColorBrush *bottomBorder = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(48.0f / 255.0f, 48.0f / 255.0f, 48.0f / 255.0f), &bottomBorder);
    D2D1_POINT_2F leftPt = D2D1::Point2F(tb.left, tb.bottom - 0.5f);
    D2D1_POINT_2F rightPt = D2D1::Point2F(tb.right, tb.bottom - 0.5f);
    ctx->DrawLine(leftPt, rightPt, bottomBorder, 1.0f);

    CustomTitleBarButtonRects button_rects = win32_get_title_bar_button_rects(hwnd, &title_bar_rect);

    D2D1_RECT_F rMin = D2D1::RectF((FLOAT)button_rects.minimize.left, (FLOAT)button_rects.minimize.top, (FLOAT)button_rects.minimize.right, (FLOAT)button_rects.minimize.bottom);
    D2D1_RECT_F rMax = D2D1::RectF((FLOAT)button_rects.maximize.left, (FLOAT)button_rects.maximize.top, (FLOAT)button_rects.maximize.right, (FLOAT)button_rects.maximize.bottom);
    D2D1_RECT_F rClose = D2D1::RectF((FLOAT)button_rects.close.left, (FLOAT)button_rects.close.top, (FLOAT)button_rects.close.right, (FLOAT)button_rects.close.bottom);
    D2D1_RECT_F rRun = D2D1::RectF((FLOAT)button_rects.run.left, (FLOAT)button_rects.run.top, (FLOAT)button_rects.run.right, (FLOAT)button_rects.run.bottom);

    ID2D1SolidColorBrush *hoverBrush = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0x3e3e42), &hoverBrush);

    ID2D1SolidColorBrush *closeHoverBrush = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0xe81123), &closeHoverBrush);

    if (hoveredButton == Window::Hovered_Minimize)
        ctx->FillRectangle(rMin, hoverBrush);
    if (hoveredButton == Window::Hovered_Run)
        ctx->FillRectangle(rRun, hoverBrush);
    if (hoveredButton == Window::Hovered_Maximize)
        ctx->FillRectangle(rMax, hoverBrush);
    if (hoveredButton == Window::Hovered_Close)
        ctx->FillRectangle(rClose, closeHoverBrush);

    UINT dpi = GetDpiForWindow(hwnd);
    bool isMaximized = win32_window_is_maximized(hwnd);

    // Brush pour les icônes
    ID2D1SolidColorBrush *iconBrush = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &iconBrush);

    ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);

    // Dessiner les icônes
    if (iconBrush)
    {
        DrawMinimizeIcon(ctx, iconBrush, rMin);
        DrawPlayIcon(ctx, dwrite, iconBrush, rRun);

        if (isMaximized)
            DrawRestoreIcon(ctx, dwrite, iconBrush, rMax);
        else
            DrawMaximizeIcon(ctx, iconBrush, rMax);

        DrawCloseIcon(ctx, iconBrush, rClose);
    }

    // Icône de l'app
    ID2D1Bitmap *iconBitmap = LoadIconBitmap(ctx, L"assets/favicon.ico");
    float padding = (float)win32_dpi_scale(12, dpi);
    if (isMaximized)
    {
        padding += (float)win32_dpi_scale(6, dpi);
    }

    float currentX = tb.left + padding;

    if (iconBitmap)
    {
        float iconSize = 16.0f;
        D2D1_RECT_F iconRect = D2D1::RectF(
            std::round(currentX),
            std::round((tb.top + tb.bottom - iconSize) / 2.0f),
            std::round(currentX + iconSize),
            std::round((tb.top + tb.bottom + iconSize) / 2.0f));
        ctx->DrawBitmap(iconBitmap, iconRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        currentX += iconSize + 8.0f;
    }

    // ============================================================================
    // JETBRAINS MONO - Format pour les menus
    // Utilisation de Regular (400) avec taille optimisée pour la lisibilité
    // ============================================================================
    IDWriteTextFormat *menuFormat = nullptr;
    if (dwrite)
    {
        dwrite->CreateTextFormat(
            L"JetBrains Mono",
            NULL,
            DWRITE_FONT_WEIGHT_REGULAR,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            12.5f,
            L"en-us",
            &menuFormat);

        if (menuFormat)
        {
            menuFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            menuFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }

    // Menu items (Terminal removed)
    std::vector<std::wstring> menus = {L"File", L"Edit", L"Selection", L"View", L"Go", L"Run", L"Help"};

    // Initialiser g_menuItems si vide
    if (g_menuItems.empty())
    {
        for (const auto &menu : menus)
        {
            g_menuItems.push_back({menu, D2D1::RectF(0, 0, 0, 0), false});
        }
    }

    ID2D1SolidColorBrush *menuTextBrush = nullptr;
    ID2D1SolidColorBrush *menuHoverBrush = nullptr;
    // Couleur légèrement plus claire pour meilleur contraste avec JetBrains Mono
    ctx->CreateSolidColorBrush(D2D1::ColorF(0xd4d4d4), &menuTextBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0x2a2d2e), &menuHoverBrush);

    // Dessiner chaque menu item
    for (size_t i = 0; i < g_menuItems.size(); i++)
    {
        auto &item = g_menuItems[i];

        if (dwrite && menuFormat)
        {
            // Calculer la largeur du texte
            IDWriteTextLayout *textLayout = nullptr;
            dwrite->CreateTextLayout(item.label.c_str(), (UINT32)item.label.size(), menuFormat, 1000.0f, 30.0f, &textLayout);

            float itemWidth = 16.0f;

            if (textLayout)
            {
                DWRITE_TEXT_METRICS metrics;
                textLayout->GetMetrics(&metrics);
                // JetBrains Mono est monospace, donc padding uniforme
                itemWidth = metrics.width + 24.0f;
                textLayout->Release();
            }

            item.rect = D2D1::RectF(
                std::round(currentX),
                tb.top,
                std::round(currentX + itemWidth),
                tb.bottom);

            if (item.hovered)
            {
                if (menuHoverBrush)
                {
                    D2D1_RECT_F hoverRect = D2D1::RectF(
                        item.rect.left + 4.0f,
                        item.rect.top + 6.0f,
                        item.rect.right - 4.0f,
                        item.rect.bottom - 6.0f);
                    D2D1_ROUNDED_RECT hoverRounded = D2D1::RoundedRect(hoverRect, 4.0f, 4.0f);
                    ctx->FillRoundedRectangle(hoverRounded, menuHoverBrush);
                }
            }

            // Dessiner le texte
            D2D1_RECT_F textRect = D2D1::RectF(
                item.rect.left + 10.0f,
                item.rect.top,
                item.rect.right - 10.0f,
                item.rect.bottom);

            ID2D1SolidColorBrush *textBrushToUse = menuTextBrush;
            if (item.hovered)
            {
                // Texte en blanc pur quand hover pour contraste maximal
                ctx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f), &textBrushToUse);
            }

            ctx->DrawTextW(
                item.label.c_str(),
                (UINT32)item.label.size(),
                menuFormat,
                textRect,
                textBrushToUse,
                D2D1_DRAW_TEXT_OPTIONS_NONE,
                DWRITE_MEASURING_MODE_NATURAL);

            if (item.hovered && textBrushToUse != menuTextBrush)
                textBrushToUse->Release();

            currentX += itemWidth;
        }
    }

    float titleLeft = currentX;
    float titleRight = rRun.left;
    float titleWidth = titleRight - titleLeft;
    if (titleWidth > 40.0f && dwrite)
    {
        IDWriteTextFormat *titleFormat = nullptr;
        dwrite->CreateTextFormat(
            L"JetBrains Mono",
            NULL,
            DWRITE_FONT_WEIGHT_REGULAR,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            12.5f,
            L"en-us",
            &titleFormat);

        if (titleFormat)
        {
            titleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            titleFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

            // Calculer le même offset vertical que les menus
            float menuVerticalOffset = 0.0f; // Les menus n'ont pas d'offset supplémentaire

            D2D1_RECT_F titleRect = D2D1::RectF(
                titleLeft,
                tb.top + menuVerticalOffset, // Même top que les menus
                titleRight,
                tb.bottom + menuVerticalOffset); // Même bottom que les menus

            std::wstring displayTitle;
            std::wstring rootPath = GetExplorerManager().GetState().rootPath;
            if (!rootPath.empty())
            {
                size_t pos = rootPath.find_last_of(L"\\/");
                std::wstring projectName = (pos != std::wstring::npos) ? rootPath.substr(pos + 1) : rootPath;
                displayTitle = title + L" - " + projectName;
            }
            else
            {
                displayTitle = title + L" - Aucun projet ouvert";
            }

            ID2D1SolidColorBrush *titleBrush = nullptr;
            ctx->CreateSolidColorBrush(D2D1::ColorF(0xb0b0b0), &titleBrush);

            D2D1_TEXT_ANTIALIAS_MODE prevTextAA = ctx->GetTextAntialiasMode();
            ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_ALIASED);
            ctx->DrawTextW(
                displayTitle.c_str(),
                (UINT32)displayTitle.size(),
                titleFormat,
                titleRect,
                titleBrush,
                D2D1_DRAW_TEXT_OPTIONS_NONE,
                DWRITE_MEASURING_MODE_NATURAL);
            ctx->SetTextAntialiasMode(prevTextAA);

            if (titleBrush)
                titleBrush->Release();
            if (titleFormat)
                titleFormat->Release();
        }
    }

    // Réutilise menuFormat au lieu de créer titleFormat
    if (menuFormat)
    {
        D2D1_RECT_F titleRect = D2D1::RectF(titleLeft, tb.top, titleRight, tb.bottom);

        std::wstring displayTitle;
        std::wstring rootPath = GetExplorerManager().GetState().rootPath;
        if (!rootPath.empty())
        {
            size_t pos = rootPath.find_last_of(L"\\/");
            std::wstring projectName = (pos != std::wstring::npos) ? rootPath.substr(pos + 1) : rootPath;
            displayTitle = title + L" - " + projectName;
        }
        else
        {
            displayTitle = title + L" - Aucun projet ouvert";
        }

        ID2D1SolidColorBrush *titleBrush = nullptr;
        ctx->CreateSolidColorBrush(D2D1::ColorF(0xb0b0b0), &titleBrush);

        // Créer un format temporaire avec CENTER alignment
        IDWriteTextFormat *centerFormat = nullptr;
        dwrite->CreateTextFormat(
            L"JetBrains Mono", NULL,
            DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            12.5f, L"en-us", &centerFormat);

        if (centerFormat)
        {
            centerFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            centerFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

            ctx->DrawTextW(displayTitle.c_str(), (UINT32)displayTitle.size(),
                           centerFormat, titleRect, titleBrush,
                           D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);

            centerFormat->Release();
        }

        if (titleBrush)
            titleBrush->Release();
    }
    if (menuHoverBrush)
        menuHoverBrush->Release();
    if (menuTextBrush)
        menuTextBrush->Release();
    if (iconBrush)
        iconBrush->Release();
    ctx->SetAntialiasMode(oldAA);
    ctx->SetTextAntialiasMode(oldTextAA);
    if (closeHoverBrush)
        closeHoverBrush->Release();
    if (hoverBrush)
        hoverBrush->Release();
    if (bottomBorder)
        bottomBorder->Release();
    if (bgBrush)
        bgBrush->Release();
}

// Fonctions pour les menus
int GetHoveredMenuItem(HWND hwnd, POINT pt)
{
    (void)hwnd;
    for (size_t i = 0; i < g_menuItems.size(); i++)
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
    {
        g_menuItems[index].hovered = hovered;
    }
}

std::vector<MenuItem> &GetMenuItems()
{
    return g_menuItems;
}

// Fonctions pour le dropdown
void ShowMenuDropdown(HWND hwnd, int menuIndex, D2D1_RECT_F menuRect)
{
    g_activeDropdown.menuIndex = menuIndex;
    g_activeDropdown.visible = true;
    g_activeDropdown.hoveredItem = -1;

    switch (menuIndex)
    {
    case 0:
        // Simplified File menu with Open Project added.
        g_activeDropdown.items = {L"New", L"New Window", L"Open...", L"Open Project", L"Close"};
        break;

    case 1:
        g_activeDropdown.items = {
            L"Undo", L"Redo", L"Cut", L"Copy", L"Paste", L"Paste Without Formatting", L"Delete",
            L"Select All", L"Find", L"Replace", L"Find in Files", L"Replace in Files", L"Toggle Comment", L"Format Document"};
        // Initialize enabled flags based on editor state and clipboard
        g_activeDropdown.enabled.clear();
        g_activeDropdown.enabled.resize(g_activeDropdown.items.size(), true);
        {
            Orion::Editor *editor = GetOrionEditor(hwnd);
            bool hasEditor = (editor != nullptr);
            bool hasSelection = false;
            bool hasContent = false;
            if (hasEditor)
            {
                std::wstring sel = editor->GetSelectionText();
                hasSelection = !sel.empty();
                hasContent = editor->HasNonEmptyContent();
            }

            // Cut/Copy/Delete enabled only if selection exists
            if (g_activeDropdown.items.size() >= 7)
            {
                g_activeDropdown.enabled[2] = hasSelection; // Cut
                g_activeDropdown.enabled[3] = hasSelection; // Copy
                g_activeDropdown.enabled[6] = hasSelection; // Delete
            }

            // Paste enabled only if clipboard has text
            bool canPaste = false;
            if (OpenClipboard(NULL))
            {
                HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                if (hData)
                    canPaste = true;
                CloseClipboard();
            }
            if (g_activeDropdown.items.size() >= 5)
            {
                g_activeDropdown.enabled[4] = canPaste; // Paste
                g_activeDropdown.enabled[5] = canPaste; // Paste Without Formatting
            }

            // Select All enabled only if there's content
            if (g_activeDropdown.items.size() >= 8)
            {
                g_activeDropdown.enabled[7] = hasContent;
            }
        }
        break;
    case 2:
        g_activeDropdown.items = {L"Select All", L"Expand Selection", L"Shrink Selection", L"Select Line"};
        break;
    case 3:
        // Add New Terminal as a View action
        g_activeDropdown.items = {L"New Terminal", L"Command Palette", L"Open View", L"Toggle Sidebar", L"Show Extensions", L"Keyboard Shortcuts"};
        break;
    case 4:
        g_activeDropdown.items = {L"Go to File", L"Go to Line", L"Go to Symbol", L"Go to Definition"};
        break;
    case 5:
        g_activeDropdown.items = {L"Start Debugging", L"Run", L"Stop", L"Restart Debugging", L"Step Over", L"Step Into"};
        break;
    case 6:
        // Help menu (moved from index 7 after removing Terminal)
        g_activeDropdown.items = {L"Welcome", L"Documentation", L"About", L"Release Notes", L"Report Issue"};
        break;
    default:
        g_activeDropdown.items = {L"Item 1", L"Item 2", L"Item 3", L"Item 4", L"Item 5", L"Item 6", L"Item 7", L"Item 8", L"Item 9", L"Item 10"};
        break;
    }

    float itemHeight = 32.0f;
    float width = 180.0f;
    if (g_activeDropdown.items.size() > 6)
    {
        width = 260.0f;
    }
    float height = itemHeight * g_activeDropdown.items.size();

    g_activeDropdown.rect = D2D1::RectF(
        menuRect.left,
        menuRect.bottom + 4.0f,
        menuRect.left + width,
        menuRect.bottom + 4.0f + height);

    for (size_t i = 0; i < g_menuItems.size(); ++i)
    {
        SetMenuItemHovered((int)i, (int)i == menuIndex);
    }

    if (hwnd)
    {
        RECT tb = win32_titlebar_rect(hwnd);
        RedrawWindow(hwnd, &tb, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    }
    // debug trace
    Logger::Instance().Log(std::wstring(L"TitleBar::ShowMenuDropdown - index = ") + std::to_wstring(menuIndex));
}

void HideMenuDropdown(HWND hwnd)
{
    g_activeDropdown.visible = false;
    for (size_t i = 0; i < g_menuItems.size(); ++i)
    {
        SetMenuItemHovered((int)i, false);
    }
    if (hwnd)
    {
        RECT tb = win32_titlebar_rect(hwnd);
        RedrawWindow(hwnd, &tb, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    }
}

bool IsMenuDropdownVisible()
{
    return g_activeDropdown.visible;
}

void DrawMenuDropdown(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite)
{
    if (!g_activeDropdown.visible || !ctx)
        return;

    D2D1_RECT_F r = g_activeDropdown.rect;

    ID2D1SolidColorBrush *bgBrush = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0x1f1f20), &bgBrush);
    D2D1_ROUNDED_RECT bgRounded = D2D1::RoundedRect(r, 4.0f, 4.0f);
    ctx->FillRoundedRectangle(bgRounded, bgBrush);

    ID2D1SolidColorBrush *hoverBrush = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0x313335), &hoverBrush);
    ID2D1SolidColorBrush *textBrush = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0xe6e6e6), &textBrush);

    IDWriteTextFormat *textFormat = nullptr;
    if (dwrite)
    {
        dwrite->CreateTextFormat(
            L"JetBrains Mono",
            NULL,
            DWRITE_FONT_WEIGHT_REGULAR,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            12.5f,
            L"en-us",
            &textFormat);

        if (textFormat)
        {
            textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }

    float itemHeight = (r.bottom - r.top) / (g_activeDropdown.items.empty() ? 1.0f : (float)g_activeDropdown.items.size());

    for (size_t i = 0; i < g_activeDropdown.items.size(); i++)
    {
        D2D1_RECT_F itemRect = D2D1::RectF(
            r.left,
            r.top + i * itemHeight,
            r.right,
            r.top + (i + 1) * itemHeight);

        bool isEnabled = true;
        if (!g_activeDropdown.enabled.empty() && i < g_activeDropdown.enabled.size())
            isEnabled = g_activeDropdown.enabled[i];

        if (isEnabled && (int)i == g_activeDropdown.hoveredItem)
        {
            D2D1_ROUNDED_RECT hoverRounded = D2D1::RoundedRect(
                D2D1::RectF(itemRect.left + 4.0f, itemRect.top + 4.0f, itemRect.right - 4.0f, itemRect.bottom - 4.0f),
                3.0f, 3.0f);
            ctx->FillRoundedRectangle(hoverRounded, hoverBrush);
        }

        if (textFormat)
        {
            D2D1_RECT_F textRect = D2D1::RectF(
                itemRect.left + 12.0f,
                itemRect.top,
                itemRect.right - 12.0f,
                itemRect.bottom);

            ID2D1SolidColorBrush *brushToUse = textBrush;
            ID2D1SolidColorBrush *disabledBrush = nullptr;
            if (!isEnabled)
            {
                ctx->CreateSolidColorBrush(D2D1::ColorF(0x7f7f7f), &disabledBrush);
                brushToUse = disabledBrush;
            }

            ctx->DrawTextW(
                g_activeDropdown.items[i].c_str(),
                (UINT32)g_activeDropdown.items[i].size(),
                textFormat,
                textRect,
                brushToUse,
                D2D1_DRAW_TEXT_OPTIONS_NONE,
                DWRITE_MEASURING_MODE_NATURAL);

            if (disabledBrush)
                disabledBrush->Release();
        }
    }

    if (textFormat)
        textFormat->Release();
    if (textBrush)
        textBrush->Release();
    if (hoverBrush)
        hoverBrush->Release();
    if (bgBrush)
        bgBrush->Release();
}

int GetDropdownHoveredItem(POINT pt)
{
    if (!g_activeDropdown.visible)
        return -1;

    D2D1_RECT_F r = g_activeDropdown.rect;
    if (pt.x < r.left || pt.x > r.right || pt.y < r.top || pt.y > r.bottom)
    {
        return -1;
    }

    float itemHeight = (r.bottom - r.top) / (g_activeDropdown.items.empty() ? 1.0f : (float)g_activeDropdown.items.size());
    int index = (int)((pt.y - r.top) / itemHeight);

    if (index >= 0 && index < (int)g_activeDropdown.items.size())
    {
        if (!g_activeDropdown.enabled.empty() && index < (int)g_activeDropdown.enabled.size())
        {
            if (!g_activeDropdown.enabled[index])
                return -1;
        }
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
    g_activeDropdown.menuIndex = -1;
    g_activeDropdown.visible = true;
    g_activeDropdown.hoveredItem = -1;
    g_activeDropdown.items = items;
    g_activeDropdown.baseId = baseId;
    g_activeDropdown.enabled.clear();
    g_activeDropdown.enabled.resize(items.size(), true);

    float itemHeight = 32.0f;
    float width = 200.0f;
    float height = itemHeight * items.size();

    g_activeDropdown.rect = D2D1::RectF(
        position.x,
        position.y,
        position.x + width,
        position.y + height);

    if (hwnd)
    {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}
