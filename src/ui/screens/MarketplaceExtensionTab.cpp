#include "MarketplaceExtensionTab.h"

#include "core/explorer/Explorer.h"
#include "ui/panels/LibraryDatabase.h"
#include "ui/theme/Theme.h"
#include "utils/logger/Logger.h"

#include <algorithm>

void MarketplaceExtensionTabView::SetLibraryName(const std::wstring &name)
{
    currentLibraryName_ = name;
}

void MarketplaceExtensionTabView::UpdateLayout(HWND hwnd, float left, float top, float right, float bottom)
{
    bounds_ = D2D1::RectF(left, top, right, bottom);

    const UINT dpi = GetDpiForWindow(hwnd);
    const float pad = static_cast<float>(MulDiv(24, dpi, 96));
    const float cardTop = bounds_.top + static_cast<float>(MulDiv(20, dpi, 96));
    const float cardBottom = (std::min)(bounds_.bottom - pad, cardTop + static_cast<float>(MulDiv(280, dpi, 96)));

    cardRect_ = D2D1::RectF(
        bounds_.left + pad,
        cardTop,
        bounds_.right - pad,
        cardBottom);

    const float btnH = static_cast<float>(MulDiv(34, dpi, 96));
    const float btnW = static_cast<float>(MulDiv(140, dpi, 96));
    const float btnRight = cardRect_.right - static_cast<float>(MulDiv(16, dpi, 96));
    const float btnTop = cardRect_.bottom - static_cast<float>(MulDiv(16, dpi, 96)) - btnH;

    actionButtonRect_ = D2D1::RectF(btnRight - btnW, btnTop, btnRight, btnTop + btnH);
}

bool MarketplaceExtensionTabView::IsPointInRect(POINT pt, const D2D1_RECT_F &rect) const
{
    return pt.x >= rect.left && pt.x <= rect.right && pt.y >= rect.top && pt.y <= rect.bottom;
}

