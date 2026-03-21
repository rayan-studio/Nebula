#include "SettingsTab.h"

#include "helpers/window_helpers.h"
#include "ui/layout/ExplorerLayoutState.h"
#include "core/window/Window.h"
#include "utils/auth/GitHubAuth.h"
#include "utils/auth/GitHubOAuth.h"
#include "ui/components/input/InputTheme.h"
#include "ui/theme/Theme.h"

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

    RefreshInputTheme();
}

void SettingsTabView::RefreshInputTheme()
{
    auto &style = tamponInput_.GetStyle();
    style.backgroundColor = UI::InputTheme::Background();
    style.borderColor = UI::InputTheme::Border();
    style.focusBorderColor = UI::InputTheme::FocusBorder();
    style.textColor = UI::InputTheme::Text();
    style.placeholderColor = UI::InputTheme::Placeholder();
    style.selectionColor = UI::InputTheme::Selection();
    style.cursorColor = UI::InputTheme::Caret();
    style.fontFamily = L"Segoe UI Variable Text";
    style.multiline = true;
}

void SettingsTabView::UpdateLayout(HWND hwnd, float left, float top, float right, float bottom)
{
    bounds_ = D2D1::RectF(left, top, right, bottom);

    UINT dpi = win32_get_dpi_for_window(hwnd);
    float padding = (float)win32_dpi_scale(24, dpi);
    float titleH = (float)win32_dpi_scale(60, dpi);
    float scale = (float)dpi / 96.0f;

    float rowTop = bounds_.top + padding + titleH + (float)win32_dpi_scale(12, dpi);
    float rowHeight = (float)win32_dpi_scale(42, dpi);
    float rowLeft = bounds_.left + padding;
    float rowRight = bounds_.right - padding;

    rowRect_ = D2D1::RectF(rowLeft, rowTop, rowRight, rowTop + rowHeight);

    float toggleWidth = (float)win32_dpi_scale(108, dpi);
    float toggleHeight = (float)win32_dpi_scale(26, dpi);
    float toggleRight = rowRect_.right - (float)win32_dpi_scale(10, dpi);
    float toggleLeft = toggleRight - toggleWidth;
    float toggleTop = rowRect_.top + (rowHeight - toggleHeight) * 0.5f;
    toggleRect_ = D2D1::RectF(toggleLeft, toggleTop, toggleRight, toggleTop + toggleHeight);

    float themeRowTop = rowRect_.bottom;
    themeRowRect_ = D2D1::RectF(rowLeft, themeRowTop, rowRight, themeRowTop + rowHeight);
    float themeToggleTop = themeRowRect_.top + (rowHeight - toggleHeight) * 0.5f;
    themeToggleRect_ = D2D1::RectF(toggleLeft, themeToggleTop, toggleRight, themeToggleTop + toggleHeight);

    float sectionTop = themeRowRect_.bottom + (float)win32_dpi_scale(26, dpi);
    float minSectionHeight = (float)win32_dpi_scale(168, dpi);
    float sectionBottom = sectionTop + minSectionHeight;
    tamponSectionRect_ = D2D1::RectF(rowLeft, sectionTop, rowRight, sectionBottom);

    float inputTop = tamponSectionRect_.top + (float)win32_dpi_scale(66, dpi);
    float inputHeight = (std::max)((float)win32_dpi_scale(72, dpi),
                                   tamponSectionRect_.bottom - inputTop - (float)win32_dpi_scale(14, dpi));
    float inputPad = (float)win32_dpi_scale(14, dpi);
    tamponInputRect_ = D2D1::RectF(
        tamponSectionRect_.left + inputPad,
        inputTop,
        tamponSectionRect_.right - inputPad,
        inputTop + inputHeight);
    tamponInput_.SetRect(tamponInputRect_);

    float githubTop = tamponSectionRect_.bottom + (float)win32_dpi_scale(24, dpi);
    float githubHeight = (float)win32_dpi_scale(126, dpi);
    float githubBottom = githubTop + githubHeight;
    githubSectionRect_ = D2D1::RectF(rowLeft, githubTop, rowRight, githubBottom);

    float githubPad = (float)win32_dpi_scale(14, dpi);
    float btnTop = githubSectionRect_.top + (float)win32_dpi_scale(70, dpi);
    float btnH = (float)win32_dpi_scale(30, dpi);
    float signInW = (float)win32_dpi_scale(186, dpi);
    float disconnectW = (float)win32_dpi_scale(116, dpi);
    githubSignInRect_ = D2D1::RectF(githubSectionRect_.left + githubPad, btnTop,
                                    githubSectionRect_.left + githubPad + signInW, btnTop + btnH);
    githubDisconnectRect_ = D2D1::RectF(githubSignInRect_.right + (float)win32_dpi_scale(10, dpi), btnTop,
                                        githubSignInRect_.right + (float)win32_dpi_scale(10, dpi) + disconnectW, btnTop + btnH);

    auto &style = tamponInput_.GetStyle();
    style.cornerRadius = 3.0f * scale;
    style.fontSize = 12.5f * scale;
    style.padding = 8.0f * scale;
}

