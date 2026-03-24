#include "MarketplacePanel.h"
#include "core/window/Window.h"
#include "core/explorer/Explorer.h"
#include "ui/theme/Theme.h"
#include "helpers/window_helpers.h"
#include "utils/logger/Logger.h"
#include <sstream>
#include <cmath>
#include <algorithm>
#include <cwctype>
#include <cctype>

// ---------------------------------------------------------------------------
// Design constants
// ---------------------------------------------------------------------------
static constexpr float kRowH      = 88.0f;   // card row height
static constexpr float kIconSz    = 44.0f;   // colored icon block size
static constexpr float kIconLeft  = 16.0f;   // icon left margin
static constexpr float kTextLeft  = 72.0f;   // text content left margin
static constexpr float kBtnW      = 84.0f;   // button width
static constexpr float kBtnH      = 28.0f;   // button height
static constexpr float kBtnRight  = 14.0f;   // button right margin
static constexpr float kBadgePadX = 8.0f;
static constexpr float kBadgeH    = 16.0f;
static constexpr float kBadgeR    = 3.0f;

// ---------------------------------------------------------------------------
// Helper: Category color map
// ---------------------------------------------------------------------------
static D2D1_COLOR_F CategoryColor(const std::wstring& cat)
{
    std::wstring l = cat;
    std::transform(l.begin(), l.end(), l.begin(), [](wchar_t c) { 
        return std::tolower(static_cast<unsigned char>(c)); 
    });

    if (l == L"ui")         return D2D1::ColorF(0.32f, 0.58f, 0.89f);
    if (l == L"graphics")   return D2D1::ColorF(0.15f, 0.65f, 0.60f);
    if (l == L"math")       return D2D1::ColorF(0.49f, 0.34f, 0.76f);
    if (l == L"utilities")  return D2D1::ColorF(1.00f, 0.44f, 0.26f);
    if (l == L"audio")      return D2D1::ColorF(0.93f, 0.25f, 0.48f);
    if (l == L"networking") return D2D1::ColorF(0.15f, 0.78f, 0.85f);
    if (l == L"physics")    return D2D1::ColorF(0.94f, 0.33f, 0.31f);
    if (l == L"testing")    return D2D1::ColorF(0.20f, 0.75f, 0.35f);
    return UI::Theme::Accent();
}

// ---------------------------------------------------------------------------
// Helper: Extract initials from library name
// ---------------------------------------------------------------------------
static std::wstring Initials(const std::wstring& name)
{
    std::wstring result;
    bool nextUpper = true;
    for (wchar_t c : name)
    {
        if (c == L'/' || c == L' ' || c == L'_') { nextUpper = true; continue; }
        if (nextUpper && std::iswalpha(c)) { 
            result += std::towupper(c); 
            nextUpper = false; 
        }
        else if (std::iswupper(c) && !result.empty()) 
            result += c;
        if (result.size() >= 2) break;
    }
    if (result.empty() && !name.empty()) 
        result += std::towupper(name[0]);
    if (result.size() == 1 && name.size() > 1) 
        result += std::towupper(name[1]);
    return result;
}

// ---------------------------------------------------------------------------
// MarketplacePanel constructor
// ---------------------------------------------------------------------------
MarketplacePanel::MarketplacePanel()
    : Panel(PanelId::Marketplace)
{
    title_ = L"Marketplace";
    config_ = PanelConfig(
        PanelId::Marketplace,
        L"\uE7BF",
        L"Marketplace",
        true,
        false,
        4
    );
}

void MarketplacePanel::Initialize()
{
    LibraryDatabase::Instance().RefreshInstallationStatus();
    UpdateCardLayout();
}

void MarketplacePanel::UpdateLayout(HWND hwnd)
{
    RECT client;
    GetClientRect(hwnd, &client);

    UINT dpi = win32_get_dpi_for_window(hwnd);
    RECT tbRect = win32_titlebar_rect(hwnd);
    int sidebarWidth = win32_dpi_scale(52, dpi);
    int footerHeight = win32_dpi_scale(28, dpi);

    float scale = dpi / 96.0f;
    state_.physicalWidth = static_cast<int>(state_.logicalWidth * scale);

    state_.leftEdge   = static_cast<float>(client.left + sidebarWidth);
    state_.rightEdge  = state_.leftEdge + static_cast<float>(state_.physicalWidth);
    state_.topEdge    = static_cast<float>(tbRect.bottom);
    state_.bottomEdge = static_cast<float>(client.bottom - footerHeight);

    if (!visible_) {
        state_.physicalWidth = 0;
        state_.rightEdge = state_.leftEdge;
    }

    UpdateCardLayout();
}

