#include "Selection.h"
#include <algorithm>
#include <dwrite.h>

// Windows defines macros `min`/`max` which break usages of `std::min`/`std::max`.
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

namespace Orion
{
    namespace Rendering
    {

        Selection::Selection(const SelectionConfig &cfg) : cfg_(cfg) {}

        Selection::~Selection() {}

        void Selection::Draw(ID2D1RenderTarget *ctx, const std::vector<D2D1_ROUNDED_RECT> &regions)
        {
            if (!ctx)
                return;

            ID2D1SolidColorBrush *brush = nullptr;
            ctx->CreateSolidColorBrush(cfg_.color, &brush);
            if (!brush)
                return;

            for (const auto &r : regions)
            {
                if (cfg_.style == SelectionStyle::Rectangle)
                {
                    ctx->FillRectangle(r.rect, brush);
                }
                else
                {
                    ctx->FillRoundedRectangle(&r, brush);
                }
            }

            brush->Release();
        }

        // Helper function to get X position using DirectWrite
        static float GetXPositionForColumn(
            const std::wstring &line,
            int column,
            float contentLeft,
            float scrollOffsetX,
            int tabSize,
            float charWidth,
            IDWriteFactory *dwriteFactory,
            IDWriteTextFormat *textFormat)
        {
            if (line.empty())
                return contentLeft - scrollOffsetX;

            // ✅ TOUJOURS utiliser DirectWrite, supprimer le check whitespace
            if (!dwriteFactory || !textFormat)
                return contentLeft - scrollOffsetX;

            IDWriteTextLayout *layout = nullptr;
            HRESULT hr = dwriteFactory->CreateTextLayout(
                line.c_str(),
                (UINT32)line.size(),
                textFormat,
                10000.0f,
                100.0f,
                &layout);

            if (FAILED(hr) || !layout)
                return contentLeft - scrollOffsetX;

            float xPos = 0.0f;
            UINT32 textPos = std::min((UINT32)column, (UINT32)line.size());

            FLOAT caretX = 0.0f;
            FLOAT caretY = 0.0f;
            DWRITE_HIT_TEST_METRICS hitMetrics = {};

            bool success = true;
            __try
            {
                layout->HitTestTextPosition(textPos, FALSE, &caretX, &caretY, &hitMetrics);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                success = false;
            }

            if (success)
            {
                // Apply overhang correction
                DWRITE_OVERHANG_METRICS om = {};
                if (SUCCEEDED(layout->GetOverhangMetrics(&om)))
                {
                    caretX -= om.left;
                }
                xPos = caretX;
            }

            layout->Release();
            return contentLeft + xPos - scrollOffsetX;
        }

        std::vector<D2D1_ROUNDED_RECT> Selection::CalculateRegions(
            CaretPosition start,
            CaretPosition end,
            const std::vector<std::wstring> &lines,
            float contentLeft,
            float topEdge,
            float scrollOffsetX,
            float scrollOffsetY,
            float lineHeight,
            int tabSize,
            float characterWidth,
            float cornerRadius,
            IDWriteFactory *dwriteFactory,
            IDWriteTextFormat *textFormat)
        {
            std::vector<D2D1_ROUNDED_RECT> out;

            // Normalize order
            auto earlier = start;
            auto later = end;
            if (earlier.line > later.line || (earlier.line == later.line && earlier.column > later.column))
            {
                std::swap(earlier, later);
            }

            int sLine = std::max(0, earlier.line);
            int eLine = std::max(0, later.line);

            for (int line = sLine; line <= eLine; ++line)
            {
                float y = topEdge + (line * lineHeight) - scrollOffsetY;

                const std::wstring empty;
                const std::wstring &ln = (line >= 0 && line < (int)lines.size()) ? lines[line] : empty;
                int lineLen = (int)ln.size();

                float x1 = contentLeft - scrollOffsetX;
                float x2 = contentLeft - scrollOffsetX;

                if (sLine == eLine)
                {
                    // Single line selection
                    int sc = std::max(0, std::min((int)earlier.column, lineLen));
                    int ec = std::max(0, std::min((int)later.column, lineLen));

                    x1 = GetXPositionForColumn(ln, sc, contentLeft, scrollOffsetX, tabSize, characterWidth, dwriteFactory, textFormat);
                    x2 = GetXPositionForColumn(ln, ec, contentLeft, scrollOffsetX, tabSize, characterWidth, dwriteFactory, textFormat);
                }
                else if (line == sLine)
                {
                    // First line of multi-line selection
                    int sc = std::max(0, std::min((int)earlier.column, lineLen));

                    x1 = GetXPositionForColumn(ln, sc, contentLeft, scrollOffsetX, tabSize, characterWidth, dwriteFactory, textFormat);
                    x2 = GetXPositionForColumn(ln, lineLen, contentLeft, scrollOffsetX, tabSize, characterWidth, dwriteFactory, textFormat);
                }
                else if (line == eLine)
                {
                    // Last line of multi-line selection
                    int ec = std::max(0, std::min((int)later.column, lineLen));

                    x1 = contentLeft - scrollOffsetX;
                    x2 = GetXPositionForColumn(ln, ec, contentLeft, scrollOffsetX, tabSize, characterWidth, dwriteFactory, textFormat);
                }
                else
                {
                    // Middle lines - full line selection
                    x1 = contentLeft - scrollOffsetX;
                    x2 = GetXPositionForColumn(ln, lineLen, contentLeft, scrollOffsetX, tabSize, characterWidth, dwriteFactory, textFormat);
                }

                if (x2 < x1)
                    std::swap(x1, x2);

                // Avoid zero-width rects
                if (x2 - x1 < 1.0f)
                    x2 = x1 + 1.0f;

                D2D1_ROUNDED_RECT rr;
                rr.rect = D2D1_RECT_F{x1, y, x2, y + lineHeight};
                rr.radiusX = cornerRadius;
                rr.radiusY = cornerRadius;
                out.push_back(rr);
            }

            return out;
        }

    } // namespace Rendering
} // namespace Orion