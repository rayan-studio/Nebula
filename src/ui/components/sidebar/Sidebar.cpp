#include "Sidebar.h"
#include "helpers/window_helpers.h"
#include "ui/panels/PanelManager.h"
#include "core/explorer/Explorer.h"
#include "core/window/Window.h"
#include "ui/panels/terminal/TerminalPanel.h"
#include "ui/theme/Theme.h"
#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>
#include <algorithm>
#include <cwctype>

namespace
{
bool EndsWithSvgInsensitive(const std::wstring &value)
{
    if (value.size() < 4)
        return false;
    const size_t base = value.size() - 4;
    return (std::towlower(value[base + 0]) == L'.' &&
            std::towlower(value[base + 1]) == L's' &&
            std::towlower(value[base + 2]) == L'v' &&
            std::towlower(value[base + 3]) == L'g');
}

std::string WideToUtf8Local(const std::wstring &text)
{
    if (text.empty())
        return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0)
        return {};
    std::string out((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), len, nullptr, nullptr);
    return out;
}

struct SidebarSvgCacheEntry
{
    ID2D1RenderTarget *target = nullptr;
    std::wstring path;
    UINT dpi = 0;
    int px = 0;
    ID2D1Bitmap *bitmap = nullptr;
};

ID2D1Bitmap *GetCachedSidebarSvg(ID2D1RenderTarget *ctx, const std::wstring &path, int px, UINT dpi)
{
    static std::vector<SidebarSvgCacheEntry> cache;
    if (!ctx || path.empty() || px <= 0)
        return nullptr;

    for (auto it = cache.begin(); it != cache.end();)
    {
        if (it->target != ctx)
        {
            if (it->bitmap)
                it->bitmap->Release();
            it = cache.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (auto &entry : cache)
    {
        if (entry.target == ctx && entry.path == path && entry.dpi == dpi && entry.px == px)
            return entry.bitmap;
    }

    std::string utf8Path = WideToUtf8Local(path);
    if (utf8Path.empty())
        return nullptr;

    ID2D1Bitmap *bitmap = GetExplorerManager().LoadSvgIconPublic(ctx, utf8Path, px, dpi);
    if (!bitmap)
        return nullptr;

    SidebarSvgCacheEntry entry;
    entry.target = ctx;
    entry.path = path;
    entry.dpi = dpi;
    entry.px = px;
    entry.bitmap = bitmap;
    cache.push_back(entry);
    return bitmap;
}

void DrawSidebarBitmapMasked(ID2D1RenderTarget *ctx, ID2D1Bitmap *bitmap, const D2D1_RECT_F &dst, ID2D1SolidColorBrush *brush)
{
    if (!ctx || !bitmap || !brush)
        return;

    D2D1_SIZE_F srcSize = bitmap->GetSize();
    D2D1_RECT_F srcRect = D2D1::RectF(0.0f, 0.0f, srcSize.width, srcSize.height);
    ctx->FillOpacityMask(bitmap, brush, D2D1_OPACITY_MASK_CONTENT_GRAPHICS, dst, srcRect);
}
}

// ============================================================================
// SidebarRenderer Implementation
// ============================================================================

SidebarRenderer& SidebarRenderer::Instance()
{
    static SidebarRenderer instance;
    return instance;
}

int SidebarRenderer::GetPhysicalWidth(HWND hwnd) const
{
    UINT dpi = win32_get_dpi_for_window(hwnd);
    return win32_dpi_scale(logicalWidth_, dpi);
}

void SidebarRenderer::UpdateItemRects(HWND hwnd)
{
    UINT dpi = win32_get_dpi_for_window(hwnd);
    RECT tbRect = win32_titlebar_rect(hwnd);
    RECT clientRect;
    GetClientRect(hwnd, &clientRect);
    
    int sidebarWidth = GetPhysicalWidth(hwnd);
    int footerH = win32_dpi_scale(28, dpi);
    
    float sbLeft = static_cast<float>(clientRect.left);
    float sbTop = static_cast<float>(tbRect.bottom);
    float sbBottom = static_cast<float>(clientRect.bottom - footerH);
    float sbRight = sbLeft + static_cast<float>(sidebarWidth);
    
    float sidePadding = (sidebarWidth - bgSize_) * 0.5f;
    
    // Get panels from manager
    auto configs = GetPanelManager().GetPanelConfigs();
    itemStates_.clear();
    
    // Separate top and bottom items
    std::vector<PanelConfig> topItems;
    std::vector<PanelConfig> bottomItems;
    
    for (const auto& cfg : configs) {
        if (cfg.pinToBottom)
            bottomItems.push_back(cfg);
        else
            topItems.push_back(cfg);
    }
    
    // Calculate positions for top items
    float currentY = sbTop + topPadding_;
    
    for (const auto& cfg : topItems) {
        SidebarItemState state;
        state.panelId = cfg.id;
        state.isActive = GetPanelManager().IsPanelActive(cfg.id);
        
        float centerY = currentY + (itemSize_ * 0.5f);
        
        // Hit rect (full clickable area)
        state.hitRect = D2D1::RectF(
            sbLeft,
            currentY,
            sbRight,
            currentY + itemSize_);
        
        // Background rect (smaller, centered)
        state.bgRect = D2D1::RectF(
            sbLeft + sidePadding,
            centerY - (bgSize_ * 0.5f),
            sbLeft + sidePadding + bgSize_,
            centerY + (bgSize_ * 0.5f));
        
        itemStates_.push_back(state);
        currentY += itemSize_ + spacing_;
    }
    
    // Calculate positions for bottom items (from bottom up)
    float bottomY = sbBottom - bottomPadding_;
    
    for (auto it = bottomItems.rbegin(); it != bottomItems.rend(); ++it) {
        const auto& cfg = *it;
        
        SidebarItemState state;
        state.panelId = cfg.id;
        state.isActive = GetPanelManager().IsPanelActive(cfg.id);
        
        float itemTop = bottomY - itemSize_;
        float centerY = itemTop + (itemSize_ * 0.5f);
        
        // Hit rect
        state.hitRect = D2D1::RectF(
            sbLeft,
            itemTop,
            sbRight,
            bottomY);
        
        // Background rect
        state.bgRect = D2D1::RectF(
            sbLeft + sidePadding,
            centerY - (bgSize_ * 0.5f),
            sbLeft + sidePadding + bgSize_,
            centerY + (bgSize_ * 0.5f));
        
        itemStates_.push_back(state);
        bottomY = itemTop - spacing_;
    }

    // Output button (always available, above bottom items)
    {
        float itemTop = bottomY - itemSize_;
        float centerY = itemTop + (itemSize_ * 0.5f);
        outputHitRect_ = D2D1::RectF(
            sbLeft,
            itemTop,
            sbRight,
            bottomY);

        outputBgRect_ = D2D1::RectF(
            sbLeft + sidePadding,
            centerY - (bgSize_ * 0.5f),
            sbLeft + sidePadding + bgSize_,
            centerY + (bgSize_ * 0.5f));

        outputVisible_ = (itemTop >= sbTop);
    }
}

int SidebarRenderer::HitTest(HWND /*hwnd*/, POINT clientPoint) const
{
    for (size_t i = 0; i < itemStates_.size(); ++i) {
        const auto& state = itemStates_[i];
        if (clientPoint.x >= state.hitRect.left && clientPoint.x <= state.hitRect.right &&
            clientPoint.y >= state.hitRect.top && clientPoint.y <= state.hitRect.bottom) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool SidebarRenderer::HitTestOutput(POINT clientPoint) const
{
    if (!outputVisible_)
        return false;
    return (clientPoint.x >= outputHitRect_.left && clientPoint.x <= outputHitRect_.right &&
            clientPoint.y >= outputHitRect_.top && clientPoint.y <= outputHitRect_.bottom);
}

void SidebarRenderer::Draw(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite, HWND hwnd)
{
    if (!ctx) return;
    
    UpdateItemRects(hwnd);
    
    UINT dpi = win32_get_dpi_for_window(hwnd);
    RECT tbRect = win32_titlebar_rect(hwnd);
    RECT clientRect;
    GetClientRect(hwnd, &clientRect);
    
    int sidebarWidth = GetPhysicalWidth(hwnd);
    int footerH = win32_dpi_scale(28, dpi);
    
    D2D1_RECT_F sbRect = D2D1::RectF(
        static_cast<float>(clientRect.left),
        static_cast<float>(tbRect.bottom),
        static_cast<float>(clientRect.left + sidebarWidth),
        static_cast<float>(clientRect.bottom - footerH));
    
    // Background
    ID2D1SolidColorBrush* bgBrush = nullptr;
    ctx->CreateSolidColorBrush(UI::Theme::ChromeBackground(), &bgBrush);
    if (bgBrush) {
        ctx->FillRectangle(sbRect, bgBrush);
        bgBrush->Release();
    }
    
    // Border
    ID2D1SolidColorBrush* borderBrush = nullptr;
    ctx->CreateSolidColorBrush(UI::Theme::ChromeBorder(), &borderBrush);
    
    // Create brushes
    ID2D1SolidColorBrush* iconNormalBrush = nullptr;
    ID2D1SolidColorBrush* iconHoverBrush = nullptr;
    ID2D1SolidColorBrush* iconActiveBrush = nullptr;
    ID2D1SolidColorBrush* hoverBgBrush = nullptr;
    ID2D1SolidColorBrush* activeBgBrush = nullptr;
    ID2D1SolidColorBrush* indicatorBrush = nullptr;
    
    const UI::Theme::Palette &palette = UI::Theme::GetPalette();
    ctx->CreateSolidColorBrush(palette.sidebarIconNormal, &iconNormalBrush);
    ctx->CreateSolidColorBrush(palette.sidebarIconHover, &iconHoverBrush);
    ctx->CreateSolidColorBrush(palette.sidebarIconActive, &iconActiveBrush);
    ctx->CreateSolidColorBrush(palette.sidebarHoverBg, &hoverBgBrush);
    ctx->CreateSolidColorBrush(palette.sidebarActiveBg, &activeBgBrush);
    ctx->CreateSolidColorBrush(palette.sidebarIndicator, &indicatorBrush);
    
    // Icon font
    IDWriteTextFormat* iconFormat = nullptr;
    dwrite->CreateTextFormat(L"Segoe Fluent Icons", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             iconSize_, L"en-us", &iconFormat);
    if (!iconFormat) {
        dwrite->CreateTextFormat(L"Segoe MDL2 Assets", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                 iconSize_, L"en-us", &iconFormat);
    }
    if (iconFormat) {
        iconFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        iconFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    
    // Get panel configs for icons
    auto configs = GetPanelManager().GetPanelConfigs();
    
    // Draw each item
    for (size_t i = 0; i < itemStates_.size(); ++i) {
        auto& state = itemStates_[i];
        state.isHovered = (static_cast<int>(i) == hoveredIndex_);
        state.isActive = GetPanelManager().IsPanelActive(state.panelId);
        
        // Find config for this panel
        const PanelConfig* cfg = nullptr;
        for (const auto& c : configs) {
            if (c.id == state.panelId) {
                cfg = &c;
                break;
            }
        }
        if (!cfg) continue;
        
        // Draw background
        if (state.isActive) {
            Panel* panel = GetPanelManager().GetPanel(state.panelId);
            if (panel && panel->IsVisible()) {
                D2D1_ROUNDED_RECT roundedBg = D2D1::RoundedRect(state.bgRect, 4.0f, 4.0f);
                ctx->FillRoundedRectangle(roundedBg, activeBgBrush);
                
                // Active indicator (blue bar on left)
                D2D1_ROUNDED_RECT indicator = D2D1::RoundedRect(
                    D2D1::RectF(
                        state.bgRect.left,
                        state.bgRect.top + 6.0f,
                        state.bgRect.left + 2.5f,
                        state.bgRect.bottom - 6.0f),
                    1.5f, 1.5f);
                ctx->FillRoundedRectangle(indicator, indicatorBrush);
            }
        } else if (state.isHovered) {
            D2D1_ROUNDED_RECT roundedBg = D2D1::RoundedRect(state.bgRect, 4.0f, 4.0f);
            ctx->FillRoundedRectangle(roundedBg, hoverBgBrush);
        }
        
        // Draw icon
        bool isActiveAndVisible = false;
        if (state.isActive) {
            Panel* panel = GetPanelManager().GetPanel(state.panelId);
            isActiveAndVisible = panel && panel->IsVisible();
        }

        bool drawnSvg = false;
        if (EndsWithSvgInsensitive(cfg->icon))
        {
            int iconPx = win32_dpi_scale((int)iconSize_, dpi);
            ID2D1Bitmap *bitmap = GetCachedSidebarSvg(ctx, cfg->icon, iconPx, dpi);
            if (bitmap)
            {
                float cx = (state.hitRect.left + state.hitRect.right) * 0.5f;
                float cy = (state.hitRect.top + state.hitRect.bottom) * 0.5f;
                D2D1_RECT_F dst = D2D1::RectF(
                    cx - (iconSize_ * 0.5f),
                    cy - (iconSize_ * 0.5f),
                    cx + (iconSize_ * 0.5f),
                    cy + (iconSize_ * 0.5f));
                ID2D1SolidColorBrush *iconBrush = isActiveAndVisible ? iconActiveBrush :
                                                  (state.isHovered ? iconHoverBrush : iconNormalBrush);
                DrawSidebarBitmapMasked(ctx, bitmap, dst, iconBrush);
                drawnSvg = true;
            }
        }

        if (!drawnSvg && iconFormat) {
            ID2D1SolidColorBrush* brush = isActiveAndVisible ? iconActiveBrush :
                                          (state.isHovered ? iconHoverBrush : iconNormalBrush);
            ctx->DrawTextW(cfg->icon.c_str(), static_cast<UINT32>(cfg->icon.length()),
                          iconFormat, state.hitRect, brush);
        }
    }

    // Output item (terminal logs)
    if (outputVisible_ && iconFormat) {
        bool isActiveAndVisible = GetTerminalPanel().IsVisible();
        ID2D1SolidColorBrush* brush = isActiveAndVisible ? iconActiveBrush :
                                      (outputHovered_ ? iconHoverBrush : iconNormalBrush);

        if (isActiveAndVisible) {
            D2D1_ROUNDED_RECT roundedBg = D2D1::RoundedRect(outputBgRect_, 4.0f, 4.0f);
            ctx->FillRoundedRectangle(roundedBg, activeBgBrush);

            D2D1_ROUNDED_RECT indicator = D2D1::RoundedRect(
                D2D1::RectF(
                    outputBgRect_.left,
                    outputBgRect_.top + 6.0f,
                    outputBgRect_.left + 2.5f,
                    outputBgRect_.bottom - 6.0f),
                1.5f, 1.5f);
            ctx->FillRoundedRectangle(indicator, indicatorBrush);
        } else if (outputHovered_) {
            D2D1_ROUNDED_RECT roundedBg = D2D1::RoundedRect(outputBgRect_, 4.0f, 4.0f);
            ctx->FillRoundedRectangle(roundedBg, hoverBgBrush);
        }

        const std::wstring outputIcon = L"\uE756"; // Command Prompt
        ctx->DrawTextW(outputIcon.c_str(), (UINT32)outputIcon.size(),
                       iconFormat, outputHitRect_, brush);
    }
    
    // Always draw the right border of sidebar (divider between sidebar and content area)
    {
        float borderX = sbRect.right - 1.0f;
        D2D1_POINT_2F p1 = D2D1::Point2F(borderX, sbRect.top);
        D2D1_POINT_2F p2 = D2D1::Point2F(borderX, sbRect.bottom);
        
        D2D1_ANTIALIAS_MODE oldAA = ctx->GetAntialiasMode();
        ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        ctx->DrawLine(p1, p2, borderBrush, 1.0f);
        ctx->SetAntialiasMode(oldAA);
    }
    
    // Cleanup
    if (iconFormat) iconFormat->Release();
    if (indicatorBrush) indicatorBrush->Release();
    if (activeBgBrush) activeBgBrush->Release();
    if (hoverBgBrush) hoverBgBrush->Release();
    if (iconActiveBrush) iconActiveBrush->Release();
    if (iconHoverBrush) iconHoverBrush->Release();
    if (iconNormalBrush) iconNormalBrush->Release();
    if (borderBrush) borderBrush->Release();
}

bool SidebarRenderer::HandleLeftClick(HWND hwnd, POINT clientPoint)
{
    int sidebarWidth = GetPhysicalWidth(hwnd);
    RECT tbRect = win32_titlebar_rect(hwnd);
    
    // Must be in sidebar area
    if (clientPoint.y < tbRect.bottom)
        return false;
    if (clientPoint.x < 0 || clientPoint.x > sidebarWidth)
        return false;
    
    UpdateItemRects(hwnd);
    if (HitTestOutput(clientPoint))
    {
        TerminalPanel& terminal = GetTerminalPanel();
        bool isVisible = terminal.IsVisible();
        bool isOutputVisible = terminal.IsOutputVisible();

        if (isVisible && isOutputVisible)
        {
            terminal.SetVisible(false);
        }
        else
        {
            terminal.SetVisible(true);
            terminal.ShowOutput(true);
            terminal.EnsureSessionExists(hwnd);
            terminal.EnsureActiveInit(hwnd);
            terminal.SetFocused(true);
        }

        SetFocus(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }

    int hitIndex = HitTest(hwnd, clientPoint);
    
    if (hitIndex >= 0 && hitIndex < static_cast<int>(itemStates_.size())) {
        PanelId clickedPanel = itemStates_[hitIndex].panelId;

        if (clickedPanel == PanelId::Settings) {
            PostMessageW(hwnd, WM_OPEN_SETTINGS, 0, 0);

            if (GetPanelManager().IsPanelActive(PanelId::Settings)) {
                GetPanelManager().SetActivePanel(PanelId::Explorer);
            }

            Panel* settingsPanel = GetPanelManager().GetPanel(PanelId::Settings);
            if (settingsPanel) {
                settingsPanel->SetVisible(false);
            }

            SetFocus(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return true;
        }
        
        // CORRECTION : Vérifier si c'est le panel actif ET visible
        bool isCurrentlyActive = GetPanelManager().IsPanelActive(clickedPanel);
        Panel* panel = GetPanelManager().GetPanel(clickedPanel);
        bool isCurrentlyVisible = panel && panel->IsVisible();
        
        if (isCurrentlyActive && isCurrentlyVisible) {
            // Si déjà actif ET visible -> juste toggle visibility (garder actif)
            panel->SetVisible(false);
        } else if (isCurrentlyActive && !isCurrentlyVisible) {
            // Si actif mais caché -> rendre visible
            panel->SetVisible(true);
        } else {
            // Pas le panel actif -> switch to this panel ET rendre visible
            GetPanelManager().SetActivePanel(clickedPanel);
            panel = GetPanelManager().GetPanel(clickedPanel);
            if (panel) {
                panel->SetVisible(true);
            }
        }
        
        // Ensure focus for keyboard input
        SetFocus(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }
    
    return false;
}

void SidebarRenderer::UpdateHover(HWND hwnd, POINT clientPoint)
{
    int sidebarWidth = GetPhysicalWidth(hwnd);
    RECT tbRect = win32_titlebar_rect(hwnd);
    
    int oldHovered = hoveredIndex_;
    hoveredIndex_ = -1;
    bool prevOutput = outputHovered_;
    outputHovered_ = false;
    
    if (clientPoint.x >= 0 && clientPoint.x <= sidebarWidth && clientPoint.y >= tbRect.bottom) {
        UpdateItemRects(hwnd);
        hoveredIndex_ = HitTest(hwnd, clientPoint);
        outputHovered_ = HitTestOutput(clientPoint);
    }
    
    if (oldHovered != hoveredIndex_ || prevOutput != outputHovered_) {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

// ============================================================================
// Backward compatibility functions
// ============================================================================

void DrawSidebarD2D(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite, HWND hwnd)
{
    SidebarRenderer::Instance().Draw(ctx, dwrite, hwnd);
}

bool HandleSidebarLeftClick(HWND hwnd, POINT clientPoint)
{
    return SidebarRenderer::Instance().HandleLeftClick(hwnd, clientPoint);
}

void UpdateSidebarHover(HWND hwnd, POINT clientPoint)
{
    SidebarRenderer::Instance().UpdateHover(hwnd, clientPoint);
}