void MarketplacePanel::UpdateCardLayout()
{
    cards_.clear();

    float x  = state_.leftEdge;
    float w  = state_.rightEdge - state_.leftEdge;
    float y  = state_.topEdge + state_.titleHeight + scrollOffset_;

    const auto& libs = LibraryDatabase::Instance().GetLibraries();
    for (size_t i = 0; i < libs.size(); ++i)
    {
        LibraryCard card;
        card.library = const_cast<LibraryInfo*>(&libs[i]);
        card.bounds  = D2D1::RectF(x, y, x + w, y + kRowH);

        // Button: vertically centered, flush right
        float btnX = card.bounds.right  - kBtnRight - kBtnW;
        float btnY = card.bounds.top    + (kRowH - kBtnH) * 0.5f;

        if (libs[i].isInstalled)
        {
            card.uninstallButtonBounds = D2D1::RectF(btnX, btnY, btnX + kBtnW, btnY + kBtnH);
        }
        else
        {
            card.installButtonBounds = D2D1::RectF(btnX, btnY, btnX + kBtnW, btnY + kBtnH);
        }

        cards_.push_back(card);
        y += kRowH;
    }
}

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------
void MarketplacePanel::Draw(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite, HWND hwnd)
{
    // Refresh installation status whenever the active project changes
    const std::wstring& currentRoot = GetExplorerManager().GetState().rootPath;
    if (currentRoot != lastKnownRootPath_)
    {
        lastKnownRootPath_ = currentRoot;
        LibraryDatabase::Instance().RefreshInstallationStatus();
        UpdateCardLayout();
    }

    DrawBackground(ctx);
    DrawTitle(ctx, dwrite);

    // Clip to content area
    D2D1_RECT_F clip = D2D1::RectF(
        state_.leftEdge,   state_.topEdge + state_.titleHeight,
        state_.rightEdge,  state_.bottomEdge);
    ctx->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);

    for (auto& card : cards_)
        DrawLibraryCard(ctx, dwrite, card);

    ctx->PopAxisAlignedClip();

    DrawRightBorder(ctx);
}

