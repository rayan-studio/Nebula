#include "SettingsTab.h"
#include "helpers/window_helpers.h"
#include "ui/layout/ExplorerLayoutState.h"
#include "core/window/Window.h"
#include <algorithm>

SettingsTabView::SettingsTabView()
{
    tamponInput_.SetPlaceholder(L"Ex: // Cree avec Nebula");
    tamponInput_.SetText(GetTamponText());
    tamponInput_.onTextChanged = [](const std::wstring &text)
    {
        if (text.empty())
            ClearTamponText();
        else
            SetTamponText(text);
    };

    auto &style = tamponInput_.GetStyle();
    style.backgroundColor = D2D1::ColorF(0.11f, 0.11f, 0.11f);
    style.borderColor = D2D1::ColorF(0.28f, 0.28f, 0.28f);
    style.focusBorderColor = D2D1::ColorF(0.24f, 0.57f, 0.92f);
    style.textColor = D2D1::ColorF(0.95f, 0.95f, 0.95f);
    style.placeholderColor = D2D1::ColorF(0.60f, 0.60f, 0.60f);
    style.selectionColor = D2D1::ColorF(0.20f, 0.57f, 1.0f, 0.26f);
    style.cursorColor = D2D1::ColorF(0.95f, 0.95f, 0.95f);
    style.fontFamily = L"Segoe UI Variable Text";
    style.multiline = true;
}

void SettingsTabView::UpdateLayout(HWND hwnd, float left, float top, float right, float bottom)
{
    bounds_ = D2D1::RectF(left, top, right, bottom);

    UINT dpi = win32_get_dpi_for_window(hwnd);
    float padding = (float)win32_dpi_scale(24, dpi);
    float titleH = (float)win32_dpi_scale(42, dpi);
    float scale = (float)dpi / 96.0f;

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

    float sectionTop = rowRect_.bottom + (float)win32_dpi_scale(16, dpi);
    float minSectionHeight = (float)win32_dpi_scale(170, dpi);
    float sectionBottom = (std::max)(sectionTop + minSectionHeight, bounds_.bottom - padding);
    tamponSectionRect_ = D2D1::RectF(rowLeft, sectionTop, rowRight, sectionBottom);

    float inputTop = tamponSectionRect_.top + (float)win32_dpi_scale(50, dpi);
    float inputHeight = (std::max)((float)win32_dpi_scale(72, dpi),
                                   tamponSectionRect_.bottom - inputTop - (float)win32_dpi_scale(12, dpi));
    float inputPad = (float)win32_dpi_scale(12, dpi);
    tamponInputRect_ = D2D1::RectF(
        tamponSectionRect_.left + inputPad,
        inputTop,
        tamponSectionRect_.right - inputPad,
        inputTop + inputHeight);
    tamponInput_.SetRect(tamponInputRect_);

    auto &style = tamponInput_.GetStyle();
    style.cornerRadius = 7.0f * scale;
    style.fontSize = 13.0f * scale;
    style.padding = 10.0f * scale;
}

