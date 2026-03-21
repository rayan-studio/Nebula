#include "TabBar.h"

#include <Windows.h>

#include <algorithm>
#include <cwctype>

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

D2D1_RECT_F MakeTabRect(float left, float top, float width, float height, bool active)
{
    const float insetLeft = 1.0f;
    const float insetRight = 1.0f;
    const float insetTop = active ? 0.0f : 3.0f;
    const float insetBottom = active ? 0.0f : 1.0f;
    const float l = std::round(left + insetLeft);
    const float t = std::round(top + insetTop);
    const float r = std::round(left + width - insetRight);
    const float b = std::round(top + height - insetBottom);
    return D2D1::RectF(l, t, r, b);
}
}

TabBar::TabBar() {}
TabBar::~TabBar() {}

int TabBar::AddTab(const std::wstring &filePath, const std::wstring &displayName)
{
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

    std::wstring removedPath = tabs_[index].filePath;
    tabs_.erase(tabs_.begin() + index);

    mruHistory_.erase(std::remove(mruHistory_.begin(), mruHistory_.end(), removedPath), mruHistory_.end());

    if (tabs_.empty())
    {
        activeTabIndex_ = -1;
        return;
    }

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

    if (activeTabIndex_ == index)
    {
        activeTabIndex_ = (std::min)(index, (int)tabs_.size() - 1);
        if (activeTabIndex_ >= 0)
            tabs_[activeTabIndex_].isActive = true;
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

    for (auto &tab : tabs_)
        tab.isActive = false;

    tabs_[index].isActive = true;
    activeTabIndex_ = index;

    const std::wstring &fp = tabs_[index].filePath;
    mruHistory_.erase(std::remove(mruHistory_.begin(), mruHistory_.end(), fp), mruHistory_.end());
    mruHistory_.insert(mruHistory_.begin(), fp);

    if (!fp.empty())
        GetExplorerManager().SetActivePath(fp);
}

const Tab *TabBar::GetActiveTab() const
{
    if (activeTabIndex_ >= 0 && activeTabIndex_ < (int)tabs_.size())
        return &tabs_[activeTabIndex_];
    return nullptr;
}

const Tab *TabBar::GetTab(int index) const
{
    if (index >= 0 && index < (int)tabs_.size())
        return &tabs_[index];
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
    if (!ctx)
        return;

    D2D1_COLOR_F normal = UI::Theme::MutedText();
    normal.a = 0.82f;
    D2D1_COLOR_F hover = UI::Theme::PrimaryText();
    D2D1_COLOR_F hoverBg = UI::Theme::GetPalette().explorerToolbarHover;

    ID2D1SolidColorBrush *bgBrush = nullptr;
    ID2D1SolidColorBrush *fgBrush = nullptr;

    if (hovered)
    {
        ctx->CreateSolidColorBrush(hoverBg, &bgBrush);
        if (bgBrush)
            ctx->FillRectangle(rect, bgBrush);
        ctx->CreateSolidColorBrush(hover, &fgBrush);
    }
    else
    {
        ctx->CreateSolidColorBrush(normal, &fgBrush);
    }

    if (!fgBrush)
    {
        if (bgBrush)
            bgBrush->Release();
        return;
    }

    const float cx = (rect.left + rect.right) * 0.5f;
    const float cy = (rect.top + rect.bottom) * 0.5f;

    if (dirty)
    {
        const float radius = (rect.right - rect.left) * 0.22f;
        ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), fgBrush);
    }
    else
    {
        const float pad = 4.0f;
        const float thickness = hovered ? 1.35f : 1.15f;

        const D2D1_POINT_2F a = D2D1::Point2F(rect.left + pad, rect.top + pad);
        const D2D1_POINT_2F b = D2D1::Point2F(rect.right - pad, rect.bottom - pad);
        const D2D1_POINT_2F c = D2D1::Point2F(rect.left + pad, rect.bottom - pad);
        const D2D1_POINT_2F d = D2D1::Point2F(rect.right - pad, rect.top + pad);

        ctx->DrawLine(a, b, fgBrush, thickness);
        ctx->DrawLine(c, d, fgBrush, thickness);
    }

    if (bgBrush)
        bgBrush->Release();
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
    if (!ctx || !dwrite)
        return;
    if (tabs_.empty())
        return;

    const UI::Theme::Palette &themePalette = UI::Theme::GetPalette();
    const D2D1_COLOR_F baseBg = UI::Theme::ChromeBackground();
    const D2D1_COLOR_F editorBg = D2D1::ColorF(24.0f / 255.0f, 26.0f / 255.0f, 29.0f / 255.0f, 1.0f);
    const D2D1_COLOR_F editorShellBg = D2D1::ColorF(22.0f / 255.0f, 24.0f / 255.0f, 27.0f / 255.0f, 1.0f);
    const D2D1_COLOR_F trackBg = editorShellBg;
    const D2D1_COLOR_F idleTabBg = BlendTabColor(baseBg, themePalette.inputBackground, 0.12f);
    const D2D1_COLOR_F hoverTabBg = BlendTabColor(baseBg, themePalette.explorerToolbarHover, 0.28f);
    const D2D1_COLOR_F activeTabBg = editorBg;

    ID2D1SolidColorBrush *bgBrush = nullptr;
    ID2D1SolidColorBrush *borderBrush = nullptr;
    ID2D1SolidColorBrush *separatorBrush = nullptr;

    ctx->CreateSolidColorBrush(trackBg, &bgBrush);
    D2D1_COLOR_F borderColor = UI::Theme::ChromeBorder();
    borderColor.a = 0.78f;
    ctx->CreateSolidColorBrush(borderColor, &borderBrush);

    D2D1_COLOR_F separatorColor = UI::Theme::ChromeBorder();
    separatorColor.a = 0.38f;
    ctx->CreateSolidColorBrush(separatorColor, &separatorBrush);

    const D2D1_RECT_F barRect = D2D1::RectF(leftEdge_, topEdge_, rightEdge_, topEdge_ + GetHeight());
    if (bgBrush)
        ctx->FillRectangle(barRect, bgBrush);

    const D2D1_ANTIALIAS_MODE oldAA = ctx->GetAntialiasMode();
    ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);

    if (borderBrush)
    {
        const float lineY = std::round(topEdge_ + tabHeight_) + 0.5f;
        ctx->DrawLine(
            D2D1::Point2F(std::round(leftEdge_), lineY),
            D2D1::Point2F(std::round(rightEdge_), lineY),
            borderBrush,
            1.0f);
    }

    IDWriteTextFormat *format = nullptr;
    dwrite->CreateTextFormat(
        L"Segoe UI",
        NULL,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        12.0f,
        L"en-us",
        &format);

    if (format)
    {
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    float x = leftEdge_;
    for (int i = 0; i < (int)tabs_.size(); ++i)
    {
        const Tab &tab = tabs_[i];
        const D2D1_RECT_F tabRect = MakeTabRect(x, topEdge_, tabWidth_, tabHeight_, tab.isActive);

        D2D1_COLOR_F fillColor = idleTabBg;
        if (tab.isActive)
            fillColor = activeTabBg;
        else if (hoveredTabIndex_ == i)
            fillColor = hoverTabBg;

        ID2D1SolidColorBrush *tabBrush = nullptr;
        ctx->CreateSolidColorBrush(fillColor, &tabBrush);

        if (tabBrush)
            ctx->FillRectangle(tabRect, tabBrush);

        if (tab.isActive && borderBrush)
        {
            const float leftX = tabRect.left + 0.5f;
            const float rightX = tabRect.right - 0.5f;
            const float topY = tabRect.top + 0.5f;
            const float bottomY = std::round(topEdge_ + tabHeight_) + 0.5f;

            ctx->DrawLine(D2D1::Point2F(leftX, bottomY), D2D1::Point2F(leftX, topY), borderBrush, 1.0f);
            ctx->DrawLine(D2D1::Point2F(leftX, topY), D2D1::Point2F(rightX, topY), borderBrush, 1.0f);
            ctx->DrawLine(D2D1::Point2F(rightX, topY), D2D1::Point2F(rightX, bottomY), borderBrush, 1.0f);
        }

        if (!tab.isActive && separatorBrush && i < (int)tabs_.size() - 1)
        {
            const float separatorX = x + tabWidth_ - 0.5f;
            ctx->DrawLine(
                D2D1::Point2F(separatorX, topEdge_ + 9.0f),
                D2D1::Point2F(separatorX, topEdge_ + tabHeight_ - 8.0f),
                separatorBrush,
                1.0f);
        }

        if (tab.isActive && tabBrush)
        {
            const float lineY = std::round(topEdge_ + tabHeight_);
            const D2D1_RECT_F coverRect = D2D1::RectF(
                tabRect.left + 1.0f,
                lineY - 1.0f,
                tabRect.right - 1.0f,
                lineY + 1.0f);
            ctx->FillRectangle(coverRect, tabBrush);
        }

        if (format)
        {
            ID2D1SolidColorBrush *textBrush = nullptr;
            if (tab.isActive)
                ctx->CreateSolidColorBrush(UI::Theme::PrimaryText(), &textBrush);
            else
                ctx->CreateSolidColorBrush(UI::Theme::MutedText(), &textBrush);

            float textLeft = tabRect.left + 14.0f;
            const UINT dpi = win32_get_dpi_for_window(hwnd);
            const int iconPx = win32_dpi_scale(16, dpi);

            if (!tab.filePath.empty())
            {
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

                ID2D1Bitmap *iconBitmap = GetExplorerManager().GetIconForItemPublic(ctx, tmp, hwnd);
                if (iconBitmap)
                {
                    const float iconY = tabRect.top + (tabRect.bottom - tabRect.top - (float)iconPx) * 0.5f;
                    const D2D1_RECT_F iconRect = D2D1::RectF(
                        tabRect.left + 12.0f,
                        iconY,
                        tabRect.left + 12.0f + (float)iconPx,
                        iconY + (float)iconPx);
                    ctx->DrawBitmap(iconBitmap, iconRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
                    textLeft = tabRect.left + 12.0f + (float)iconPx + 8.0f;
                }
            }

            const float gapToClose = 6.0f;
            const D2D1_RECT_F closeRect = CloseRectForTab(i);
            float textRight = closeRect.left - gapToClose;

            const bool showPreviewToggle = tab.isMarkdown && (tab.isActive || hoveredTabIndex_ == i);
            if (showPreviewToggle)
            {
                const D2D1_RECT_F previewRect = PreviewRectForTab(i);
                textRight = previewRect.left - gapToClose;
            }

            if (textRight < textLeft + 10.0f)
                textRight = textLeft + 10.0f;

            const D2D1_RECT_F textRect = D2D1::RectF(textLeft, tabRect.top, textRight, tabRect.bottom);

            IDWriteTextLayout *textLayout = nullptr;
            HRESULT hr = dwrite->CreateTextLayout(
                tab.displayName.c_str(),
                (UINT32)tab.displayName.size(),
                format,
                textRect.right - textRect.left,
                textRect.bottom - textRect.top,
                &textLayout);

            if (SUCCEEDED(hr) && textLayout && textBrush)
            {
                textLayout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

                DWRITE_TRIMMING trimming = {};
                trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
                IDWriteInlineObject *ellipsisToken = nullptr;
                if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(format, &ellipsisToken)))
                    textLayout->SetTrimming(&trimming, ellipsisToken);

                ctx->DrawTextLayout(D2D1::Point2F(textRect.left, textRect.top), textLayout, textBrush);

                if (ellipsisToken)
                    ellipsisToken->Release();
                textLayout->Release();
            }
            else if (textBrush)
            {
                ctx->DrawTextW(tab.displayName.c_str(), (UINT32)tab.displayName.size(), format, textRect, textBrush);
                if (textLayout)
                    textLayout->Release();
            }

            if (textBrush)
                textBrush->Release();
        }

        const bool showClose = (tab.isActive || hoveredTabIndex_ == i || hoveredCloseIndex_ == i);
        if (showClose)
        {
            const D2D1_RECT_F closeRect = CloseRectForTab(i);
            const bool isHoveredClose = (hoveredCloseIndex_ == i);
            DrawCloseOrDirty(ctx, closeRect, isHoveredClose, tab.isDirty);
        }

        if (tab.isMarkdown)
        {
            const bool showPreviewToggle = (tab.isActive || hoveredTabIndex_ == i);
            if (showPreviewToggle)
            {
                const D2D1_RECT_F previewRect = PreviewRectForTab(i);
                const bool hovered = (hoveredPreviewIndex_ == i);

                if (hovered)
                {
                    ID2D1SolidColorBrush *previewBgBrush = nullptr;
                    ctx->CreateSolidColorBrush(themePalette.explorerToolbarHover, &previewBgBrush);
                    if (previewBgBrush)
                    {
                        ctx->FillRectangle(previewRect, previewBgBrush);
                        previewBgBrush->Release();
                    }
                }

                const std::string iconPath = tab.markdownPreview
                                                 ? "assets/ressource/icons/folder-review-open.svg"
                                                 : "assets/ressource/icons/folder-review.svg";
                const UINT dpi = win32_get_dpi_for_window(hwnd);
                const int iconPx = win32_dpi_scale(14, dpi);
                ID2D1Bitmap *iconBmp = GetExplorerManager().LoadSvgIconPublic(ctx, iconPath, iconPx, dpi);
                if (iconBmp)
                {
                    const float iconY = previewRect.top + (previewRect.bottom - previewRect.top - (float)iconPx) * 0.5f;
                    const float iconX = previewRect.left + (previewRect.right - previewRect.left - (float)iconPx) * 0.5f;
                    const D2D1_RECT_F iconRect = D2D1::RectF(iconX, iconY, iconX + (float)iconPx, iconY + (float)iconPx);
                    ctx->DrawBitmap(iconBmp, iconRect, tab.markdownPreview ? 1.0f : 0.8f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
                }
            }
        }

        if (tabBrush)
            tabBrush->Release();

        x += tabWidth_;
    }

    if (format)
        format->Release();
    if (separatorBrush)
        separatorBrush->Release();
    if (borderBrush)
        borderBrush->Release();
    if (bgBrush)
        bgBrush->Release();
    ctx->SetAntialiasMode(oldAA);
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

        if (IsPointInCloseRect(i, pt))
        {
            lastCloseRequestIndex_ = i;
            hoveredCloseIndex_ = -1;
            hoveredTabIndex_ = -1;
            return TAB_CLICKED_CLOSE;
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

int TabBar::OnMouseMove(POINT pt)
{
    int prevTab = hoveredTabIndex_;
    int prevClose = hoveredCloseIndex_;
    int prevPreview = hoveredPreviewIndex_;

    int newTab = -1;
    int newClose = -1;
    int newPreview = -1;

    const float horizTolerance = 4.0f;

    float x = leftEdge_;
    for (int i = 0; i < (int)tabs_.size(); ++i)
    {
        D2D1_RECT_F tabRect = D2D1::RectF(
            x - horizTolerance,
            topEdge_,
            x + tabWidth_ + horizTolerance,
            topEdge_ + tabHeight_);

        if (pt.x >= tabRect.left && pt.x < tabRect.right &&
            pt.y >= tabRect.top && pt.y < tabRect.bottom)
        {
            newTab = i;

            D2D1_RECT_F closeRect = CloseRectForTab(i);
            const float closeTolerance = 8.0f;
            bool inCloseZone = (pt.x >= closeRect.left - closeTolerance &&
                                pt.x <= closeRect.right + closeTolerance &&
                                pt.y >= closeRect.top - closeTolerance &&
                                pt.y <= closeRect.bottom + closeTolerance);
            if (inCloseZone)
                newClose = i;

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

            break;
        }

        x += tabWidth_;
    }

    if (newTab != prevTab || newClose != prevClose || newPreview != prevPreview)
    {
        hoveredTabIndex_ = newTab;
        hoveredCloseIndex_ = newClose;
        hoveredPreviewIndex_ = newPreview;
        return newTab;
    }

    return -2;
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
    if (!ctx)
        return;

    D2D1_COLOR_F normal = UI::Theme::MutedText();
    normal.a = 0.82f;
    D2D1_COLOR_F hover = UI::Theme::PrimaryText();
    D2D1_COLOR_F hoverBg = UI::Theme::GetPalette().explorerToolbarHover;

    ID2D1SolidColorBrush *bgBrush = nullptr;
    ID2D1SolidColorBrush *xBrush = nullptr;

    if (hovered)
    {
        ctx->CreateSolidColorBrush(hoverBg, &bgBrush);
        if (bgBrush)
            ctx->FillRectangle(rect, bgBrush);
        ctx->CreateSolidColorBrush(hover, &xBrush);
    }
    else
    {
        ctx->CreateSolidColorBrush(normal, &xBrush);
    }

    if (!xBrush)
    {
        if (bgBrush)
            bgBrush->Release();
        return;
    }

    const float pad = 4.0f;
    const float thickness = hovered ? 1.35f : 1.15f;

    const D2D1_POINT_2F a = D2D1::Point2F(rect.left + pad, rect.top + pad);
    const D2D1_POINT_2F b = D2D1::Point2F(rect.right - pad, rect.bottom - pad);
    const D2D1_POINT_2F c = D2D1::Point2F(rect.left + pad, rect.bottom - pad);
    const D2D1_POINT_2F d = D2D1::Point2F(rect.right - pad, rect.top + pad);

    ctx->DrawLine(a, b, xBrush, thickness);
    ctx->DrawLine(c, d, xBrush, thickness);

    if (bgBrush)
        bgBrush->Release();
    xBrush->Release();
}

void TabBar::UpdateTabPath(int index, const std::wstring &filePath, const std::wstring &displayName)
{
    if (index < 0 || index >= (int)tabs_.size())
        return;

    tabs_[index].filePath = filePath;
    tabs_[index].displayName = displayName;

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
