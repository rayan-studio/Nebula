#include "SettingsPanel.h"
#include "helpers/window_helpers.h"
#include "ui/layout/ExplorerLayoutState.h"

SettingsPanel::SettingsPanel()
    : Panel(PanelId::Settings)
{
    config_ = PanelConfig(
        PanelId::Settings,
        L"\uE713",
        L"Settings",
        true,
        true,
        100);
    title_ = L"SETTINGS";
}

void SettingsPanel::Initialize()
{
}

void SettingsPanel::UpdateLayout(HWND hwnd)
{
    UINT dpi = win32_get_dpi_for_window(hwnd);
    int sidebarWidth = win32_dpi_scale(52, dpi);
    UpdateBaseLayout(hwnd, static_cast<float>(sidebarWidth));

    if (!visible_)
    {
        state_.physicalWidth = 0;
        state_.rightEdge = state_.leftEdge;
        return;
    }

    float rowTop = state_.topEdge + state_.titleHeight + 16.0f;
    float rowHeight = 44.0f;
    float rowLeft = state_.leftEdge + state_.leftPadding;
    float rowRight = state_.rightEdge - state_.leftPadding;
    rowRect_ = D2D1::RectF(rowLeft, rowTop, rowRight, rowTop + rowHeight);

    float toggleWidth = 100.0f;
    float toggleHeight = 26.0f;
    float toggleRight = rowRect_.right - 6.0f;
    float toggleLeft = toggleRight - toggleWidth;
    float toggleTop = rowRect_.top + (rowHeight - toggleHeight) * 0.5f;
    toggleRect_ = D2D1::RectF(toggleLeft, toggleTop, toggleRight, toggleTop + toggleHeight);
}

void SettingsPanel::Draw(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND /*hwnd*/)
{
    if (!visible_ || state_.physicalWidth <= 0)
        return;

    D2D1_RECT_F clipRect = D2D1::RectF(
        state_.leftEdge,
        state_.topEdge,
        state_.rightEdge,
        state_.bottomEdge);

    ctx->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    Panel::DrawBackground(ctx);
    Panel::DrawTitle(ctx, dwrite);

    ID2D1SolidColorBrush *rowBgBrush = nullptr;
    ID2D1SolidColorBrush *rowBorderBrush = nullptr;
    ID2D1SolidColorBrush *labelBrush = nullptr;
    ID2D1SolidColorBrush *toggleBgBrush = nullptr;
    ID2D1SolidColorBrush *toggleTextBrush = nullptr;

    D2D1_COLOR_F rowBg = rowHovered_ ? D2D1::ColorF(0.16f, 0.16f, 0.16f) : D2D1::ColorF(0.13f, 0.13f, 0.13f);
    ctx->CreateSolidColorBrush(rowBg, &rowBgBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.25f, 0.25f, 0.25f), &rowBorderBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.85f, 0.85f, 0.85f), &labelBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.20f, 0.45f, 0.80f), &toggleBgBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f), &toggleTextBrush);

    if (rowBgBrush)
    {
        D2D1_ROUNDED_RECT rowRounded = D2D1::RoundedRect(rowRect_, 6.0f, 6.0f);
        ctx->FillRoundedRectangle(rowRounded, rowBgBrush);
    }
    if (rowBorderBrush)
    {
        D2D1_ROUNDED_RECT rowRounded = D2D1::RoundedRect(rowRect_, 6.0f, 6.0f);
        ctx->DrawRoundedRectangle(rowRounded, rowBorderBrush, 1.0f);
    }

    IDWriteTextFormat *labelFormat = nullptr;
    IDWriteTextFormat *toggleFormat = nullptr;
    dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             12.5f, L"en-us", &labelFormat);
    dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             12.0f, L"en-us", &toggleFormat);

    if (labelFormat)
    {
        labelFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        labelFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (toggleFormat)
    {
        toggleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        toggleFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    const wchar_t *label = L"Explorer Position";
    if (labelFormat && labelBrush)
    {
        D2D1_RECT_F labelRect = D2D1::RectF(
            rowRect_.left + 12.0f,
            rowRect_.top,
            toggleRect_.left - 10.0f,
            rowRect_.bottom);
        ctx->DrawTextW(label, (UINT32)wcslen(label), labelFormat, labelRect, labelBrush);
    }

    ExplorerPlacement placement = GetExplorerLayoutState().placement;
    const wchar_t *value = placement == ExplorerPlacement::Right ? L"Right" : L"Left";

    if (toggleBgBrush)
    {
        D2D1_ROUNDED_RECT toggleRounded = D2D1::RoundedRect(toggleRect_, 12.0f, 12.0f);
        ctx->FillRoundedRectangle(toggleRounded, toggleBgBrush);
    }
    if (toggleFormat && toggleTextBrush)
    {
        ctx->DrawTextW(value, (UINT32)wcslen(value), toggleFormat, toggleRect_, toggleTextBrush);
    }

    if (labelFormat)
        labelFormat->Release();
    if (toggleFormat)
        toggleFormat->Release();
    if (rowBgBrush)
        rowBgBrush->Release();
    if (rowBorderBrush)
        rowBorderBrush->Release();
    if (labelBrush)
        labelBrush->Release();
    if (toggleBgBrush)
        toggleBgBrush->Release();
    if (toggleTextBrush)
        toggleTextBrush->Release();

    Panel::DrawRightBorder(ctx);
    ctx->PopAxisAlignedClip();
}

void SettingsPanel::OnMouseMove(HWND hwnd, POINT clientPoint)
{
    bool wasHovered = rowHovered_;
    rowHovered_ = clientPoint.x >= rowRect_.left && clientPoint.x <= rowRect_.right &&
                  clientPoint.y >= rowRect_.top && clientPoint.y <= rowRect_.bottom;

    bool stateChanged = HandleResizeMouseMove(hwnd, clientPoint);
    if (state_.isResizing || state_.isHoveringResizeZone)
        return;

    if (wasHovered != rowHovered_ || stateChanged)
        InvalidateRect(hwnd, nullptr, FALSE);
}

void SettingsPanel::OnLeftButtonDown(HWND hwnd, POINT clientPoint)
{
    if (HandleResizeLeftButtonDown(hwnd, clientPoint))
        return;

    bool hitRow = clientPoint.x >= rowRect_.left && clientPoint.x <= rowRect_.right &&
                  clientPoint.y >= rowRect_.top && clientPoint.y <= rowRect_.bottom;
    if (hitRow)
    {
        ExplorerLayoutState &layout = GetExplorerLayoutState();
        layout.placement = layout.placement == ExplorerPlacement::Left
                               ? ExplorerPlacement::Right
                               : ExplorerPlacement::Left;
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

void SettingsPanel::OnLeftButtonUp(HWND hwnd)
{
    HandleResizeLeftButtonUp(hwnd);
}
