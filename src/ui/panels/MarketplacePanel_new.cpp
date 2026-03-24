#include "MarketplacePanel.h"
#include "ui/theme/Theme.h"
#include "helpers/window_helpers.h"
#include "utils/logger/Logger.h"
#include <sstream>
#include <cmath>

MarketplacePanel::MarketplacePanel()
    : Panel(PanelId::Marketplace)
{
    title_ = L"Marketplace";
    config_ = PanelConfig(
        PanelId::Marketplace,
        L"\uE71E",  // Store icon
        L"Marketplace",
        true,
        false,
        4  // Order after Settings
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

    state_.leftEdge = static_cast<float>(client.left + sidebarWidth);
    state_.rightEdge = state_.leftEdge + static_cast<float>(state_.physicalWidth);
    state_.topEdge = static_cast<float>(tbRect.bottom);
    state_.bottomEdge = static_cast<float>(client.bottom - footerHeight);

    if (!visible_) {
        state_.physicalWidth = 0;
        state_.rightEdge = state_.leftEdge;
    }

    // Search bar position (below title)
    searchBarBounds_ = D2D1::RectF(
        state_.leftEdge + CARD_PADDING,
        state_.topEdge + state_.titleHeight + CARD_PADDING,
        state_.rightEdge - CARD_PADDING,
        state_.topEdge + state_.titleHeight + CARD_PADDING + SEARCH_BAR_HEIGHT
    );

    UpdateCardLayout();
}

void MarketplacePanel::UpdateCardLayout()
{
    cards_.clear();

    float x = state_.leftEdge + CARD_PADDING;
    float maxWidth = state_.rightEdge - state_.leftEdge - CARD_PADDING * 2;
    float y = searchBarBounds_.bottom + CARD_PADDING + scrollOffset_;

    for (size_t i = 0; i < LibraryDatabase::Instance().GetLibraries().size(); ++i)
    {
        LibraryCard card;
        const auto& libs = LibraryDatabase::Instance().GetLibraries();
        card.library = const_cast<LibraryInfo*>(&libs[i]);
        card.bounds = D2D1::RectF(x, y, x + maxWidth, y + CARD_HEIGHT);

        // Layout within card (with icon on left)
        float contentLeft = card.bounds.left + 12.0f + ICON_SIZE + 12.0f;
        float contentRight = card.bounds.right - 12.0f - 80.0f;  // Room for button
        float contentTop = card.bounds.top + 12.0f;

        // Icon (left side, 48x48)
        card.iconRect = D2D1::RectF(
            card.bounds.left + 12.0f,
            contentTop,
            card.bounds.left + 12.0f + ICON_SIZE,
            contentTop + ICON_SIZE
        );

        // Header: Title + Author
        card.headerRect = D2D1::RectF(
            contentLeft, contentTop,
            contentRight, contentTop + 28.0f
        );

        // Rating: stars + downloads
        card.ratingRect = D2D1::RectF(
            contentLeft, contentTop + 32.0f,
            contentRight, contentTop + 50.0f
        );

        // Description
        card.descRect = D2D1::RectF(
            contentLeft, contentTop + 54.0f,
            contentRight, contentTop + 110.0f
        );

        // Tags
        card.tagsRect = D2D1::RectF(
            contentLeft, contentTop + 114.0f,
            contentRight, contentTop + 140.0f
        );

        // Install button (right side, bottom)
        float btnWidth = 80.0f;
        float btnHeight = 28.0f;
        float btnX = card.bounds.right - btnWidth - 12.0f;
        float btnY = card.bounds.bottom - btnHeight - 12.0f;

        if (libs[i].isInstalled)
        {
            card.uninstallButtonBounds = D2D1::RectF(btnX, btnY, btnX + btnWidth, btnY + btnHeight);
        }
        else
        {
            card.installButtonBounds = D2D1::RectF(btnX, btnY, btnX + btnWidth, btnY + btnHeight);
        }

        cards_.push_back(card);
        y += CARD_HEIGHT + CARD_PADDING;
    }
}

void MarketplacePanel::Draw(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite, HWND hwnd)
{
    DrawBackground(ctx);
    DrawTitle(ctx, dwrite);
    DrawSearchBar(ctx, dwrite);

    // Draw cards
    for (auto& card : cards_)
    {
        DrawLibraryCard(ctx, dwrite, card);
    }

    DrawRightBorder(ctx);
}

void MarketplacePanel::DrawSearchBar(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite)
{
    // Search bar background
    ID2D1SolidColorBrush* bgBrush = nullptr;
    ctx->CreateSolidColorBrush(UI::Theme::GetPalette().inputBackground, &bgBrush);

    if (bgBrush)
    {
        ctx->FillRoundedRectangle(
            D2D1::RoundedRect(searchBarBounds_, 6.0f, 6.0f),
            bgBrush
        );
        bgBrush->Release();
    }

    // Search bar border
    ID2D1SolidColorBrush* borderBrush = nullptr;
    ctx->CreateSolidColorBrush(UI::Theme::GetPalette().inputBorder, &borderBrush);

    if (borderBrush)
    {
        ctx->DrawRoundedRectangle(
            D2D1::RoundedRect(searchBarBounds_, 6.0f, 6.0f),
            borderBrush, 1.0f
        );
        borderBrush->Release();
    }

    // Placeholder text
    IDWriteTextFormat* format = nullptr;
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                            12.0f, L"en-us", &format);

    if (format)
    {
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    ID2D1SolidColorBrush* textBrush = nullptr;
    ctx->CreateSolidColorBrush(UI::Theme::GetPalette().inputPlaceholder, &textBrush);

    D2D1_RECT_F textRect = D2D1::RectF(
        searchBarBounds_.left + 12.0f,
        searchBarBounds_.top,
        searchBarBounds_.right - 12.0f,
        searchBarBounds_.bottom
    );

    if (format && textBrush)
    {
        ctx->DrawTextW(L"\uE71E Search libraries...", 20, format, textRect, textBrush);
    }

    if (format) format->Release();
    if (textBrush) textBrush->Release();
}

void MarketplacePanel::DrawLibraryCard(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite,
                                       LibraryCard& card)
{
    if (!card.library) return;

    // Card background with shadow depth
    D2D1_COLOR_F bgColor = card.isHoveringCard
        ? UI::Theme::GetPalette().explorerRowActive
        : UI::Theme::GetPalette().explorerRowHover;

    // Subtle shadow
    ID2D1SolidColorBrush* shadowBrush = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.1f), &shadowBrush);
    if (shadowBrush)
    {
        D2D1_RECT_F shadowRect = card.bounds;
        shadowRect.top += 4.0f;
        ctx->FillRoundedRectangle(
            D2D1::RoundedRect(shadowRect, CARD_CORNER_RADIUS, CARD_CORNER_RADIUS),
            shadowBrush
        );
        shadowBrush->Release();
    }

    // Card background
    ID2D1SolidColorBrush* bgBrush = nullptr;
    ctx->CreateSolidColorBrush(bgColor, &bgBrush);
    if (bgBrush)
    {
        ctx->FillRoundedRectangle(
            D2D1::RoundedRect(card.bounds, CARD_CORNER_RADIUS, CARD_CORNER_RADIUS),
            bgBrush
        );
        bgBrush->Release();
    }

    // Card border
    ID2D1SolidColorBrush* borderBrush = nullptr;
    D2D1_COLOR_F borderColor = card.isHoveringCard
        ? UI::Theme::Accent()
        : UI::Theme::ChromeBorder();

    ctx->CreateSolidColorBrush(borderColor, &borderBrush);
    if (borderBrush)
    {
        ctx->DrawRoundedRectangle(
            D2D1::RoundedRect(card.bounds, CARD_CORNER_RADIUS, CARD_CORNER_RADIUS),
            borderBrush,
            card.isHoveringCard ? 2.0f : 1.0f
        );
        borderBrush->Release();
    }

    // Draw card elements
    DrawCardIcon(ctx, dwrite, card.iconRect, card.library->icon);
    DrawCardHeader(ctx, dwrite, card.headerRect, card.library);
    DrawRatingStars(ctx, dwrite, card.ratingRect, card.library->rating, card.library->downloads);
    DrawDescription(ctx, dwrite, card.descRect, card.library->description);
    DrawTags(ctx, dwrite, card.tagsRect, card.library);

    // Draw buttons
    if (card.library->isInstalled)
    {
        DrawButton(ctx, dwrite, card.uninstallButtonBounds, L"Uninstall",
                  card.isHoveringUninstallBtn, true);
    }
    else
    {
        DrawButton(ctx, dwrite, card.installButtonBounds, L"Install",
                  card.isHoveringInstallBtn, false);
    }
}

