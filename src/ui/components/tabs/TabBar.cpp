#include "TabBar.h"
#include <algorithm>
#include <filesystem>
#include <Windows.h>
#include "core/explorer/Explorer.h"
#include "helpers/window_helpers.h"

TabBar::TabBar() {}
TabBar::~TabBar() {}

int TabBar::AddTab(const std::wstring &filePath, const std::wstring &displayName)
{
    // If a tab with the same file path already exists, activate it and return its index
    for (int i = 0; i < (int)tabs_.size(); ++i)
    {
        if (tabs_[i].filePath == filePath)
        {
            SetActiveTab(i);
            return i;
        }
    }

    Tab tab;
    tab.filePath = filePath;
    tab.displayName = displayName;
    tab.isActive = false;
    tabs_.push_back(tab);

    int index = (int)tabs_.size() - 1;
    SetActiveTab(index);
    return index;
}

void TabBar::CloseTab(int index)
{
    if (index < 0 || index >= (int)tabs_.size())
        return;

    // Remember the filePath being removed and erase from tabs
    std::wstring removedPath = tabs_[index].filePath;
    tabs_.erase(tabs_.begin() + index);

    // Remove from MRU history
    mruHistory_.erase(std::remove(mruHistory_.begin(), mruHistory_.end(), removedPath), mruHistory_.end());

    // If there are no tabs left
    if (tabs_.empty())
    {
        activeTabIndex_ = -1;
        return;
    }

    // Try to activate the most-recently-used tab that still exists
    if (!mruHistory_.empty())
    {
        for (const auto &path : mruHistory_)
        {
            int idx = FindTabIndexByFilePath(path);
            if (idx >= 0)
            {
                SetActiveTab(idx);
                return;
            }
        }
    }

    // Fallback: pick nearest index
    if (activeTabIndex_ == index)
    {
        activeTabIndex_ = (std::min)(index, (int)tabs_.size() - 1);
        if (activeTabIndex_ >= 0)
        {
            tabs_[activeTabIndex_].isActive = true;
        }
    }
    else if (activeTabIndex_ > index)
    {
        activeTabIndex_--;
    }
}

void TabBar::SetActiveTab(int index)
{
    if (index < 0 || index >= (int)tabs_.size())
        return;

    // clear active flags
    for (auto &tab : tabs_)
    {
        tab.isActive = false;
    }

    tabs_[index].isActive = true;
    activeTabIndex_ = index;

    // Update MRU: move this filePath to front
    const std::wstring &fp = tabs_[index].filePath;
    mruHistory_.erase(std::remove(mruHistory_.begin(), mruHistory_.end(), fp), mruHistory_.end());
    mruHistory_.insert(mruHistory_.begin(), fp);

    // Notify explorer of the active file so it can highlight the corresponding item
    GetExplorerManager().SetActivePath(fp);
}

const Tab *TabBar::GetActiveTab() const
{
    if (activeTabIndex_ >= 0 && activeTabIndex_ < (int)tabs_.size())
    {
        return &tabs_[activeTabIndex_];
    }
    return nullptr;
}

const Tab *TabBar::GetTab(int index) const
{
    if (index >= 0 && index < (int)tabs_.size())
    {
        return &tabs_[index];
    }
    return nullptr;
}

void TabBar::UpdateLayout(float left, float top, float right)
{
    leftEdge_ = left;
    topEdge_ = top;
    rightEdge_ = right;
}

