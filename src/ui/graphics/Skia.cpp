#include "Skia.h"
#include <stdexcept>
#include <cmath>
#include <sstream>
#include "ui/components/menu/DropdownMenu.h"
#include "ui/components/titlebar/TitleBar.h"
#include "ui/components/sidebar/Sidebar.h"
#include "core/explorer/Explorer.h"
#include "ui/panels/PanelManager.h"
#include "ui/panels/terminal/TerminalPanel.h"
#include "ui/panels/ggwave/GGWavePanel.h"
#include "orion/editor/Editor.h"
#include "core/window/Window.h"
#include "ui/components/footer/Footer.h"
#include "helpers/window_helpers.h"
#include "ui/screens/Welcome.h"
#include "ui/screens/SettingsTab.h"
#include "ui/theme/Theme.h"
#include "utils/logger/Logger.h"
#include "ui/layout/ExplorerLayoutState.h"
#include "utils/update/UpdateService.h"

static void SafeRelease(IUnknown **p)
{
    if (p && *p)
    {
        (*p)->Release();
        *p = nullptr;
    }
}

Skia::Skia()
    : hwnd_(nullptr), pFactory_(nullptr), pRenderTarget_(nullptr), pDWriteFactory_(nullptr), pSKText_(nullptr), surfaceWidth_(0), surfaceHeight_(0) {}

Skia::~Skia()
{
    if (pSKText_)
        delete pSKText_;
    SafeRelease(reinterpret_cast<IUnknown **>(&pRenderTarget_));
    SafeRelease(reinterpret_cast<IUnknown **>(&pFactory_));
    SafeRelease(reinterpret_cast<IUnknown **>(&pDWriteFactory_));
}

bool Skia::Init(HWND hwnd)
{
    hwnd_ = hwnd;

    // Create D2D factory
    D2D1_FACTORY_OPTIONS options = {};
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &options, reinterpret_cast<void **>(&pFactory_));
    if (FAILED(hr) || !pFactory_)
        return false;

    // Create DWrite factory
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown **>(&pDWriteFactory_));
    if (FAILED(hr) || !pDWriteFactory_)
        return false;

    // Create a software DCRenderTarget (CPU raster) and bind to HDC at paint time
    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        96.0f,
        96.0f);

    hr = pFactory_->CreateDCRenderTarget(&rtProps, &pRenderTarget_);
    if (FAILED(hr) || !pRenderTarget_)
        return false;

    // Create SKText with render target
    pSKText_ = new SKText(pRenderTarget_, pDWriteFactory_);

    return true;
}

void Skia::Resize(UINT width, UINT height)
{
    surfaceWidth_ = width;
    surfaceHeight_ = height;
    (void)width;
    (void)height;
}

