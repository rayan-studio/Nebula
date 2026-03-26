#include "Gutter.h"
#include <algorithm>
#include <cwchar>
#include <cmath>

namespace Orion
{
    Gutter::Gutter()
    {
    }

    void Gutter::DrawGutter(ID2D1RenderTarget *ctx,
                            const EditorState &state,
                            const EditorTheme &theme,
                            const EditorMetrics &metrics)
    {
        if (!ctx)
            return;

        ID2D1SolidColorBrush *bgBrush = nullptr;

        // Utilise le thème, pas une couleur hardcodée
        D2D1_COLOR_F bg = theme.gutterBackground;
        bg.a = 1.0f;

        if (FAILED(ctx->CreateSolidColorBrush(bg, &bgBrush)) || !bgBrush)
            return;

        // Utilise le layout réel
        D2D1_RECT_F gutterRect = D2D1::RectF(
            state.leftEdge,
            state.topEdge,
            state.leftEdge + metrics.gutterWidth,
            state.bottomEdge);

        ctx->FillRectangle(gutterRect, bgBrush);

        if (!state.hasSelection)
        {
            int caretVisualLine = state.caret.line;
            if (!state.visualLineByActual.empty())
            {
                int clamped = (std::max)(0, (std::min)(state.caret.line, (int)state.visualLineByActual.size() - 1));
                caretVisualLine = state.visualLineByActual[(size_t)clamped];
            }

            const float lineY = state.topEdge + (caretVisualLine * metrics.lineHeight) - state.scrollOffsetY;
            if (lineY + metrics.lineHeight >= state.topEdge && lineY <= state.bottomEdge)
            {
                ID2D1SolidColorBrush *activeLineBrush = nullptr;
                if (SUCCEEDED(ctx->CreateSolidColorBrush(theme.activeLineBackground, &activeLineBrush)) && activeLineBrush)
                {
                    D2D1_RECT_F activeRect = D2D1::RectF(
                        state.leftEdge,
                        lineY,
                        state.leftEdge + metrics.gutterWidth,
                        lineY + metrics.lineHeight);
                    ctx->FillRectangle(activeRect, activeLineBrush);
                    activeLineBrush->Release();
                }
            }
        }

        ID2D1SolidColorBrush *borderBrush = nullptr;
        D2D1_COLOR_F border = theme.lineNumberText;
        border.a = 0.18f;
        if (SUCCEEDED(ctx->CreateSolidColorBrush(border, &borderBrush)) && borderBrush)
        {
            const float x = std::round(state.leftEdge + metrics.gutterWidth) - 0.5f;
            ctx->DrawLine(
                D2D1::Point2F(x, state.topEdge),
                D2D1::Point2F(x, state.bottomEdge),
                borderBrush,
                1.0f);
            borderBrush->Release();
        }

        bgBrush->Release();
    }

    void Gutter::DrawLineNumbers(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite,
                                 const EditorState &state, const EditorTheme &theme,
                                 const EditorMetrics &metrics, IDWriteFontCollection *customFontCollection)
    {
        const wchar_t *editorFont = L"JetBrains Mono";
        const float editorFontSize = 13.0f;
        IDWriteTextFormat *format = nullptr;
        dwrite->CreateTextFormat(editorFont, customFontCollection,
                                 DWRITE_FONT_WEIGHT_NORMAL,
                                 DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_FONT_STRETCH_NORMAL,
                                 editorFontSize, L"en-us",
                                 &format);
        if (format)
        {
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        ID2D1SolidColorBrush *textBrush = nullptr;
        ID2D1SolidColorBrush *activeTextBrush = nullptr;
        ctx->CreateSolidColorBrush(theme.lineNumberText, &textBrush);
        ctx->CreateSolidColorBrush(theme.activeLineNumberText, &activeTextBrush);

        int visibleCount = state.actualLineByVisual.empty()
            ? (int)state.lines.size()
            : (int)state.actualLineByVisual.size();
        int firstVisibleLine = (int)(state.scrollOffsetY / metrics.lineHeight);
        int lastVisibleLine = (int)((state.scrollOffsetY + (state.bottomEdge - state.topEdge)) / metrics.lineHeight) + 1;

        firstVisibleLine = (std::max)(0, firstVisibleLine);
        lastVisibleLine = (std::min)(visibleCount, lastVisibleLine);

        const float leftPadding = 8.0f;

        for (int i = firstVisibleLine; i < lastVisibleLine; ++i)
        {
            int actualLine = state.actualLineByVisual.empty() ? i : state.actualLineByVisual[(size_t)i];
            float lineY = state.topEdge + (i * metrics.lineHeight) - state.scrollOffsetY;

            wchar_t lineNum[16];
            swprintf_s(lineNum, 16, L"%d", actualLine + 1);

            D2D1_RECT_F rect = D2D1::RectF(
                state.leftEdge + leftPadding,
                lineY,
                state.leftEdge + metrics.gutterWidth,
                lineY + metrics.lineHeight);

            ID2D1SolidColorBrush *brushToUse = (actualLine == state.caret.line) ? activeTextBrush : textBrush;
            ctx->DrawTextW(
                lineNum,
                (UINT32)wcslen(lineNum),
                format,
                rect,
                brushToUse,
                D2D1_DRAW_TEXT_OPTIONS_NONE,
                DWRITE_MEASURING_MODE_NATURAL);
        }

        if (format)
            format->Release();
        if (textBrush)
            textBrush->Release();
        if (activeTextBrush)
            activeTextBrush->Release();
    }

    float Gutter::CalculateGutterWidth(const EditorState &state, const EditorMetrics &metrics) const
    {
        int lines = (std::max)(1, (int)state.lines.size());
        int digits = 1;
        int tmp = lines;
        while (tmp >= 10)
        {
            tmp /= 10;
            ++digits;
        }

        float padding = 16.0f; // left/right padding inside gutter
        float computedWidth = digits * metrics.characterWidth + padding;
        // Ensure we never go below the configured gutter width
        return (std::max)(metrics.gutterWidth, computedWidth);
    }
}
