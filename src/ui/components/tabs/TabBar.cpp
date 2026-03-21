#include "TabBar.h"
#include <algorithm>
#include <filesystem>
#include <cwctype>
#include <Windows.h>
#include "core/explorer/Explorer.h"
#include "helpers/window_helpers.h"
#include "ui/theme/Theme.h"

namespace
{
D2D1_COLOR_F BlendTabColor(D2D1_COLOR_F a, D2D1_COLOR_F b, float t)
{
    t = (std::max)(0.0f, (std::min)(1.0f, t));
    return D2D1::ColorF(
        a.r + (b.r - a.r) * t,
        a.g + (b.g - a.g) * t,
        a.b + (b.b - a.b) * t,
        1.0f);
}
}

TabBar::TabBar() {}
TabBar::~TabBar() {}

int TabBar::AddTab(const std::wstring &filePath, const std::wstring &displayName)
{
    // If a tab with the same file path already exists, activate it and return its index
    // (skip this for empty paths, used by non-file tabs like terminals)
    if (!filePath.empty())
    {
        for (int i = 0; i < (int)tabs_.size(); ++i)
        {
            if (tabs_[i].filePath == filePath)
            {
                SetActiveTab(i);
                return i;
            }
        }
    }

    Tab tab;
    tab.filePath = filePath;
    tab.displayName = displayName;
    tab.isDirty = false;
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

    if (activeTabIndex_ == index && tabs_[index].isActive)
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

    // Notify explorer only for real file tabs.
    if (!fp.empty())
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

void TabBar::SetTabDirty(int index, bool dirty)
{
    if (index < 0 || index >= (int)tabs_.size())
        return;
    tabs_[index].isDirty = dirty;
}

bool TabBar::IsTabDirty(int index) const
{
    if (index < 0 || index >= (int)tabs_.size())
        return false;
    return tabs_[index].isDirty;
}

void TabBar::DrawCloseOrDirty(ID2D1RenderTarget *ctx, const D2D1_RECT_F &rect, bool hovered, bool dirty) const
{
    D2D1_COLOR_F normal = UI::Theme::MutedText();
    normal.a = 0.75f;
    D2D1_COLOR_F hover = UI::Theme::PrimaryText();
    D2D1_COLOR_F ring = UI::Theme::GetPalette().explorerToolbarHover;
    ring.a = 0.95f;

    ID2D1SolidColorBrush *ringBrush = nullptr;
    ID2D1SolidColorBrush *fgBrush = nullptr;

    if (hovered)
    {
        float cx = (rect.left + rect.right) * 0.5f;
        float cy = (rect.top + rect.bottom) * 0.5f;
        float radius = (rect.right - rect.left) * 0.5f;
        ctx->CreateSolidColorBrush(ring, &ringBrush);
        ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), ringBrush);
        ctx->CreateSolidColorBrush(hover, &fgBrush);
    }
    else
    {
        ctx->CreateSolidColorBrush(normal, &fgBrush);
    }

    if (!fgBrush)
    {
        if (ringBrush)
            ringBrush->Release();
        return;
    }

    float cx = (rect.left + rect.right) * 0.5f;
    float cy = (rect.top + rect.bottom) * 0.5f;

    if (dirty)
    {
        // ● point (remplace la croix)
        float r = (rect.right - rect.left) * 0.22f; // taille du point
        ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), fgBrush);
    }
    else
    {
        // X (ton code existant)
        float pad = 4.0f;
        float thickness = hovered ? 1.5f : 1.1f;

        D2D1_POINT_2F a = D2D1::Point2F(rect.left + pad, rect.top + pad);
        D2D1_POINT_2F b = D2D1::Point2F(rect.right - pad, rect.bottom - pad);
        D2D1_POINT_2F c = D2D1::Point2F(rect.left + pad, rect.bottom - pad);
        D2D1_POINT_2F d = D2D1::Point2F(rect.right - pad, rect.top + pad);

        ctx->DrawLine(a, b, fgBrush, thickness);
        ctx->DrawLine(c, d, fgBrush, thickness);
    }

    if (ringBrush)
        ringBrush->Release();
    if (fgBrush)
        fgBrush->Release();
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
    const UI::Theme::Palette &themePalette = UI::Theme::GetPalette();
    const D2D1_COLOR_F baseBg = UI::Theme::ChromeBackground();
    const D2D1_COLOR_F hoverBg = BlendTabColor(baseBg, themePalette.explorerToolbarHover, 0.88f);
    const D2D1_COLOR_F activeBg = BlendTabColor(baseBg, themePalette.explorerToolbarHover, 1.0f);

    // Background de la tab bar
    ID2D1SolidColorBrush *bgBrush = nullptr;
    ctx->CreateSolidColorBrush(baseBg, &bgBrush);

    D2D1_RECT_F barRect = D2D1::RectF(leftEdge_, topEdge_, rightEdge_, topEdge_ + GetHeight());
    ctx->FillRectangle(barRect, bgBrush);

    // Draw bottom border matching the titlebar border color only when there are tabs
    if (!tabs_.empty())
    {
        ID2D1SolidColorBrush *tabBorderBrush = nullptr;
        ctx->CreateSolidColorBrush(UI::Theme::ChromeBorder(), &tabBorderBrush);
        // draw the border inside the tab bar's rect so layout and hit-tests include it
        D2D1_POINT_2F bl = D2D1::Point2F(leftEdge_, topEdge_ + GetHeight() - 0.5f);
        D2D1_POINT_2F br = D2D1::Point2F(rightEdge_, topEdge_ + GetHeight() - 0.5f);
        ctx->DrawLine(bl, br, tabBorderBrush, 1.0f);
        if (tabBorderBrush)
            tabBorderBrush->Release();
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
            ctx->CreateSolidColorBrush(activeBg, &tabBrush);
        }
        else if (i == hoveredTabIndex_)
        {
            ctx->CreateSolidColorBrush(hoverBg, &tabBrush);
        }
        else
        {
            ctx->CreateSolidColorBrush(baseBg, &tabBrush);
        }
        ctx->FillRectangle(tabRect, tabBrush);
        tabBrush->Release();

        // Bordure en bas pour l'onglet actif avec coins arrondis
        if (tab.isActive)
        {
            ID2D1SolidColorBrush *borderBrush = nullptr;
            ctx->CreateSolidColorBrush(UI::Theme::Accent(), &borderBrush);

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

        if (tab.isActive)
        {
            ID2D1SolidColorBrush *activeCoverBrush = nullptr;
            ctx->CreateSolidColorBrush(activeBg, &activeCoverBrush);
            if (activeCoverBrush)
            {
                ctx->FillRectangle(
                    D2D1::RectF(x + 3.0f, topEdge_ + tabHeight_ - 2.0f, x + tabWidth_ - 3.0f, topEdge_ + tabHeight_),
                    activeCoverBrush);
                activeCoverBrush->Release();
            }

            ID2D1SolidColorBrush *topLineBrush = nullptr;
            D2D1_COLOR_F outline = UI::Theme::ChromeBorder();
            outline.a = 0.95f;
            ctx->CreateSolidColorBrush(outline, &topLineBrush);
            if (topLineBrush)
            {
                ctx->FillRectangle(D2D1::RectF(x, topEdge_, x + tabWidth_, topEdge_ + 1.0f), topLineBrush);
                topLineBrush->Release();
            }
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
            if (tab.isActive)
            {
                ctx->CreateSolidColorBrush(UI::Theme::PrimaryText(), &textBrush);
            }
            else
            {
                ctx->CreateSolidColorBrush(UI::Theme::MutedText(), &textBrush);
            }

            // Determine if we have an icon for this tab (re-use Explorer's icon loader)
            float textLeft = x + 10;
            UINT dpi = win32_get_dpi_for_window(hwnd);
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

            // --- réserve la place du bouton close/dot même quand il est caché ---
            const float gapToClose = 6.0f; // petit espace entre texte et bouton

            D2D1_RECT_F closeRect = CloseRectForTab(i);
            float textRight = closeRect.left - gapToClose;
            bool showPreviewToggle = tab.isMarkdown && (tab.isActive || hoveredTabIndex_ == i);
            if (showPreviewToggle)
            {
                D2D1_RECT_F previewRect = PreviewRectForTab(i);
                textRight = previewRect.left - gapToClose;
            }

            // sécurité si onglet trop petit
            float minTextWidth = 10.0f;
            if (textRight < textLeft + minTextWidth)
                textRight = textLeft + minTextWidth;

            D2D1_RECT_F textRect = D2D1::RectF(
                textLeft,
                topEdge_,
                textRight,
                topEdge_ + tabHeight_);

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

        bool showClose = (tab.isActive || hoveredTabIndex_ == i || hoveredCloseIndex_ == i);

        if (showClose)
        {
            D2D1_RECT_F closeRect = CloseRectForTab(i);
            bool isHoveredClose = (hoveredCloseIndex_ == i);
            DrawCloseOrDirty(ctx, closeRect, isHoveredClose, tab.isDirty);
        }
        if (tab.isMarkdown)
        {
            bool showPreviewToggle = (tab.isActive || hoveredTabIndex_ == i);
            if (showPreviewToggle)
            {
                D2D1_RECT_F previewRect = PreviewRectForTab(i);
                bool hovered = (hoveredPreviewIndex_ == i);

                if (hovered)
                {
                    ID2D1SolidColorBrush *bg = nullptr;
                    ctx->CreateSolidColorBrush(themePalette.explorerToolbarHover, &bg);
                    if (bg)
                    {
                        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(previewRect, 3.0f, 3.0f);
                        ctx->FillRoundedRectangle(rr, bg);
                        bg->Release();
                    }
                }

                std::string iconPath = tab.markdownPreview
                                           ? "assets/ressource/icons/folder-review-open.svg"
                                           : "assets/ressource/icons/folder-review.svg";
                UINT dpi = win32_get_dpi_for_window(hwnd);
                int iconPx = win32_dpi_scale(14, dpi);
                ID2D1Bitmap *iconBmp = GetExplorerManager().LoadSvgIconPublic(ctx, iconPath, iconPx, dpi);
                if (iconBmp)
                {
                    float iconY = previewRect.top + (previewRect.bottom - previewRect.top - (float)iconPx) * 0.5f;
                    float iconX = previewRect.left + (previewRect.right - previewRect.left - (float)iconPx) * 0.5f;
                    D2D1_RECT_F iconRect = D2D1::RectF(iconX, iconY, iconX + (float)iconPx, iconY + (float)iconPx);
                    ctx->DrawBitmap(iconBmp, iconRect, tab.markdownPreview ? 1.0f : 0.8f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                }
            }
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
        if (tabs_[i].isMarkdown && IsPointInPreviewRect(i, pt))
        {
            lastPreviewToggleIndex_ = i;
            hoveredPreviewIndex_ = -1;
            hoveredCloseIndex_ = -1;
            hoveredTabIndex_ = -1;
            return TAB_CLICKED_TOGGLE_PREVIEW;
        }


        // 1) Close button has priority -> signal close request (UI only)
        if (IsPointInCloseRect(i, pt))
        {
            lastCloseRequestIndex_ = i;
            hoveredCloseIndex_ = -1;
            hoveredTabIndex_ = -1;
            return TAB_CLICKED_CLOSE; // -3 : signal to Window that user requested close
        }

        // 2) Sinon : click sur tab
        if (pt.x >= tabRect.left && pt.x < tabRect.right &&
            pt.y >= tabRect.top && pt.y < tabRect.bottom)
        {
            SetActiveTab(i);
            return i; // ✅ handled: tab activated
        }

        x += tabWidth_;
    }

    return -1; // not handled
}

int TabBar::OnMouseMove(POINT pt)
{
    int prevTab = hoveredTabIndex_;
    int prevClose = hoveredCloseIndex_;
    int prevPreview = hoveredPreviewIndex_;

    // Reset hover states
    int newTab = -1;
    int newClose = -1;
    int newPreview = -1;

    // Tolérance horizontale pour éviter les pertes de hover
    const float horizTolerance = 4.0f;

    float x = leftEdge_;
    for (int i = 0; i < (int)tabs_.size(); ++i)
    {
        // Zone étendue de l'onglet pour le hover
        D2D1_RECT_F tabRect = D2D1::RectF(
            x - horizTolerance,
            topEdge_,
            x + tabWidth_ + horizTolerance,
            topEdge_ + tabHeight_);

        // Vérifier si le pointeur est dans la zone de l'onglet
        if (pt.x >= tabRect.left && pt.x < tabRect.right &&
            pt.y >= tabRect.top && pt.y < tabRect.bottom)
        {

            newTab = i;

            // ✨ NOUVELLE LOGIQUE : Sticky close avec zone élargie
            // Une fois qu'on survole le bouton close, il reste visible
            // tant qu'on reste dans l'onglet
            D2D1_RECT_F closeRect = CloseRectForTab(i);
            const float closeTolerance = 8.0f; // Zone élargie pour le hover

            bool inCloseZone = (pt.x >= closeRect.left - closeTolerance &&
                                pt.x <= closeRect.right + closeTolerance &&
                                pt.y >= closeRect.top - closeTolerance &&
                                pt.y <= closeRect.bottom + closeTolerance);

            if (inCloseZone)
            {
                newClose = i;
            }

            if (tabs_[i].isMarkdown)
            {
                D2D1_RECT_F previewRect = PreviewRectForTab(i);
                const float previewTolerance = 6.0f;
                bool inPreviewZone = (pt.x >= previewRect.left - previewTolerance &&
                                      pt.x <= previewRect.right + previewTolerance &&
                                      pt.y >= previewRect.top - previewTolerance &&
                                      pt.y <= previewRect.bottom + previewTolerance);
                if (inPreviewZone)
                    newPreview = i;
            }

            break; // On a trouvé l'onglet survolé
        }

        x += tabWidth_;
    }

    // ✨ OPTIMISATION : Ne redessiner QUE si quelque chose a vraiment changé
    if (newTab != prevTab || newClose != prevClose || newPreview != prevPreview)
    {
        hoveredTabIndex_ = newTab;
        hoveredCloseIndex_ = newClose;
        hoveredPreviewIndex_ = newPreview;
        return newTab; // Signal qu'il faut redessiner
    }

    return -2; // Pas de changement, pas besoin de redessiner
}

bool TabBar::ClearHover()
{
    bool hadHover = (hoveredTabIndex_ >= 0 || hoveredCloseIndex_ >= 0 || hoveredPreviewIndex_ >= 0);
    hoveredTabIndex_ = -1;
    hoveredCloseIndex_ = -1;
    hoveredPreviewIndex_ = -1;
    return hadHover;
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

D2D1_RECT_F TabBar::PreviewRectForTab(int index) const
{
    float x = leftEdge_ + index * tabWidth_;
    float size = 14.0f;
    float closePadding = 10.0f;
    float gap = 6.0f;
    float right = x + tabWidth_ - closePadding - 14.0f - gap;
    return D2D1::RectF(
        right - size,
        topEdge_ + (tabHeight_ - size) * 0.5f,
        right,
        topEdge_ + (tabHeight_ + size) * 0.5f);
}

bool TabBar::IsPointInPreviewRect(int index, POINT pt) const
{
    if (index < 0 || index >= (int)tabs_.size())
        return false;
    if (!tabs_[index].isMarkdown)
        return false;

    D2D1_RECT_F r = PreviewRectForTab(index);
    const float tolerance = 2.0f;

    return (pt.x >= r.left - tolerance &&
            pt.x <= r.right + tolerance &&
            pt.y >= r.top - tolerance &&
            pt.y <= r.bottom + tolerance);
}

bool TabBar::IsPointInCloseRect(int index, POINT pt) const
{
    if (index < 0 || index >= (int)tabs_.size())
        return false;

    D2D1_RECT_F r = CloseRectForTab(index);

    const float tolerance = 2.0f;

    return (pt.x >= r.left - tolerance &&
            pt.x <= r.right + tolerance &&
            pt.y >= r.top - tolerance &&
            pt.y <= r.bottom + tolerance);
}

void TabBar::DrawCloseButton(ID2D1RenderTarget *ctx, const D2D1_RECT_F &rect, bool hovered) const
{
    D2D1_COLOR_F xColorNormal = UI::Theme::MutedText();
    xColorNormal.a = 0.75f;
    D2D1_COLOR_F xColorHover = UI::Theme::PrimaryText();
    D2D1_COLOR_F ringColor = UI::Theme::GetPalette().explorerToolbarHover;
    ringColor.a = 0.95f;

    ID2D1SolidColorBrush *ringBrush = nullptr;
    ID2D1SolidColorBrush *xBrush = nullptr;

    if (hovered)
    {
        // Cercle de fond pour le hover
        float cx = (rect.left + rect.right) * 0.5f;
        float cy = (rect.top + rect.bottom) * 0.5f;
        float radius = (rect.right - rect.left) * 0.5f;

        ctx->CreateSolidColorBrush(ringColor, &ringBrush);
        D2D1_ELLIPSE ellipse = D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius);
        ctx->FillEllipse(ellipse, ringBrush);

        ctx->CreateSolidColorBrush(xColorHover, &xBrush);
    }
    else
    {
        ctx->CreateSolidColorBrush(xColorNormal, &xBrush);
    }

    // Dessiner le X avec épaisseur variable
    float pad = 4.0f;
    float thickness = hovered ? 1.5f : 1.1f;

    D2D1_POINT_2F a = D2D1::Point2F(rect.left + pad, rect.top + pad);
    D2D1_POINT_2F b = D2D1::Point2F(rect.right - pad, rect.bottom - pad);
    D2D1_POINT_2F c = D2D1::Point2F(rect.left + pad, rect.bottom - pad);
    D2D1_POINT_2F d = D2D1::Point2F(rect.right - pad, rect.top + pad);

    ctx->DrawLine(a, b, xBrush, thickness);
    ctx->DrawLine(c, d, xBrush, thickness);

    if (ringBrush)
        ringBrush->Release();
    if (xBrush)
        xBrush->Release();
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

    size_t pos = filePath.find_last_of(L'.');
    std::wstring ext = (pos != std::wstring::npos) ? filePath.substr(pos) : L"";
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    tabs_[index].isMarkdown = (ext == L".md");
    if (!tabs_[index].isMarkdown)
        tabs_[index].markdownPreview = false;
}

void TabBar::SetTabMarkdown(int index, bool isMarkdown)
{
    if (index < 0 || index >= (int)tabs_.size())
        return;
    tabs_[index].isMarkdown = isMarkdown;
    if (!isMarkdown)
        tabs_[index].markdownPreview = false;
}

void TabBar::SetTabMarkdownPreview(int index, bool enabled)
{
    if (index < 0 || index >= (int)tabs_.size())
        return;
    tabs_[index].markdownPreview = enabled;
}