void TabBar::Draw(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd)
{
    (void)hwnd;

    // Background de la tab bar
    ID2D1SolidColorBrush *bgBrush = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.08f, 0.08f), &bgBrush);

        D2D1_RECT_F barRect = D2D1::RectF(leftEdge_, topEdge_, rightEdge_, topEdge_ + GetHeight());
    ctx->FillRectangle(barRect, bgBrush);

    // Draw bottom border matching the titlebar border color only when there are tabs
    if (!tabs_.empty())
    {
        ID2D1SolidColorBrush *tabBorderBrush = nullptr;
        ctx->CreateSolidColorBrush(D2D1::ColorF(48.0f / 255.0f, 48.0f / 255.0f, 48.0f / 255.0f), &tabBorderBrush);
        // draw the border inside the tab bar's rect so layout and hit-tests include it
            D2D1_POINT_2F bl = D2D1::Point2F(leftEdge_, topEdge_ + GetHeight() - 0.5f);
            D2D1_POINT_2F br = D2D1::Point2F(rightEdge_, topEdge_ + GetHeight() - 0.5f);
        ctx->DrawLine(bl, br, tabBorderBrush, 1.0f);
        if (tabBorderBrush) tabBorderBrush->Release();
    }

    // Dessiner chaque tab
    float x = leftEdge_;
    for (int i = 0; i < (int)tabs_.size(); ++i)
    {
        const Tab &tab = tabs_[i];

        D2D1_RECT_F tabRect = D2D1::RectF(x, topEdge_, x + tabWidth_, topEdge_ + tabHeight_);

        // Background de la tab
        ID2D1SolidColorBrush *tabBrush = nullptr;
        if (tab.isActive)
        {
            // Fond sombre pour l'onglet actif (25, 25, 25)
            ctx->CreateSolidColorBrush(D2D1::ColorF(0.098f, 0.098f, 0.098f), &tabBrush);
        }
        else if (i == hoveredTabIndex_)
        {
            ctx->CreateSolidColorBrush(D2D1::ColorF(0.18f, 0.18f, 0.18f), &tabBrush);
        }
        else
        {
            ctx->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.08f, 0.08f), &tabBrush);
        }
        ctx->FillRectangle(tabRect, tabBrush);
        tabBrush->Release();

        // Bordure en bas pour l'onglet actif avec coins arrondis
        if (tab.isActive)
        {
            ID2D1SolidColorBrush *borderBrush = nullptr;
            // Couleur accent pour la bordure (bleu par exemple, tu peux changer)
            ctx->CreateSolidColorBrush(D2D1::ColorF(0.3f, 0.6f, 1.0f), &borderBrush);

            // Ligne avec coins arrondis en bas de l'onglet
            float borderHeight = 2.0f;
            D2D1_RECT_F borderRect = D2D1::RectF(
                x + 4.0f, // Petit padding à gauche
                topEdge_ + tabHeight_ - borderHeight,
                x + tabWidth_ - 4.0f, // Petit padding à droite
                topEdge_ + tabHeight_);
            D2D1_ROUNDED_RECT roundedBorder = D2D1::RoundedRect(borderRect, 1.5f, 1.5f);
            ctx->FillRoundedRectangle(roundedBorder, borderBrush);
            borderBrush->Release();
        }

        // Texte de la tab
        IDWriteTextFormat *format = nullptr;
        dwrite->CreateTextFormat(L"Segoe UI", NULL,
                                 DWRITE_FONT_WEIGHT_NORMAL,
                                 DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_FONT_STRETCH_NORMAL,
                                 12.0f, L"en-us", &format);

        if (format)
        {
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

            ID2D1SolidColorBrush *textBrush = nullptr;
            // Texte plus clair pour l'onglet actif
            if (tab.isActive)
            {
                ctx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f), &textBrush);
            }
            else
            {
                ctx->CreateSolidColorBrush(D2D1::ColorF(0.7f, 0.7f, 0.7f), &textBrush);
            }

            // Determine if we have an icon for this tab (re-use Explorer's icon loader)
            float textLeft = x + 10;
            UINT dpi = GetDpiForWindow(hwnd);
            int iconPx = win32_dpi_scale(16, dpi);
            ID2D1Bitmap *iconBitmap = nullptr;
            if (!tab.filePath.empty())
            {
                // extract extension
                size_t pos = tab.filePath.find_last_of(L'.');
                std::string ext;
                if (pos != std::wstring::npos)
                {
                    std::wstring wext = tab.filePath.substr(pos);
                    int needed = WideCharToMultiByte(CP_UTF8, 0, wext.c_str(), (int)wext.size(), NULL, 0, NULL, NULL);
                    if (needed > 0)
                    {
                        ext.resize(needed);
                        WideCharToMultiByte(CP_UTF8, 0, wext.c_str(), (int)wext.size(), ext.data(), needed, NULL, NULL);
                    }
                }

                ExplorerItem tmp;
                tmp.extension = ext;
                tmp.isDirectory = false;
                tmp.fullPath = tab.filePath;

                iconBitmap = GetExplorerManager().GetIconForItemPublic(ctx, tmp, hwnd);
                if (iconBitmap)
                {
                    float iconY = topEdge_ + (tabHeight_ - (float)iconPx) * 0.5f;
                    D2D1_RECT_F iconRect = D2D1::RectF(
                        x + 8.0f,
                        iconY,
                        x + 8.0f + (float)iconPx,
                        iconY + (float)iconPx);
                    ctx->DrawBitmap(iconBitmap, iconRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    textLeft = x + 8.0f + (float)iconPx + 8.0f;
                }
            }

            D2D1_RECT_F textRect = D2D1::RectF(textLeft, topEdge_, x + tabWidth_ - 20, topEdge_ + tabHeight_);

            // Create a text layout to prevent wrapping and enable ellipsis trimming
            IDWriteTextLayout *textLayout = nullptr;
            HRESULT hr = dwrite->CreateTextLayout(tab.displayName.c_str(), (UINT32)tab.displayName.size(),
                                                  format, textRect.right - textRect.left, textRect.bottom - textRect.top,
                                                  &textLayout);
            if (SUCCEEDED(hr) && textLayout)
            {
                // Disable word wrapping
                textLayout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

                // Setup trimming (ellipsis)
                DWRITE_TRIMMING trimming = {};
                trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
                IDWriteInlineObject *ellipsisToken = nullptr;
                if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(format, &ellipsisToken)))
                {
                    // Use SetTrimming with trimming options and the ellipsis inline object.
                    textLayout->SetTrimming(&trimming, ellipsisToken);
                }

                // Draw the layout (it will be clipped to textRect)
                ctx->DrawTextLayout(D2D1::Point2F(textRect.left, textRect.top), textLayout, textBrush);

                if (ellipsisToken)
                    ellipsisToken->Release();
                textLayout->Release();
            }
            else
            {
                // Fallback: simple DrawText if layout creation fails
                ctx->DrawTextW(tab.displayName.c_str(), (UINT32)tab.displayName.size(), format, textRect, textBrush);
            }

            textBrush->Release();
            format->Release();
        }

        // Close button (appears when hovering this tab)
        if (hoveredTabIndex_ == i)
        {
            D2D1_RECT_F closeRect = CloseRectForTab(i);
            DrawCloseButton(ctx, closeRect);
        }

        x += tabWidth_;
    }

    bgBrush->Release();
}

