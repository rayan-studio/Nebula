#include "orion/editor/Editor.h"

#include <algorithm>
#include <cmath>

#include "orion/geometry/TextColumns.h"

// Ensure Windows min/max macros don't interfere with std::min/std::max
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

namespace Orion
{
    bool Editor::CreateStyledTextLayout(const std::wstring &line, int actualLine, IDWriteTextLayout **outLayout)
    {
        if (outLayout)
            *outLayout = nullptr;
        if (!outLayout || !pDWriteFactory_ || !cachedTextFormat_)
            return false;

        const float tabStop = metrics_.characterWidth * (float)GetIndentConfig().tabSize;
        cachedTextFormat_->SetIncrementalTabStop(tabStop);

        IDWriteTextLayout *layout = nullptr;
        HRESULT hr = pDWriteFactory_->CreateTextLayout(
            line.c_str(),
            (UINT32)line.size(),
            cachedTextFormat_,
            10000.0f,
            metrics_.lineHeight,
            &layout);
        if (FAILED(hr) || !layout)
            return false;

        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

        if (!cachedTypography_ && pDWriteFactory_)
        {
            if (SUCCEEDED(pDWriteFactory_->CreateTypography(&cachedTypography_)) && cachedTypography_)
            {
                DWRITE_FONT_FEATURE features[] = {
                    {DWRITE_MAKE_FONT_FEATURE_TAG('l', 'i', 'g', 'a'), 1},
                    {DWRITE_MAKE_FONT_FEATURE_TAG('c', 'a', 'l', 't'), 1},
                    {DWRITE_MAKE_FONT_FEATURE_TAG('d', 'l', 'i', 'g'), 1},
                };
                for (auto &f : features)
                    cachedTypography_->AddFontFeature(f);
            }
        }

        if (cachedTypography_ && !line.empty())
        {
            DWRITE_TEXT_RANGE fullRange = {0, (UINT32)line.size()};
            layout->SetTypography(cachedTypography_, fullRange);
        }

        if (highlighter_ && actualLine >= 0 && actualLine < (int)state_.lines.size() && GetFileExtension() == L".md")
        {
            bool mdInCodeBlock = false;
            std::wstring mdFenceLang;
            for (int li = 0; li < actualLine; ++li)
                highlighter_->AdvanceMarkdownState(state_.lines[(size_t)li], mdInCodeBlock, mdFenceLang);

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

        *outLayout = layout;
        return true;
    }

    bool Editor::TryGetStyledColumnX(const std::wstring &line, int actualLine, int column, float &outX)
    {
        outX = 0.0f;

        IDWriteTextLayout *layout = nullptr;
        if (!CreateStyledTextLayout(line, actualLine, &layout))
            return false;

        const int clampedColumn = (std::max)(0, (std::min)(column, (int)line.size()));
        DWRITE_HIT_TEST_METRICS metrics{};
        FLOAT hitX = 0.0f;
        FLOAT hitY = 0.0f;
        const HRESULT hr = layout->HitTestTextPosition((UINT32)clampedColumn, FALSE, &hitX, &hitY, &metrics);
        layout->Release();
        if (FAILED(hr))
            return false;

        outX = hitX;
        return true;
    }

    bool Editor::TryGetStyledColumnFromX(const std::wstring &line, int actualLine, float localX, int &outColumn)
    {
        outColumn = 0;

        IDWriteTextLayout *layout = nullptr;
        if (!CreateStyledTextLayout(line, actualLine, &layout))
            return false;

        BOOL isTrailing = FALSE;
        BOOL isInside = FALSE;
        DWRITE_HIT_TEST_METRICS metrics{};
        const HRESULT hr = layout->HitTestPoint(localX, metrics_.lineHeight * 0.5f, &isTrailing, &isInside, &metrics);
        layout->Release();
        if (FAILED(hr))
            return false;

        int column = (int)metrics.textPosition + (isTrailing ? 1 : 0);
        outColumn = (std::max)(0, (std::min)(column, (int)line.size()));
        return true;
    }

    D2D1_POINT_2F Editor::TextToScreenPosition(CaretPosition pos)
    {
        EnsureFoldLineMaps();
        const float contentLeft = state_.leftEdge + metrics_.gutterWidth + metrics_.leftPadding;
        const float baseX = contentLeft - state_.scrollOffsetX;

        if (state_.lines.empty())
            return D2D1::Point2F(baseX, state_.topEdge - state_.scrollOffsetY);

        int line = pos.line;
        if (line < 0) line = 0;
        if (line >= (int)state_.lines.size()) line = (int)state_.lines.size() - 1;

        int visibleLine = ActualLineToVisibleLine(line);
        const float y = state_.topEdge + (visibleLine * metrics_.lineHeight) - state_.scrollOffsetY;

        const std::wstring &ln = state_.lines[line];
        int col = pos.column;
        if (col < 0) col = 0;
        if (col > (int)ln.size()) col = (int)ln.size();

        float styledX = 0.0f;
        float x = baseX;
        if (TryGetStyledColumnX(ln, line, col, styledX))
        {
            x += styledX;
        }
        else
        {
            int visualCol = 0;
            const int tabSize = GetIndentConfig().tabSize;
            for (int i = 0; i < col; ++i)
                visualCol = Orion::Geometry::AdvanceVisualCol(visualCol, ln[i], tabSize);

            x += visualCol * metrics_.characterWidth;
        }
        return D2D1::Point2F(x, y);
    }

    CaretPosition Editor::ScreenToTextPosition(POINT screenPoint)
    {
        EnsureFoldLineMaps();
        CaretPosition out{0, 0};
        if (state_.lines.empty())
            return out;

        const float contentLeft = state_.leftEdge + metrics_.gutterWidth + metrics_.leftPadding;
        const float baseX = contentLeft - state_.scrollOffsetX;

        const float adjustedY = (float)screenPoint.y + state_.scrollOffsetY;
        int visibleLine = (int)((adjustedY - state_.topEdge) / metrics_.lineHeight);
        int line = VisibleLineToActualLine(visibleLine);

        const std::wstring &ln = state_.lines[line];

        float localX = (float)screenPoint.x - baseX;
        if (localX < 0.0f) localX = 0.0f;

        int col = 0;
        if (!TryGetStyledColumnFromX(ln, line, localX, col))
        {
            float targetVisual = localX / metrics_.characterWidth;
            if (targetVisual < 0.0f) targetVisual = 0.0f;

            const int tabSize = GetIndentConfig().tabSize;

            int visual = 0;
            for (int i = 0; i < (int)ln.size(); ++i)
            {
                int nextVisual = Orion::Geometry::AdvanceVisualCol(visual, ln[i], tabSize);

                float mid = 0.5f * ((float)visual + (float)nextVisual);
                if (targetVisual < mid)
                {
                    col = i;
                    break;
                }

                visual = nextVisual;
                col = i + 1;
            }
        }

        if (col < 0) col = 0;
        if (col > (int)ln.size()) col = (int)ln.size();

        out.line = line;
        out.column = col;
        return out;
    }
}
