#include "orion/editor/Editor.h"
#include "orion/editor/internal/Editor_Internal.h"
#include "orion/completion/popup/Popup.h"

#include <algorithm>
#include <cmath>

#include "orion/rendering/gutter/Gutter.h"
#include "orion/rendering/GuideRenderer.h"
#include "orion/geometry/IndentationHelper.h"
#include "orion/geometry/TextColumns.h"
#include "orion/selection/Selection.h"
#include "orion/caret/Caret.h"
#include "core/explorer/Explorer.h"
#include "ui/theme/Theme.h"
#include "ui/panels/git/GitDiffDecorations.h"

// Ensure Windows min/max macros don't interfere with std::min/std::max
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

namespace Orion
{
    void Editor::EnsureFoldLineMaps()
    {
        if (!foldLineMapsDirty_)
            return;
        RebuildFoldLineMaps();
    }

    void Editor::RebuildFoldLineMaps()
    {
        const int lineCount = (int)state_.lines.size();

        // Drop invalid or overlapping folds to keep mapping deterministic.
        for (auto it = collapsedFolds_.begin(); it != collapsedFolds_.end();)
        {
            if (it->first < 0 || it->first >= lineCount || it->second <= it->first || it->second >= lineCount)
                it = collapsedFolds_.erase(it);
            else
                ++it;
        }
        int prevEnd = -1;
        for (auto it = collapsedFolds_.begin(); it != collapsedFolds_.end();)
        {
            if (it->first <= prevEnd)
            {
                it = collapsedFolds_.erase(it);
                continue;
            }
            prevEnd = it->second;
            ++it;
        }

        state_.visualLineByActual.assign((size_t)(std::max)(0, lineCount), 0);
        state_.actualLineByVisual.clear();
        if (lineCount <= 0)
        {
            foldLineMapsDirty_ = false;
            return;
        }

        int visible = 0;
        int line = 0;
        while (line < lineCount)
        {
            state_.actualLineByVisual.push_back(line);
            state_.visualLineByActual[(size_t)line] = visible;

            auto f = collapsedFolds_.find(line);
            if (f != collapsedFolds_.end())
            {
                int end = f->second;
                if (end > line)
                {
                    for (int h = line + 1; h <= end && h < lineCount; ++h)
                        state_.visualLineByActual[(size_t)h] = visible;
                    line = end + 1;
                }
                else
                {
                    ++line;
                }
            }
            else
            {
                ++line;
            }
            ++visible;
        }

        foldLineMapsDirty_ = false;
    }

    int Editor::GetVisibleLineCount()
    {
        EnsureFoldLineMaps();
        if (state_.actualLineByVisual.empty())
            return 1;
        return (int)state_.actualLineByVisual.size();
    }

    int Editor::VisibleLineToActualLine(int visibleLine)
    {
        EnsureFoldLineMaps();
        if (state_.lines.empty())
            return 0;
        if (state_.actualLineByVisual.empty())
            return (std::max)(0, (std::min)(visibleLine, (int)state_.lines.size() - 1));
        int clamped = (std::max)(0, (std::min)(visibleLine, (int)state_.actualLineByVisual.size() - 1));
        return state_.actualLineByVisual[(size_t)clamped];
    }

    int Editor::ActualLineToVisibleLine(int actualLine)
    {
        EnsureFoldLineMaps();
        if (state_.lines.empty())
            return 0;
        int clamped = (std::max)(0, (std::min)(actualLine, (int)state_.lines.size() - 1));
        if (state_.visualLineByActual.empty())
            return clamped;
        return state_.visualLineByActual[(size_t)clamped];
    }

    bool Editor::IsLineHiddenByFold(int actualLine)
    {
        for (const auto &kv : collapsedFolds_)
        {
            if (actualLine <= kv.first)
                break;
            if (actualLine <= kv.second)
                return true;
        }
        return false;
    }

    bool Editor::IsCollapsedFoldStart(int line, int *outEnd)
    {
        auto it = collapsedFolds_.find(line);
        if (it == collapsedFolds_.end())
            return false;
        if (outEnd)
            *outEnd = it->second;
        return true;
    }

    int Editor::FindFoldEndLineForStart(int startLine) const
    {
        const int lineCount = (int)state_.lines.size();
        if (startLine < 0 || startLine >= lineCount)
            return -1;

        bool inBlockComment = false;
        bool foundOpen = false;
        bool openedOnStart = false;
        int depth = 0;

        for (int li = startLine; li < lineCount; ++li)
        {
            const std::wstring &ln = state_.lines[(size_t)li];
            bool inString = false;
            bool inChar = false;
            bool escaped = false;

            for (size_t ci = 0; ci < ln.size(); ++ci)
            {
                wchar_t c = ln[ci];
                wchar_t n = (ci + 1 < ln.size()) ? ln[ci + 1] : 0;

                if (inBlockComment)
                {
                    if (c == L'*' && n == L'/')
                    {
                        inBlockComment = false;
                        ++ci;
                    }
                    continue;
                }
                if (inString)
                {
                    if (escaped)
                    {
                        escaped = false;
                        continue;
                    }
                    if (c == L'\\')
                    {
                        escaped = true;
                        continue;
                    }
                    if (c == L'"')
                        inString = false;
                    continue;
                }
                if (inChar)
                {
                    if (escaped)
                    {
                        escaped = false;
                        continue;
                    }
                    if (c == L'\\')
                    {
                        escaped = true;
                        continue;
                    }
                    if (c == L'\'')
                        inChar = false;
                    continue;
                }

                if (c == L'/' && n == L'/')
                    break;
                if (c == L'/' && n == L'*')
                {
                    inBlockComment = true;
                    ++ci;
                    continue;
                }
                if (c == L'"')
                {
                    inString = true;
                    continue;
                }
                if (c == L'\'')
                {
                    inChar = true;
                    continue;
                }

                if (c == L'{')
                {
                    ++depth;
                    foundOpen = true;
                    if (li == startLine)
                        openedOnStart = true;
                    continue;
                }
                if (c == L'}' && foundOpen && depth > 0)
                {
                    --depth;
                    if (depth == 0 && openedOnStart)
                        return li;
                }
            }
        }

        return -1;
    }