int TabBar::OnLeftButtonDown(POINT pt)
{
    float x = leftEdge_;
    for (int i = 0; i < (int)tabs_.size(); ++i)
    {
        D2D1_RECT_F tabRect = D2D1::RectF(x, topEdge_, x + tabWidth_, topEdge_ + tabHeight_);
        // Check close button area first (use helper)
        if (IsPointInCloseRect(i, pt))
        {
            CloseTab(i);
            return -1;
        }

        if (pt.x >= tabRect.left && pt.x < tabRect.right &&
            pt.y >= tabRect.top && pt.y < tabRect.bottom)
        {
            SetActiveTab(i);
            return i;
        }
        x += tabWidth_;
    }
    return -1;
}

int TabBar::OnMouseMove(POINT pt) {
    const float horizTolerance = 6.0f;

    int prev = hoveredTabIndex_;
    int newHover = -1;
    float x = leftEdge_;
    for (int i = 0; i < (int)tabs_.size(); ++i) {
        D2D1_RECT_F tabRect = D2D1::RectF(x - horizTolerance, topEdge_, x + tabWidth_ + horizTolerance, topEdge_ + tabHeight_);
        if (pt.x >= tabRect.left && pt.x < tabRect.right &&
            pt.y >= tabRect.top && pt.y < tabRect.bottom) {
            newHover = i;
            break;  // IMPORTANT: sortir de la boucle dès qu'on trouve
        }
        x += tabWidth_;
    }

    // Si pointer toujours dans les limites verticales, garde le précédent hover
    if (newHover == -1 && pt.y >= (int)topEdge_ && pt.y < (int)(topEdge_ + tabHeight_)) {
        newHover = prev;
    }

    // Apply if changed
    if (newHover != prev) {
        hoveredTabIndex_ = newHover;
        return newHover;
    }

    // No change
    return -2;
}

