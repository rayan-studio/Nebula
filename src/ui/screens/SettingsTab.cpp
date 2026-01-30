#include "SettingsTab.h"
#include "helpers/window_helpers.h"
#include "ui/layout/ExplorerLayoutState.h"

void SettingsTabView::UpdateLayout(HWND hwnd, float left, float top, float right, float bottom)
{
    bounds_ = D2D1::RectF(left, top, right, bottom);

    UINT dpi = win32_get_dpi_for_window(hwnd);
    float padding = (float)win32_dpi_scale(24, dpi);
    float titleH = (float)win32_dpi_scale(42, dpi);

    float rowTop = bounds_.top + padding + titleH + 8.0f;
    float rowHeight = (float)win32_dpi_scale(46, dpi);
    float rowLeft = bounds_.left + padding;
    float rowRight = bounds_.right - padding;

    rowRect_ = D2D1::RectF(rowLeft, rowTop, rowRight, rowTop + rowHeight);

    float toggleWidth = (float)win32_dpi_scale(110, dpi);
    float toggleHeight = (float)win32_dpi_scale(28, dpi);
    float toggleRight = rowRect_.right - 8.0f;
    float toggleLeft = toggleRight - toggleWidth;
    float toggleTop = rowRect_.top + (rowHeight - toggleHeight) * 0.5f;
    toggleRect_ = D2D1::RectF(toggleLeft, toggleTop, toggleRight, toggleTop + toggleHeight);
}

void SettingsTabView::Draw(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd)
{
    if (!ctx || !dwrite)
        return;

    D2D1_RECT_F clip = bounds_;
    ctx->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    ID2D1SolidColorBrush *bgBrush = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.09f, 0.09f, 0.09f), &bgBrush);
    if (bgBrush)
    {
        ctx->FillRectangle(bounds_, bgBrush);
        bgBrush->Release();
    }

    IDWriteTextFormat *titleFormat = nullptr;
    IDWriteTextFormat *labelFormat = nullptr;
    IDWriteTextFormat *toggleFormat = nullptr;

    dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             20.0f, L"en-us", &titleFormat);
    dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             13.0f, L"en-us", &labelFormat);
    dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             12.0f, L"en-us", &toggleFormat);

    if (titleFormat)
    {
        titleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        titleFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
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

    ID2D1SolidColorBrush *titleBrush = nullptr;
    ID2D1SolidColorBrush *labelBrush = nullptr;
    ID2D1SolidColorBrush *rowBgBrush = nullptr;
    ID2D1SolidColorBrush *rowBorderBrush = nullptr;
    ID2D1SolidColorBrush *toggleBgBrush = nullptr;
    ID2D1SolidColorBrush *toggleTextBrush = nullptr;

    ctx->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.95f, 0.95f), &titleBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.86f, 0.86f, 0.86f), &labelBrush);
    ctx->CreateSolidColorBrush(rowHovered_ ? D2D1::ColorF(0.16f, 0.16f, 0.16f) : D2D1::ColorF(0.13f, 0.13f, 0.13f), &rowBgBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.25f, 0.25f, 0.25f), &rowBorderBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.20f, 0.45f, 0.80f), &toggleBgBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f), &toggleTextBrush);

    if (titleFormat && titleBrush)
    {
        D2D1_RECT_F titleRect = D2D1::RectF(bounds_.left + 24.0f, bounds_.top + 18.0f, bounds_.right, bounds_.top + 52.0f);
        const wchar_t *title = L"Settings";
        ctx->DrawTextW(title, (UINT32)wcslen(title), titleFormat, titleRect, titleBrush);
    }

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

    const wchar_t *label = L"Explorer Position";
    if (labelFormat && labelBrush)
    {
        D2D1_RECT_F labelRect = D2D1::RectF(rowRect_.left + 12.0f, rowRect_.top, toggleRect_.left - 10.0f, rowRect_.bottom);
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

    if (titleFormat)
        titleFormat->Release();
    if (labelFormat)
        labelFormat->Release();
    if (toggleFormat)
        toggleFormat->Release();
    if (titleBrush)
        titleBrush->Release();
    if (labelBrush)
        labelBrush->Release();
    if (rowBgBrush)
        rowBgBrush->Release();
    if (rowBorderBrush)
        rowBorderBrush->Release();
    if (toggleBgBrush)
        toggleBgBrush->Release();
    if (toggleTextBrush)
        toggleTextBrush->Release();

    ctx->PopAxisAlignedClip();
}

void SettingsTabView::OnMouseMove(HWND hwnd, POINT clientPoint)
{
    bool wasHovered = rowHovered_;
    rowHovered_ = clientPoint.x >= rowRect_.left && clientPoint.x <= rowRect_.right &&
                  clientPoint.y >= rowRect_.top && clientPoint.y <= rowRect_.bottom;

    if (wasHovered != rowHovered_)
        InvalidateRect(hwnd, nullptr, FALSE);
}

void SettingsTabView::OnLeftButtonDown(HWND hwnd, POINT clientPoint)
{
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

void SettingsTabView::OnLeftButtonUp(HWND /*hwnd*/)
{
}

bool SettingsTabView::IsPointInView(POINT clientPoint) const
{
    return clientPoint.x >= bounds_.left && clientPoint.x <= bounds_.right &&
           clientPoint.y >= bounds_.top && clientPoint.y <= bounds_.bottom;
}