void Skia::Render(const std::wstring &text, HWND hwnd, int titlebarHoveredButton, bool titlebarHasFocus, const std::wstring &titleText, HDC hdc)
{
    (void)text; // currently not rendered; silence unused param warning
    if (!pRenderTarget_)
        return;
    // Ensure we have an HDC to bind. If caller provided one (from BeginPaint), use it.
    HDC localHdc = hdc;
    bool needRelease = false;
    if (!localHdc)
    {
        localHdc = GetDC(hwnd);
        needRelease = true;
    }

    RECT rc;
    GetClientRect(hwnd, &rc);
    pRenderTarget_->BindDC(localHdc, &rc);

    pRenderTarget_->BeginDraw();
    pRenderTarget_->Clear(UI::Theme::TitlebarBackground(titlebarHasFocus));

    DrawCustomTitleBarD2D(pRenderTarget_, pDWriteFactory_, hwnd, titlebarHoveredButton, titlebarHasFocus, titleText);

    Window* window = GetWindowFromHwnd(hwnd);

    if (window && window->IsNewProjectOverlayVisible())
    {
        RECT clientOverlay;
        GetClientRect(hwnd, &clientOverlay);
        window->DrawNewProjectOverlay(pRenderTarget_, pDWriteFactory_, clientOverlay);

        HRESULT hr = pRenderTarget_->EndDraw();
        if (FAILED(hr))
        {
            // ignore for now
        }
        if (needRelease)
            ReleaseDC(hwnd, localHdc);
        return;
    }

    // Sidebar GPU-rendered (sous la titlebar, à gauche)
    DrawSidebarD2D(pRenderTarget_, pDWriteFactory_, hwnd);

    // Panel layout is updated up-front; drawing happens inside the central shell.
    GetPanelManager().UpdateLayout(hwnd);

    if (window)
    {
        TabBar *tabBar = window->GetTabBar();

        RECT client;
        GetClientRect(hwnd, &client);
        RECT tbRect = win32_titlebar_rect(hwnd);
        UINT dpi = win32_get_dpi_for_window(hwnd);
        // Use the same sidebar width as Sidebar.cpp (scaled 52 logical px) to avoid overlap/gaps
        float sidebarWidth = static_cast<float>(win32_dpi_scale(52, dpi));
        int footerLogicalH = 28;
        int footerH = win32_dpi_scale(footerLogicalH, dpi);
        
        // Get active panel width from PanelManager
        float panelLeftWidth = 0.0f;
        float panelRightWidth = 0.0f;
        Panel* activePanel = GetPanelManager().GetActivePanel();
        if (activePanel && activePanel->IsVisible()) {
            float activeWidth = static_cast<float>(activePanel->GetState().physicalWidth);
            if (activePanel->GetId() == PanelId::Explorer &&
                GetExplorerLayoutState().placement == ExplorerPlacement::Right) {
                panelRightWidth = activeWidth;
            } else {
                panelLeftWidth = activeWidth;
            }
        }

        D2D1_RECT_F centralShellRect = D2D1::RectF(
            sidebarWidth,
            static_cast<float>(tbRect.bottom),
            static_cast<float>(client.right),
            static_cast<float>(client.bottom - footerH));
        const float centralCornerRadius = static_cast<float>(win32_dpi_scale(12, dpi));

        ID2D1SolidColorBrush *centralBgBrush = nullptr;
        ID2D1SolidColorBrush *centralBorderBrush = nullptr;
        ID2D1RoundedRectangleGeometry *centralClipGeometry = nullptr;
        ID2D1Layer *centralClipLayer = nullptr;

        pRenderTarget_->CreateSolidColorBrush(UI::Theme::ChromeBackground(), &centralBgBrush);
        D2D1_COLOR_F centralBorderColor = UI::Theme::ChromeBorder();
        centralBorderColor.a = 0.9f;
        pRenderTarget_->CreateSolidColorBrush(centralBorderColor, &centralBorderBrush);

        const D2D1_ROUNDED_RECT centralRoundedRect = D2D1::RoundedRect(
            centralShellRect,
            centralCornerRadius,
            centralCornerRadius);

        if (centralBgBrush)
            pRenderTarget_->FillRoundedRectangle(centralRoundedRect, centralBgBrush);
        if (centralBorderBrush)
            pRenderTarget_->DrawRoundedRectangle(centralRoundedRect, centralBorderBrush, 1.0f);

        if (pFactory_)
            pFactory_->CreateRoundedRectangleGeometry(&centralRoundedRect, &centralClipGeometry);
        if (centralClipGeometry)
            pRenderTarget_->CreateLayer(nullptr, &centralClipLayer);
        if (centralClipLayer && centralClipGeometry)
            pRenderTarget_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), centralClipGeometry), centralClipLayer);

        GetPanelManager().DrawActivePanel(pRenderTarget_, pDWriteFactory_, hwnd);

        // Dessiner la TabBar
        float tabBarLeft = sidebarWidth + panelLeftWidth;
        float tabBarTop = (float)tbRect.bottom;
        float tabBarRight = (float)client.right - panelRightWidth;

        tabBar->UpdateLayout(tabBarLeft, tabBarTop, tabBarRight);
        tabBar->Draw(pRenderTarget_, pDWriteFactory_, hwnd);

        // Dessiner l'éditeur actif
        int activeTabIndex = tabBar->GetActiveTabIndex();

        if (activeTabIndex >= 0)
        {
            UINT dpiInner = win32_get_dpi_for_window(hwnd);

            float editorLeft = tabBarLeft;
            float editorTop = tabBarTop + tabBar->GetHeight();
            float editorRight = tabBarRight;
            // Reserve footer area so editor content doesn't overlap it
            float editorBottom = (float)(client.bottom - footerH);

            // If terminal is visible, reserve a fixed terminal height and reduce editor space
            TerminalPanel& terminal = GetTerminalPanel();
            if (terminal.IsVisible()) {
                float termH = terminal.GetHeightPx();
                if (termH <= 0.0f)
                {
                    termH = (float)win32_dpi_scale(260, dpiInner);
                    terminal.SetHeightPx(termH);
                }
                // Prevent terminal from reaching the title bar by keeping a minimum editor area
                int minEditorLogicalH = 140;
                float minEditorH = (float)win32_dpi_scale(minEditorLogicalH, dpiInner);
                float maxTermH = editorBottom - (editorTop + minEditorH);
                if (maxTermH > 0.0f && termH > maxTermH)
                    termH = maxTermH;
                float termTop = editorBottom - termH;
                float termBottom = editorBottom;

                terminal.UpdateLayout(hwnd, editorLeft, termTop, editorRight, termBottom);
                editorBottom = termTop;
            }

            Orion::Editor *editor = nullptr;
            if (window->IsSettingsTabIndex(activeTabIndex))
            {
                SettingsTabView *settings = window->GetSettingsTabView();
                if (settings)
                {
                    settings->UpdateLayout(hwnd, editorLeft, editorTop, editorRight, editorBottom);
                    settings->Draw(pRenderTarget_, pDWriteFactory_, hwnd);
                }
            }
            else
            {
                editor = window->GetEditorForTab(activeTabIndex);
                if (editor)
                {
                    editor->UpdateLayout(hwnd, editorLeft, editorTop, editorRight, editorBottom);
                    editor->Draw(pRenderTarget_, pDWriteFactory_);
                }
            }

            // Draw terminal after editor/settings
            if (terminal.IsVisible()) {
                if (editor)
                {
                    std::vector<TerminalPanel::ProblemItem> problems;
                    std::wstring filePath = editor->GetFilePath();
                    std::wstring fileName = filePath;
                    size_t lastSlash = filePath.find_last_of(L"\/");
                    if (lastSlash != std::wstring::npos)
                        fileName = filePath.substr(lastSlash + 1);
                    for (const auto& diag : editor->GetDiagnostics())
                    {
                        TerminalPanel::ProblemItem item;
                        item.fileName = fileName.empty() ? L"<untitled>" : fileName;
                        item.line = diag.line + 1;
                        item.column = diag.startCol + 1;
                        item.isError = diag.isError;
                        item.message = diag.message;
                        item.suggestion = diag.suggestion;
                        problems.push_back(std::move(item));
                    }
                    terminal.SetProblems(filePath, problems);
                }
                terminal.Draw(pRenderTarget_, pDWriteFactory_, hwnd);
            }

            // Draw GGWave listener button (positioned from right edge)
            float footerTopForGGWave = (float)(client.bottom - footerH);
            GGWavePanel& ggwave = GetGGWavePanel();
            // Position relative to window width (right edge)
            float windowWidth = (float)client.right;
            ggwave.UpdateLayout(hwnd, footerTopForGGWave, windowWidth);
            ggwave.Draw(pRenderTarget_, pDWriteFactory_, hwnd);
        }
        else
        {
            // No tabs open -> use the dedicated welcome renderer
            float editorLeft = tabBarLeft;
            float editorTop = tabBarTop + tabBar->GetHeight();
            float editorRight = tabBarRight;
            float editorBottom = (float)(client.bottom - footerH);
            
            UINT dpiInner = win32_get_dpi_for_window(hwnd);
            float footerTop = (float)(client.bottom - footerH);
            
            // If terminal is visible, reserve a fixed terminal height and reduce welcome space
            TerminalPanel& terminal = GetTerminalPanel();
            if (terminal.IsVisible()) {
                float termH = terminal.GetHeightPx();
                if (termH <= 0.0f)
                {
                    termH = (float)win32_dpi_scale(260, dpiInner);
                    terminal.SetHeightPx(termH);
                }
                // Prevent terminal from reaching the title bar by keeping a minimum editor area
                int minEditorLogicalH = 140;
                float minEditorH = (float)win32_dpi_scale(minEditorLogicalH, dpiInner);
                float maxTermH = footerTop - (editorTop + minEditorH);
                if (maxTermH > 0.0f && termH > maxTermH)
                    termH = maxTermH;
                float termTop = footerTop - termH;
                float termBottom = footerTop;

                terminal.UpdateLayout(hwnd, editorLeft, termTop, editorRight, termBottom);
                editorBottom = termTop;
            }
            
            UI::DrawWelcomeD2D(pRenderTarget_, pDWriteFactory_, hwnd, window->GetCustomFontPath(), editorLeft, editorTop, editorRight, editorBottom);
            
            // Draw terminal after welcome
            if (terminal.IsVisible()) {
                terminal.SetProblems(L"", {});
                terminal.Draw(pRenderTarget_, pDWriteFactory_, hwnd);
            }
            
            // Draw GGWave listener button (positioned from right edge)
            float windowWidth = (float)client.right;
            GGWavePanel& ggwave = GetGGWavePanel();
            ggwave.UpdateLayout(hwnd, footerTop, windowWidth);
            ggwave.Draw(pRenderTarget_, pDWriteFactory_, hwnd);
        }

        if (centralClipLayer && centralClipGeometry)
            pRenderTarget_->PopLayer();

        SafeRelease(reinterpret_cast<IUnknown **>(&centralClipLayer));
        SafeRelease(reinterpret_cast<IUnknown **>(&centralClipGeometry));
        SafeRelease(reinterpret_cast<IUnknown **>(&centralBorderBrush));
        SafeRelease(reinterpret_cast<IUnknown **>(&centralBgBrush));
    }

    // Always draw footer (even if no file/editor is open)
    {
        std::wstring fp = L"";
        int ln = 0, col = 0;
        std::wstring enc = L"";
        if (window)
        {
            TabBar *tabBar = window->GetTabBar();
            int activeTabIndex = tabBar ? tabBar->GetActiveTabIndex() : -1;
            if (activeTabIndex >= 0)
            {
                Orion::Editor *editor = window->GetEditorForTab(activeTabIndex);
                if (editor)
                {
                    fp = editor->GetFilePath();
                    Orion::CaretPosition c = editor->GetCaret();
                    ln = c.line;
                    col = c.column;
                    enc = editor->GetEncoding();
                }
            }
        }
        DrawFooterD2D(pRenderTarget_, pDWriteFactory_, hwnd, fp, ln, col, enc);
    }

    if (window)
    {
        if (UpdateService::GetState() == UpdateService::State::UpdateAvailable)
        {
            UpdateService::LatestInfo latest = UpdateService::GetLatestInfo();
            UINT dpi = win32_get_dpi_for_window(hwnd);
            float margin = (float)win32_dpi_scale(12, dpi);
            float badgeH = (float)win32_dpi_scale(46, dpi);
            float footerH = (float)win32_dpi_scale(28, dpi);
            float sidebarW = (float)win32_dpi_scale(52, dpi);
            float panelLeftW = 0.0f;
            Panel *activePanel = GetPanelManager().GetActivePanel();
            if (activePanel && activePanel->IsVisible())
            {
                bool explorerOnRight = activePanel->GetId() == PanelId::Explorer &&
                                       GetExplorerLayoutState().placement == ExplorerPlacement::Right;
                if (!explorerOnRight)
                    panelLeftW = (float)activePanel->GetState().physicalWidth;
            }

            float badgeLeft = sidebarW + panelLeftW + margin;
            float badgeBottom = (float)rc.bottom - footerH - margin;
            float maxBadgeW = (float)rc.right - badgeLeft - margin;
            bool canDrawBadge = maxBadgeW > (float)win32_dpi_scale(140, dpi);
            float preferredBadgeW = (float)win32_dpi_scale(340, dpi);
            float badgeW = 0.0f;
            if (canDrawBadge)
            {
                badgeW = (std::max)((float)win32_dpi_scale(220, dpi), (std::min)(preferredBadgeW, maxBadgeW));
                if (badgeW > maxBadgeW)
                {
                    badgeLeft = margin;
                    maxBadgeW = (float)rc.right - badgeLeft - margin;
                    badgeW = (std::min)(badgeW, maxBadgeW);
                }
                canDrawBadge = badgeW > (float)win32_dpi_scale(140, dpi);
            }
            if (!canDrawBadge)
            {
                window->ClearUpdateToastRect();
            }
            else
            {
                D2D1_RECT_F badgeRect = D2D1::RectF(badgeLeft, badgeBottom - badgeH, badgeLeft + badgeW, badgeBottom);
                window->SetUpdateToastRect(badgeRect);

                const bool hovered = window->IsUpdateToastHovered();
                const UI::Theme::Palette &themePalette = UI::Theme::GetPalette();
                ID2D1SolidColorBrush *bgBrush = nullptr;
                ID2D1SolidColorBrush *borderBrush = nullptr;
                ID2D1SolidColorBrush *accentBrush = nullptr;
                ID2D1SolidColorBrush *textBrush = nullptr;
                ID2D1SolidColorBrush *mutedBrush = nullptr;
                auto blendColor = [](D2D1_COLOR_F a, D2D1_COLOR_F b, float t) -> D2D1_COLOR_F
                {
                    t = (std::max)(0.0f, (std::min)(1.0f, t));
                    return D2D1::ColorF(
                        a.r + (b.r - a.r) * t,
                        a.g + (b.g - a.g) * t,
                        a.b + (b.b - a.b) * t,
                        1.0f);
                };
                D2D1_COLOR_F toastBg = blendColor(UI::Theme::ChromeBackground(), themePalette.inputBackground, hovered ? 0.48f : 0.38f);
                toastBg.a = 0.985f;
                pRenderTarget_->CreateSolidColorBrush(toastBg, &bgBrush);
                D2D1_COLOR_F toastBorder = UI::Theme::ChromeBorder();
                toastBorder.a = hovered ? 0.82f : 0.62f;
                pRenderTarget_->CreateSolidColorBrush(toastBorder, &borderBrush);
                D2D1_COLOR_F accentColor = UI::Theme::Accent();
                accentColor.a = hovered ? 1.0f : 0.92f;
                pRenderTarget_->CreateSolidColorBrush(accentColor, &accentBrush);
                pRenderTarget_->CreateSolidColorBrush(UI::Theme::PrimaryText(), &textBrush);
                pRenderTarget_->CreateSolidColorBrush(UI::Theme::MutedText(), &mutedBrush);

                const float corner = (float)win32_dpi_scale(10, dpi);
                if (bgBrush)
                    pRenderTarget_->FillRoundedRectangle(D2D1::RoundedRect(badgeRect, corner, corner), bgBrush);
                if (borderBrush)
                    pRenderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(badgeRect, corner, corner), borderBrush, 1.0f);
                if (accentBrush)
                {
                    const float dotRadius = (float)win32_dpi_scale(4, dpi);
                    const float dotX = badgeRect.left + (float)win32_dpi_scale(16, dpi);
                    const float dotY = badgeRect.top + (badgeRect.bottom - badgeRect.top) * 0.5f;
                    pRenderTarget_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dotX, dotY), dotRadius, dotRadius), accentBrush);
                }

                std::wstring title = L"Mise a jour disponible";
                if (!latest.version.empty())
                    title = L"Version " + latest.version + L" disponible";
                std::wstring sub = L"Cliquer pour installer et redemarrer";

                IDWriteTextFormat *titleFmt = nullptr;
                IDWriteTextFormat *subFmt = nullptr;
                pDWriteFactory_->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                                  DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                  11.5f, L"fr-fr", &titleFmt);
                pDWriteFactory_->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                  DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                  10.5f, L"fr-fr", &subFmt);
                if (titleFmt)
                {
                    titleFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                    titleFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                    titleFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                }
                if (subFmt)
                {
                    subFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                    subFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                    subFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                }

                if (textBrush && mutedBrush && titleFmt && subFmt)
                {
                    float textLeft = badgeRect.left + (float)win32_dpi_scale(28, dpi);
                    D2D1_RECT_F titleRect = D2D1::RectF(textLeft, badgeRect.top + (float)win32_dpi_scale(7, dpi),
                                                        badgeRect.right - margin, badgeRect.top + (float)win32_dpi_scale(23, dpi));
                    D2D1_RECT_F subRect = D2D1::RectF(textLeft, badgeRect.top + (float)win32_dpi_scale(21, dpi),
                                                      badgeRect.right - margin, badgeRect.bottom - (float)win32_dpi_scale(5, dpi));
                    pRenderTarget_->DrawTextW(title.c_str(), (UINT32)title.size(), titleFmt, titleRect, textBrush);
                    pRenderTarget_->DrawTextW(sub.c_str(), (UINT32)sub.size(), subFmt, subRect, mutedBrush);
                }

                if (titleFmt)
                    titleFmt->Release();
                if (subFmt)
                    subFmt->Release();
                if (bgBrush)
                    bgBrush->Release();
                if (borderBrush)
                    borderBrush->Release();
                if (accentBrush)
                    accentBrush->Release();
                if (textBrush)
                    textBrush->Release();
                if (mutedBrush)
                    mutedBrush->Release();
            }
        }
        else
        {
            window->ClearUpdateToastRect();
        }
    }

    // Dessiner le dropdown par-dessus tout
    DrawMenuDropdown(pRenderTarget_, pDWriteFactory_);
    DrawSubmenuDropdown(pRenderTarget_, pDWriteFactory_);


    HRESULT hr = pRenderTarget_->EndDraw();
    if (FAILED(hr))
    {
        std::wstringstream ss;
        ss << L"EndDraw failed hr=0x" << std::hex << (unsigned long)hr;
        Logger::Instance().Log(L"[Skia] " + ss.str());
    }

    // Welcome icon drawing is handled by the D2D welcome renderer earlier.

    if (needRelease)
        ReleaseDC(hwnd, localHdc);
}

