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
        bgBrush->Release();
    }

    void Gutter::DrawLineNumbers(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite,
                                 const EditorState &state, const EditorTheme &theme,
                                 const EditorMetrics &metrics, IDWriteFontCollection *customFontCollection)
    {
        const wchar_t *editorFont = L"JetBrains Mono";
        const float editorFontSize = 14.0f;
        IDWriteTextFormat *format = nullptr;
        dwrite->CreateTextFormat(editorFont, customFontCollection,
                                 DWRITE_FONT_WEIGHT_NORMAL,
                                 DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_FONT_STRETCH_NORMAL,
                                 editorFontSize, L"en-us",
                                 &format);
        if (format)
        {
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        ID2D1SolidColorBrush *textBrush = nullptr;
        ID2D1SolidColorBrush *activeTextBrush = nullptr;
        ctx->CreateSolidColorBrush(theme.lineNumberText, &textBrush);
        ctx->CreateSolidColorBrush(theme.text, &activeTextBrush);

        int firstVisibleLine = (int)(state.scrollOffsetY / metrics.lineHeight);
        int lastVisibleLine = (int)((state.scrollOffsetY + (state.bottomEdge - state.topEdge)) / metrics.lineHeight) + 1;

        firstVisibleLine = (std::max)(0, firstVisibleLine);
        lastVisibleLine = (std::min)((int)state.lines.size(), lastVisibleLine);

        for (int i = firstVisibleLine; i < lastVisibleLine; ++i)
        {
            float lineY = state.topEdge + (i * metrics.lineHeight) - state.scrollOffsetY;

            wchar_t lineNum[16];
            swprintf_s(lineNum, 16, L"%d", i + 1);

            D2D1_RECT_F rect = D2D1::RectF(
                state.leftEdge,
                lineY,
                state.leftEdge + metrics.gutterWidth,
                lineY + metrics.lineHeight);

            ID2D1SolidColorBrush *brushToUse = (i == state.caret.line) ? activeTextBrush : textBrush;
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
