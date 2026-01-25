#include "ui/screens/NewProjectOverlay.h"
#include "core/window/Window.h"

namespace UI
{
void NewProjectCreateView::Draw(Window &window, ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, const NewProjectRenderContext &rc)
{
    const D2D1_RECT_F &rightPanel = rc.rightPanel;

    float backH = 30.0f * rc.scale;
    window.newProjOpenRect_ = D2D1::RectF(rightPanel.left + 12.0f * rc.scale, rightPanel.top + 12.0f * rc.scale,
                                          rightPanel.left + 140.0f * rc.scale, rightPanel.top + 12.0f * rc.scale + backH);

    auto drawAction = [&](const D2D1_RECT_F &rect, const wchar_t *icon, const wchar_t *label, bool hovered)
    {
        ID2D1SolidColorBrush *btnBg = nullptr;
        ctx->CreateSolidColorBrush(hovered ? D2D1::ColorF(0.22f, 0.22f, 0.22f, 1.0f)
                                           : D2D1::ColorF(0.18f, 0.18f, 0.18f, 1.0f), &btnBg);
        if (btnBg)
        {
            ctx->FillRoundedRectangle(D2D1::RoundedRect(rect, 6.0f * rc.scale, 6.0f * rc.scale), btnBg);
            btnBg->Release();
        }
        if (rc.panelBorder)
            ctx->DrawRoundedRectangle(D2D1::RoundedRect(rect, 6.0f * rc.scale, 6.0f * rc.scale), rc.panelBorder, 1.0f);

        if (rc.iconFmt && rc.muted)
        {
            D2D1_RECT_F iconRect = D2D1::RectF(rect.left + 10.0f * rc.scale, rect.top, rect.left + 28.0f * rc.scale, rect.bottom);
            ctx->DrawTextW(icon, 1, rc.iconFmt, iconRect, rc.muted);
        }
        if (rc.actionFmt && rc.text)
        {
            rc.actionFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            rc.actionFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            D2D1_RECT_F labelRect = D2D1::RectF(rect.left + 32.0f * rc.scale, rect.top, rect.right - 10.0f * rc.scale, rect.bottom);
            ctx->DrawTextW(label, (UINT32)wcslen(label), rc.actionFmt, labelRect, rc.text);
        }
    };

    drawAction(window.newProjOpenRect_, L"\uE8B7", L"Retour", window.newProjOpenHover_);
    window.newProjBrowseRect_ = D2D1::RectF(0, 0, 0, 0);

    float formTitleY = window.newProjOpenRect_.bottom + 12.0f * rc.scale;
    if (rc.labelFmt && rc.muted)
    {
        D2D1_RECT_F formTitle = D2D1::RectF(rightPanel.left + 14.0f * rc.scale, formTitleY, rightPanel.right - 14.0f * rc.scale, formTitleY + 16.0f * rc.scale);
        ctx->DrawTextW(L"Creer un projet", 15, rc.labelFmt, formTitle, rc.muted);
    }

    float inputPad = 14.0f * rc.scale;
    float inputW = (rightPanel.right - rightPanel.left) - inputPad * 2.0f;
    float inputH = 32.0f * rc.scale;
    float nameY = formTitleY + 18.0f * rc.scale;
    window.newProjNameInput_.SetRect(D2D1::RectF(rightPanel.left + inputPad, nameY, rightPanel.left + inputPad + inputW, nameY + inputH));

    auto &styleName = window.newProjNameInput_.GetStyle();
    styleName.backgroundColor = D2D1::ColorF(0.16f, 0.16f, 0.16f, 1.0f);
    styleName.borderColor = D2D1::ColorF(0.26f, 0.26f, 0.26f, 1.0f);
    styleName.focusBorderColor = D2D1::ColorF(0.29f, 0.62f, 0.92f, 1.0f);
    styleName.cornerRadius = 8.0f * rc.scale;
    styleName.fontSize = 12.0f * rc.scale;
    styleName.padding = 9.0f * rc.scale;
    styleName.textColor = D2D1::ColorF(0.92f, 0.92f, 0.92f, 1.0f);
    styleName.placeholderColor = D2D1::ColorF(0.48f, 0.48f, 0.48f, 1.0f);
    styleName.fontFamily = rc.uiFont;
    styleName.fontCollection = rc.uiCollection;
    window.newProjNameInput_.Draw(ctx, dwrite);

    // Template list
    window.newProjTemplateRects_.clear();
    const wchar_t *templateTitles[] = {
        L"Application console (C++)",
        L"Application GUI (Win32)",
        L"Bibliotheque C++",
        L"Script Python",
        L"Projet Web"};
    const wchar_t *templateSubs[] = {
        L"main.cpp + Hello World",
        L"WinMain minimal",
        L"Header + source de base",
        L"main.py pret a lancer",
        L"index.html + style + script"};
    const int templateCount = (int)(sizeof(templateTitles) / sizeof(templateTitles[0]));
    float tY = nameY + inputH + 22.0f * rc.scale;
    if (rc.sectionFmt && rc.muted)
    {
        D2D1_RECT_F templatesLabel = D2D1::RectF(rightPanel.left + inputPad, tY - 16.0f * rc.scale, rightPanel.right - inputPad, tY);
        ctx->DrawTextW(L"Modeles", 7, rc.sectionFmt, templatesLabel, rc.muted);
    }
    float tH = 44.0f * rc.scale;
    for (int i = 0; i < templateCount; ++i)
    {
        D2D1_RECT_F tRect = D2D1::RectF(rightPanel.left + inputPad, tY + i * (tH + 8.0f * rc.scale), rightPanel.right - inputPad,
                                        tY + i * (tH + 8.0f * rc.scale) + tH);
        window.newProjTemplateRects_.push_back(tRect);
    }
    ID2D1SolidColorBrush *tBorder = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.24f, 0.24f, 0.24f, 1.0f), &tBorder);
    IDWriteTextFormat *tFmt = nullptr;
    dwrite->CreateTextFormat(rc.uiFont, rc.uiCollection, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             11.5f * rc.scale, L"en-us", &tFmt);
    if (tFmt)
    {
        tFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        tFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    for (int i = 0; i < templateCount; ++i)
    {
        D2D1_RECT_F r = window.newProjTemplateRects_[i];
        bool selected = (i == window.newProjTemplateIndex_);
        bool hovered = (i == window.newProjTemplateHover_);
        ID2D1SolidColorBrush *selBg = nullptr;
        if (selected)
            ctx->CreateSolidColorBrush(D2D1::ColorF(0.20f, 0.28f, 0.42f, 1.0f), &selBg);
        else if (hovered)
            ctx->CreateSolidColorBrush(D2D1::ColorF(0.18f, 0.18f, 0.18f, 1.0f), &selBg);
        if (selBg)
        {
            ctx->FillRoundedRectangle(D2D1::RoundedRect(r, 6.0f * rc.scale, 6.0f * rc.scale), selBg);
            selBg->Release();
        }
        if (tBorder)
            ctx->DrawRoundedRectangle(D2D1::RoundedRect(r, 6.0f * rc.scale, 6.0f * rc.scale), tBorder, 1.0f);
        if (tFmt && rc.text)
        {
            D2D1_RECT_F tr = D2D1::RectF(r.left + 10.0f * rc.scale, r.top + 6.0f * rc.scale, r.right - 10.0f * rc.scale, r.bottom);
            ctx->DrawTextW(templateTitles[i], (UINT32)wcslen(templateTitles[i]), tFmt, tr, rc.text);
        }
        if (rc.subFmt && rc.muted)
        {
            D2D1_RECT_F sr = D2D1::RectF(r.left + 10.0f * rc.scale, r.top + 22.0f * rc.scale, r.right - 10.0f * rc.scale, r.bottom - 6.0f * rc.scale);
            rc.subFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            rc.subFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            ctx->DrawTextW(templateSubs[i], (UINT32)wcslen(templateSubs[i]), rc.subFmt, sr, rc.muted);
        }
    }
    if (tBorder) tBorder->Release();
    if (tFmt) tFmt->Release();

    float btnY = rightPanel.bottom - 36.0f * rc.scale;
    window.newProjCreateRect_ = D2D1::RectF(rightPanel.right - 140.0f * rc.scale, btnY, rightPanel.right - 14.0f * rc.scale, btnY + 28.0f * rc.scale);
    window.newProjCancelRect_ = D2D1::RectF(0, 0, 0, 0);

    ID2D1SolidColorBrush *createBrush = nullptr;
    ID2D1SolidColorBrush *btnText = nullptr;
    ctx->CreateSolidColorBrush(window.newProjCreateHover_ ? D2D1::ColorF(0.24f, 0.56f, 0.95f, 1.0f)
                                                          : D2D1::ColorF(0.18f, 0.46f, 0.86f, 1.0f), &createBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.96f, 0.96f, 1.0f), &btnText);
    if (createBrush)
    {
        ctx->FillRoundedRectangle(D2D1::RoundedRect(window.newProjCreateRect_, 6.0f * rc.scale, 6.0f * rc.scale), createBrush);
        createBrush->Release();
    }
    if (btnText)
    {
        IDWriteTextFormat *btnFmt = nullptr;
        dwrite->CreateTextFormat(rc.uiFont, rc.uiCollection, DWRITE_FONT_WEIGHT_MEDIUM,
                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                 11.0f * rc.scale, L"en-us", &btnFmt);
        if (btnFmt)
        {
            btnFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            btnFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            ctx->DrawTextW(L"Creer", 5, btnFmt, window.newProjCreateRect_, btnText);
            btnFmt->Release();
        }
        btnText->Release();
    }
}
} // namespace UI
