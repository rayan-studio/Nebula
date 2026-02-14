#include "TitleBar.h"
#include "helpers/window_helpers.h"
#include "helpers/path_helpers.h"
#include "utils/logger/Logger.h"
#include "utils/auth/GitHubAuth.h"
#include "core/window/Window.h"
#include "core/explorer/Explorer.h"
#include "ui/theme/Theme.h"
#include <windows.h>
#include <wincodec.h>
#include <algorithm>
#include <unordered_map>
#include <cmath>
#include <filesystem>
#include <cwctype>

// Disable min/max macros from Windows headers
#undef min
#undef max

// Cache global pour l'icône
static ID2D1Bitmap *g_iconBitmap = nullptr;
static ID2D1Bitmap *g_titlebarGitHubBitmap = nullptr;
static ID2D1RenderTarget *g_titlebarGitHubCtx = nullptr;
static bool g_titlebarGitHubConnected = false;
static bool g_titlebarGitHubInit = false;
static DWORD g_titlebarGitHubLastCheckTick = 0;

// État global des menus
static std::vector<MenuItem> g_menuItems;
static MenuDropdown g_activeDropdown = {-1, std::vector<std::wstring>(), std::vector<std::wstring>(), std::vector<wchar_t>(),
    std::vector<bool>(), std::vector<bool>(), D2D1::RectF(), -1, false, 0, std::vector<bool>()};
static MenuDropdown g_subDropdown = {-1, std::vector<std::wstring>(), std::vector<std::wstring>(), std::vector<wchar_t>(),
    std::vector<bool>(), std::vector<bool>(), D2D1::RectF(), -1, false, 0, std::vector<bool>()};
static std::unordered_map<std::string, ID2D1Bitmap *> g_contextIconCache;
static ID2D1RenderTarget *g_contextIconCtx = nullptr;

static void ClearContextIconCache()
{
    for (auto &pair : g_contextIconCache)
    {
        if (pair.second)
            pair.second->Release();
    }
    g_contextIconCache.clear();
}

static std::string ContextMenuIconPathForLabel(const std::wstring &label)
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
    if (label.find(L"Rename") != std::wstring::npos)
        return "assets\\ressource\\icons\\folder-tools-open.svg";
    if (label.find(L"Delete") != std::wstring::npos)
        return "assets\\ressource\\icons\\folder-trash.svg";
    if (label.find(L"Renommer") != std::wstring::npos)
        return "assets\\ressource\\icons\\folder-tools-open.svg";
    if (label.find(L"Supprimer") != std::wstring::npos)
        return "assets\\ressource\\icons\\folder-trash.svg";
    return {};
}

static std::string TitleBarMenuIconPathForLabel(const std::wstring &label)
{
    if (label == L"File")
        return "assets/ressource/icons/folder-open.svg";
    return {};
}

static std::string MenuDropdownIconPathForLabel(const std::wstring &label)
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

static ID2D1Bitmap *GetContextMenuIconBitmap(ID2D1RenderTarget *ctx, const std::string &path, int pxSize, UINT dpi)
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

static bool IsGitHubConnectedForTitleBar()
{
    DWORD now = GetTickCount();
    if (!g_titlebarGitHubInit || (now - g_titlebarGitHubLastCheckTick) >= 1200)
    {
        g_titlebarGitHubConnected = GitHubAuth::HasToken();
        g_titlebarGitHubInit = true;
        g_titlebarGitHubLastCheckTick = now;
    }
    return g_titlebarGitHubConnected;
}

static ID2D1Bitmap *GetTitleBarGitHubBadgeIcon(ID2D1RenderTarget *ctx, UINT dpi)
{
    if (!ctx)
        return nullptr;

    if (g_titlebarGitHubCtx != ctx)
    {
        if (g_titlebarGitHubBitmap)
        {
            g_titlebarGitHubBitmap->Release();
            g_titlebarGitHubBitmap = nullptr;
        }
        g_titlebarGitHubCtx = ctx;
    }

    if (!g_titlebarGitHubBitmap)
    {
        int px = win32_dpi_scale(14, dpi);
        g_titlebarGitHubBitmap = GetExplorerManager().LoadSvgIconPublic(
            ctx, "assets\\ressource\\icons\\git.svg", px, dpi);
    }
    return g_titlebarGitHubBitmap;
}

// Helper pour charger l'icône
static std::wstring ResolveUiAssetPath(const wchar_t *filename)
{
    if (!filename || !*filename)
        return L"";

    std::filesystem::path requested(filename);
    if (requested.is_absolute())
        return requested.wstring();

    std::wstring generic = requested.generic_wstring();
    std::wstring lower = generic;
    for (wchar_t &ch : lower)
        ch = (wchar_t)towlower(ch);

    if (lower.rfind(L"assets/", 0) == 0)
    {
        std::filesystem::path rel = std::filesystem::path(generic).lexically_relative(std::filesystem::path(L"assets"));
        return NebulaAssetPath(rel).wstring();
    }

    return (NebulaExeDir() / requested).wstring();
}

static ID2D1Bitmap *LoadIconBitmap(ID2D1RenderTarget *ctx, const wchar_t *filename)
{
    if (g_iconBitmap)
        return g_iconBitmap;

    IWICImagingFactory *wicFactory = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory));
    if (!wicFactory)
        return nullptr;

    std::wstring resolvedPath = ResolveUiAssetPath(filename);
    IWICBitmapDecoder *decoder = nullptr;
    wicFactory->CreateDecoderFromFilename(resolvedPath.c_str(), NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);

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

    float left = std::floor(centerX - lineWidth / 2.0f) + 0.5f;
    float right = std::floor(centerX + lineWidth / 2.0f) + 0.5f;
    float y = std::floor(centerY) + 0.5f;
    D2D1_RECT_F lineRect = D2D1::RectF(left, y - 0.5f, right, y + 0.5f);

    ctx->FillRectangle(lineRect, brush);
}