void MarketplacePanel::DrawCardIcon(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite,
                                     const D2D1_RECT_F& rect, const std::wstring& icon)
{
    // Icon background
    ID2D1SolidColorBrush* bgBrush = nullptr;
    ctx->CreateSolidColorBrush(UI::Theme::Accent(), &bgBrush);
    if (bgBrush)
    {
        ctx->FillRoundedRectangle(
            D2D1::RoundedRect(rect, 4.0f, 4.0f),
            bgBrush
        );
        bgBrush->Release();
    }

    // Icon text (emoji)
    IDWriteTextFormat* format = nullptr;
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                            28.0f, L"en-us", &format);

    if (format)
    {
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    ID2D1SolidColorBrush* textBrush = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &textBrush);

    if (format && textBrush)
    {
        ctx->DrawTextW(icon.c_str(), (UINT32)icon.length(), format, rect, textBrush);
    }

    if (format) format->Release();
    if (textBrush) textBrush->Release();
}

void MarketplacePanel::DrawCardHeader(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite,
                                       const D2D1_RECT_F& rect, const LibraryInfo* lib)
{
    if (!lib) return;

    // Title
    IDWriteTextFormat* titleFormat = nullptr;
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                            13.0f, L"en-us", &titleFormat);

    if (titleFormat)
    {
        titleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        titleFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }

    ID2D1SolidColorBrush* titleBrush = nullptr;
    ctx->CreateSolidColorBrush(UI::Theme::PrimaryText(), &titleBrush);

    D2D1_RECT_F titleRect = D2D1::RectF(rect.left, rect.top, rect.right, rect.top + 14.0f);
    if (titleFormat && titleBrush)
    {
        ctx->DrawTextW(lib->name.c_str(), (UINT32)lib->name.length(), titleFormat, titleRect, titleBrush);
    }

    // Author
    IDWriteTextFormat* authorFormat = nullptr;
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                            10.0f, L"en-us", &authorFormat);

    if (authorFormat)
    {
        authorFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        authorFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }

    ID2D1SolidColorBrush* authorBrush = nullptr;
    ctx->CreateSolidColorBrush(UI::Theme::MutedText(), &authorBrush);

    D2D1_RECT_F authorRect = D2D1::RectF(rect.left, rect.top + 16.0f, rect.right, rect.bottom);
    if (authorFormat && authorBrush)
    {
        ctx->DrawTextW(lib->author.c_str(), (UINT32)lib->author.length(), authorFormat, authorRect, authorBrush);
    }

    if (titleFormat) titleFormat->Release();
    if (authorFormat) authorFormat->Release();
    if (titleBrush) titleBrush->Release();
    if (authorBrush) authorBrush->Release();
}