// ---------------------------------------------------------------------------
// DrawLibraryCard — compact horizontal row design
// ---------------------------------------------------------------------------
void MarketplacePanel::DrawLibraryCard(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite,
                                        LibraryCard& card)
{
    if (!card.library) return;

    const float L  = card.bounds.left;
    const float R  = card.bounds.right;
    const float T  = card.bounds.top;
    const float B  = card.bounds.bottom;
    const float W  = R - L;

    // ── Row hover background ────────────────────────────────────────────────
    if (card.isHoveringCard)
    {
        ID2D1SolidColorBrush* hov = nullptr;
        ctx->CreateSolidColorBrush(UI::Theme::GetPalette().explorerRowHover, &hov);
        if (hov) { ctx->FillRectangle(card.bounds, hov); hov->Release(); }
    }

    // ── Bottom separator line ───────────────────────────────────────────────
    {
        ID2D1SolidColorBrush* sep = nullptr;
        D2D1_COLOR_F sepCol = UI::Theme::ChromeBorder();
        sepCol.a *= 0.6f;
        ctx->CreateSolidColorBrush(sepCol, &sep);
        if (sep) {
            ctx->DrawLine(D2D1::Point2F(L + kTextLeft, B - 0.5f),
                          D2D1::Point2F(R, B - 0.5f), sep, 0.5f);
            sep->Release();
        }
    }

    // ── Colored icon block ──────────────────────────────────────────────────
    D2D1_COLOR_F catColor = CategoryColor(card.library->category);
    float iconX = L + kIconLeft;
    float iconY = T + (kRowH - kIconSz) * 0.5f;
    D2D1_RECT_F iconRect = D2D1::RectF(iconX, iconY, iconX + kIconSz, iconY + kIconSz);

    // Icon background (gradient-like: filled + darker right edge)
    {
        ID2D1SolidColorBrush* ibr = nullptr;
        ctx->CreateSolidColorBrush(catColor, &ibr);
        if (ibr) {
            ctx->FillRoundedRectangle(D2D1::RoundedRect(iconRect, 8.0f, 8.0f), ibr);
            ibr->Release();
        }
    }

    // Icon initials (2 chars, white, bold, centered)
    {
        std::wstring ini = Initials(card.library->name);
        IDWriteTextFormat* fmt = nullptr;
        dwrite->CreateTextFormat(L"Segoe UI Variable Display", nullptr,
            DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 15.0f, L"en-us", &fmt);
        if (!fmt)
            dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                15.0f, L"en-us", &fmt);
        ID2D1SolidColorBrush* wbr = nullptr;
        ctx->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.92f), &wbr);
        if (fmt && wbr) {
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            ctx->DrawTextW(ini.c_str(), (UINT32)ini.size(), fmt, iconRect, wbr);
        }
        if (fmt) fmt->Release();
        if (wbr) wbr->Release();
    }

    // ── Text content area ───────────────────────────────────────────────────
    float textL = L + kTextLeft;
    float btnAreaW = kBtnW + kBtnRight + 8.0f;
    float textR = R - btnAreaW;

    // Name (bold, 13.5px) + version (muted, right of name)
    {
        IDWriteTextFormat* nameFmt = nullptr;
        dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 13.5f, L"en-us", &nameFmt);
        if (!nameFmt)
            dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                13.5f, L"en-us", &nameFmt);

        ID2D1SolidColorBrush* txtBr = nullptr;
        ctx->CreateSolidColorBrush(UI::Theme::PrimaryText(), &txtBr);

        if (nameFmt && txtBr) {
            nameFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            nameFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            D2D1_RECT_F nameRect = D2D1::RectF(textL, T + 14.0f, textR, T + 33.0f);
            ctx->DrawTextW(card.library->name.c_str(),
                           (UINT32)card.library->name.size(), nameFmt, nameRect, txtBr);
        }
        if (nameFmt) nameFmt->Release();
        if (txtBr)   txtBr->Release();
    }

    // Version badge (after name, muted small text)
    if (!card.library->version.empty())
    {
        IDWriteTextFormat* vFmt = nullptr;
        dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            10.5f, L"en-us", &vFmt);
        ID2D1SolidColorBrush* vBr = nullptr;
        D2D1_COLOR_F mutedCol = UI::Theme::MutedText();
        mutedCol.a *= 0.7f;
        ctx->CreateSolidColorBrush(mutedCol, &vBr);
        if (vFmt && vBr) {
            vFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            vFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            std::wstring ver = L"v" + card.library->version;
            // Estimate name width to place version to the right
            // (simple: render at fixed offset after typical name length)
            D2D1_RECT_F vRect = D2D1::RectF(textL, T + 16.0f, textR, T + 30.0f);
            // We draw it right-aligned to avoid overlap
            vFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            ctx->DrawTextW(ver.c_str(), (UINT32)ver.size(), vFmt, vRect, vBr);
        }
        if (vFmt) vFmt->Release();
        if (vBr)  vBr->Release();
    }

    // Description (12px, muted, single line clipped)
    {
        IDWriteTextFormat* dFmt = nullptr;
        dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 11.5f, L"en-us", &dFmt);
        if (!dFmt)
            dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                11.5f, L"en-us", &dFmt);
        ID2D1SolidColorBrush* dBr = nullptr;
        ctx->CreateSolidColorBrush(UI::Theme::MutedText(), &dBr);
        if (dFmt && dBr) {
            dFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            dFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            dFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            D2D1_RECT_F descRect = D2D1::RectF(textL, T + 36.0f, textR, T + 54.0f);
            ctx->DrawTextW(card.library->description.c_str(),
                           (UINT32)card.library->description.size(),
                           dFmt, descRect, dBr,
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (dFmt) dFmt->Release();
        if (dBr)  dBr->Release();
    }

    // Category pill badge
    DrawCardCategory(ctx, dwrite, card.library->category, catColor,
                     textL, T + 58.0f);

    // ── Install / Uninstall button ──────────────────────────────────────────
    if (card.library->isInstalled)
        DrawInstalledButton(ctx, dwrite, card.uninstallButtonBounds,
                            card.isHoveringUninstallBtn);
    else
        DrawInstallButton(ctx, dwrite, card.installButtonBounds,
                          card.isHoveringInstallBtn);
}

// ---------------------------------------------------------------------------
// DrawCardCategory — small pill badge with category-specific color
// ---------------------------------------------------------------------------
void MarketplacePanel::DrawCardCategory(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite,
                                         const std::wstring& category,
                                         D2D1_COLOR_F color,
                                         float x, float y)
{
    // Measure text to compute badge width
    IDWriteTextFormat* fmt = nullptr;
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        10.0f, L"en-us", &fmt);

    float badgeW = (float)category.size() * 6.5f + kBadgePadX * 2.0f;
    badgeW = (std::max)(badgeW, 32.0f);

    D2D1_RECT_F badgeRect = D2D1::RectF(x, y, x + badgeW, y + kBadgeH);

    // Badge background — very subtle, category-tinted
    D2D1_COLOR_F bgCol = D2D1::ColorF(color.r, color.g, color.b, 0.18f);
    ID2D1SolidColorBrush* bgBr = nullptr;
    ctx->CreateSolidColorBrush(bgCol, &bgBr);
    if (bgBr) {
        ctx->FillRoundedRectangle(D2D1::RoundedRect(badgeRect, kBadgeR, kBadgeR), bgBr);
        bgBr->Release();
    }

    // Badge text — category color
    ID2D1SolidColorBrush* txtBr = nullptr;
    D2D1_COLOR_F txtCol = D2D1::ColorF(color.r, color.g, color.b, 0.90f);
    ctx->CreateSolidColorBrush(txtCol, &txtBr);
    if (fmt && txtBr) {
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        ctx->DrawTextW(category.c_str(), (UINT32)category.size(), fmt, badgeRect, txtBr);
    }
    if (fmt)   fmt->Release();
    if (txtBr) txtBr->Release();
}