void SettingsTabView::Draw(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd)
{
    if (!ctx || !dwrite)
        return;

    const std::wstring &tampon = GetTamponText();
    if (!tamponInput_.IsFocused() && tamponInput_.GetText() != tampon)
        tamponInput_.SetText(tampon);

    D2D1_RECT_F clip = bounds_;
    ctx->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    D2D1_TEXT_ANTIALIAS_MODE oldTextAA = ctx->GetTextAntialiasMode();
    ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

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
    IDWriteTextFormat *descFormat = nullptr;
    IDWriteTextFormat *stateFormat = nullptr;

    dwrite->CreateTextFormat(L"Segoe UI Variable Text", NULL, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             20.0f, L"en-us", &titleFormat);
    dwrite->CreateTextFormat(L"Segoe UI Variable Text", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             13.0f, L"en-us", &labelFormat);
    dwrite->CreateTextFormat(L"Segoe UI Variable Text", NULL, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             12.0f, L"en-us", &toggleFormat);
    dwrite->CreateTextFormat(L"Segoe UI Variable Text", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             12.0f, L"en-us", &descFormat);
    dwrite->CreateTextFormat(L"Segoe UI Variable Text", NULL, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             11.0f, L"en-us", &stateFormat);

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
    if (descFormat)
    {
        descFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        descFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    if (stateFormat)
    {
        stateFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        stateFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    ID2D1SolidColorBrush *titleBrush = nullptr;
    ID2D1SolidColorBrush *labelBrush = nullptr;
    ID2D1SolidColorBrush *descBrush = nullptr;
    ID2D1SolidColorBrush *rowBgBrush = nullptr;
    ID2D1SolidColorBrush *rowBorderBrush = nullptr;
    ID2D1SolidColorBrush *toggleBgBrush = nullptr;
    ID2D1SolidColorBrush *toggleTextBrush = nullptr;
    ID2D1SolidColorBrush *sectionBgBrush = nullptr;
    ID2D1SolidColorBrush *sectionBorderBrush = nullptr;
    ID2D1SolidColorBrush *stateBrush = nullptr;

    ctx->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.95f, 0.95f), &titleBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.86f, 0.86f, 0.86f), &labelBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.64f, 0.64f, 0.64f), &descBrush);
    ctx->CreateSolidColorBrush(rowHovered_ ? D2D1::ColorF(0.16f, 0.16f, 0.16f) : D2D1::ColorF(0.13f, 0.13f, 0.13f), &rowBgBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.25f, 0.25f, 0.25f), &rowBorderBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.20f, 0.45f, 0.80f), &toggleBgBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f), &toggleTextBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.12f, 0.12f), &sectionBgBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.24f, 0.24f, 0.24f), &sectionBorderBrush);
    ctx->CreateSolidColorBrush(HasTamponText() ? D2D1::ColorF(0.36f, 0.78f, 0.49f) : D2D1::ColorF(0.70f, 0.70f, 0.70f), &stateBrush);

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

    if (sectionBgBrush)
    {
        D2D1_ROUNDED_RECT sectionRounded = D2D1::RoundedRect(tamponSectionRect_, 8.0f, 8.0f);
        ctx->FillRoundedRectangle(sectionRounded, sectionBgBrush);
    }
    if (sectionBorderBrush)
    {
        D2D1_ROUNDED_RECT sectionRounded = D2D1::RoundedRect(tamponSectionRect_, 8.0f, 8.0f);
        ctx->DrawRoundedRectangle(sectionRounded, sectionBorderBrush, 1.0f);
    }

    if (labelFormat && labelBrush)
    {
        const wchar_t *tamponTitle = L"Tampon des nouveaux fichiers";
        D2D1_RECT_F titleRect = D2D1::RectF(
            tamponSectionRect_.left + 12.0f,
            tamponSectionRect_.top + 10.0f,
            tamponSectionRect_.right - 120.0f,
            tamponSectionRect_.top + 32.0f);
        ctx->DrawTextW(tamponTitle, (UINT32)wcslen(tamponTitle), labelFormat, titleRect, labelBrush);
    }
    if (stateFormat && stateBrush)
    {
        const wchar_t *stateText = HasTamponText() ? L"Actif" : L"Desactive";
        D2D1_RECT_F stateRect = D2D1::RectF(
            tamponSectionRect_.right - 110.0f,
            tamponSectionRect_.top + 10.0f,
            tamponSectionRect_.right - 12.0f,
            tamponSectionRect_.top + 32.0f);
        ctx->DrawTextW(stateText, (UINT32)wcslen(stateText), stateFormat, stateRect, stateBrush);
    }
    if (descFormat && descBrush)
    {
        const wchar_t *desc = L"Texte ajoute au debut de chaque nouveau fichier (Entree = nouvelle ligne, vide = desactive).";
        D2D1_RECT_F descRect = D2D1::RectF(
            tamponSectionRect_.left + 12.0f,
            tamponSectionRect_.top + 30.0f,
            tamponSectionRect_.right - 12.0f,
            tamponSectionRect_.top + 48.0f);
        ctx->DrawTextW(desc, (UINT32)wcslen(desc), descFormat, descRect, descBrush);
    }

    tamponInput_.Draw(ctx, dwrite);

    if (titleFormat)
        titleFormat->Release();
    if (labelFormat)
        labelFormat->Release();
    if (toggleFormat)
        toggleFormat->Release();
    if (descFormat)
        descFormat->Release();
    if (stateFormat)
        stateFormat->Release();
    if (titleBrush)
        titleBrush->Release();
    if (labelBrush)
        labelBrush->Release();
    if (descBrush)
        descBrush->Release();
    if (rowBgBrush)
        rowBgBrush->Release();
    if (rowBorderBrush)
        rowBorderBrush->Release();
    if (toggleBgBrush)
        toggleBgBrush->Release();
    if (toggleTextBrush)
        toggleTextBrush->Release();
    if (sectionBgBrush)
        sectionBgBrush->Release();
    if (sectionBorderBrush)
        sectionBorderBrush->Release();
    if (stateBrush)
        stateBrush->Release();

    ctx->SetTextAntialiasMode(oldTextAA);
    ctx->PopAxisAlignedClip();
}

void SettingsTabView::OnMouseMove(HWND hwnd, POINT clientPoint)
{
    bool wasHovered = rowHovered_;
    rowHovered_ = clientPoint.x >= rowRect_.left && clientPoint.x <= rowRect_.right &&
                  clientPoint.y >= rowRect_.top && clientPoint.y <= rowRect_.bottom;

    bool usedByInput = tamponInput_.OnMouseMove(hwnd, clientPoint);

    if (wasHovered != rowHovered_ || usedByInput)
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
    }

    bool hitInput = tamponInput_.OnLeftButtonDown(hwnd, clientPoint);
    if (hitRow || hitInput)
        InvalidateRect(hwnd, nullptr, FALSE);
}

void SettingsTabView::OnLeftButtonUp(HWND hwnd)
{
    POINT pt = {0, 0};
    GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);
    if (tamponInput_.OnLeftButtonUp(hwnd, pt))
        InvalidateRect(hwnd, nullptr, FALSE);
}

bool SettingsTabView::OnChar(wchar_t ch)
{
    return tamponInput_.OnChar(ch);
}

bool SettingsTabView::OnKeyDown(WPARAM key)
{
    if (key == VK_ESCAPE && tamponInput_.IsFocused())
    {
        tamponInput_.SetFocused(false);
        return true;
    }
    return tamponInput_.OnKeyDown(key);
}

bool SettingsTabView::IsPointInView(POINT clientPoint) const
{
    return clientPoint.x >= bounds_.left && clientPoint.x <= bounds_.right &&
           clientPoint.y >= bounds_.top && clientPoint.y <= bounds_.bottom;
}
