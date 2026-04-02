#include "MarketplacePanel.h"
#include "core/window/Window.h"
#include "core/explorer/Explorer.h"
#include "ui/marketplace/MarketplaceVisuals.h"
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
static constexpr float kControlsAreaH = 82.0f;

// ---------------------------------------------------------------------------
// MarketplacePanel constructor
// ---------------------------------------------------------------------------
MarketplacePanel::MarketplacePanel()
    : Panel(PanelId::Marketplace)
{
    title_ = L"Libraries";
    config_ = PanelConfig(
        PanelId::Marketplace,
        L"assets/ressource/icons/cpp-libs-sidebar.svg",
        L"C++ Libraries",
        true,
        false,
        4
    );
}

void MarketplacePanel::Initialize()
{
    searchInput_.SetPlaceholder(L"Search libraries...");
    searchInput_.SetIcon(L"\uE721");  // Search icon from Segoe MDL2
    searchInput_.SetIconFont(L"Segoe MDL2 Assets");
    searchInput_.GetStyle().useSearchBoxStyle = false;
    searchInput_.GetStyle().fontSize = 12.5f;
    searchInput_.onTextChanged = [this](const std::wstring& text) {
        searchQuery_ = text;
        scrollOffset_ = 0.0f;
        if (hwnd_)
            LibraryDatabase::Instance().RequestLibrariesAsync(searchQuery_, hwnd_);
    };

    LibraryDatabase::Instance().RefreshInstallationStatus(GetExplorerManager().GetState().rootPath);
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

    // Position search bar just below the title
    const float kSearchPad  = 8.0f;
    const float kSearchH    = 32.0f;
    float searchTop = state_.topEdge + state_.titleHeight + kSearchPad;
    searchBarBounds_ = D2D1::RectF(
        state_.leftEdge  + kSearchPad,
        searchTop,
        state_.rightEdge - kSearchPad,
        searchTop + kSearchH);
    searchInput_.SetRect(searchBarBounds_);

    const float filterTop = searchBarBounds_.bottom + 8.0f;
    const float filterH = 24.0f;
    allFilterBounds_ = D2D1::RectF(
        state_.leftEdge + kSearchPad,
        filterTop,
        state_.leftEdge + kSearchPad + 52.0f,
        filterTop + filterH);
    installedFilterBounds_ = D2D1::RectF(
        allFilterBounds_.right + 8.0f,
        filterTop,
        allFilterBounds_.right + 8.0f + 76.0f,
        filterTop + filterH);

    UpdateCardLayout();
}