// ---------------------------------------------------------------------------
// DrawInstallButton — accent blue, white text
// ---------------------------------------------------------------------------
void MarketplacePanel::DrawInstallButton(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite,
                                          const D2D1_RECT_F& bounds, bool hover)
{
    D2D1_COLOR_F bg = hover ? UI::Theme::AccentStrong() : UI::Theme::Accent();

    ID2D1SolidColorBrush* bgBr = nullptr;
    ctx->CreateSolidColorBrush(bg, &bgBr);
    if (bgBr) {
        ctx->FillRoundedRectangle(D2D1::RoundedRect(bounds, 5.0f, 5.0f), bgBr);
        bgBr->Release();
    }

    IDWriteTextFormat* fmt = nullptr;
    dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 11.5f, L"en-us", &fmt);
    if (!fmt)
        dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            11.5f, L"en-us", &fmt);

    ID2D1SolidColorBrush* txtBr = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &txtBr);
    if (fmt && txtBr) {
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        ctx->DrawTextW(L"Install", 7, fmt, bounds, txtBr);
    }
    if (fmt)   fmt->Release();
    if (txtBr) txtBr->Release();
}

// ---------------------------------------------------------------------------
// DrawInstalledButton — outlined green when not hovering, red "Remove" on hover
// ---------------------------------------------------------------------------
void MarketplacePanel::DrawInstalledButton(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite,
                                            const D2D1_RECT_F& bounds, bool hover)
{
    static const D2D1_COLOR_F kGreen = D2D1::ColorF(0.31f, 0.78f, 0.47f);
    static const D2D1_COLOR_F kRed   = D2D1::ColorF(0.94f, 0.33f, 0.31f);

    if (hover)
    {
        // Red filled "Remove" button
        ID2D1SolidColorBrush* bgBr = nullptr;
        D2D1_COLOR_F redBg = D2D1::ColorF(kRed.r, kRed.g, kRed.b, 0.15f);
        ctx->CreateSolidColorBrush(redBg, &bgBr);
        if (bgBr) {
            ctx->FillRoundedRectangle(D2D1::RoundedRect(bounds, 5.0f, 5.0f), bgBr);
            bgBr->Release();
        }
        ID2D1SolidColorBrush* brdBr = nullptr;
        ctx->CreateSolidColorBrush(D2D1::ColorF(kRed.r, kRed.g, kRed.b, 0.6f), &brdBr);
        if (brdBr) {
            ctx->DrawRoundedRectangle(D2D1::RoundedRect(bounds, 5.0f, 5.0f), brdBr, 1.0f);
            brdBr->Release();
        }

        IDWriteTextFormat* fmt = nullptr;
        dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.5f, L"en-us", &fmt);
        ID2D1SolidColorBrush* txtBr = nullptr;
        ctx->CreateSolidColorBrush(kRed, &txtBr);
        if (fmt && txtBr) {
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            ctx->DrawTextW(L"Remove", 6, fmt, bounds, txtBr);
        }
        if (fmt)   fmt->Release();
        if (txtBr) txtBr->Release();
    }
    else
    {
        // Green outlined "Installed ✓"
        ID2D1SolidColorBrush* brdBr = nullptr;
        ctx->CreateSolidColorBrush(D2D1::ColorF(kGreen.r, kGreen.g, kGreen.b, 0.5f), &brdBr);
        if (brdBr) {
            ctx->DrawRoundedRectangle(D2D1::RoundedRect(bounds, 5.0f, 5.0f), brdBr, 1.0f);
            brdBr->Release();
        }

        IDWriteTextFormat* fmt = nullptr;
        dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"en-us", &fmt);
        ID2D1SolidColorBrush* txtBr = nullptr;
        ctx->CreateSolidColorBrush(kGreen, &txtBr);
        if (fmt && txtBr) {
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            ctx->DrawTextW(L"Installed \u2713", 12, fmt, bounds, txtBr);
        }
        if (fmt)   fmt->Release();
        if (txtBr) txtBr->Release();
    }
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------
void MarketplacePanel::OnMouseMove(HWND hwnd, POINT clientPoint)
{
    HandleResizeMouseMove(hwnd, clientPoint);
    if (state_.isResizing || state_.isHoveringResizeZone) return;

    bool changed = false;
    for (auto& card : cards_)
    {
        bool wasCard = card.isHoveringCard;
        card.isHoveringCard =
            clientPoint.x >= (int)card.bounds.left  &&
            clientPoint.x <= (int)card.bounds.right &&
            clientPoint.y >= (int)card.bounds.top   &&
            clientPoint.y <= (int)card.bounds.bottom;

        auto htest = [&](const D2D1_RECT_F& r) {
            return clientPoint.x >= (int)r.left  && clientPoint.x <= (int)r.right &&
                   clientPoint.y >= (int)r.top   && clientPoint.y <= (int)r.bottom;
        };

        bool wasI  = card.isHoveringInstallBtn;
        bool wasU  = card.isHoveringUninstallBtn;
        card.isHoveringInstallBtn   = card.isHoveringCard && htest(card.installButtonBounds);
        card.isHoveringUninstallBtn = card.isHoveringCard && htest(card.uninstallButtonBounds);

        if (wasCard != card.isHoveringCard ||
            wasI    != card.isHoveringInstallBtn ||
            wasU    != card.isHoveringUninstallBtn)
            changed = true;
    }

    if (changed) InvalidateRect(hwnd, nullptr, FALSE);
}