    bool Editor::ToggleFoldAtLine(int startLine)
    {
        if (state_.lines.empty())
            return false;

        auto existing = collapsedFolds_.find(startLine);
        if (existing != collapsedFolds_.end())
        {
            collapsedFolds_.erase(existing);
            foldLineMapsDirty_ = true;
            EnsureFoldLineMaps();
            return true;
        }

        int endLine = FindFoldEndLineForStart(startLine);
        if (endLine <= startLine)
            return false;

        collapsedFolds_[startLine] = endLine;
        foldLineMapsDirty_ = true;
        EnsureFoldLineMaps();

        if (state_.caret.line > startLine && state_.caret.line <= endLine)
        {
            state_.caret.line = startLine;
            int maxCol = (int)state_.lines[(size_t)startLine].size();
            state_.caret.column = (std::min)(state_.caret.column, maxCol);
            state_.hasSelection = false;
        }

        return true;
    }

    void Editor::UpdateLayout([[maybe_unused]] HWND hwnd, float left, float top, float right, float bottom)
    {
        (void)hwnd;
        state_.leftEdge = left;
        state_.topEdge = top;
        state_.rightEdge = right;
        state_.bottomEdge = bottom;

        float width = state_.rightEdge - state_.leftEdge;
        float height = state_.bottomEdge - state_.topEdge;

        if (isGitSplitDiffView_)
        {
            int splitRows = (std::max)(1, (int)gitSplitDiffRows_.size());
            float baseContentHeight = (float)splitRows * metrics_.lineHeight;

            float extra = height - metrics_.lineHeight;
            if (extra < 0.0f)
                extra = 0.0f;

            float contentHeight = baseContentHeight + extra;
            scrollbar_.UpdateLayout(state_.leftEdge, state_.topEdge, width, height, contentHeight);
            state_.scrollOffsetY = scrollbar_.GetScrollOffset();

            hScrollbarVisible_ = false;
            hContentWidth_ = 0.0f;
            hViewportWidth_ = 0.0f;
            hThumbWidth_ = 0.0f;
            hThumbPos_ = 0.0f;
            hIsDragging_ = false;
            state_.scrollOffsetX = 0.0f;

            float editorWidth = right - left - metrics_.gutterWidth;
            searchBox_.UpdateLayout(left + metrics_.gutterWidth, top, editorWidth);
            return;
        }

        if (markdownViewMode_ == MarkdownViewMode::Split)
        {
            EnsureFoldLineMaps();

            float dividerX = GetGitSplitDividerX();
            float leftPaneRight = dividerX - 5.0f;
            float availableWidth = (std::max)(120.0f, leftPaneRight - left - metrics_.gutterWidth);

            float baseContentHeight = (float)GetVisibleLineCount() * metrics_.lineHeight;
            float extra = height - metrics_.lineHeight;
            if (extra < 0.0f)
                extra = 0.0f;
            float contentHeight = baseContentHeight + extra;

            scrollbar_.UpdateLayout(state_.leftEdge, state_.topEdge, width, height, contentHeight);
            state_.scrollOffsetY = scrollbar_.GetScrollOffset();
            if (pendingRevealCaret_)
            {
                float caretTop = ActualLineToVisibleLine(state_.caret.line) * metrics_.lineHeight;
                float margin = metrics_.lineHeight * 2.0f;
                float desired = caretTop - margin;
                if (desired < 0.0f)
                    desired = 0.0f;
                scrollbar_.SetScrollOffset(desired);
                state_.scrollOffsetY = scrollbar_.GetScrollOffset();
                pendingRevealCaret_ = false;
            }

            hScrollbarVisible_ = false;
            hContentWidth_ = 0.0f;
            hViewportWidth_ = 0.0f;
            hThumbWidth_ = 0.0f;
            hThumbPos_ = 0.0f;
            hIsDragging_ = false;
            state_.scrollOffsetX = 0.0f;

            searchBox_.UpdateLayout(left + metrics_.gutterWidth, top, availableWidth);
            return;
        }

        if ((int)state_.visualLineByActual.size() != (int)state_.lines.size())
            foldLineMapsDirty_ = true;
        EnsureFoldLineMaps();

        float baseContentHeight = (float)GetVisibleLineCount() * metrics_.lineHeight;

        float extra = height - metrics_.lineHeight;
        if (extra < 0.0f)
            extra = 0.0f;

        float contentHeight = baseContentHeight + extra;

        scrollbar_.UpdateLayout(state_.leftEdge, state_.topEdge, width, height, contentHeight);

        state_.scrollOffsetY = scrollbar_.GetScrollOffset();
        if (pendingRevealCaret_)
        {
            // Reveal with a small top margin so the caret isn't glued to the bottom.
            float caretTop = ActualLineToVisibleLine(state_.caret.line) * metrics_.lineHeight;
            float margin = metrics_.lineHeight * 2.0f;
            float desired = caretTop - margin;
            if (desired < 0.0f)
                desired = 0.0f;
            scrollbar_.SetScrollOffset(desired);
            state_.scrollOffsetY = scrollbar_.GetScrollOffset();
            pendingRevealCaret_ = false;
        }

        float availableWidth = (right - left) - metrics_.gutterWidth;
        if (scrollbar_.IsVisible())
            availableWidth -= 14.0f;

        size_t maxLen = 0;
        for (const auto &ln : state_.lines)
            maxLen = (std::max)(maxLen, ln.size());

        float contentWidth = maxLen * metrics_.characterWidth + 20.0f;
        hContentWidth_ = contentWidth;
        hViewportWidth_ = availableWidth;
        hScrollbarVisible_ = contentWidth > availableWidth;

        if (hScrollbarVisible_)
        {
            float ratio = hViewportWidth_ / hContentWidth_;
            hThumbWidth_ = (std::max)(hViewportWidth_ * ratio, 30.0f);

            float maxScroll = hContentWidth_ - hViewportWidth_;
            float availableTrack = hViewportWidth_ - hThumbWidth_;
            if (maxScroll > 0.0f)
                hThumbPos_ = (state_.scrollOffsetX / maxScroll) * availableTrack;
            else
                hThumbPos_ = 0.0f;
        }

        float editorWidth = right - left - metrics_.gutterWidth;
        searchBox_.UpdateLayout(left + metrics_.gutterWidth, top, editorWidth);

        if (completionPopup_ && state_.caret.line >= 0 && state_.caret.line < (int)state_.lines.size())
        {
            D2D1_POINT_2F screen = TextToScreenPosition(state_.caret);
            completionPopup_->UpdateLayout(screen.x, screen.y + metrics_.lineHeight, 400.0f, metrics_.lineHeight);
        }
    }