void MarketplacePanel::UpdateCardLayout()
{
    cards_.clear();

    float x  = state_.leftEdge;
    float w  = state_.rightEdge - state_.leftEdge;
    float y  = state_.topEdge + state_.titleHeight + kControlsAreaH + scrollOffset_;

    const std::vector<LibraryInfo> libs = LibraryDatabase::Instance().GetLibrariesCopy();
    for (size_t i = 0; i < libs.size(); ++i)
    {
        if (showInstalledOnly_ && !libs[i].isInstalled)
            continue;

        LibraryCard card;
        card.library = libs[i];
        card.bounds  = D2D1::RectF(x, y, x + w, y + kRowH);
        card.installButtonBounds = D2D1::RectF(0, 0, 0, 0);
        card.uninstallButtonBounds = D2D1::RectF(0, 0, 0, 0);

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
    hwnd_ = hwnd;
    LibraryDatabase::Instance().RequestLibrariesAsync(searchQuery_, hwnd);

    // Refresh installation status whenever the active project changes
    const std::wstring& currentRoot = GetExplorerManager().GetState().rootPath;
    if (currentRoot != lastKnownRootPath_)
    {
        lastKnownRootPath_ = currentRoot;
        LibraryDatabase::Instance().RefreshInstallationStatus(currentRoot);
    }

    const unsigned long long revision = LibraryDatabase::Instance().GetRevision();
    if (revision != dataRevision_)
    {
        dataRevision_ = revision;
        UpdateCardLayout();
    }

    DrawBackground(ctx);
    DrawTitle(ctx, dwrite);

    // Draw search bar (outside clip so it's always visible)
    searchInput_.Draw(ctx, dwrite);
    DrawButton(ctx, dwrite, allFilterBounds_, L"All", isHoveringAllFilter_, !showInstalledOnly_);
    DrawButton(ctx, dwrite, installedFilterBounds_, L"Installed", isHoveringInstalledFilter_, showInstalledOnly_);

    // Clip to content area (below search bar)
    D2D1_RECT_F clip = D2D1::RectF(
        state_.leftEdge,   state_.topEdge + state_.titleHeight + kControlsAreaH,
        state_.rightEdge,  state_.bottomEdge);
    ctx->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);

    if (!cards_.empty())
    {
        for (auto& card : cards_)
            DrawLibraryCard(ctx, dwrite, card);
    }
    else
    {
        const std::wstring error = LibraryDatabase::Instance().GetLastError();
        const std::wstring message = !error.empty()
            ? error
            : (LibraryDatabase::Instance().IsLoading()
                ? L"Loading C++ libraries..."
                : (showInstalledOnly_
                    ? L"No installed libraries found."
                    : L"No libraries found."));

        IDWriteTextFormat* fmt = nullptr;
        dwrite->CreateTextFormat(
            L"Segoe UI Variable Text",
            nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            12.5f,
            L"en-us",
            &fmt);
        if (!fmt)
            dwrite->CreateTextFormat(
                L"Segoe UI",
                nullptr,
                DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                12.5f,
                L"en-us",
                &fmt);

        ID2D1SolidColorBrush* brush = nullptr;
        ctx->CreateSolidColorBrush(UI::Theme::MutedText(), &brush);
        if (fmt && brush)
        {
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            ctx->DrawTextW(message.c_str(), static_cast<UINT32>(message.size()), fmt, clip, brush);
        }
        if (fmt) fmt->Release();
        if (brush) brush->Release();
    }

    ctx->PopAxisAlignedClip();

    DrawRightBorder(ctx);
}