void MarketplacePanel::OnLeftButtonDown(HWND hwnd, POINT clientPoint)
{
    if (HandleResizeLeftButtonDown(hwnd, clientPoint)) return;

    auto hit = [&](const D2D1_RECT_F& r) {
        return clientPoint.x >= (int)r.left  && clientPoint.x <= (int)r.right &&
               clientPoint.y >= (int)r.top   && clientPoint.y <= (int)r.bottom;
    };

    for (auto& card : cards_)
    {
        if (!card.library)
            continue;

        if (hit(card.installButtonBounds))
        {
            HandleInstallLibrary(hwnd, &card);
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }

        if (hit(card.uninstallButtonBounds))
        {
            HandleUninstallLibrary(hwnd, &card);
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }

        if (hit(card.bounds))
        {
            auto *libraryName = new std::wstring(card.library->name);
            if (!PostMessageW(hwnd, WM_OPEN_MARKETPLACE_LIBRARY, 0, reinterpret_cast<LPARAM>(libraryName)))
                delete libraryName;
            return;
        }
    }
}

void MarketplacePanel::OnLeftButtonUp(HWND hwnd)
{
    HandleResizeLeftButtonUp(hwnd);
}

void MarketplacePanel::OnMouseWheel(HWND hwnd, int delta)
{
    scrollOffset_ += delta * 0.5f;
    float minScroll = -((float)cards_.size() * kRowH - (state_.bottomEdge - state_.topEdge - state_.titleHeight));
    if (minScroll > 0.0f) minScroll = 0.0f;
    scrollOffset_ = (std::max)(minScroll, (std::min)(0.0f, scrollOffset_));
    UpdateCardLayout();
    InvalidateRect(hwnd, nullptr, FALSE);
}

void MarketplacePanel::HandleInstallLibrary(HWND hwnd, LibraryCard* card)
{
    if (!card || !card->library) return;

    std::wstring installError;
    bool ok = GetExplorerManager().InstallLibraryFromGitUrl(
        hwnd,
        card->library->gitUrl,
        card->library->name,
        &installError);

    if (ok)
    {
        LibraryDatabase::Instance().SetInstalled(card->library->name, true);
        Logger::Instance().Log(L"Installing library: " + card->library->name);
    }
    else
    {
        LibraryDatabase::Instance().SetInstalled(card->library->name, false);
        if (installError.empty())
            installError = L"Failed to install library.";
        MessageBoxW(hwnd, installError.c_str(), L"Marketplace Install", MB_OK | MB_ICONERROR);
        Logger::Instance().Log(L"Install failed for " + card->library->name + L": " + installError);
    }
}

void MarketplacePanel::HandleUninstallLibrary(HWND hwnd, LibraryCard* card)
{
    (void)hwnd;
    if (!card || !card->library) return;
    LibraryDatabase::Instance().SetInstalled(card->library->name, false);
    Logger::Instance().Log(L"Uninstalling library: " + card->library->name);
}