// --- SKText implementation ---
SKText::SKText(ID2D1RenderTarget *target, IDWriteFactory *dwrite)
    : pTarget_(target), pTextFormat_(nullptr), pBrush_(nullptr), initialized_(false), fixedLayoutRect_({0, 0, 0, 0})
{
    if (pTarget_)
        pTarget_->AddRef();

    if (dwrite)
    {
        pDWriteFactory_ = dwrite;
        const float baseSize = 48.0f;
        pDWriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, baseSize, L"en-us", &pTextFormat_);
        currentFontSize_ = baseSize;
        // Disable word-wrapping so the text doesn't reflow when the window width changes.
        if (pTextFormat_)
        {
            pTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            pTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            pTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
    }

    if (pTarget_)
    {
        pTarget_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &pBrush_);
    }
}

SKText::~SKText()
{
    SafeRelease(reinterpret_cast<IUnknown **>(&pBrush_));
    SafeRelease(reinterpret_cast<IUnknown **>(&pTextFormat_));
    SafeRelease(reinterpret_cast<IUnknown **>(&pTarget_));
}

void SKText::Draw(const std::wstring &text)
{
    if (!pTarget_)
        return;

    // Use a fixed layout rectangle so the text adapts into it instead of
    // growing with the entire window surface.
    D2D1_SIZE_F rtSize = pTarget_->GetSize();
    const float margin = 20.0f;
    const float maxLayoutWidth = 760.0f;
    const float maxLayoutHeight = 240.0f;

    float availWidth = rtSize.width - 2.0f * margin;
    if (availWidth < 0)
        availWidth = 0;
    float layoutWidth = std::min(availWidth, maxLayoutWidth);
    float layoutHeight = std::min(rtSize.height - 2.0f * margin, maxLayoutHeight);
    if (layoutHeight < 0)
        layoutHeight = 0;

    D2D1_RECT_F layoutRect;
    layoutRect.left = margin;
    layoutRect.top = margin;
    layoutRect.right = margin + layoutWidth;
    layoutRect.bottom = margin + layoutHeight;

    if (pBrush_ && pDWriteFactory_)
    {
        const float baseFontSize = 48.0f;
        const float minFontSize = 12.0f;

        // Binary-search the largest font size that fits within layoutWidth/Height
        float lo = minFontSize;
        float hi = baseFontSize;
        float best = lo;

        while (hi - lo > 0.5f)
        {
            float mid = (lo + hi) * 0.5f;

            IDWriteTextFormat *tmpFormat = nullptr;
            HRESULT hrFmt = pDWriteFactory_->CreateTextFormat(
                L"Segoe UI",
                nullptr,
                DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                mid,
                L"en-us",
                &tmpFormat);
            if (SUCCEEDED(hrFmt) && tmpFormat)
            {
                tmpFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                tmpFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                tmpFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

                IDWriteTextLayout *layout = nullptr;
                HRESULT hrLayout = pDWriteFactory_->CreateTextLayout(
                    text.c_str(),
                    static_cast<UINT32>(text.size()),
                    tmpFormat,
                    layoutWidth,
                    layoutHeight,
                    &layout);
                if (SUCCEEDED(hrLayout) && layout)
                {
                    DWRITE_TEXT_METRICS metrics = {};
                    layout->GetMetrics(&metrics);
                    if (metrics.height <= layoutHeight + 0.5f && metrics.width <= layoutWidth + 0.5f)
                    {
                        best = mid;
                        lo = mid;
                    }
                    else
                    {
                        hi = mid;
                    }
                    layout->Release();
                }
                else
                {
                    hi = mid;
                }
                tmpFormat->Release();
            }
            else
            {
                hi = mid;
            }
        }

        // Recreate the persistent text format only if it changed
        if (!pTextFormat_ || fabs(best - currentFontSize_) > 0.5f)
        {
            SafeRelease(reinterpret_cast<IUnknown **>(&pTextFormat_));
            if (SUCCEEDED(pDWriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, best, L"en-us", &pTextFormat_)))
            {
                currentFontSize_ = best;
                if (pTextFormat_)
                {
                    pTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                    pTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                    pTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                }
            }
        }

        // Draw using a text layout so wrapping is honored and metrics matched
        if (pTextFormat_)
        {
            IDWriteTextLayout *finalLayout = nullptr;
            if (SUCCEEDED(pDWriteFactory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), pTextFormat_, layoutWidth, layoutHeight, &finalLayout)) && finalLayout)
            {
                D2D1_POINT_2F origin = {layoutRect.left, layoutRect.top};
                pTarget_->DrawTextLayout(origin, finalLayout, pBrush_);
                finalLayout->Release();
            }
        }
    }
}