// Fonction helper pour dessiner l'icône maximize (carré simple)
static void DrawMaximizeIcon(ID2D1RenderTarget *ctx, ID2D1SolidColorBrush *brush, D2D1_RECT_F rect)
{
    float centerX = std::round((rect.left + rect.right) / 2.0f);
    float centerY = std::round((rect.top + rect.bottom) / 2.0f);
    float size = 10.0f;

    float half = size / 2.0f;
    float left = std::floor(centerX - half) + 0.5f;
    float right = std::floor(centerX + half) + 0.5f;
    float top = std::floor(centerY - half) + 0.5f;
    float bottom = std::floor(centerY + half) + 0.5f;

    D2D1_RECT_F iconRect = D2D1::RectF(left, top, right, bottom);

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
static void DrawCloseIcon(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, ID2D1SolidColorBrush *brush, D2D1_RECT_F rect)
{
    if (!dwrite || !ctx || !brush)
        return;

    float height = rect.bottom - rect.top;
    float fontSize = std::clamp(height * 0.36f, 10.0f, height - 4.0f);

    IDWriteTextFormat *iconFormat = nullptr;
    if (SUCCEEDED(dwrite->CreateTextFormat(
            L"Segoe MDL2 Assets", NULL,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            fontSize, L"en-us", &iconFormat)))
    {
        iconFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        iconFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        const wchar_t glyph[2] = {0xE8BB, 0}; // Close icon

        D2D1_TEXT_ANTIALIAS_MODE prevTextAA = ctx->GetTextAntialiasMode();
        ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_ALIASED);
        ctx->DrawTextW(glyph, 1, iconFormat, rect, brush,
                       D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
        ctx->SetTextAntialiasMode(prevTextAA);

        iconFormat->Release();
    }
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
    (void)title;
    if (!ctx)
        return;

    RECT title_bar_rect = win32_titlebar_rect(hwnd);
    D2D1_RECT_F tb = D2D1::RectF((FLOAT)title_bar_rect.left, (FLOAT)title_bar_rect.top, (FLOAT)title_bar_rect.right, (FLOAT)title_bar_rect.bottom);

    D2D1_ANTIALIAS_MODE oldAA = ctx->GetAntialiasMode();
    D2D1_TEXT_ANTIALIAS_MODE oldTextAA = ctx->GetTextAntialiasMode();
    ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

    Window *window = GetWindowFromHwnd(hwnd);
    float aMin = window ? window->GetTitlebarHoverAlpha(Window::Hovered_Minimize) : (hoveredButton == Window::Hovered_Minimize ? 1.0f : 0.0f);
    float aMax = window ? window->GetTitlebarHoverAlpha(Window::Hovered_Maximize) : (hoveredButton == Window::Hovered_Maximize ? 1.0f : 0.0f);
    float aClose = window ? window->GetTitlebarHoverAlpha(Window::Hovered_Close) : (hoveredButton == Window::Hovered_Close ? 1.0f : 0.0f);
    float aRun = window ? window->GetTitlebarHoverAlpha(Window::Hovered_Run) : (hoveredButton == Window::Hovered_Run ? 1.0f : 0.0f);
    const D2D1_COLOR_F titlebarBg = UI::Theme::TitlebarBackground(hasFocus);
    const D2D1_COLOR_F titlebarBorder = UI::Theme::TitlebarBorder(hasFocus);
    const D2D1_COLOR_F titlebarText = UI::Theme::TitlebarText(hasFocus);
    const D2D1_COLOR_F titlebarIcon = UI::Theme::TitlebarIcon(hasFocus);
    const UI::Theme::Palette &themePalette = UI::Theme::GetPalette();
    const float titlebarHoverOpacity = (UI::Theme::GetMode() == UI::Theme::Mode::Light) ? 0.88f : 0.75f;
    if (window && window->IsNewProjectOverlayVisible())
    {
        // Minimal titlebar for the new-project screen (no editor menus).
        ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        ID2D1SolidColorBrush *bgBrush = nullptr;
        ID2D1SolidColorBrush *bottomBorder = nullptr;
        ctx->CreateSolidColorBrush(titlebarBg, &bgBrush);
        ctx->CreateSolidColorBrush(titlebarBorder, &bottomBorder);

        if (bgBrush)
            ctx->FillRectangle(tb, bgBrush);
        if (bottomBorder)
            ctx->DrawLine(D2D1::Point2F(tb.left, tb.bottom - 0.5f), D2D1::Point2F(tb.right, tb.bottom - 0.5f), bottomBorder, 1.0f);

        CustomTitleBarButtonRects button_rects = win32_get_title_bar_button_rects(hwnd, &title_bar_rect);
        D2D1_RECT_F rMin = D2D1::RectF((FLOAT)button_rects.minimize.left, (FLOAT)button_rects.minimize.top, (FLOAT)button_rects.minimize.right, (FLOAT)button_rects.minimize.bottom);
        D2D1_RECT_F rMax = D2D1::RectF((FLOAT)button_rects.maximize.left, (FLOAT)button_rects.maximize.top, (FLOAT)button_rects.maximize.right, (FLOAT)button_rects.maximize.bottom);
        D2D1_RECT_F rClose = D2D1::RectF((FLOAT)button_rects.close.left, (FLOAT)button_rects.close.top, (FLOAT)button_rects.close.right, (FLOAT)button_rects.close.bottom);

        ID2D1SolidColorBrush *hoverBrush = nullptr;
        ID2D1SolidColorBrush *closeHoverBrush = nullptr;
        D2D1_COLOR_F hoverColor = themePalette.explorerToolbarHover;
        hoverColor.a = titlebarHoverOpacity * aMin;
        ctx->CreateSolidColorBrush(hoverColor, &hoverBrush);
        ctx->CreateSolidColorBrush(D2D1::ColorF(0xe81123, 0.85f * aClose), &closeHoverBrush);

        if (hoverBrush && aMin > 0.01f)
            ctx->FillRectangle(rMin, hoverBrush);
        if (hoverBrush && aMax > 0.01f)
        {
            D2D1_COLOR_F maxHoverColor = themePalette.explorerToolbarHover;
            maxHoverColor.a = titlebarHoverOpacity * aMax;
            hoverBrush->SetColor(maxHoverColor);
            ctx->FillRectangle(rMax, hoverBrush);
        }
        if (closeHoverBrush && aClose > 0.01f)
            ctx->FillRectangle(rClose, closeHoverBrush);

        ID2D1SolidColorBrush *iconBrush = nullptr;
        ctx->CreateSolidColorBrush(titlebarIcon, &iconBrush);
        if (iconBrush)
        {
            ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
            DrawMinimizeIcon(ctx, iconBrush, rMin);
            if (win32_window_is_maximized(hwnd))
                DrawRestoreIcon(ctx, dwrite, iconBrush, rMax);
            else
                DrawMaximizeIcon(ctx, iconBrush, rMax);
            DrawCloseIcon(ctx, dwrite, iconBrush, rClose);
        }

        // App icon + title
        ID2D1Bitmap *iconBitmap = LoadIconBitmap(ctx, L"assets/favicon.ico");
        UINT dpi = win32_get_dpi_for_window(hwnd);
        float padding = (float)win32_dpi_scale(12, dpi);
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

        if (dwrite)
        {
            IDWriteTextFormat *titleFmt = nullptr;
            dwrite->CreateTextFormat(
                L"Segoe UI Variable Text",
                NULL,
                DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                12.5f,
                L"en-us",
                &titleFmt);
            if (titleFmt)
            {
                titleFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                titleFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                D2D1_RECT_F titleRect = D2D1::RectF(currentX, tb.top, rMax.left - 10.0f, tb.bottom);
                ID2D1SolidColorBrush *titleBrush = nullptr;
                ctx->CreateSolidColorBrush(titlebarText, &titleBrush);
                if (titleBrush)
                {
                    ctx->DrawTextW(L"Nebula - Commencez", 19, titleFmt, titleRect, titleBrush);
                    titleBrush->Release();
                }
                titleFmt->Release();
            }
        }

        if (iconBrush) iconBrush->Release();
        if (hoverBrush) hoverBrush->Release();
        if (closeHoverBrush) closeHoverBrush->Release();
        if (bottomBorder) bottomBorder->Release();
        if (bgBrush) bgBrush->Release();

        ctx->SetAntialiasMode(oldAA);
        ctx->SetTextAntialiasMode(oldTextAA);
        return;
    }

    // Background - dark theme
    ID2D1SolidColorBrush *bgBrush = nullptr;
    ctx->CreateSolidColorBrush(titlebarBg, &bgBrush);
    ctx->FillRectangle(tb, bgBrush);

    // Bottom border
    ID2D1SolidColorBrush *bottomBorder = nullptr;
    ctx->CreateSolidColorBrush(titlebarBorder, &bottomBorder);
    D2D1_POINT_2F leftPt = D2D1::Point2F(tb.left, tb.bottom - 0.5f);
    D2D1_POINT_2F rightPt = D2D1::Point2F(tb.right, tb.bottom - 0.5f);
    ctx->DrawLine(leftPt, rightPt, bottomBorder, 1.0f);

    CustomTitleBarButtonRects button_rects = win32_get_title_bar_button_rects(hwnd, &title_bar_rect);

    D2D1_RECT_F rMin = D2D1::RectF((FLOAT)button_rects.minimize.left, (FLOAT)button_rects.minimize.top, (FLOAT)button_rects.minimize.right, (FLOAT)button_rects.minimize.bottom);
    D2D1_RECT_F rMax = D2D1::RectF((FLOAT)button_rects.maximize.left, (FLOAT)button_rects.maximize.top, (FLOAT)button_rects.maximize.right, (FLOAT)button_rects.maximize.bottom);
    D2D1_RECT_F rClose = D2D1::RectF((FLOAT)button_rects.close.left, (FLOAT)button_rects.close.top, (FLOAT)button_rects.close.right, (FLOAT)button_rects.close.bottom);
    D2D1_RECT_F rRun = D2D1::RectF((FLOAT)button_rects.run.left, (FLOAT)button_rects.run.top, (FLOAT)button_rects.run.right, (FLOAT)button_rects.run.bottom);

    ID2D1SolidColorBrush *hoverBrush = nullptr;
    D2D1_COLOR_F hoverColor = themePalette.explorerToolbarHover;
    hoverColor.a = titlebarHoverOpacity * aMin;
    ctx->CreateSolidColorBrush(hoverColor, &hoverBrush);

    ID2D1SolidColorBrush *closeHoverBrush = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0xe81123, 0.85f * aClose), &closeHoverBrush);

    if (hoverBrush && aMin > 0.01f)
        ctx->FillRectangle(rMin, hoverBrush);
    if (hoverBrush && aRun > 0.01f)
    {
        D2D1_COLOR_F runHoverColor = themePalette.explorerToolbarHover;
        runHoverColor.a = titlebarHoverOpacity * aRun;
        hoverBrush->SetColor(runHoverColor);
        ctx->FillRectangle(rRun, hoverBrush);
    }
    if (hoverBrush && aMax > 0.01f)
    {
        D2D1_COLOR_F maxHoverColor = themePalette.explorerToolbarHover;
        maxHoverColor.a = titlebarHoverOpacity * aMax;
        hoverBrush->SetColor(maxHoverColor);
        ctx->FillRectangle(rMax, hoverBrush);
    }
    if (closeHoverBrush && aClose > 0.01f)
        ctx->FillRectangle(rClose, closeHoverBrush);

    UINT dpi = win32_get_dpi_for_window(hwnd);
    bool isMaximized = win32_window_is_maximized(hwnd);

    // Brush pour les icônes
    ID2D1SolidColorBrush *iconBrush = nullptr;
    ctx->CreateSolidColorBrush(titlebarIcon, &iconBrush);

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

        DrawCloseIcon(ctx, dwrite, iconBrush, rClose);
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
    std::vector<std::wstring> menus = {L"File", L"Edit", L"Tampon", L"Selection"};

    // Keep cached menu list in sync with the current top-level menu model.
    if (g_menuItems.size() != menus.size())
    {
        g_menuItems.clear();
        for (const auto &menu : menus)
        {
            g_menuItems.push_back({menu, D2D1::RectF(0, 0, 0, 0), false});
        }
    }

    ID2D1SolidColorBrush *menuTextBrush = nullptr;
    ID2D1SolidColorBrush *menuHoverBrush = nullptr;
    ctx->CreateSolidColorBrush(titlebarText, &menuTextBrush);
    ctx->CreateSolidColorBrush(themePalette.explorerToolbarHover, &menuHoverBrush);

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
                ctx->CreateSolidColorBrush(UI::Theme::PrimaryText(), &textBrushToUse);
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

    const bool githubConnected = IsGitHubConnectedForTitleBar();
    D2D1_RECT_F githubBadgeRect = D2D1::RectF(0, 0, 0, 0);
    if (githubConnected)
    {
        float badgeHeight = (float)win32_dpi_scale(22, dpi);
        float badgeWidth = (float)win32_dpi_scale(114, dpi);
        float badgeRight = rRun.left - (float)win32_dpi_scale(10, dpi);
        float badgeTop = std::round((tb.top + tb.bottom - badgeHeight) * 0.5f);
        githubBadgeRect = D2D1::RectF(
            std::round(badgeRight - badgeWidth),
            badgeTop,
            std::round(badgeRight),
            std::round(badgeTop + badgeHeight));
    }

    float titleLeft = currentX;
    float titleRight = githubConnected ? (githubBadgeRect.left - (float)win32_dpi_scale(10, dpi)) : rRun.left;
    float titleWidth = titleRight - titleLeft;
    // Reuse menuFormat to avoid double rendering and keep ClearType
    if (menuFormat && titleWidth > 40.0f)
    {
        D2D1_RECT_F titleRect = D2D1::RectF(
            std::round(titleLeft),
            std::round(tb.top),
            std::round(titleRight),
            std::round(tb.bottom));

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
        D2D1_COLOR_F titleColor = UI::Theme::TitlebarCenterTitle(hasFocus);
        ctx->CreateSolidColorBrush(titleColor, &titleBrush);

        // Créer un format temporaire avec CENTER alignment
        IDWriteTextFormat *centerFormat = nullptr;
        dwrite->CreateTextFormat(
            L"Segoe UI", NULL,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            12.0f, L"en-us", &centerFormat);

        if (centerFormat)
        {
            centerFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            centerFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

            D2D1_TEXT_ANTIALIAS_MODE prevTextAA = ctx->GetTextAntialiasMode();
            // Grayscale avoids color fringing on custom titlebars and often looks cleaner.
            ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            ctx->DrawTextW(displayTitle.c_str(), (UINT32)displayTitle.size(),
                           centerFormat, titleRect, titleBrush,
                           D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
            ctx->SetTextAntialiasMode(prevTextAA);

            centerFormat->Release();
        }

        if (titleBrush)
            titleBrush->Release();
    }

    if (githubConnected && menuFormat)
    {
        float badgeAlpha = hasFocus ? 1.0f : 0.82f;
        ID2D1SolidColorBrush *badgeBg = nullptr;
        ID2D1SolidColorBrush *badgeBorder = nullptr;
        ID2D1SolidColorBrush *badgeText = nullptr;
        ID2D1SolidColorBrush *badgeDot = nullptr;
        D2D1_COLOR_F badgeBgColor = themePalette.inputBackground;
        badgeBgColor.a = 0.95f * badgeAlpha;
        D2D1_COLOR_F badgeBorderColor = UI::Theme::ChromeBorder();
        badgeBorderColor.a = 0.90f * badgeAlpha;
        D2D1_COLOR_F badgeTextColor = UI::Theme::PrimaryText();
        badgeTextColor.a = badgeAlpha;
        D2D1_COLOR_F badgeDotColor = UI::Theme::AccentStrong();
        badgeDotColor.a = badgeAlpha;
        ctx->CreateSolidColorBrush(badgeBgColor, &badgeBg);
        ctx->CreateSolidColorBrush(badgeBorderColor, &badgeBorder);
        ctx->CreateSolidColorBrush(badgeTextColor, &badgeText);
        ctx->CreateSolidColorBrush(badgeDotColor, &badgeDot);

        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(githubBadgeRect, 6.0f, 6.0f);
        if (badgeBg)
            ctx->FillRoundedRectangle(rr, badgeBg);
        if (badgeBorder)
            ctx->DrawRoundedRectangle(rr, badgeBorder, 1.0f);

        ID2D1Bitmap *ghBmp = GetTitleBarGitHubBadgeIcon(ctx, dpi);
        if (ghBmp)
        {
            float iconSize = (float)win32_dpi_scale(14, dpi);
            float iconLeft = std::round(githubBadgeRect.left + (float)win32_dpi_scale(8, dpi));
            float iconTop = std::round((githubBadgeRect.top + githubBadgeRect.bottom - iconSize) * 0.5f);
            D2D1_RECT_F iconRect = D2D1::RectF(iconLeft, iconTop, iconLeft + iconSize, iconTop + iconSize);
            ctx->DrawBitmap(ghBmp, iconRect, badgeAlpha, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        }

        if (badgeText)
        {
            D2D1_RECT_F textRect = D2D1::RectF(
                std::round(githubBadgeRect.left + (float)win32_dpi_scale(28, dpi)),
                githubBadgeRect.top,
                std::round(githubBadgeRect.right - (float)win32_dpi_scale(14, dpi)),
                githubBadgeRect.bottom);
            ctx->DrawTextW(L"GitHub", 6, menuFormat, textRect, badgeText,
                           D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
        }

        if (badgeDot)
        {
            float cx = std::round(githubBadgeRect.right - (float)win32_dpi_scale(8, dpi));
            float cy = std::round((githubBadgeRect.top + githubBadgeRect.bottom) * 0.5f);
            float r = (float)win32_dpi_scale(2, dpi);
            ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), badgeDot);
        }

        if (badgeDot)
            badgeDot->Release();
        if (badgeText)
            badgeText->Release();
        if (badgeBorder)
            badgeBorder->Release();
        if (badgeBg)
            badgeBg->Release();
    }
    if (menuHoverBrush)
        menuHoverBrush->Release();
    if (menuTextBrush)
        menuTextBrush->Release();
    if (menuFormat)
        menuFormat->Release();
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
        // File menu with Open Recent submenu.
        g_activeDropdown.items = {L"New", L"New Window", L"Open...", L"Open Recent", L"Open Project", L"Close"};
        g_activeDropdown.shortcuts = {L"Ctrl+N", L"", L"Ctrl+O", L"", L"Ctrl+Shift+O", L"Ctrl+W"};
        g_activeDropdown.icons = {0xE710, 0xE8A7, 0xE8B7, 0xE8B7, 0xE8B7, 0xE8BB};
        g_activeDropdown.separators = {false, false, false, false, false, false};
        g_activeDropdown.hasSubmenu = {false, false, false, true, false, false};
        break;

    case 1:
        // Edit menu
        g_activeDropdown.items = {L"Undo", L"Cut", L"Copy", L"Paste", L"Delete", L"Select All"};
        g_activeDropdown.shortcuts = {L"Ctrl+Z", L"Ctrl+X", L"Ctrl+C", L"Ctrl+V", L"Del", L"Ctrl+A"};
        g_activeDropdown.icons.assign(g_activeDropdown.items.size(), 0);
        g_activeDropdown.separators.assign(g_activeDropdown.items.size(), false);
        g_activeDropdown.hasSubmenu.assign(g_activeDropdown.items.size(), false);
        g_activeDropdown.enabled.clear();
        g_activeDropdown.enabled.resize(g_activeDropdown.items.size(), true);
        break;

    case 2:
        // Tampon (template) menu
        g_activeDropdown.items = {L"Definir Tampon...", L"Effacer Tampon", L"Voir Tampon"};
        g_activeDropdown.shortcuts.assign(g_activeDropdown.items.size(), L"");
        g_activeDropdown.icons.assign(g_activeDropdown.items.size(), 0);
        g_activeDropdown.separators.assign(g_activeDropdown.items.size(), false);
        g_activeDropdown.hasSubmenu.assign(g_activeDropdown.items.size(), false);
        g_activeDropdown.enabled.clear();
        g_activeDropdown.enabled.resize(g_activeDropdown.items.size(), true);
        break;
    case 3:
        g_activeDropdown.items = {L"Select All", L"Expand Selection", L"Shrink Selection", L"Select Line"};
        g_activeDropdown.shortcuts = {L"Ctrl+A", L"Shift+Alt+Right", L"Shift+Alt+Left", L"Ctrl+L"};
        g_activeDropdown.icons.assign(g_activeDropdown.items.size(), 0);
        g_activeDropdown.separators.assign(g_activeDropdown.items.size(), false);
        g_activeDropdown.hasSubmenu.assign(g_activeDropdown.items.size(), false);
        break;
    case 4:
        // Add New Terminal as a View action
        g_activeDropdown.items = {L"New Terminal", L"Command Palette", L"Open View", L"Toggle Sidebar", L"Show Extensions", L"Keyboard Shortcuts"};
        g_activeDropdown.shortcuts = {L"Ctrl+`", L"Ctrl+Shift+P", L"", L"Ctrl+B", L"Ctrl+Shift+X", L"Ctrl+K, Ctrl+S"};
        g_activeDropdown.icons.assign(g_activeDropdown.items.size(), 0);
        g_activeDropdown.separators.assign(g_activeDropdown.items.size(), false);
        g_activeDropdown.hasSubmenu.assign(g_activeDropdown.items.size(), false);
        break;
    case 5:
        g_activeDropdown.items = {L"Go to File", L"Go to Line", L"Go to Symbol", L"Go to Definition"};
        g_activeDropdown.shortcuts = {L"Ctrl+P", L"Ctrl+G", L"Ctrl+Shift+O", L"F12"};
        g_activeDropdown.icons.assign(g_activeDropdown.items.size(), 0);
        g_activeDropdown.separators.assign(g_activeDropdown.items.size(), false);
        g_activeDropdown.hasSubmenu.assign(g_activeDropdown.items.size(), false);
        break;
    case 6:
        g_activeDropdown.items = {L"Start Debugging", L"Run", L"Stop", L"Restart Debugging", L"Step Over", L"Step Into"};
        g_activeDropdown.shortcuts = {L"F5", L"Ctrl+F5", L"Shift+F5", L"Ctrl+Shift+F5", L"F10", L"F11"};
        g_activeDropdown.icons.assign(g_activeDropdown.items.size(), 0);
        g_activeDropdown.separators.assign(g_activeDropdown.items.size(), false);
        g_activeDropdown.hasSubmenu.assign(g_activeDropdown.items.size(), false);
        break;
    case 7:
        // Help menu (moved from index 7 after removing Terminal)
        g_activeDropdown.items = {L"Welcome", L"Documentation", L"About", L"Release Notes", L"Report Issue"};
        g_activeDropdown.shortcuts = {L"", L"", L"", L"", L""};
        g_activeDropdown.icons.assign(g_activeDropdown.items.size(), 0);
        g_activeDropdown.separators.assign(g_activeDropdown.items.size(), false);
        g_activeDropdown.hasSubmenu.assign(g_activeDropdown.items.size(), false);
        break;
    default:
        g_activeDropdown.items = {L"Item 1", L"Item 2", L"Item 3", L"Item 4", L"Item 5", L"Item 6", L"Item 7", L"Item 8", L"Item 9", L"Item 10"};
        g_activeDropdown.shortcuts.assign(g_activeDropdown.items.size(), L"");
        g_activeDropdown.icons.assign(g_activeDropdown.items.size(), 0);
        g_activeDropdown.separators.assign(g_activeDropdown.items.size(), false);
        g_activeDropdown.hasSubmenu.assign(g_activeDropdown.items.size(), false);
        break;
    }

    if (g_activeDropdown.enabled.size() != g_activeDropdown.items.size())
        g_activeDropdown.enabled.assign(g_activeDropdown.items.size(), true);

    float itemHeight = 28.0f;
    float width = 210.0f;
    bool hasLongShortcut = false;
    for (const auto &sc : g_activeDropdown.shortcuts)
    {
        if (sc.size() >= 10)
        {
            hasLongShortcut = true;
            break;
        }
    }
    if (g_activeDropdown.menuIndex == 3)
    {
        width = 320.0f;
    }
    else if (hasLongShortcut)
    {
        width = 290.0f;
    }
    else if (g_activeDropdown.items.size() > 6)
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
    g_subDropdown.visible = false;
    g_subDropdown.hoveredItem = -1;
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
    const UI::Theme::Palette &themePalette = UI::Theme::GetPalette();
    const D2D1_COLOR_F dropdownBg = themePalette.inputBackground;
    const D2D1_COLOR_F dropdownBorder = themePalette.inputBorder;
    const D2D1_COLOR_F dropdownHover = themePalette.explorerToolbarHover;
    const D2D1_COLOR_F dropdownText = UI::Theme::PrimaryText();
    const D2D1_COLOR_F dropdownDisabled = UI::Theme::MutedText();
    const D2D1_COLOR_F dropdownSeparator = UI::Theme::ChromeBorder();

    if (g_activeDropdown.menuIndex == -1)
    {
        D2D1_RECT_F r = g_activeDropdown.rect;
        r = D2D1::RectF(std::round(r.left), std::round(r.top), std::round(r.right), std::round(r.bottom));
        g_activeDropdown.rect = r;

        ID2D1SolidColorBrush *shadowBrush = nullptr;
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.18f), &shadowBrush);
        ID2D1SolidColorBrush *bgBrush = nullptr;
        ctx->CreateSolidColorBrush(dropdownBg, &bgBrush);
        ID2D1SolidColorBrush *borderBrush = nullptr;
        ctx->CreateSolidColorBrush(dropdownBorder, &borderBrush);
        ID2D1SolidColorBrush *hoverBrush = nullptr;
        ctx->CreateSolidColorBrush(dropdownHover, &hoverBrush);
        ID2D1SolidColorBrush *textBrush = nullptr;
        ctx->CreateSolidColorBrush(dropdownText, &textBrush);
        ID2D1SolidColorBrush *disabledBrush = nullptr;
        ctx->CreateSolidColorBrush(dropdownDisabled, &disabledBrush);

        IDWriteTextFormat *textFormat = nullptr;
        if (dwrite)
        {
            dwrite->CreateTextFormat(
                L"Segoe UI Variable Text",
                NULL,
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
                    NULL,
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

        const float itemHeight = (r.bottom - r.top) / (g_activeDropdown.items.empty() ? 1.0f : (float)g_activeDropdown.items.size());
        const float iconSize = 16.0f;
        const bool drawIcons = true;
        const float iconColumnWidth = drawIcons ? 26.0f : 0.0f;
        const float leftPad = drawIcons ? 6.0f : 12.0f;
        const float rightPad = 12.0f;
        const float minWidth = 220.0f;
        const float maxWidth = 380.0f;

        if (textFormat && dwrite)
        {
            float maxTextWidth = 0.0f;
            for (const auto &label : g_activeDropdown.items)
            {
                IDWriteTextLayout *layout = nullptr;
                if (SUCCEEDED(dwrite->CreateTextLayout(label.c_str(), (UINT32)label.size(), textFormat, 1000.0f, itemHeight, &layout)) && layout)
                {
                    DWRITE_TEXT_METRICS metrics;
                    layout->GetMetrics(&metrics);
                    maxTextWidth = std::max(maxTextWidth, metrics.widthIncludingTrailingWhitespace);
                    layout->Release();
                }
            }

            float desiredWidth = maxTextWidth + iconColumnWidth + leftPad + rightPad;
            if (desiredWidth < minWidth)
                desiredWidth = minWidth;
            if (desiredWidth > maxWidth)
                desiredWidth = maxWidth;
            float currentWidth = r.right - r.left;
            if ((currentWidth - desiredWidth > 0.5f) || (desiredWidth - currentWidth > 0.5f))
            {
                r.right = r.left + desiredWidth;
                g_activeDropdown.rect = r;
            }
        }

        D2D1_ROUNDED_RECT shadowRounded = D2D1::RoundedRect(
            D2D1::RectF(r.left + 1.0f, r.top + 1.0f, r.right + 1.0f, r.bottom + 1.0f),
            4.0f, 4.0f);
        if (shadowBrush)
            ctx->FillRoundedRectangle(shadowRounded, shadowBrush);

        D2D1_ROUNDED_RECT bgRounded = D2D1::RoundedRect(r, 4.0f, 4.0f);
        if (bgBrush)
            ctx->FillRoundedRectangle(bgRounded, bgBrush);
        if (borderBrush)
        {
            D2D1_ROUNDED_RECT borderRounded = D2D1::RoundedRect(
                D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f),
                3.5f, 3.5f);
            ctx->DrawRoundedRectangle(borderRounded, borderBrush, 1.0f);
        }

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
            bool isSeparator = (!g_activeDropdown.separators.empty() && i < g_activeDropdown.separators.size() && g_activeDropdown.separators[i]);

            if (isSeparator)
            {
                float y = std::floor(itemRect.top + itemHeight * 0.5f) + 0.5f;
                ID2D1SolidColorBrush *sepBrush = nullptr;
                ctx->CreateSolidColorBrush(dropdownSeparator, &sepBrush);
                if (sepBrush)
                {
                    ctx->DrawLine(D2D1::Point2F(itemRect.left + 10.0f, y),
                                  D2D1::Point2F(itemRect.right - 10.0f, y), sepBrush, 1.0f);
                    sepBrush->Release();
                }
                continue;
            }

            if (isEnabled && (int)i == g_activeDropdown.hoveredItem)
            {
                D2D1_RECT_F hoverRect = D2D1::RectF(
                    itemRect.left + 4.0f, itemRect.top + 2.0f,
                    itemRect.right - 4.0f, itemRect.bottom - 2.0f);
                D2D1_ROUNDED_RECT hoverRounded = D2D1::RoundedRect(hoverRect, 3.0f, 3.0f);
                if (hoverBrush)
                    ctx->FillRoundedRectangle(hoverRounded, hoverBrush);
            }

            if (textFormat)
            {
                D2D1_RECT_F textRect = D2D1::RectF(itemRect.left + iconColumnWidth + leftPad, itemRect.top,
                                                   itemRect.right - rightPad, itemRect.bottom);
                ID2D1SolidColorBrush *brushToUse = isEnabled ? textBrush : disabledBrush;

                std::string iconPath = ContextMenuIconPathForLabel(g_activeDropdown.items[i]);
                if (drawIcons && !iconPath.empty())
                {
                    UINT dpi = 96;
                    if (ctx)
                    {
                        FLOAT dpiX = 96.0f, dpiY = 96.0f;
                        ctx->GetDpi(&dpiX, &dpiY);
                        dpi = (UINT)dpiX;
                    }

                    ID2D1Bitmap *iconBmp = GetContextMenuIconBitmap(ctx, iconPath, (int)iconSize, dpi);
                    if (iconBmp)
                    {
                        float iconLeft = itemRect.left + 6.0f;
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
                dwrite->CreateTextLayout(g_activeDropdown.items[i].c_str(), (UINT32)g_activeDropdown.items[i].size(),
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
        return;
    }

    D2D1_RECT_F r = g_activeDropdown.rect;

    // Theme-driven dropdown panel.
    ID2D1SolidColorBrush *bgBrush = nullptr;
    ctx->CreateSolidColorBrush(dropdownBg, &bgBrush);
    D2D1_ROUNDED_RECT bgRounded = D2D1::RoundedRect(r, 6.0f, 6.0f);
    ctx->FillRoundedRectangle(bgRounded, bgBrush);

    ID2D1SolidColorBrush *borderBrush = nullptr;
    ctx->CreateSolidColorBrush(dropdownBorder, &borderBrush);
    if (borderBrush)
    {
        ctx->DrawRoundedRectangle(bgRounded, borderBrush, 0.8f);
        borderBrush->Release();
    }

    ID2D1SolidColorBrush *hoverBrush = nullptr;
    ctx->CreateSolidColorBrush(dropdownHover, &hoverBrush);
    ID2D1SolidColorBrush *textBrush = nullptr;
    ctx->CreateSolidColorBrush(dropdownText, &textBrush);
    ID2D1SolidColorBrush *disabledBrush = nullptr;
    ctx->CreateSolidColorBrush(dropdownDisabled, &disabledBrush);
    // Icons removed for menu items

    IDWriteTextFormat *textFormat = nullptr;
    if (dwrite)
    {
        dwrite->CreateTextFormat(
            L"Segoe UI Variable Text",
            NULL,
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
                NULL,
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

    float itemHeight = (r.bottom - r.top) / (g_activeDropdown.items.empty() ? 1.0f : (float)g_activeDropdown.items.size());
    const float iconSize = 14.0f;
    const float iconPad = 10.0f;
    const float textPadLeft = 8.0f;
    float shortcutColWidth = 66.0f;
    for (const auto &sc : g_activeDropdown.shortcuts)
    {
        if (!sc.empty())
        {
            float approx = 10.0f + (float)sc.size() * 6.0f;
            shortcutColWidth = (std::max)(shortcutColWidth, (std::min)(approx, 150.0f));
        }
    }

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
        bool isSeparator = (!g_activeDropdown.separators.empty() && i < g_activeDropdown.separators.size() && g_activeDropdown.separators[i]);

        if (isSeparator)
        {
            float y = std::floor(itemRect.top + itemHeight * 0.5f) + 0.5f;
            ID2D1SolidColorBrush *sepBrush = nullptr;
            ctx->CreateSolidColorBrush(dropdownSeparator, &sepBrush);
            if (sepBrush)
            {
                ctx->DrawLine(D2D1::Point2F(itemRect.left + 10.0f, y),
                              D2D1::Point2F(itemRect.right - 10.0f, y), sepBrush, 1.0f);
                sepBrush->Release();
            }
            continue;
        }

        if (isEnabled && (int)i == g_activeDropdown.hoveredItem)
        {
            D2D1_RECT_F hoverRect = D2D1::RectF(itemRect.left + 6.0f, itemRect.top + 3.0f,
                                                itemRect.right - 6.0f, itemRect.bottom - 3.0f);
            D2D1_ROUNDED_RECT hoverRounded = D2D1::RoundedRect(hoverRect, 4.0f, 4.0f);
            ctx->FillRoundedRectangle(hoverRounded, hoverBrush);
        }

        if (textFormat)
        {
            const float rightPad = 12.0f;
            const float shortcutGap = 14.0f;
            D2D1_RECT_F shortcutRect = D2D1::RectF(itemRect.right - (shortcutColWidth + rightPad), itemRect.top,
                                                   itemRect.right - rightPad, itemRect.bottom);
            D2D1_RECT_F textRect = D2D1::RectF(itemRect.left + iconPad + iconSize + textPadLeft, itemRect.top,
                                               shortcutRect.left - shortcutGap, itemRect.bottom);

            ID2D1SolidColorBrush *brushToUse = isEnabled ? textBrush : disabledBrush;

            std::string iconPath = MenuDropdownIconPathForLabel(g_activeDropdown.items[i]);
            if (!iconPath.empty())
            {
                UINT dpi = 96;
                if (ctx)
                {
                    FLOAT dpiX = 96.0f, dpiY = 96.0f;
                    ctx->GetDpi(&dpiX, &dpiY);
                    dpi = (UINT)dpiX;
                }

                ID2D1Bitmap *iconBmp = GetContextMenuIconBitmap(ctx, iconPath, (int)iconSize, dpi);
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

            // Label with ellipsis
            IDWriteTextLayout *labelLayout = nullptr;
            dwrite->CreateTextLayout(g_activeDropdown.items[i].c_str(), (UINT32)g_activeDropdown.items[i].size(),
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

            // Shortcut (right aligned)
            if (!g_activeDropdown.shortcuts.empty() && i < g_activeDropdown.shortcuts.size() && !g_activeDropdown.shortcuts[i].empty())
            {
                IDWriteTextLayout *scLayout = nullptr;
                dwrite->CreateTextLayout(g_activeDropdown.shortcuts[i].c_str(), (UINT32)g_activeDropdown.shortcuts[i].size(),
                                         textFormat, shortcutRect.right - shortcutRect.left, itemHeight, &scLayout);
                if (scLayout)
                {
                    scLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                    scLayout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                    ctx->DrawTextLayout(D2D1::Point2F(shortcutRect.left, shortcutRect.top), scLayout, brushToUse);
                    scLayout->Release();
                }
            }

            // Chevron indicator for submenu (File -> Open Recent)
            bool hasSub = (!g_activeDropdown.hasSubmenu.empty() && i < g_activeDropdown.hasSubmenu.size() && g_activeDropdown.hasSubmenu[i]);
            if (hasSub)
            {
                D2D1_RECT_F chevronRect = D2D1::RectF(
                    itemRect.right - 22.0f,
                    itemRect.top,
                    itemRect.right - 8.0f,
                    itemRect.bottom);
                ctx->DrawTextW(L"\u203A", 1, textFormat, chevronRect, brushToUse,
                               D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
            }
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
    if (bgBrush)
        bgBrush->Release();
}

void ShowSubmenuDropdown(HWND hwnd, const std::vector<std::wstring> &items, D2D1_POINT_2F position, int baseId)
{
    // Match context submenu styling when invoked from context menus (baseId 9000..9099).
    g_subDropdown.menuIndex = (baseId >= 9000 && baseId < 9100) ? -1 : -2;
    g_subDropdown.visible = true;
    g_subDropdown.hoveredItem = -1;
    g_subDropdown.items = items;
    g_subDropdown.shortcuts.clear();
    g_subDropdown.icons.clear();
    g_subDropdown.separators.clear();
    g_subDropdown.hasSubmenu.clear();
    g_subDropdown.baseId = baseId;
    g_subDropdown.enabled.clear();
    g_subDropdown.enabled.resize(items.size(), true);

    float itemHeight = 28.0f;
    float width = 320.0f;
    float height = itemHeight * items.size();

    g_subDropdown.rect = D2D1::RectF(
        position.x,
        position.y,
        position.x + width,
        position.y + height);

    if (hwnd)
        InvalidateRect(hwnd, nullptr, FALSE);
}

void HideSubmenuDropdown(HWND hwnd)
{
    g_subDropdown.visible = false;
    g_subDropdown.hoveredItem = -1;
    if (hwnd)
        InvalidateRect(hwnd, nullptr, FALSE);
}

bool IsSubmenuDropdownVisible()
{
    return g_subDropdown.visible;
}

void DrawSubmenuDropdown(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite)
{
    if (!g_subDropdown.visible || !ctx)
        return;

    // Reuse the same rendering as the main dropdown
    MenuDropdown backup = g_activeDropdown;
    g_activeDropdown = g_subDropdown;
    DrawMenuDropdown(ctx, dwrite);
    g_activeDropdown = backup;
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
        if (!g_subDropdown.enabled.empty() && index < (int)g_subDropdown.enabled.size())
        {
            if (!g_subDropdown.enabled[index])
                return -1;
        }
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
        if (!g_activeDropdown.separators.empty() && index < (int)g_activeDropdown.separators.size())
        {
            if (g_activeDropdown.separators[index])
                return -1;
        }
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
    g_activeDropdown.enabled.clear();
    g_activeDropdown.enabled.resize(items.size(), true);

    float itemHeight = 28.0f;
    float width = 240.0f;
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