// ---------------------------------------------------------------------------
// DrawLibraryCard — compact horizontal row design
// ---------------------------------------------------------------------------
void MarketplacePanel::DrawLibraryCard(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite,
                                        LibraryCard& card)
{
    if (card.library.name.empty()) return;

    const float L  = card.bounds.left;
    const float R  = card.bounds.right;
    const float T  = card.bounds.top;
    const float B  = card.bounds.bottom;
    // ── Row hover background ────────────────────────────────────────────────
    if (card.isHoveringCard)
    {
        D2D1_COLOR_F cat = MarketplaceCategoryColor(card.library.category);

        // Strong overlay to make hover state clearly visible.
        ID2D1SolidColorBrush* hovBg = nullptr;
        ctx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f), &hovBg);
        if (hovBg) {
            ctx->FillRectangle(card.bounds, hovBg);
            hovBg->Release();
        }

        // Thin outline helps distinguish the hovered row from adjacent rows.
        ID2D1SolidColorBrush* hovBorder = nullptr;
        ctx->CreateSolidColorBrush(D2D1::ColorF(cat.r, cat.g, cat.b, 0.55f), &hovBorder);
        if (hovBorder) {
            D2D1_RECT_F borderRect = D2D1::RectF(
                card.bounds.left + 0.5f,
                card.bounds.top + 0.5f,
                card.bounds.right - 0.5f,
                card.bounds.bottom - 0.5f);
            ctx->DrawRectangle(borderRect, hovBorder, 1.5f);
            hovBorder->Release();
        }

        // Left accent bar remains for quick scan.
        ID2D1SolidColorBrush* hovBar = nullptr;
        ctx->CreateSolidColorBrush(cat, &hovBar);
        if (hovBar) {
            D2D1_RECT_F bar = D2D1::RectF(card.bounds.left, card.bounds.top + 5.0f,
                                          card.bounds.left + 3.0f, card.bounds.bottom - 5.0f);
            ctx->FillRectangle(bar, hovBar);
            hovBar->Release();
        }
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
    D2D1_COLOR_F catColor = MarketplaceCategoryColor(card.library.category);
    float iconX = L + kIconLeft;
    float iconY = T + (kRowH - kIconSz) * 0.5f;
    D2D1_RECT_F iconRect = D2D1::RectF(iconX, iconY, iconX + kIconSz, iconY + kIconSz);

    MarketplaceEnsureAvatarAsync(card.library.avatarUrl, hwnd_);
    ID2D1Bitmap* avatar = MarketplaceLoadAvatarBitmap(ctx, card.library.avatarUrl);
    if (avatar)
    {
        ctx->DrawBitmap(avatar, iconRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        ID2D1SolidColorBrush* border = nullptr;
        ctx->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.10f), &border);
        if (border) {
            ctx->DrawRoundedRectangle(D2D1::RoundedRect(iconRect, 8.0f, 8.0f), border, 1.0f);
            border->Release();
        }
    }
    else
    {
        ID2D1SolidColorBrush* bg = nullptr;
        ctx->CreateSolidColorBrush(catColor, &bg);
        if (bg) {
            ctx->FillRoundedRectangle(D2D1::RoundedRect(iconRect, 8.0f, 8.0f), bg);
            bg->Release();
        }

        std::wstring ini = MarketplaceInitials(card.library.name);
        IDWriteTextFormat* fmt = nullptr;
        dwrite->CreateTextFormat(L"Segoe UI Variable Display", nullptr,
            DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 15.0f, L"en-us", &fmt);
        if (!fmt)
            dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                15.0f, L"en-us", &fmt);
        ID2D1SolidColorBrush* fg = nullptr;
        ctx->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.92f), &fg);
        if (fmt && fg) {
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            ctx->DrawTextW(ini.c_str(), (UINT32)ini.size(), fmt, iconRect, fg);
        }
        if (fmt) fmt->Release();
        if (fg) fg->Release();
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
            D2D1_RECT_F nameRect = D2D1::RectF(textL, T + 9.0f, textR, T + 24.0f);
            ctx->DrawTextW(card.library.name.c_str(),
                           (UINT32)card.library.name.size(), nameFmt, nameRect, txtBr);
        }
        if (nameFmt) nameFmt->Release();
        if (txtBr)   txtBr->Release();
    }

    // Version badge (right-aligned, same row as name)
    if (!card.library.version.empty())
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
            std::wstring ver = L"v" + card.library.version;
            vFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            vFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            D2D1_RECT_F vRect = D2D1::RectF(textL, T + 11.0f, textR, T + 24.0f);
            ctx->DrawTextW(ver.c_str(), (UINT32)ver.size(), vFmt, vRect, vBr);
        }
        if (vFmt) vFmt->Release();
        if (vBr)  vBr->Release();
    }

    // Author line ("by authorname") — CLion style
    if (!card.library.author.empty())
    {
        IDWriteTextFormat* aFmt = nullptr;
        dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"en-us", &aFmt);
        if (!aFmt)
            dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                11.0f, L"en-us", &aFmt);
        ID2D1SolidColorBrush* aBr = nullptr;
        D2D1_COLOR_F authorCol = UI::Theme::MutedText();
        authorCol.a *= 0.75f;
        ctx->CreateSolidColorBrush(authorCol, &aBr);
        if (aFmt && aBr) {
            aFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            aFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            aFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            std::wstring byLine = L"by " + card.library.author;
            D2D1_RECT_F aRect = D2D1::RectF(textL, T + 26.0f, textR, T + 38.0f);
            ctx->DrawTextW(byLine.c_str(), (UINT32)byLine.size(), aFmt, aRect, aBr,
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (aFmt) aFmt->Release();
        if (aBr)  aBr->Release();
    }

    // Description (muted, single line clipped)
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
            D2D1_RECT_F descRect = D2D1::RectF(textL, T + 40.0f, textR, T + 54.0f);
            ctx->DrawTextW(card.library.description.c_str(),
                           (UINT32)card.library.description.size(),
                           dFmt, descRect, dBr,
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (dFmt) dFmt->Release();
        if (dBr)  dBr->Release();
    }

    if (card.library.installState != LibraryInfo::InstallState::Idle || !card.library.installMessage.empty())
    {
        DrawCardStatus(ctx, dwrite, card.library, textL, T + 57.0f, textR - textL);
    }
    else
    {
        DrawCardCategory(ctx, dwrite, card.library.category, catColor,
                         textL, T + 57.0f);
    }

    // ── Install / Uninstall button ──────────────────────────────────────────
    if (card.library.installState == LibraryInfo::InstallState::Installing)
    {
        DrawInstallButton(ctx, dwrite, card.installButtonBounds, false, L"Installing", false);
    }
    else if (card.library.isInstalled)
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

void MarketplacePanel::DrawCardStatus(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite,
                                      const LibraryInfo& lib, float x, float y, float width)
{
    std::wstring status = lib.installMessage;
    if (status.empty())
    {
        if (lib.installState == LibraryInfo::InstallState::Installing)
            status = L"Installing...";
        else if (lib.installState == LibraryInfo::InstallState::Error)
            status = L"Install failed.";
    }

    if (status.empty())
        return;

    IDWriteTextFormat* fmt = nullptr;
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 10.5f, L"en-us", &fmt);

    D2D1_COLOR_F color = UI::Theme::MutedText();
    if (lib.installState == LibraryInfo::InstallState::Installing)
        color = UI::Theme::Accent();
    else if (lib.installState == LibraryInfo::InstallState::Error)
        color = D2D1::ColorF(0.90f, 0.38f, 0.35f, 1.0f);

    ID2D1SolidColorBrush* brush = nullptr;
    ctx->CreateSolidColorBrush(color, &brush);
    if (fmt && brush)
    {
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        D2D1_RECT_F rect = D2D1::RectF(x, y, x + width, y + 14.0f);
        ctx->DrawTextW(status.c_str(), (UINT32)status.size(), fmt, rect, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    if (fmt) fmt->Release();
    if (brush) brush->Release();
}

// ---------------------------------------------------------------------------
// DrawInstallButton — accent blue, white text
// ---------------------------------------------------------------------------
void MarketplacePanel::DrawInstallButton(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite,
                                          const D2D1_RECT_F& bounds, bool hover,
                                          const std::wstring& label, bool enabled)
{
    D2D1_COLOR_F bg = hover ? UI::Theme::AccentStrong() : UI::Theme::Accent();
    if (!enabled)
        bg.a *= 0.55f;

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
    ctx->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, enabled ? 1.0f : 0.78f), &txtBr);
    if (fmt && txtBr) {
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        ctx->DrawTextW(label.c_str(), (UINT32)label.size(), fmt, bounds, txtBr);
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

void MarketplacePanel::DrawButton(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite,
                                  const D2D1_RECT_F& bounds, const std::wstring& text,
                                  bool isHovering, bool isActive)
{
    D2D1_COLOR_F bg = UI::Theme::ChromeBackground();
    D2D1_COLOR_F border = UI::Theme::ChromeBorder();
    D2D1_COLOR_F fg = UI::Theme::MutedText();

    if (isActive)
    {
        bg = D2D1::ColorF(UI::Theme::Accent().r, UI::Theme::Accent().g, UI::Theme::Accent().b, 0.16f);
        border = D2D1::ColorF(UI::Theme::Accent().r, UI::Theme::Accent().g, UI::Theme::Accent().b, 0.55f);
        fg = UI::Theme::PrimaryText();
    }
    else if (isHovering)
    {
        bg = D2D1::ColorF(1, 1, 1, 0.06f);
        fg = UI::Theme::PrimaryText();
    }

    ID2D1SolidColorBrush* bgBr = nullptr;
    ctx->CreateSolidColorBrush(bg, &bgBr);
    if (bgBr) {
        ctx->FillRoundedRectangle(D2D1::RoundedRect(bounds, 12.0f, 12.0f), bgBr);
        bgBr->Release();
    }

    ID2D1SolidColorBrush* borderBr = nullptr;
    ctx->CreateSolidColorBrush(border, &borderBr);
    if (borderBr) {
        ctx->DrawRoundedRectangle(D2D1::RoundedRect(bounds, 12.0f, 12.0f), borderBr, 1.0f);
        borderBr->Release();
    }

    IDWriteTextFormat* fmt = nullptr;
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 10.5f, L"en-us", &fmt);
    ID2D1SolidColorBrush* textBr = nullptr;
    ctx->CreateSolidColorBrush(fg, &textBr);
    if (fmt && textBr) {
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        ctx->DrawTextW(text.c_str(), (UINT32)text.size(), fmt, bounds, textBr);
    }
    if (fmt) fmt->Release();
    if (textBr) textBr->Release();
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------
void MarketplacePanel::OnMouseMove(HWND hwnd, POINT clientPoint)
{
    HandleResizeMouseMove(hwnd, clientPoint);
    if (state_.isResizing || state_.isHoveringResizeZone) return;

    searchInput_.OnMouseMove(hwnd, clientPoint);

    auto htest = [&](const D2D1_RECT_F& r) {
        return clientPoint.x >= (int)r.left  && clientPoint.x <= (int)r.right &&
               clientPoint.y >= (int)r.top   && clientPoint.y <= (int)r.bottom;
    };

    bool changed = false;
    bool wasAll = isHoveringAllFilter_;
    bool wasInstalled = isHoveringInstalledFilter_;
    isHoveringAllFilter_ = htest(allFilterBounds_);
    isHoveringInstalledFilter_ = htest(installedFilterBounds_);
    if (wasAll != isHoveringAllFilter_ || wasInstalled != isHoveringInstalledFilter_)
        changed = true;

    const D2D1_RECT_F contentClip = D2D1::RectF(
        state_.leftEdge,
        state_.topEdge + state_.titleHeight + kControlsAreaH,
        state_.rightEdge,
        state_.bottomEdge);
    const bool inContentArea = htest(contentClip);

    for (auto& card : cards_)
    {
        bool wasCard = card.isHoveringCard;
        card.isHoveringCard = inContentArea &&
            clientPoint.x >= (int)card.bounds.left  &&
            clientPoint.x <= (int)card.bounds.right &&
            clientPoint.y >= (int)card.bounds.top   &&
            clientPoint.y <= (int)card.bounds.bottom;

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

    if (searchInput_.OnLeftButtonDown(hwnd, clientPoint))
    {
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    auto hit = [&](const D2D1_RECT_F& r) {
        return clientPoint.x >= (int)r.left  && clientPoint.x <= (int)r.right &&
               clientPoint.y >= (int)r.top   && clientPoint.y <= (int)r.bottom;
    };

    if (hit(allFilterBounds_))
    {
        if (showInstalledOnly_)
        {
            showInstalledOnly_ = false;
            scrollOffset_ = 0.0f;
            UpdateCardLayout();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return;
    }

    if (hit(installedFilterBounds_))
    {
        if (!showInstalledOnly_)
        {
            showInstalledOnly_ = true;
            scrollOffset_ = 0.0f;
            UpdateCardLayout();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return;
    }

    const D2D1_RECT_F contentClip = D2D1::RectF(
        state_.leftEdge,
        state_.topEdge + state_.titleHeight + kControlsAreaH,
        state_.rightEdge,
        state_.bottomEdge);
    if (!hit(contentClip))
        return;

    for (auto& card : cards_)
    {
        if (card.library.name.empty())
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
            auto *libraryName = new std::wstring(card.library.name);
            if (!PostMessageW(hwnd, WM_OPEN_MARKETPLACE_LIBRARY, 0, reinterpret_cast<LPARAM>(libraryName)))
                delete libraryName;
            return;
        }
    }
}

void MarketplacePanel::OnLeftButtonUp(HWND hwnd)
{
    HandleResizeLeftButtonUp(hwnd);

    POINT clientPoint{};
    if (GetCursorPos(&clientPoint))
        ScreenToClient(hwnd, &clientPoint);
    searchInput_.OnLeftButtonUp(hwnd, clientPoint);
}

void MarketplacePanel::OnMouseWheel(HWND hwnd, int delta)
{
    scrollOffset_ += delta * 0.5f;

    // Match the exact clip height used in Draw() so the last card can be reached.
    const float viewportH = state_.bottomEdge - (state_.topEdge + state_.titleHeight + kControlsAreaH);
    float minScroll = -((float)cards_.size() * kRowH - viewportH);

    if (minScroll > 0.0f) minScroll = 0.0f;
    scrollOffset_ = (std::max)(minScroll, (std::min)(0.0f, scrollOffset_));
    UpdateCardLayout();
    InvalidateRect(hwnd, nullptr, FALSE);
}

void MarketplacePanel::HandleInstallLibrary(HWND hwnd, LibraryCard* card)
{
    if (!card || card->library.name.empty()) return;
    if (card->library.installState == LibraryInfo::InstallState::Installing) return;

    LibraryDatabase::Instance().SetInstallStatus(card->library.name, LibraryInfo::InstallState::Installing, L"Installing...");
    card->library.installState = LibraryInfo::InstallState::Installing;
    card->library.installMessage = L"Installing...";
    UpdateCardLayout();
    InvalidateRect(hwnd, nullptr, FALSE);

    std::wstring installError;
    std::wstring installDetails;
    bool ok = GetExplorerManager().InstallLibraryFromGitUrl(
        hwnd,
        card->library.gitUrl,
        card->library.name,
        &installError,
        &installDetails);

    if (ok)
    {
        card->library.isInstalled = true;
        LibraryDatabase::Instance().SetInstalled(card->library.name, true);
        LibraryDatabase::Instance().SetInstallStatus(card->library.name, LibraryInfo::InstallState::Idle, installDetails);
        card->library.installState = LibraryInfo::InstallState::Idle;
        card->library.installMessage = installDetails;
        Logger::Instance().Log(L"Installing library: " + card->library.name);
        UpdateCardLayout();
    }
    else
    {
        card->library.isInstalled = false;
        LibraryDatabase::Instance().SetInstalled(card->library.name, false);
        if (installError.empty())
            installError = L"Failed to install library.";
        LibraryDatabase::Instance().SetInstallStatus(card->library.name, LibraryInfo::InstallState::Error, installError);
        card->library.installState = LibraryInfo::InstallState::Error;
        card->library.installMessage = installError;
        MessageBoxW(hwnd, installError.c_str(), L"Marketplace Install", MB_OK | MB_ICONERROR);
        Logger::Instance().Log(L"Install failed for " + card->library.name + L": " + installError);
    }
}

void MarketplacePanel::HandleUninstallLibrary(HWND hwnd, LibraryCard* card)
{
    if (!card || card->library.name.empty()) return;

    std::wstring uninstallError;
    bool ok = GetExplorerManager().UninstallLibraryFromGitUrl(
        hwnd,
        card->library.gitUrl,
        card->library.name,
        &uninstallError);

    if (ok)
    {
        card->library.isInstalled = false;
        card->library.installState = LibraryInfo::InstallState::Idle;
        card->library.installMessage.clear();
        LibraryDatabase::Instance().SetInstallStatus(card->library.name, LibraryInfo::InstallState::Idle, L"");
        LibraryDatabase::Instance().RefreshInstallationStatus(GetExplorerManager().GetState().rootPath);
        Logger::Instance().Log(L"Uninstalled library: " + card->library.name);
        UpdateCardLayout();
    }
    else
    {
        if (uninstallError.empty())
            uninstallError = L"Failed to remove library.";
        MessageBoxW(hwnd, uninstallError.c_str(), L"Marketplace Remove", MB_OK | MB_ICONERROR);
        Logger::Instance().Log(L"Uninstall failed for " + card->library.name + L": " + uninstallError);
    }
}

void MarketplacePanel::OnChar(wchar_t ch)
{
    searchInput_.OnChar(ch);
}

void MarketplacePanel::OnKeyDown(WPARAM key)
{
    searchInput_.OnKeyDown(key);
}

bool MarketplacePanel::HandleSearchChar(wchar_t ch)
{
    if (!searchInput_.IsFocused())
        return false;
    return searchInput_.OnChar(ch);
}

bool MarketplacePanel::HandleSearchKeyDown(WPARAM key)
{
    if (!searchInput_.IsFocused())
        return false;
    return searchInput_.OnKeyDown(key);
}

bool MarketplacePanel::IsSearchInputFocused() const
{
    return searchInput_.IsFocused();
}

void MarketplacePanel::UnfocusSearchInput()
{
    searchInput_.SetFocused(false);
}