void MarketplacePanel::DrawRatingStars(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite,
                                        const D2D1_RECT_F& rect, float rating, int downloads)
{
    // Draw stars (simplified: just show rating number)
    IDWriteTextFormat* ratingFormat = nullptr;
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                            10.0f, L"en-us", &ratingFormat);

    if (ratingFormat)
    {
        ratingFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        ratingFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }

    ID2D1SolidColorBrush* ratingBrush = nullptr;
    ctx->CreateSolidColorBrush(UI::Theme::MutedText(), &ratingBrush);

    // Format: ⭐ 4.8 (1.2K downloads)
    std::wostringstream ss;
    ss.precision(1);
    ss << std::fixed << L"⭐ " << rating << L" (" << (downloads / 1000.0f) << L"K downloads)";
    std::wstring ratingText = ss.str();

    if (ratingFormat && ratingBrush)
    {
        ctx->DrawTextW(ratingText.c_str(), (UINT32)ratingText.length(), ratingFormat, rect, ratingBrush);
    }

    if (ratingFormat) ratingFormat->Release();
    if (ratingBrush) ratingBrush->Release();
}

void MarketplacePanel::DrawDescription(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite,
                                        const D2D1_RECT_F& rect, const std::wstring& desc)
{
    IDWriteTextFormat* format = nullptr;
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                            11.0f, L"en-us", &format);

    if (format)
    {
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }

    ID2D1SolidColorBrush* brush = nullptr;
    ctx->CreateSolidColorBrush(UI::Theme::MutedText(), &brush);

    if (format && brush)
    {
        ctx->DrawTextW(desc.c_str(), (UINT32)desc.length(), format, rect, brush);
    }

    if (format) format->Release();
    if (brush) brush->Release();
}