    void Editor::Draw(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite)
    {
        if (!ctx || !dwrite)
            return;

        SyncThemeFromUi();

        pDWriteFactory_ = dwrite;

        if (!cachedTextFormat_)
        {
            HRESULT hr = pDWriteFactory_->CreateTextFormat(
                L"JetBrains Mono",
                customFontCollection_,
                DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                13.0f,
                L"en-us",
                &cachedTextFormat_);

            if (SUCCEEDED(hr) && cachedTextFormat_)
            {
                cachedTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                cachedTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                cachedTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            }
        }

        if (!fontMetricsInitialized_ && cachedTextFormat_ && pDWriteFactory_)
        {
            // Measure real advance using spaces (tab stops depend on space width).
            const wchar_t *spaceSample = L"          ";
            const UINT32 spaceLen = 10;
            IDWriteTextLayout *layout = nullptr;
            if (SUCCEEDED(pDWriteFactory_->CreateTextLayout(
                    spaceSample,
                    spaceLen,
                    cachedTextFormat_,
                    10000.0f,
                    metrics_.lineHeight,
                    &layout)) &&
                layout)
            {
                DWRITE_TEXT_METRICS tm = {};
                if (SUCCEEDED(layout->GetMetrics(&tm)) && tm.widthIncludingTrailingWhitespace > 0.0f)
                {
                    metrics_.characterWidth = tm.widthIncludingTrailingWhitespace / (float)spaceLen;
                    if (tm.height > 0.0f)
                        metrics_.lineHeight = (std::max)(metrics_.lineHeight, tm.height);
                }
                layout->Release();
            }
            if (metrics_.characterWidth <= 0.0f)
                metrics_.characterWidth = 8.4f;
            fontMetricsInitialized_ = true;
        }

        if (cachedTextFormat_)
        {
            const float tabStop = metrics_.characterWidth * (float)GetIndentConfig().tabSize;
            cachedTextFormat_->SetIncrementalTabStop(tabStop);
        }

        D2D1_ANTIALIAS_MODE oldAA = ctx->GetAntialiasMode();
        D2D1_TEXT_ANTIALIAS_MODE oldTextAA = ctx->GetTextAntialiasMode();
        ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

        D2D1_RECT_F editorClip = D2D1::RectF(
            state_.leftEdge,
            state_.topEdge,
            state_.rightEdge,
            state_.bottomEdge);

        ctx->PushAxisAlignedClip(editorClip, D2D1_ANTIALIAS_MODE_ALIASED);

        {
            ID2D1SolidColorBrush *bg = nullptr;
            ctx->CreateSolidColorBrush(theme_.background, &bg);
            if (bg)
            {
                ctx->FillRectangle(editorClip, bg);
                bg->Release();
            }
        }

        if (isPreview_)
        {
            DrawPreview(ctx, dwrite);
            ctx->PopAxisAlignedClip();
            ctx->SetAntialiasMode(oldAA);
            ctx->SetTextAntialiasMode(oldTextAA);
            return;
        }

        if (isGitSplitDiffView_)
        {
            DrawGitSplitDiff(ctx, dwrite);
            scrollbar_.Draw(ctx);
            ctx->PopAxisAlignedClip();
            ctx->SetAntialiasMode(oldAA);
            ctx->SetTextAntialiasMode(oldTextAA);
            return;
        }

        if (markdownViewMode_ == MarkdownViewMode::Split)
        {
            DrawMarkdownSplitView(ctx, dwrite);
        }
        else
        {
            DrawEditorTextPane(ctx, dwrite, true);
        }

        ctx->PopAxisAlignedClip();

        if (searchBox_.IsVisible())
        {
            searchBox_.Draw(ctx, dwrite);
        }

        ctx->SetAntialiasMode(oldAA);
        ctx->SetTextAntialiasMode(oldTextAA);
    }