void MarketplaceExtensionTabView::Draw(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND /*hwnd*/)
{
    if (!ctx || !dwrite)
        return;

    LibraryInfo *lib = nullptr;
    if (!currentLibraryName_.empty())
        lib = LibraryDatabase::Instance().FindLibrary(currentLibraryName_);

    ID2D1SolidColorBrush *bgBrush = nullptr;
    ID2D1SolidColorBrush *cardBrush = nullptr;
    ID2D1SolidColorBrush *borderBrush = nullptr;
    ID2D1SolidColorBrush *titleBrush = nullptr;
    ID2D1SolidColorBrush *mutedBrush = nullptr;
    ID2D1SolidColorBrush *buttonBrush = nullptr;
    ID2D1SolidColorBrush *buttonTextBrush = nullptr;

    const UI::Theme::Palette &palette = UI::Theme::GetPalette();
    ctx->CreateSolidColorBrush(UI::Theme::ChromeBackground(), &bgBrush);
    ctx->CreateSolidColorBrush(palette.inputBackground, &cardBrush);
    ctx->CreateSolidColorBrush(UI::Theme::ChromeBorder(), &borderBrush);
    ctx->CreateSolidColorBrush(UI::Theme::PrimaryText(), &titleBrush);
    ctx->CreateSolidColorBrush(UI::Theme::MutedText(), &mutedBrush);

    const D2D1_COLOR_F installColor = actionButtonHovered_ ? UI::Theme::AccentStrong() : UI::Theme::Accent();
    const D2D1_COLOR_F installedColor = actionButtonHovered_ ? D2D1::ColorF(0.80f, 0.36f, 0.36f) : D2D1::ColorF(0.33f, 0.77f, 0.47f);
    const D2D1_COLOR_F buttonColor = (lib && lib->isInstalled) ? installedColor : installColor;
    ctx->CreateSolidColorBrush(buttonColor, &buttonBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &buttonTextBrush);

    if (bgBrush)
        ctx->FillRectangle(bounds_, bgBrush);
    if (cardBrush)
        ctx->FillRoundedRectangle(D2D1::RoundedRect(cardRect_, 8.0f, 8.0f), cardBrush);
    if (borderBrush)
        ctx->DrawRoundedRectangle(D2D1::RoundedRect(cardRect_, 8.0f, 8.0f), borderBrush, 1.0f);

    IDWriteTextFormat *titleFmt = nullptr;
    IDWriteTextFormat *bodyFmt = nullptr;
    IDWriteTextFormat *buttonFmt = nullptr;

    dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
                             DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                             DWRITE_FONT_STRETCH_NORMAL, 24.0f, L"en-us", &titleFmt);
    dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
                             DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                             DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &bodyFmt);
    dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
                             DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                             DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &buttonFmt);

    if (titleFmt)
    {
        titleFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        titleFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    if (bodyFmt)
    {
        bodyFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        bodyFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        bodyFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }
    if (buttonFmt)
    {
        buttonFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        buttonFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    const float x = cardRect_.left + 22.0f;
    float y = cardRect_.top + 18.0f;
    const float textRight = cardRect_.right - 22.0f;

    if (!lib)
    {
        if (titleFmt && titleBrush)
        {
            const wchar_t *missing = L"Extension introuvable";
            ctx->DrawTextW(missing, static_cast<UINT32>(wcslen(missing)), titleFmt,
                           D2D1::RectF(x, y, textRight, y + 36.0f), titleBrush);
        }
        if (bodyFmt && mutedBrush)
        {
            const wchar_t *hint = L"Retourne dans Marketplace et clique sur une extension pour ouvrir sa page detail.";
            ctx->DrawTextW(hint, static_cast<UINT32>(wcslen(hint)), bodyFmt,
                           D2D1::RectF(x, y + 46.0f, textRight, y + 120.0f), mutedBrush);
        }
    }
    else
    {
        if (titleFmt && titleBrush)
            ctx->DrawTextW(lib->name.c_str(), static_cast<UINT32>(lib->name.size()), titleFmt,
                           D2D1::RectF(x, y, textRight, y + 40.0f), titleBrush);

        y += 44.0f;
        std::wstring meta = L"by " + lib->author + L"    Version " + lib->version + L"    " + lib->category;
        if (bodyFmt && mutedBrush)
            ctx->DrawTextW(meta.c_str(), static_cast<UINT32>(meta.size()), bodyFmt,
                           D2D1::RectF(x, y, textRight, y + 24.0f), mutedBrush);

        y += 30.0f;
        if (bodyFmt && titleBrush)
            ctx->DrawTextW(lib->description.c_str(), static_cast<UINT32>(lib->description.size()), bodyFmt,
                           D2D1::RectF(x, y, textRight, y + 70.0f), titleBrush);

        y += 76.0f;
        std::wstring stats = L"Rating " + std::to_wstring(lib->rating).substr(0, 3) + L" / 5    Downloads " + std::to_wstring(lib->downloads) +
                             L"    Stars " + std::to_wstring(lib->stars);
        if (bodyFmt && mutedBrush)
            ctx->DrawTextW(stats.c_str(), static_cast<UINT32>(stats.size()), bodyFmt,
                           D2D1::RectF(x, y, textRight, y + 24.0f), mutedBrush);

        y += 28.0f;
        if (bodyFmt && mutedBrush)
            ctx->DrawTextW(lib->gitUrl.c_str(), static_cast<UINT32>(lib->gitUrl.size()), bodyFmt,
                           D2D1::RectF(x, y, textRight, y + 22.0f), mutedBrush);

        if (buttonBrush)
            ctx->FillRoundedRectangle(D2D1::RoundedRect(actionButtonRect_, 5.0f, 5.0f), buttonBrush);

        if (buttonFmt && buttonTextBrush)
        {
            const wchar_t *label = lib->isInstalled ? L"Uninstall" : L"Install";
            ctx->DrawTextW(label, static_cast<UINT32>(wcslen(label)), buttonFmt, actionButtonRect_, buttonTextBrush);
        }
    }

    if (titleFmt)
        titleFmt->Release();
    if (bodyFmt)
        bodyFmt->Release();
    if (buttonFmt)
        buttonFmt->Release();

    if (bgBrush)
        bgBrush->Release();
    if (cardBrush)
        cardBrush->Release();
    if (borderBrush)
        borderBrush->Release();
    if (titleBrush)
        titleBrush->Release();
    if (mutedBrush)
        mutedBrush->Release();
    if (buttonBrush)
        buttonBrush->Release();
    if (buttonTextBrush)
        buttonTextBrush->Release();
}

void MarketplaceExtensionTabView::OnMouseMove(HWND hwnd, POINT clientPoint)
{
    const bool wasHover = actionButtonHovered_;
    actionButtonHovered_ = IsPointInRect(clientPoint, actionButtonRect_);
    if (wasHover != actionButtonHovered_)
        InvalidateRect(hwnd, nullptr, FALSE);
}

void MarketplaceExtensionTabView::OnLeftButtonDown(HWND hwnd, POINT clientPoint)
{
    if (!IsPointInRect(clientPoint, actionButtonRect_))
        return;

    if (currentLibraryName_.empty())
        return;

    LibraryInfo *lib = LibraryDatabase::Instance().FindLibrary(currentLibraryName_);
    if (!lib)
        return;

    if (!lib->isInstalled)
    {
        std::wstring installError;
        bool ok = GetExplorerManager().InstallLibraryFromGitUrl(hwnd, lib->gitUrl, lib->name, &installError);
        if (ok)
        {
            LibraryDatabase::Instance().SetInstalled(currentLibraryName_, true);
            Logger::Instance().Log(L"Installed: " + currentLibraryName_);
        }
        else
        {
            if (installError.empty())
                installError = L"Failed to install library.";
            MessageBoxW(hwnd, installError.c_str(), L"Marketplace Install", MB_OK | MB_ICONERROR);
            Logger::Instance().Log(L"Install failed: " + currentLibraryName_ + L" - " + installError);
        }
    }
    else
    {
        LibraryDatabase::Instance().SetInstalled(currentLibraryName_, false);
        Logger::Instance().Log(L"Uninstalled (state only): " + currentLibraryName_);
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

void MarketplaceExtensionTabView::OnLeftButtonUp(HWND /*hwnd*/)
{
}

bool MarketplaceExtensionTabView::IsPointInView(POINT clientPoint) const
{
    return IsPointInRect(clientPoint, bounds_);
}