void SettingsTabView::Draw(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd)
{
    (void)hwnd;
    if (!ctx || !dwrite)
        return;

    const std::wstring &tampon = GetTamponText();
    if (!tamponInput_.IsFocused() && tamponInput_.GetText() != tampon)
        tamponInput_.SetText(tampon);
    RefreshInputTheme();

    D2D1_RECT_F clip = bounds_;
    ctx->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    D2D1_TEXT_ANTIALIAS_MODE oldTextAA = ctx->GetTextAntialiasMode();
    ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

    ID2D1SolidColorBrush *bgBrush = nullptr;
    ctx->CreateSolidColorBrush(UI::Theme::ChromeBackground(), &bgBrush);
    if (bgBrush)
    {
        ctx->FillRectangle(bounds_, bgBrush);
        bgBrush->Release();
    }

    IDWriteTextFormat *titleFormat = nullptr;
    IDWriteTextFormat *subtitleFormat = nullptr;
    IDWriteTextFormat *sectionFormat = nullptr;
    IDWriteTextFormat *labelFormat = nullptr;
    IDWriteTextFormat *valueFormat = nullptr;
    IDWriteTextFormat *descFormat = nullptr;
    IDWriteTextFormat *stateFormat = nullptr;

    dwrite->CreateTextFormat(L"Segoe UI Variable Text", NULL, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             20.0f, L"en-us", &titleFormat);
    dwrite->CreateTextFormat(L"Segoe UI Variable Text", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             12.0f, L"en-us", &subtitleFormat);
    dwrite->CreateTextFormat(L"Segoe UI Variable Text", NULL, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             11.0f, L"en-us", &sectionFormat);
    dwrite->CreateTextFormat(L"Segoe UI Variable Text", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             13.0f, L"en-us", &labelFormat);
    dwrite->CreateTextFormat(L"Segoe UI Variable Text", NULL, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             12.0f, L"en-us", &valueFormat);
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
    if (subtitleFormat)
    {
        subtitleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        subtitleFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    if (sectionFormat)
    {
        sectionFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        sectionFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (labelFormat)
    {
        labelFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        labelFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (valueFormat)
    {
        valueFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        valueFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
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
    ID2D1SolidColorBrush *sectionLabelBrush = nullptr;
    ID2D1SolidColorBrush *panelBgBrush = nullptr;
    ID2D1SolidColorBrush *rowBgBrush = nullptr;
    ID2D1SolidColorBrush *rowBorderBrush = nullptr;
    ID2D1SolidColorBrush *separatorBrush = nullptr;
    ID2D1SolidColorBrush *valueBgBrush = nullptr;
    ID2D1SolidColorBrush *valueBorderBrush = nullptr;
    ID2D1SolidColorBrush *valueTextBrush = nullptr;
    ID2D1SolidColorBrush *buttonSecondaryBrush = nullptr;
    ID2D1SolidColorBrush *buttonDangerBrush = nullptr;
    ID2D1SolidColorBrush *stateBrush = nullptr;

    const UI::Theme::Palette &palette = UI::Theme::GetPalette();
    ctx->CreateSolidColorBrush(UI::Theme::PrimaryText(), &titleBrush);
    ctx->CreateSolidColorBrush(UI::Theme::PrimaryText(), &labelBrush);
    ctx->CreateSolidColorBrush(UI::Theme::MutedText(), &descBrush);
    ctx->CreateSolidColorBrush(UI::Theme::MutedText(), &sectionLabelBrush);
    ctx->CreateSolidColorBrush(palette.inputBackground, &panelBgBrush);
    ctx->CreateSolidColorBrush(rowHovered_ ? palette.explorerRowHover : palette.inputBackground, &rowBgBrush);
    ctx->CreateSolidColorBrush(UI::Theme::ChromeBorder(), &rowBorderBrush);
    ctx->CreateSolidColorBrush(UI::Theme::ChromeBorder(), &separatorBrush);
    ctx->CreateSolidColorBrush(palette.explorerToolbarHover, &valueBgBrush);
    ctx->CreateSolidColorBrush(palette.inputBorder, &valueBorderBrush);
    ctx->CreateSolidColorBrush(UI::Theme::PrimaryText(), &valueTextBrush);
    ctx->CreateSolidColorBrush(
        githubSignInHovered_
            ? D2D1::ColorF(UI::Theme::AccentStrong().r, UI::Theme::AccentStrong().g, UI::Theme::AccentStrong().b, 0.30f)
            : D2D1::ColorF(UI::Theme::Accent().r, UI::Theme::Accent().g, UI::Theme::Accent().b, 0.18f),
        &buttonSecondaryBrush);
    ctx->CreateSolidColorBrush(
        githubDisconnectHovered_
            ? D2D1::ColorF(0.78f, 0.32f, 0.32f, 0.24f)
            : D2D1::ColorF(0.72f, 0.28f, 0.28f, 0.16f),
        &buttonDangerBrush);
    ctx->CreateSolidColorBrush(HasTamponText() ? D2D1::ColorF(0.36f, 0.78f, 0.49f) : UI::Theme::MutedText(), &stateBrush);

    if (titleFormat && titleBrush)
    {
        D2D1_RECT_F titleRect = D2D1::RectF(bounds_.left + 24.0f, bounds_.top + 18.0f, bounds_.right, bounds_.top + 48.0f);
        const wchar_t *title = L"Settings";
        ctx->DrawTextW(title, (UINT32)wcslen(title), titleFormat, titleRect, titleBrush);
    }
    if (subtitleFormat && descBrush)
    {
        const wchar_t *subtitle = L"Configure editor behavior, file creation defaults and source control integration.";
        D2D1_RECT_F subtitleRect = D2D1::RectF(bounds_.left + 24.0f, bounds_.top + 48.0f, bounds_.right - 24.0f, bounds_.top + 76.0f);
        ctx->DrawTextW(subtitle, (UINT32)wcslen(subtitle), subtitleFormat, subtitleRect, descBrush);
    }

    D2D1_RECT_F environmentSectionRect = D2D1::RectF(rowRect_.left, rowRect_.top - 20.0f, rowRect_.right, themeRowRect_.bottom);
    if (sectionFormat && sectionLabelBrush)
    {
        const wchar_t *environmentTitle = L"ENVIRONMENT";
        D2D1_RECT_F sectionRect = D2D1::RectF(environmentSectionRect.left, environmentSectionRect.top, environmentSectionRect.right, environmentSectionRect.top + 16.0f);
        ctx->DrawTextW(environmentTitle, (UINT32)wcslen(environmentTitle), sectionFormat, sectionRect, sectionLabelBrush);
    }
    if (panelBgBrush)
        ctx->FillRectangle(D2D1::RectF(environmentSectionRect.left, rowRect_.top, environmentSectionRect.right, environmentSectionRect.bottom), panelBgBrush);
    if (rowBorderBrush)
        ctx->DrawRectangle(D2D1::RectF(environmentSectionRect.left, rowRect_.top, environmentSectionRect.right, environmentSectionRect.bottom), rowBorderBrush, 1.0f);
    if (separatorBrush)
        ctx->DrawLine(D2D1::Point2F(rowRect_.left, rowRect_.bottom), D2D1::Point2F(rowRect_.right, rowRect_.bottom), separatorBrush, 1.0f);

    if (rowBgBrush)
        ctx->FillRectangle(rowRect_, rowBgBrush);

    if (labelFormat && labelBrush)
    {
        const wchar_t *label = L"Explorer Position";
        D2D1_RECT_F labelRect = D2D1::RectF(rowRect_.left + 14.0f, rowRect_.top, toggleRect_.left - 12.0f, rowRect_.bottom);
        ctx->DrawTextW(label, (UINT32)wcslen(label), labelFormat, labelRect, labelBrush);
    }

    ExplorerPlacement placement = GetExplorerLayoutState().placement;
    const wchar_t *value = placement == ExplorerPlacement::Right ? L"Right" : L"Left";
    if (valueBgBrush)
        ctx->FillRectangle(toggleRect_, valueBgBrush);
    if (valueBorderBrush)
        ctx->DrawRectangle(toggleRect_, valueBorderBrush, 1.0f);
    if (valueFormat && valueTextBrush)
        ctx->DrawTextW(value, (UINT32)wcslen(value), valueFormat, toggleRect_, valueTextBrush);

    if (themeRowHovered_ && rowBgBrush)
    {
        ID2D1SolidColorBrush *themeRowBrush = nullptr;
        ctx->CreateSolidColorBrush(palette.explorerRowHover, &themeRowBrush);
        if (themeRowBrush)
        {
            ctx->FillRectangle(themeRowRect_, themeRowBrush);
            themeRowBrush->Release();
        }
    }
    if (labelFormat && labelBrush)
    {
        const wchar_t *themeLabel = L"Theme";
        D2D1_RECT_F labelRect = D2D1::RectF(themeRowRect_.left + 14.0f, themeRowRect_.top, themeToggleRect_.left - 12.0f, themeRowRect_.bottom);
        ctx->DrawTextW(themeLabel, (UINT32)wcslen(themeLabel), labelFormat, labelRect, labelBrush);
    }
    if (valueBgBrush)
        ctx->FillRectangle(themeToggleRect_, valueBgBrush);
    if (valueBorderBrush)
        ctx->DrawRectangle(themeToggleRect_, valueBorderBrush, 1.0f);
    if (valueFormat && valueTextBrush)
    {
        const wchar_t *themeValue = UI::Theme::GetMode() == UI::Theme::Mode::Dark ? L"Dark" : L"Light";
        ctx->DrawTextW(themeValue, (UINT32)wcslen(themeValue), valueFormat, themeToggleRect_, valueTextBrush);
    }

    if (sectionFormat && sectionLabelBrush)
    {
        const wchar_t *editingTitle = L"FILE CREATION";
        D2D1_RECT_F sectionRect = D2D1::RectF(tamponSectionRect_.left, tamponSectionRect_.top - 20.0f, tamponSectionRect_.right, tamponSectionRect_.top - 4.0f);
        ctx->DrawTextW(editingTitle, (UINT32)wcslen(editingTitle), sectionFormat, sectionRect, sectionLabelBrush);
    }
    if (panelBgBrush)
        ctx->FillRectangle(tamponSectionRect_, panelBgBrush);
    if (rowBorderBrush)
        ctx->DrawRectangle(tamponSectionRect_, rowBorderBrush, 1.0f);

    if (labelFormat && labelBrush)
    {
        const wchar_t *tamponTitle = L"Tampon des nouveaux fichiers";
        D2D1_RECT_F titleRect = D2D1::RectF(
            tamponSectionRect_.left + 14.0f,
            tamponSectionRect_.top + 12.0f,
            tamponSectionRect_.right - 120.0f,
            tamponSectionRect_.top + 34.0f);
        ctx->DrawTextW(tamponTitle, (UINT32)wcslen(tamponTitle), labelFormat, titleRect, labelBrush);
    }
    if (stateFormat && stateBrush)
    {
        const wchar_t *stateText = HasTamponText() ? L"Actif" : L"Desactive";
        D2D1_RECT_F stateRect = D2D1::RectF(
            tamponSectionRect_.right - 120.0f,
            tamponSectionRect_.top + 12.0f,
            tamponSectionRect_.right - 14.0f,
            tamponSectionRect_.top + 34.0f);
        ctx->DrawTextW(stateText, (UINT32)wcslen(stateText), stateFormat, stateRect, stateBrush);
    }
    if (descFormat && descBrush)
    {
        const wchar_t *desc = L"Texte ajoute au debut de chaque nouveau fichier (Entree = nouvelle ligne, vide = desactive).";
        D2D1_RECT_F descRect = D2D1::RectF(
            tamponSectionRect_.left + 14.0f,
            tamponSectionRect_.top + 34.0f,
            tamponSectionRect_.right - 14.0f,
            tamponSectionRect_.top + 56.0f);
        ctx->DrawTextW(desc, (UINT32)wcslen(desc), descFormat, descRect, descBrush);
    }
    tamponInput_.Draw(ctx, dwrite);

    if (sectionFormat && sectionLabelBrush)
    {
        const wchar_t *sourceTitle = L"SOURCE CONTROL";
        D2D1_RECT_F sectionRect = D2D1::RectF(githubSectionRect_.left, githubSectionRect_.top - 20.0f, githubSectionRect_.right, githubSectionRect_.top - 4.0f);
        ctx->DrawTextW(sourceTitle, (UINT32)wcslen(sourceTitle), sectionFormat, sectionRect, sectionLabelBrush);
    }
    if (panelBgBrush)
        ctx->FillRectangle(githubSectionRect_, panelBgBrush);
    if (rowBorderBrush)
        ctx->DrawRectangle(githubSectionRect_, rowBorderBrush, 1.0f);

    const bool githubConnected = GitHubAuth::HasToken();
    if (labelFormat && labelBrush)
    {
        const wchar_t *gitTitle = L"GitHub";
        D2D1_RECT_F titleRect = D2D1::RectF(
            githubSectionRect_.left + 14.0f,
            githubSectionRect_.top + 12.0f,
            githubSectionRect_.right - 120.0f,
            githubSectionRect_.top + 34.0f);
        ctx->DrawTextW(gitTitle, (UINT32)wcslen(gitTitle), labelFormat, titleRect, labelBrush);
    }
    if (stateFormat && stateBrush)
    {
        stateBrush->SetColor(githubConnected ? D2D1::ColorF(0.36f, 0.78f, 0.49f) : D2D1::ColorF(0.87f, 0.43f, 0.43f));
        const wchar_t *stateText = githubConnected ? L"Connected" : L"Not connected";
        D2D1_RECT_F stateRect = D2D1::RectF(
            githubSectionRect_.right - 160.0f,
            githubSectionRect_.top + 12.0f,
            githubSectionRect_.right - 14.0f,
            githubSectionRect_.top + 34.0f);
        ctx->DrawTextW(stateText, (UINT32)wcslen(stateText), stateFormat, stateRect, stateBrush);
    }
    if (descFormat && descBrush)
    {
        const wchar_t *desc = L"OAuth app login for bot workflow. Commit and push are blocked when disconnected.";
        D2D1_RECT_F descRect = D2D1::RectF(
            githubSectionRect_.left + 14.0f,
            githubSectionRect_.top + 34.0f,
            githubSectionRect_.right - 14.0f,
            githubSectionRect_.top + 58.0f);
        ctx->DrawTextW(desc, (UINT32)wcslen(desc), descFormat, descRect, descBrush);
    }

    if (buttonSecondaryBrush)
        ctx->FillRectangle(githubSignInRect_, buttonSecondaryBrush);
    if (buttonDangerBrush)
        ctx->FillRectangle(githubDisconnectRect_, buttonDangerBrush);
    if (valueBorderBrush)
    {
        ctx->DrawRectangle(githubSignInRect_, valueBorderBrush, 1.0f);
        ctx->DrawRectangle(githubDisconnectRect_, valueBorderBrush, 1.0f);
    }

    if (valueFormat && valueTextBrush)
    {
        const wchar_t *oauthText = L"Sign in with GitHub";
        const wchar_t *disconnectText = L"Disconnect";
        ctx->DrawTextW(oauthText, (UINT32)wcslen(oauthText), valueFormat, githubSignInRect_, valueTextBrush);
        ctx->DrawTextW(disconnectText, (UINT32)wcslen(disconnectText), valueFormat, githubDisconnectRect_, valueTextBrush);
    }

    if (descFormat && !githubStatusMessage_.empty() && descBrush)
    {
        D2D1_RECT_F msgRect = D2D1::RectF(githubSectionRect_.left + 14.0f, githubSignInRect_.bottom + 8.0f,
                                          githubSectionRect_.right - 14.0f, githubSectionRect_.bottom - 10.0f);
        ctx->DrawTextW(githubStatusMessage_.c_str(), (UINT32)githubStatusMessage_.size(), descFormat, msgRect, descBrush);
    }

    if (titleFormat)
        titleFormat->Release();
    if (subtitleFormat)
        subtitleFormat->Release();
    if (sectionFormat)
        sectionFormat->Release();
    if (labelFormat)
        labelFormat->Release();
    if (valueFormat)
        valueFormat->Release();
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
    if (sectionLabelBrush)
        sectionLabelBrush->Release();
    if (panelBgBrush)
        panelBgBrush->Release();
    if (rowBgBrush)
        rowBgBrush->Release();
    if (rowBorderBrush)
        rowBorderBrush->Release();
    if (separatorBrush)
        separatorBrush->Release();
    if (valueBgBrush)
        valueBgBrush->Release();
    if (valueBorderBrush)
        valueBorderBrush->Release();
    if (valueTextBrush)
        valueTextBrush->Release();
    if (buttonSecondaryBrush)
        buttonSecondaryBrush->Release();
    if (buttonDangerBrush)
        buttonDangerBrush->Release();
    if (stateBrush)
        stateBrush->Release();

    ctx->SetTextAntialiasMode(oldTextAA);
    ctx->PopAxisAlignedClip();
}

void SettingsTabView::OnMouseMove(HWND hwnd, POINT clientPoint)
{
    bool wasHovered = rowHovered_;
    bool wasThemeHovered = themeRowHovered_;
    bool wasSignInHovered = githubSignInHovered_;
    bool wasDisconnectHovered = githubDisconnectHovered_;

    rowHovered_ = clientPoint.x >= rowRect_.left && clientPoint.x <= rowRect_.right &&
                  clientPoint.y >= rowRect_.top && clientPoint.y <= rowRect_.bottom;
    themeRowHovered_ = clientPoint.x >= themeRowRect_.left && clientPoint.x <= themeRowRect_.right &&
                       clientPoint.y >= themeRowRect_.top && clientPoint.y <= themeRowRect_.bottom;
    githubSignInHovered_ = clientPoint.x >= githubSignInRect_.left && clientPoint.x <= githubSignInRect_.right &&
                           clientPoint.y >= githubSignInRect_.top && clientPoint.y <= githubSignInRect_.bottom;
    githubDisconnectHovered_ = clientPoint.x >= githubDisconnectRect_.left && clientPoint.x <= githubDisconnectRect_.right &&
                               clientPoint.y >= githubDisconnectRect_.top && clientPoint.y <= githubDisconnectRect_.bottom;

    bool usedByInput = tamponInput_.OnMouseMove(hwnd, clientPoint);

    if (wasHovered != rowHovered_ ||
        wasThemeHovered != themeRowHovered_ ||
        wasSignInHovered != githubSignInHovered_ ||
        wasDisconnectHovered != githubDisconnectHovered_ ||
        usedByInput)
    {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
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

    bool hitSignIn = clientPoint.x >= githubSignInRect_.left && clientPoint.x <= githubSignInRect_.right &&
                     clientPoint.y >= githubSignInRect_.top && clientPoint.y <= githubSignInRect_.bottom;
    bool hitDisconnect = clientPoint.x >= githubDisconnectRect_.left && clientPoint.x <= githubDisconnectRect_.right &&
                         clientPoint.y >= githubDisconnectRect_.top && clientPoint.y <= githubDisconnectRect_.bottom;
    bool hitThemeRow = clientPoint.x >= themeRowRect_.left && clientPoint.x <= themeRowRect_.right &&
                       clientPoint.y >= themeRowRect_.top && clientPoint.y <= themeRowRect_.bottom;

    if (hitThemeRow)
        UI::Theme::ToggleMode();

    if (hitSignIn)
        BeginGitHubSignIn();
    if (hitDisconnect)
        ClearGitHubToken();

    bool hitInput = tamponInput_.OnLeftButtonDown(hwnd, clientPoint);
    if (hitRow || hitThemeRow || hitSignIn || hitDisconnect || hitInput)
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

void SettingsTabView::BeginGitHubSignIn()
{
    githubStatusMessage_ = L"Waiting for GitHub OAuth callback on localhost...";
    GitHubOAuth::AuthResult res = GitHubOAuth::SignInViaBrowser();
    githubStatusMessage_ = res.message;
}

void SettingsTabView::ClearGitHubToken()
{
    std::wstring err;
    if (GitHubAuth::ClearToken(err))
        githubStatusMessage_ = L"GitHub session removed.";
    else
        githubStatusMessage_ = err.empty() ? L"Failed to remove GitHub session." : err;
}

bool SettingsTabView::IsPointInView(POINT clientPoint) const
{
    return clientPoint.x >= bounds_.left && clientPoint.x <= bounds_.right &&
           clientPoint.y >= bounds_.top && clientPoint.y <= bounds_.bottom;
}