    void Editor::DrawEditorTextPane(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, bool drawScrollbars)
    {
        EnsureFoldLineMaps();
        Orion::Gutter gutter;
        metrics_.gutterWidth = gutter.CalculateGutterWidth(state_, metrics_);
        gutter.DrawGutter(ctx, state_, theme_, metrics_);
        gutter.DrawLineNumbers(ctx, dwrite, state_, theme_, metrics_, customFontCollection_);
        DrawFoldMarkers(ctx, dwrite);

        float contentLeft = state_.leftEdge + metrics_.gutterWidth + metrics_.leftPadding;
        float verticalScrollbarWidth = (drawScrollbars && scrollbar_.IsVisible()) ? 14.0f : 0.0f;
        float horizontalScrollbarHeight = (drawScrollbars && hScrollbarVisible_) ? 14.0f : 0.0f;
        float contentRight = state_.rightEdge - verticalScrollbarWidth;
        float contentBottom = state_.bottomEdge - horizontalScrollbarHeight;
        D2D1_RECT_F contentClip = D2D1::RectF(contentLeft, state_.topEdge, contentRight, contentBottom);
        ctx->PushAxisAlignedClip(contentClip, D2D1_ANTIALIAS_MODE_ALIASED);

        DrawActiveLine(ctx);
        DrawGitDiffDecorations(ctx);
        DrawSelection(ctx);
        DrawTextContent(ctx, dwrite);
        DrawSearchMatches(ctx);
        DrawCaret(ctx);

        if (completionPopup_ && completionPopup_->IsVisible())
            completionPopup_->Draw(ctx, dwrite);

        if (diagHoverVisible_ && !diagHoverText_.empty())
        {
            ID2D1SolidColorBrush *tooltipBg = nullptr;
            ID2D1SolidColorBrush *tooltipBorder = nullptr;
            ID2D1SolidColorBrush *tooltipText = nullptr;

            ctx->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.12f, 0.12f, 0.96f), &tooltipBg);
            ctx->CreateSolidColorBrush(D2D1::ColorF(0.26f, 0.26f, 0.26f, 1.0f), &tooltipBorder);
            ctx->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.95f, 0.95f, 1.0f), &tooltipText);

            IDWriteTextFormat *tipFormat = nullptr;
            if (dwrite)
            {
                dwrite->CreateTextFormat(
                    L"Segoe UI",
                    NULL,
                    DWRITE_FONT_WEIGHT_NORMAL,
                    DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL,
                    12.0f,
                    L"en-us",
                    &tipFormat);
            }

            if (tipFormat && tooltipBg && tooltipText)
            {
                float maxWidth = 320.0f;
                IDWriteTextLayout *layout = nullptr;
                dwrite->CreateTextLayout(
                    diagHoverText_.c_str(),
                    (UINT32)diagHoverText_.size(),
                    tipFormat,
                    maxWidth,
                    200.0f,
                    &layout);

                if (layout)
                {
                    DWRITE_TEXT_METRICS tm = {};
                    layout->GetMetrics(&tm);

                    float padX = 8.0f;
                    float padY = 6.0f;
                    float w = tm.width + padX * 2.0f;
                    float h = tm.height + padY * 2.0f;

                    float x = (float)diagHoverPos_.x + 14.0f;
                    float y = (float)diagHoverPos_.y + 18.0f;

                    if (x + w > state_.rightEdge)
                        x = state_.rightEdge - w - 6.0f;
                    if (y + h > state_.bottomEdge)
                        y = state_.bottomEdge - h - 6.0f;
                    if (x < state_.leftEdge)
                        x = state_.leftEdge + 6.0f;
                    if (y < state_.topEdge)
                        y = state_.topEdge + 6.0f;

                    D2D1_RECT_F box = D2D1::RectF(x, y, x + w, y + h);
                    D2D1_ROUNDED_RECT round = D2D1::RoundedRect(box, 4.0f, 4.0f);
                    ctx->FillRoundedRectangle(round, tooltipBg);
                    if (tooltipBorder)
                        ctx->DrawRoundedRectangle(round, tooltipBorder, 1.0f);

                    D2D1_POINT_2F textPos = D2D1::Point2F(x + padX, y + padY);
                    CustomTextRenderer tipRenderer(ctx, tooltipText);
                    layout->Draw(nullptr, &tipRenderer, textPos.x, textPos.y);
                    layout->Release();
                }
            }

            if (tipFormat)
                tipFormat->Release();
            if (tooltipText)
                tooltipText->Release();
            if (tooltipBorder)
                tooltipBorder->Release();
            if (tooltipBg)
                tooltipBg->Release();
        }

        ctx->PopAxisAlignedClip();

        if (drawScrollbars)
        {
            scrollbar_.Draw(ctx);

            if (hScrollbarVisible_)
            {
                float hLeft = state_.leftEdge + metrics_.gutterWidth;
                float hTop = state_.bottomEdge - 14.0f;
                float hBottom = state_.bottomEdge;
                D2D1_COLOR_F thumbColor = hIsDragging_
                                              ? D2D1::ColorF(0.45f, 0.45f, 0.45f, 0.9f)
                                              : D2D1::ColorF(0.25f, 0.25f, 0.25f, 0.4f);

                ID2D1SolidColorBrush *thumbBrush = nullptr;
                ctx->CreateSolidColorBrush(thumbColor, &thumbBrush);

                float thumbLeft = hLeft + hThumbPos_;
                float thumbRight = thumbLeft + hThumbWidth_;
                D2D1_ROUNDED_RECT thumbRect = D2D1::RoundedRect(
                    D2D1::RectF(thumbLeft + 4.0f, hTop + 2.0f, thumbRight - 4.0f, hBottom - 2.0f),
                    3.0f, 3.0f);

                if (thumbBrush)
                {
                    ctx->FillRoundedRectangle(thumbRect, thumbBrush);
                    thumbBrush->Release();
                }
            }
        }
    }

    void Editor::DrawMarkdownSplitView(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite)
    {
        if (!ctx || !dwrite)
            return;

        const float dividerX = GetGitSplitDividerX();
        const float splitGap = 10.0f;
        const float leftRight = dividerX - splitGap * 0.5f;
        const float rightLeft = dividerX + splitGap * 0.5f;

        ID2D1SolidColorBrush *dividerBrush = nullptr;
        D2D1_COLOR_F dividerColor = UI::Theme::ChromeBorder();
        dividerColor.a = 0.56f;
        ctx->CreateSolidColorBrush(dividerColor, &dividerBrush);

        const float savedLeft = state_.leftEdge;
        const float savedRight = state_.rightEdge;
        const float savedGutterWidth = metrics_.gutterWidth;

        state_.rightEdge = leftRight;
        DrawEditorTextPane(ctx, dwrite, false);

        state_.leftEdge = rightLeft;
        state_.rightEdge = savedRight;
        DrawPreview(ctx, dwrite);

        state_.leftEdge = savedLeft;
        state_.rightEdge = savedRight;
        metrics_.gutterWidth = savedGutterWidth;

        if (dividerBrush)
        {
            const float half = gitSplitDividerDragging_ ? 2.0f : 1.0f;
            D2D1_RECT_F dividerRect = D2D1::RectF(dividerX - half, state_.topEdge, dividerX + half, state_.bottomEdge);
            ctx->FillRectangle(dividerRect, dividerBrush);
            dividerBrush->Release();
        }
    }

    void Editor::DrawFoldMarkers(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite)
    {
        if (!ctx || !dwrite || state_.lines.empty())
            return;

        EnsureFoldLineMaps();

        int visibleCount = GetVisibleLineCount();
        if (visibleCount <= 0)
            return;

        int firstVisible = (int)(state_.scrollOffsetY / metrics_.lineHeight);
        int lastVisible = (int)((state_.scrollOffsetY + (state_.bottomEdge - state_.topEdge)) / metrics_.lineHeight) + 1;
        firstVisible = (std::max)(0, firstVisible);
        lastVisible = (std::min)(visibleCount, lastVisible);

        FLOAT dpiX = 96.0f, dpiY = 96.0f;
        ctx->GetDpi(&dpiX, &dpiY);
        const UINT dpi = (UINT)std::round(dpiX);
        const float arrowSize = (12.0f * dpiX) / 96.0f;
        const float iconRightPad = 3.0f;

        for (int v = firstVisible; v < lastVisible; ++v)
        {
            int line = VisibleLineToActualLine(v);
            if (line != gutterHoverLine_)
                continue;
            int collapsedEnd = -1;
            bool isCollapsed = IsCollapsedFoldStart(line, &collapsedEnd);
            int foldEnd = isCollapsed ? collapsedEnd : FindFoldEndLineForStart(line);

            if (foldEnd <= line)
                continue;

            float lineY = state_.topEdge + (v * metrics_.lineHeight) - state_.scrollOffsetY;
            std::string iconPath = isCollapsed
                ? "assets\\ressource\\icons\\chevron-right.svg"
                : "assets\\ressource\\icons\\chevron-up.svg";
            ID2D1Bitmap *bmp = GetExplorerManager().LoadSvgIconPublic(ctx, iconPath, static_cast<int>(std::round(arrowSize)), dpi);
            if (!bmp)
                continue;

            float centerX = std::round(state_.leftEdge + metrics_.gutterWidth - arrowSize * 0.5f - iconRightPad);
            float centerY = std::round(lineY + metrics_.lineHeight * 0.5f);
            float left = centerX - arrowSize * 0.5f;
            float top = centerY - arrowSize * 0.5f;
            D2D1_RECT_F dst = D2D1::RectF(left, top, left + arrowSize, top + arrowSize);
            ctx->DrawBitmap(bmp, dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        }
    }

    void Editor::DrawActiveLine(ID2D1RenderTarget *ctx)
    {
        if (state_.hasSelection)
            return;

        ID2D1SolidColorBrush *brush = nullptr;
        ctx->CreateSolidColorBrush(theme_.activeLineBackground, &brush);

        float contentLeft = state_.leftEdge + metrics_.gutterWidth + metrics_.leftPadding;
        float lineY = state_.topEdge + (ActualLineToVisibleLine(state_.caret.line) * metrics_.lineHeight) - state_.scrollOffsetY;

        D2D1_RECT_F rect = D2D1::RectF(
            contentLeft - metrics_.leftPadding,
            lineY,
            state_.rightEdge,
            lineY + metrics_.lineHeight);

        ctx->FillRectangle(rect, brush);

        if (brush)
            brush->Release();
    }

    void Editor::DrawGitDiffDecorations(ID2D1RenderTarget *ctx)
    {
        if (!ctx || state_.filePath.empty() || state_.lines.empty())
            return;

        GitDiffDecorations::LineSets lines;
        if (!GitDiffDecorations::GetForFile(state_.filePath, lines))
            return;

        ID2D1SolidColorBrush *addedBrush = nullptr;
        ID2D1SolidColorBrush *deletedBrush = nullptr;
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.24f, 0.58f, 0.30f, 0.22f), &addedBrush);
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.70f, 0.24f, 0.24f, 0.20f), &deletedBrush);

        const float contentLeft = state_.leftEdge + metrics_.gutterWidth;
        const float contentRight = state_.rightEdge - (scrollbar_.IsVisible() ? 14.0f : 0.0f);
        const float lineHeight = metrics_.lineHeight;
        const int lineCount = (int)state_.lines.size();

        auto drawLineBg = [&](int actualLine, ID2D1SolidColorBrush *brush)
        {
            if (!brush || actualLine < 0 || actualLine >= lineCount)
                return;
            int visibleLine = ActualLineToVisibleLine(actualLine);
            float y = state_.topEdge + (visibleLine * lineHeight) - state_.scrollOffsetY;
            if (y + lineHeight < state_.topEdge || y > state_.bottomEdge)
                return;
            D2D1_RECT_F rect = D2D1::RectF(contentLeft, y, contentRight, y + lineHeight);
            ctx->FillRectangle(rect, brush);
        };

        for (int line : lines.deletedLines)
            drawLineBg(line, deletedBrush);
        for (int line : lines.addedLines)
            drawLineBg(line, addedBrush);

        if (addedBrush)
            addedBrush->Release();
        if (deletedBrush)
            deletedBrush->Release();
    }

    void Editor::DrawGitSplitDiff(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite)
    {
        if (!ctx || !dwrite)
            return;

        const float contentLeft = state_.leftEdge;
        const float contentTop = state_.topEdge;
        const float contentRight = GetGitSplitContentRight();
        const float contentBottom = state_.bottomEdge;
        const float splitGap = 10.0f;
        const float dividerX = GetGitSplitDividerX();
        const float paneNumberWidth = 46.0f;

        D2D1_RECT_F leftPane = D2D1::RectF(contentLeft, contentTop, dividerX - splitGap * 0.5f, contentBottom);
        D2D1_RECT_F rightPane = D2D1::RectF(dividerX + splitGap * 0.5f, contentTop, contentRight, contentBottom);

        ID2D1SolidColorBrush *dividerBrush = nullptr;
        ID2D1SolidColorBrush *lineNumBrush = nullptr;
        ID2D1SolidColorBrush *textBrush = nullptr;
        ID2D1SolidColorBrush *addedBrush = nullptr;
        ID2D1SolidColorBrush *deletedBrush = nullptr;

        ctx->CreateSolidColorBrush(D2D1::ColorF(0.18f, 0.18f, 0.18f, 1.0f), &dividerBrush);
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.54f, 0.54f, 0.54f, 1.0f), &lineNumBrush);
        ctx->CreateSolidColorBrush(theme_.text, &textBrush);
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.24f, 0.58f, 0.30f, 0.20f), &addedBrush);
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.70f, 0.24f, 0.24f, 0.18f), &deletedBrush);

        if (dividerBrush)
        {
            float half = gitSplitDividerDragging_ ? 2.0f : 1.0f;
            D2D1_RECT_F dividerRect = D2D1::RectF(dividerX - half, contentTop, dividerX + half, contentBottom);
            ctx->FillRectangle(dividerRect, dividerBrush);
        }

        IDWriteTextFormat *lineNumFormat = nullptr;
        dwrite->CreateTextFormat(
            L"JetBrains Mono",
            customFontCollection_,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            12.0f,
            L"en-us",
            &lineNumFormat);
        if (lineNumFormat)
        {
            lineNumFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            lineNumFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            lineNumFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }

        const int rowCount = (int)gitSplitDiffRows_.size();
        if (rowCount <= 0)
        {
            if (cachedTextFormat_ && textBrush)
            {
                const std::wstring msg = L"No diff available.";
                D2D1_RECT_F msgRect = D2D1::RectF(contentLeft + 12.0f, contentTop + 12.0f, contentRight - 12.0f, contentTop + 40.0f);
                ctx->DrawTextW(msg.c_str(), (UINT32)msg.size(), cachedTextFormat_, msgRect, textBrush);
            }
            if (lineNumFormat)
                lineNumFormat->Release();
            if (deletedBrush)
                deletedBrush->Release();
            if (addedBrush)
                addedBrush->Release();
            if (textBrush)
                textBrush->Release();
            if (lineNumBrush)
                lineNumBrush->Release();
            if (dividerBrush)
                dividerBrush->Release();
            return;
        }

        std::vector<int> leftNumbers((size_t)rowCount, 0);
        std::vector<int> rightNumbers((size_t)rowCount, 0);
        int leftLine = 1;
        int rightLine = 1;
        for (int i = 0; i < rowCount; ++i)
        {
            const auto &row = gitSplitDiffRows_[(size_t)i];
            const bool hasLeft = row.hasLeft;
            const bool hasRight = row.hasRight;
            if (hasLeft)
                leftNumbers[(size_t)i] = leftLine++;
            if (hasRight)
                rightNumbers[(size_t)i] = rightLine++;
        }

        const float lineHeight = metrics_.lineHeight;
        const int firstRow = (std::max)(0, (int)(state_.scrollOffsetY / lineHeight));
        const int lastRow = (std::min)(rowCount, (int)((state_.scrollOffsetY + (contentBottom - contentTop)) / lineHeight) + 1);

        D2D1_RECT_F leftClip = D2D1::RectF(leftPane.left, leftPane.top, leftPane.right, leftPane.bottom);
        D2D1_RECT_F rightClip = D2D1::RectF(rightPane.left, rightPane.top, rightPane.right, rightPane.bottom);

        for (int i = firstRow; i < lastRow; ++i)
        {
            const auto &row = gitSplitDiffRows_[(size_t)i];
            const float y = contentTop + (i * lineHeight) - state_.scrollOffsetY;

            D2D1_RECT_F leftRowRect = D2D1::RectF(leftPane.left, y, leftPane.right, y + lineHeight);
            D2D1_RECT_F rightRowRect = D2D1::RectF(rightPane.left, y, rightPane.right, y + lineHeight);

            if (row.leftDeleted && deletedBrush)
                ctx->FillRectangle(leftRowRect, deletedBrush);
            if (row.rightAdded && addedBrush)
                ctx->FillRectangle(rightRowRect, addedBrush);

            if (lineNumFormat && lineNumBrush)
            {
                if (leftNumbers[(size_t)i] > 0)
                {
                    std::wstring num = std::to_wstring(leftNumbers[(size_t)i]);
                    D2D1_RECT_F numRect = D2D1::RectF(leftPane.left + 4.0f, y, leftPane.left + paneNumberWidth, y + lineHeight);
                    ctx->DrawTextW(num.c_str(), (UINT32)num.size(), lineNumFormat, numRect, lineNumBrush);
                }
                if (rightNumbers[(size_t)i] > 0)
                {
                    std::wstring num = std::to_wstring(rightNumbers[(size_t)i]);
                    D2D1_RECT_F numRect = D2D1::RectF(rightPane.left + 4.0f, y, rightPane.left + paneNumberWidth, y + lineHeight);
                    ctx->DrawTextW(num.c_str(), (UINT32)num.size(), lineNumFormat, numRect, lineNumBrush);
                }
            }

            if (cachedTextFormat_ && textBrush)
            {
                D2D1_RECT_F leftTextRect = D2D1::RectF(leftPane.left + paneNumberWidth + 8.0f, y, leftPane.right - 6.0f, y + lineHeight);
                D2D1_RECT_F rightTextRect = D2D1::RectF(rightPane.left + paneNumberWidth + 8.0f, y, rightPane.right - 6.0f, y + lineHeight);

                ctx->PushAxisAlignedClip(leftClip, D2D1_ANTIALIAS_MODE_ALIASED);
                if (row.hasLeft)
                    ctx->DrawTextW(row.leftText.c_str(), (UINT32)row.leftText.size(), cachedTextFormat_, leftTextRect, textBrush);
                ctx->PopAxisAlignedClip();

                ctx->PushAxisAlignedClip(rightClip, D2D1_ANTIALIAS_MODE_ALIASED);
                if (row.hasRight)
                    ctx->DrawTextW(row.rightText.c_str(), (UINT32)row.rightText.size(), cachedTextFormat_, rightTextRect, textBrush);
                ctx->PopAxisAlignedClip();
            }
        }

        if (lineNumFormat)
            lineNumFormat->Release();
        if (deletedBrush)
            deletedBrush->Release();
        if (addedBrush)
            addedBrush->Release();
        if (textBrush)
            textBrush->Release();
        if (lineNumBrush)
            lineNumBrush->Release();
        if (dividerBrush)
            dividerBrush->Release();
    }

    void Editor::DrawSelection(ID2D1RenderTarget *ctx)
    {
        if (!state_.hasSelection)
            return;
        if (!selection_)
            return;

        float contentLeft = state_.leftEdge + metrics_.gutterWidth + metrics_.leftPadding;

        Orion::CaretPosition start = {state_.selectionStart.line, state_.selectionStart.column};
        Orion::CaretPosition end = {state_.caret.line, state_.caret.column};
        const std::vector<std::wstring> *linesForSelection = &state_.lines;
        std::vector<std::wstring> visibleLines;
        std::vector<int> actualLineBySelectionLine;
        if (!collapsedFolds_.empty())
        {
            EnsureFoldLineMaps();
            visibleLines.reserve(state_.actualLineByVisual.size());
            actualLineBySelectionLine.reserve(state_.actualLineByVisual.size());
            for (int actual : state_.actualLineByVisual)
            {
                if (actual >= 0 && actual < (int)state_.lines.size())
                {
                    visibleLines.push_back(state_.lines[(size_t)actual]);
                    actualLineBySelectionLine.push_back(actual);
                }
            }
            start.line = ActualLineToVisibleLine(start.line);
            end.line = ActualLineToVisibleLine(end.line);
            linesForSelection = &visibleLines;
        }
        else
        {
            actualLineBySelectionLine.resize(state_.lines.size());
            for (int i = 0; i < (int)state_.lines.size(); ++i)
                actualLineBySelectionLine[(size_t)i] = i;
        }

        std::vector<D2D1_ROUNDED_RECT> regions;
        Orion::CaretPosition a = start;
        Orion::CaretPosition b = end;
        if (a.line > b.line || (a.line == b.line && a.column > b.column))
            std::swap(a, b);

        if (!linesForSelection->empty())
        {
            int sLine = (std::max)(0, (std::min)(a.line, (int)linesForSelection->size() - 1));
            int eLine = (std::max)(0, (std::min)(b.line, (int)linesForSelection->size() - 1));
            regions.reserve((size_t)(eLine - sLine + 1));

            for (int line = sLine; line <= eLine; ++line)
            {
                const std::wstring &ln = (*linesForSelection)[(size_t)line];
                const int actualLine = (line >= 0 && line < (int)actualLineBySelectionLine.size())
                                           ? actualLineBySelectionLine[(size_t)line]
                                           : line;
                const int lineLen = (int)ln.size();
                const float y = state_.topEdge + (line * metrics_.lineHeight) - state_.scrollOffsetY;

                auto getXForColumn = [&](int column) -> float
                {
                    float hitX = 0.0f;
                    if (TryGetStyledColumnX(ln, actualLine, column, hitX))
                        return contentLeft - state_.scrollOffsetX + hitX;

                    int visualCol = 0;
                    const int clampedColumn = (std::max)(0, (std::min)(column, lineLen));
                    const int tabSize = GetIndentConfig().tabSize;
                    for (int i = 0; i < clampedColumn; ++i)
                        visualCol = Orion::Geometry::AdvanceVisualCol(visualCol, ln[(size_t)i], tabSize);
                    return contentLeft - state_.scrollOffsetX + (visualCol * metrics_.characterWidth);
                };

                float x1 = contentLeft - state_.scrollOffsetX;
                float x2 = contentLeft - state_.scrollOffsetX;

                if (sLine == eLine)
                {
                    x1 = getXForColumn(a.column);
                    x2 = getXForColumn(b.column);
                }
                else if (line == sLine)
                {
                    x1 = getXForColumn(a.column);
                    x2 = getXForColumn(lineLen);
                }
                else if (line == eLine)
                {
                    x1 = contentLeft - state_.scrollOffsetX;
                    x2 = getXForColumn(b.column);
                }
                else
                {
                    x1 = contentLeft - state_.scrollOffsetX;
                    x2 = getXForColumn(lineLen);
                }

                if (x2 < x1)
                    std::swap(x1, x2);

                x2 += metrics_.characterWidth * 0.3f;

                const float minWidth = metrics_.characterWidth * 0.5f;
                if (x2 - x1 < minWidth)
                    x2 = x1 + minWidth;

                D2D1_ROUNDED_RECT rr{};
                rr.rect = D2D1::RectF(x1, y, x2, y + metrics_.lineHeight);
                rr.radiusX = 3.0f;
                rr.radiusY = 3.0f;
                regions.push_back(rr);
            }
        }

        std::vector<D2D1_ROUNDED_RECT> valid;
        for (const auto &r : regions)
        {
            const D2D1_RECT_F &rc = r.rect;
            if (!std::isfinite(rc.left) || !std::isfinite(rc.top) || !std::isfinite(rc.right) || !std::isfinite(rc.bottom))
                continue;
            if (rc.right <= rc.left || rc.bottom <= rc.top)
                continue;
            valid.push_back(r);
        }

        if (!valid.empty())
            selection_->Draw(ctx, valid);
    }

    void Editor::DrawCaret(ID2D1RenderTarget *ctx)
    {
        if (!state_.caretVisible)
            return;

        ID2D1SolidColorBrush *brush = nullptr;
        ctx->CreateSolidColorBrush(theme_.caret, &brush);

        D2D1_POINT_2F caretPos = TextToScreenPosition(state_.caret);

        const float caretHeight = metrics_.lineHeight * 0.80f;
        const float caretTop = caretPos.y + (metrics_.lineHeight - caretHeight) * 0.5f;

        // Pixel-align for crisp 2px caret
        float x = std::round(caretPos.x);
        float y = std::round(caretTop);
        float h = std::round(caretHeight);
        const float w = 2.0f;

        D2D1_RECT_F rect = D2D1::RectF(x, y, x + w, y + h);
        D2D1_ANTIALIAS_MODE oldAA = ctx->GetAntialiasMode();
        ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        ctx->FillRectangle(rect, brush);

        for (const auto &extraCaret : secondaryCarets_)
        {
            D2D1_POINT_2F p = TextToScreenPosition(extraCaret);
            float ex = std::round(p.x);
            float ey = std::round(p.y + (metrics_.lineHeight - caretHeight) * 0.5f);
            D2D1_RECT_F eRect = D2D1::RectF(ex, ey, ex + w, ey + h);
            ctx->FillRectangle(eRect, brush);
        }

        ctx->SetAntialiasMode(oldAA);

        if (brush)
            brush->Release();
    }

    void Editor::RevealCaretOnNextLayout()
    {
        pendingRevealCaret_ = true;
    }


    // ---- Search UI ----
    void Editor::ShowSearch()
    {
        searchBox_.Show();
        if (state_.hasSelection)
        {
            std::wstring sel = GetSelectionText();
            if (!sel.empty() && sel.find(L'\n') == std::wstring::npos)
                searchBox_.SetSearchText(sel);
        }
    }

    void Editor::HideSearch()
    {
        searchBox_.Hide();
    }

    void Editor::DrawSearchMatches(ID2D1RenderTarget *ctx)
    {
        if (!searchBox_.IsVisible())
            return;

        const std::wstring ext = GetFileExtension();
        const auto &matches = searchBox_.GetMatches();
        if (matches.empty())
            return;

        int currentMatchIdx = searchBox_.GetCurrentMatchIndex();

        ID2D1SolidColorBrush *matchBrush = nullptr;
        ID2D1SolidColorBrush *currentMatchBrush = nullptr;
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.8f, 0.6f, 0.0f, 0.4f), &matchBrush);
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.9f, 0.4f, 0.0f, 0.6f), &currentMatchBrush);

        float contentLeft = state_.leftEdge + metrics_.gutterWidth + metrics_.leftPadding;

        for (size_t i = 0; i < matches.size(); ++i)
        {
            const auto &match = matches[i];

            if (IsLineHiddenByFold(match.line))
                continue;
            float lineY = state_.topEdge + (ActualLineToVisibleLine(match.line) * metrics_.lineHeight) - state_.scrollOffsetY;
            if (lineY + metrics_.lineHeight < state_.topEdge || lineY > state_.bottomEdge)
                continue;

            float startX = contentLeft - state_.scrollOffsetX;
            float endX = contentLeft - state_.scrollOffsetX;

            if (pDWriteFactory_ && match.line >= 0 && match.line < (int)state_.lines.size())
            {
                const std::wstring &line = state_.lines[match.line];
                IDWriteTextFormat *format = cachedTextFormat_;
                IDWriteTextFormat *tmpFmt = nullptr;

                if (!format)
                {
                    pDWriteFactory_->CreateTextFormat(
                        L"JetBrains Mono", customFontCollection_,
                        DWRITE_FONT_WEIGHT_NORMAL,
                        DWRITE_FONT_STYLE_NORMAL,
                        DWRITE_FONT_STRETCH_NORMAL,
                        13.0f, L"en-us",
                        &tmpFmt);
                    if (tmpFmt)
                        format = tmpFmt;
                }

                if (format)
                {
                    const float tabStop = metrics_.characterWidth * (float)GetIndentConfig().tabSize;
                    format->SetIncrementalTabStop(tabStop);
                    IDWriteTextLayout *layout = nullptr;
                    if (SUCCEEDED(pDWriteFactory_->CreateTextLayout(
                            line.c_str(),
                            (UINT32)line.size(),
                            format,
                            10000.0f,
                            metrics_.lineHeight,
                            &layout)) &&
                        layout)
                    {
                        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

                        IDWriteTypography *typography = nullptr;
                        if (SUCCEEDED(pDWriteFactory_->CreateTypography(&typography)) && typography)
                        {
                            DWRITE_FONT_FEATURE features[] = {
                                {DWRITE_MAKE_FONT_FEATURE_TAG('l', 'i', 'g', 'a'), 1},
                                {DWRITE_MAKE_FONT_FEATURE_TAG('c', 'a', 'l', 't'), 1},
                                {DWRITE_MAKE_FONT_FEATURE_TAG('d', 'l', 'i', 'g'), 1},
                            };

                            for (auto &f : features)
                                typography->AddFontFeature(f);

                            DWRITE_TEXT_RANGE fullRange = {0, (UINT32)line.size()};
                            layout->SetTypography(typography, fullRange);
                            typography->Release();
                        }

                        // Match markdown styling so hit-testing aligns with rendered glyphs
                        if (highlighter_ && ext == L".md")
                        {
                            bool mdInCodeBlock = false;
                            std::wstring mdFenceLang;
                            for (int li = 0; li < match.line; ++li)
                                highlighter_->AdvanceMarkdownState(state_.lines[li], mdInCodeBlock, mdFenceLang);

                            auto tokens = highlighter_->TokenizeMarkdownLine(line, mdInCodeBlock, mdFenceLang);
                            for (const auto &t : tokens)
                            {
                                DWRITE_TEXT_RANGE r;
                                r.startPosition = (UINT32)t.start;
                                r.length = (UINT32)t.length;
                                switch (t.type)
                                {
                                case ::Orion::Syntax::TokenType::MarkdownHeading:
                                case ::Orion::Syntax::TokenType::MarkdownStrong:
                                    layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, r);
                                    break;
                                case ::Orion::Syntax::TokenType::MarkdownEmphasis:
                                    layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, r);
                                    break;
                                case ::Orion::Syntax::TokenType::MarkdownLinkText:
                                    layout->SetUnderline(TRUE, r);
                                    break;
                                default:
                                    break;
                                }
                            }
                        }

                        float hitStartX = 0.0f;
                        float hitStartY = 0.0f;
                        float hitEndX = 0.0f;
                        float hitEndY = 0.0f;
                        DWRITE_HIT_TEST_METRICS metricsStart{};
                        DWRITE_HIT_TEST_METRICS metricsEnd{};

                        if (SUCCEEDED(layout->HitTestTextPosition(
                                (UINT32)match.startColumn,
                                FALSE,
                                &hitStartX,
                                &hitStartY,
                                &metricsStart)) &&
                            SUCCEEDED(layout->HitTestTextPosition(
                                (UINT32)match.endColumn,
                                FALSE,
                                &hitEndX,
                                &hitEndY,
                                &metricsEnd)))
                        {
                            float drawX = contentLeft - state_.scrollOffsetX;
                            startX = drawX + hitStartX;
                            endX = drawX + hitEndX;
                            if (endX <= startX)
                                endX = startX + metrics_.characterWidth;
                        }

                        layout->Release();
                    }

                    if (tmpFmt)
                        tmpFmt->Release();
                }
            }

            D2D1_RECT_F matchRect = D2D1::RectF(startX, lineY, endX, lineY + metrics_.lineHeight);

            bool isCurrent = (currentMatchIdx >= 0 && (int)i == currentMatchIdx);
            ID2D1SolidColorBrush *brush = isCurrent ? currentMatchBrush : matchBrush;

            if (brush)
                ctx->FillRectangle(matchRect, brush);

            if (isCurrent)
            {
                ID2D1SolidColorBrush *borderBrush = nullptr;
                ctx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.5f, 0.0f), &borderBrush);
                if (borderBrush)
                {
                    ctx->DrawRectangle(matchRect, borderBrush, 1.5f);
                    borderBrush->Release();
                }
            }
        }

        if (matchBrush)
            matchBrush->Release();
        if (currentMatchBrush)
            currentMatchBrush->Release();
    }
}