void TabBar::ClearHover()
{
    hoveredTabIndex_ = -1;
}

int TabBar::FindTabIndexByFilePath(const std::wstring &filePath) const
{
    for (int i = 0; i < (int)tabs_.size(); ++i)
    {
        if (tabs_[i].filePath == filePath)
            return i;
    }
    return -1;
}

// Helper implementations
D2D1_RECT_F TabBar::CloseRectForTab(int index) const
{
    float x = leftEdge_ + index * tabWidth_;
    float closeSize = 14.0f;
    float closePadding = 10.0f;
    return D2D1::RectF(
        x + tabWidth_ - closePadding - closeSize,
        topEdge_ + (tabHeight_ - closeSize) * 0.5f,
        x + tabWidth_ - closePadding,
        topEdge_ + (tabHeight_ + closeSize) * 0.5f);
}

bool TabBar::IsPointInCloseRect(int index, POINT pt) const
{
    if (index < 0 || index >= (int)tabs_.size())
        return false;
    D2D1_RECT_F r = CloseRectForTab(index);
    return (pt.x >= r.left && pt.x <= r.right && pt.y >= r.top && pt.y <= r.bottom);
}

void TabBar::DrawCloseButton(ID2D1RenderTarget *ctx, const D2D1_RECT_F &rect) const
{
    ID2D1SolidColorBrush *closeBg = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.14f, 0.14f, 0.14f), &closeBg);
    ctx->FillRectangle(rect, closeBg);

    ID2D1SolidColorBrush *closeBrush = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.85f, 0.85f, 0.85f), &closeBrush);

    D2D1_POINT_2F a = D2D1::Point2F(rect.left + 3.0f, rect.top + 3.0f);
    D2D1_POINT_2F b = D2D1::Point2F(rect.right - 3.0f, rect.bottom - 3.0f);
    D2D1_POINT_2F c = D2D1::Point2F(rect.left + 3.0f, rect.bottom - 3.0f);
    D2D1_POINT_2F d = D2D1::Point2F(rect.right - 3.0f, rect.top + 3.0f);
    ctx->DrawLine(a, b, closeBrush, 1.5f);
    ctx->DrawLine(c, d, closeBrush, 1.5f);

    if (closeBg)
        closeBg->Release();
    if (closeBrush)
        closeBrush->Release();
}

void TabBar::UpdateTabPath(int index, const std::wstring &filePath, const std::wstring &displayName)
{
    if (index < 0 || index >= (int)tabs_.size())
        return;
    tabs_[index].filePath = filePath;
    tabs_[index].displayName = displayName;

    // update MRU
    mruHistory_.erase(std::remove(mruHistory_.begin(), mruHistory_.end(), filePath), mruHistory_.end());
    mruHistory_.insert(mruHistory_.begin(), filePath);
}