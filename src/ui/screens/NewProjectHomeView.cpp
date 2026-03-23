#include "ui/screens/NewProjectOverlay.h"
#include "core/window/Window.h"
#include "ui/theme/Theme.h"
#include <filesystem>
#include <shlobj.h>

namespace UI
{
void NewProjectHomeView::Draw(Window &window, ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, const NewProjectRenderContext &rc)
{
    (void)dwrite;
    const D2D1_RECT_F &rightPanel = rc.rightPanel;
    const UI::Theme::Palette &palette = UI::Theme::GetPalette();

    // Right: actions (home page)
    D2D1_RECT_F actionsTitle = D2D1::RectF(rightPanel.left + 14.0f * rc.scale, rightPanel.top + 12.0f * rc.scale,
                                           rightPanel.right - 14.0f * rc.scale, rightPanel.top + 30.0f * rc.scale);
    if (rc.sectionFmt && rc.text)
        ctx->DrawTextW(L"Actions", 7, rc.sectionFmt, actionsTitle, rc.text);

    float actionH = 34.0f * rc.scale;
    float actionGap = 4.0f * rc.scale;
    float actionY = actionsTitle.bottom + 8.0f * rc.scale;
    window.newProjCreateRect_ = D2D1::RectF(rightPanel.left + 12.0f * rc.scale, actionY, rightPanel.right - 12.0f * rc.scale, actionY + actionH);
    window.newProjOpenRect_ = D2D1::RectF(rightPanel.left + 12.0f * rc.scale, actionY + actionH + actionGap,
                                          rightPanel.right - 12.0f * rc.scale, actionY + (actionH + actionGap) + actionH);
    window.newProjBrowseRect_ = D2D1::RectF(rightPanel.left + 12.0f * rc.scale, actionY + (actionH + actionGap) * 2.0f,
                                            rightPanel.right - 12.0f * rc.scale, actionY + (actionH + actionGap) * 2.0f + actionH);

    auto drawAction = [&](const D2D1_RECT_F &rect, const wchar_t *icon, const wchar_t *label, bool hovered)
    {
        if (hovered)
        {
            ID2D1SolidColorBrush *btnBg = nullptr;
            ctx->CreateSolidColorBrush(palette.explorerRowHover, &btnBg);
            if (btnBg)
            {
                ctx->FillRoundedRectangle(D2D1::RoundedRect(rect, 5.0f * rc.scale, 5.0f * rc.scale), btnBg);
                btnBg->Release();
            }
        }

        if (rc.iconFmt && rc.accent)
        {
            D2D1_RECT_F iconRect = D2D1::RectF(rect.left + 10.0f * rc.scale, rect.top, rect.left + 28.0f * rc.scale, rect.bottom);
            ctx->DrawTextW(icon, 1, rc.iconFmt, iconRect, rc.accent);
        }

        if (rc.actionFmt && rc.text)
        {
            rc.actionFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            rc.actionFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            D2D1_RECT_F labelRect = D2D1::RectF(rect.left + 32.0f * rc.scale, rect.top,
                                                rect.right - 10.0f * rc.scale, rect.bottom);
            ctx->DrawTextW(label, (UINT32)wcslen(label), rc.actionFmt, labelRect, rc.text);
        }
    };

    drawAction(window.newProjCreateRect_, L"\uE710", L"Creer un projet", window.newProjCreateHover_);
    drawAction(window.newProjOpenRect_, L"\uE8B7", L"Ouvrir un dossier", window.newProjOpenHover_);
    drawAction(window.newProjBrowseRect_, L"\uE7C3", L"Ouvrir mes projets", window.newProjBrowseHover_);

    window.newProjTemplateRects_.clear();
    window.newProjNameInput_.SetRect(D2D1::RectF(0, 0, 0, 0));

    // Bottom continue button (ghost style)
    float btnY = rightPanel.bottom - 36.0f * rc.scale;
    window.newProjCancelRect_ = D2D1::RectF(rightPanel.right - 190.0f * rc.scale, btnY, rightPanel.right - 14.0f * rc.scale, btnY + 28.0f * rc.scale);
    if (window.newProjCancelHover_)
    {
        ID2D1SolidColorBrush *cancelBrush = nullptr;
        ctx->CreateSolidColorBrush(palette.explorerToolbarHover, &cancelBrush);
        if (cancelBrush)
        {
            ctx->FillRoundedRectangle(D2D1::RoundedRect(window.newProjCancelRect_, 5.0f * rc.scale, 5.0f * rc.scale), cancelBrush);
            cancelBrush->Release();
        }
    }
    if (rc.muted)
    {
        IDWriteTextFormat *btnFmt = nullptr;
        dwrite->CreateTextFormat(rc.uiFont, rc.uiCollection, DWRITE_FONT_WEIGHT_MEDIUM,
                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                 11.0f * rc.scale, L"en-us", &btnFmt);
        if (btnFmt)
        {
            btnFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            btnFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            ctx->DrawTextW(L"Continuer sans code", 20, btnFmt, window.newProjCancelRect_,
                           window.newProjCancelHover_ ? rc.text : rc.muted);
            btnFmt->Release();
        }
    }
}
} // namespace UI