void MarketplacePanel::DrawTags(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite,
                                 const D2D1_RECT_F& rect, const LibraryInfo* lib)
{
    if (!lib) return;

    // Draw category tag
    D2D1_RECT_F tagRect = D2D1::RectF(rect.left, rect.top, rect.left + 70.0f, rect.top + 16.0f);

    // Tag background
    ID2D1SolidColorBrush* tagBgBrush = nullptr;
    ctx->CreateSolidColorBrush(UI::Theme::Accent(), &tagBgBrush);

    if (tagBgBrush)
    {
        ctx->FillRoundedRectangle(
            D2D1::RoundedRect(tagRect, 3.0f, 3.0f),
            tagBgBrush
        );
        tagBgBrush->Release();
    }

    // Tag text
    IDWriteTextFormat* tagFormat = nullptr;
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                            9.0f, L"en-us", &tagFormat);

    if (tagFormat)
    {
        tagFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        tagFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    ID2D1SolidColorBrush* tagTextBrush = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &tagTextBrush);

    if (tagFormat && tagTextBrush)
    {
        ctx->DrawTextW(lib->category.c_str(), (UINT32)lib->category.length(), tagFormat, tagRect, tagTextBrush);
    }

    if (tagFormat) tagFormat->Release();
    if (tagTextBrush) tagTextBrush->Release();
}

void MarketplacePanel::DrawButton(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite,
                                  const D2D1_RECT_F& bounds, const std::wstring& text,
                                  bool isHovering, bool isActive)
{
    // Button background
    ID2D1SolidColorBrush* bgBrush = nullptr;
    D2D1_COLOR_F bgColor;

    if (isActive)
    {
        bgColor = isHovering ? UI::Theme::AccentStrong() : UI::Theme::Accent();
    }
    else
    {
        bgColor = isHovering ? UI::Theme::GetPalette().explorerRowHover : UI::Theme::ChromeBorder();
    }

    ctx->CreateSolidColorBrush(bgColor, &bgBrush);

    if (bgBrush)
    {
        ctx->FillRoundedRectangle(
            D2D1::RoundedRect(bounds, 4.0f, 4.0f),
            bgBrush
        );
        bgBrush->Release();
    }

    // Button text
    IDWriteTextFormat* format = nullptr;
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                            10.0f, L"en-us", &format);

    if (format)
    {
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    ID2D1SolidColorBrush* textBrush = nullptr;
    D2D1_COLOR_F textColor = isActive
        ? D2D1::ColorF(D2D1::ColorF::White)
        : UI::Theme::MutedText();

    ctx->CreateSolidColorBrush(textColor, &textBrush);

    if (format && textBrush)
    {
        ctx->DrawTextW(text.c_str(), (UINT32)text.length(), format, bounds, textBrush);
    }

    if (format) format->Release();
    if (textBrush) textBrush->Release();
}

void MarketplacePanel::OnMouseMove(HWND hwnd, POINT clientPoint)
{
    // Handle resize first (from Panel base)
    HandleResizeMouseMove(hwnd, clientPoint);

    // If resizing or hovering resize zone, don't process other interactions
    if (state_.isResizing || state_.isHoveringResizeZone)
    {
        return;
    }

    bool stateChanged = false;

    // Check if hovering over cards/buttons
    for (auto& card : cards_)
    {
        bool wasHoveringCard = card.isHoveringCard;
        card.isHoveringCard = (clientPoint.x >= (int)card.bounds.left &&
                              clientPoint.x <= (int)card.bounds.right &&
                              clientPoint.y >= (int)card.bounds.top &&
                              clientPoint.y <= (int)card.bounds.bottom);

        if (card.isHoveringCard)
        {
            bool wasHoveringInstall = card.isHoveringInstallBtn;
            card.isHoveringInstallBtn = (clientPoint.x >= (int)card.installButtonBounds.left &&
                                        clientPoint.x <= (int)card.installButtonBounds.right &&
                                        clientPoint.y >= (int)card.installButtonBounds.top &&
                                        clientPoint.y <= (int)card.installButtonBounds.bottom);

            bool wasHoveringUninstall = card.isHoveringUninstallBtn;
            card.isHoveringUninstallBtn = (clientPoint.x >= (int)card.uninstallButtonBounds.left &&
                                          clientPoint.x <= (int)card.uninstallButtonBounds.right &&
                                          clientPoint.y >= (int)card.uninstallButtonBounds.top &&
                                          clientPoint.y <= (int)card.uninstallButtonBounds.bottom);

            if (wasHoveringInstall != card.isHoveringInstallBtn ||
                wasHoveringUninstall != card.isHoveringUninstallBtn)
            {
                stateChanged = true;
            }
        }

        if (wasHoveringCard != card.isHoveringCard)
            stateChanged = true;
    }

    if (stateChanged)
        InvalidateRect(hwnd, nullptr, FALSE);
}

void MarketplacePanel::OnLeftButtonDown(HWND hwnd, POINT clientPoint)
{
    // Handle resize first (from Panel base)
    if (HandleResizeLeftButtonDown(hwnd, clientPoint))
    {
        return;
    }

    // Check if clicked button
    for (auto& card : cards_)
    {
        if (card.isHoveringInstallBtn && card.library)
        {
            HandleInstallLibrary(&card);
            return;
        }

        if (card.isHoveringUninstallBtn && card.library)
        {
            HandleUninstallLibrary(&card);
            return;
        }
    }
}

void MarketplacePanel::OnLeftButtonUp(HWND hwnd)
{
    // Handle resize first (from Panel base)
    if (HandleResizeLeftButtonUp(hwnd))
    {
        return;
    }
}

void MarketplacePanel::OnMouseWheel(HWND hwnd, int delta)
{
    scrollOffset_ += delta * 0.5f;

    // Limit scroll
    float maxScroll = 0.0f;
    float minScroll = -((float)cards_.size() * (CARD_HEIGHT + CARD_PADDING));

    if (scrollOffset_ > maxScroll)
        scrollOffset_ = maxScroll;
    if (scrollOffset_ < minScroll)
        scrollOffset_ = minScroll;

    UpdateCardLayout();
    InvalidateRect(hwnd, nullptr, FALSE);
}

void MarketplacePanel::HandleInstallLibrary(LibraryCard* card)
{
    if (!card || !card->library)
        return;

    Logger::Instance().Log(L"Installing library: " + card->library->name);

    // TODO: Clone repository from card->library->gitUrl to external/{name}
    // TODO: Create CMakeLists.txt if needed
    // TODO: Add to root CMakeLists.txt
}

void MarketplacePanel::HandleUninstallLibrary(LibraryCard* card)
{
    if (!card || !card->library)
        return;

    Logger::Instance().Log(L"Uninstalling library: " + card->library->name);

    // TODO: Remove external/{name} directory
    // TODO: Remove from root CMakeLists.txt
}
