#include "GuideRenderer.h"
#include <dwrite.h>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <map>
#include <set>
#include "utils/logger/Logger.h"
#include "../geometry/TextColumns.h"

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

namespace Orion::Rendering
{
    GuideRenderer::GuideRenderer(const Geometry::IndentConfig &indentConfig, const GuideStyle &style)
        : indentHelper_(indentConfig), style_(style), tabSize_(indentConfig.tabSize)
    {
    }
    void GuideRenderer::DrawCppGuides(
        ID2D1RenderTarget *ctx,
        const std::vector<std::wstring> &lines,
        const GuideRenderContext &renderCtx)
    {
        if (!ctx || lines.empty() || !renderCtx.textFormat || !renderCtx.dwriteFactory)
            return;

        // Compute approximate space width using text layout for accurate X positions
        float spaceWidth = 8.0f;
        {
            std::wstring oneSpace = L" ";
            IDWriteTextLayout *layout = nullptr;
            if (SUCCEEDED(renderCtx.dwriteFactory->CreateTextLayout(
                    oneSpace.c_str(), 1, renderCtx.textFormat, 10000.0f, renderCtx.lineHeight, &layout)) &&
                layout)
            {
                DWRITE_TEXT_METRICS metrics = {};
                if (SUCCEEDED(layout->GetMetrics(&metrics)))
                    spaceWidth = metrics.width;

                DWRITE_OVERHANG_METRICS om = {};
                if (SUCCEEDED(layout->GetOverhangMetrics(&om)))
                    spaceWidth -= om.left;

                layout->Release();
            }
        }

        // Helper: compute visual column (tabs expanded) from a character index
        auto VisualColFromCharIndex = [&](const std::wstring &s, size_t charIndex) -> int
        {
            int vc = 0;
            size_t n = (std::min)(charIndex, s.size());
            for (size_t i = 0; i < n; ++i)
                vc = Orion::Geometry::AdvanceVisualCol(vc, s[i], tabSize_);
            return vc;
        };

        // For each visible line, if it contains "static" or "if" draw a simple guide
        for (int li = renderCtx.firstVisibleLine; li < renderCtx.lastVisibleLine && li < (int)lines.size(); ++li)
        {
            const std::wstring &ln = lines[li];

            // crude token search: look for standalone "static" or "if"
            bool match = false;
            auto findWord = [&](const std::wstring &word) -> bool
            {
                size_t p = ln.find(word);
                if (p == std::wstring::npos)
                    return false;
                // ensure preceding/next chars are non-identifier
                if (p > 0 && (iswalnum(ln[p - 1]) || ln[p - 1] == L'_'))
                    return false;
                size_t n = p + word.size();
                if (n < ln.size() && (iswalnum(ln[n]) || ln[n] == L'_'))
                    return false;
                return true;
            };

            if (findWord(L"static") || findWord(L"if"))
                match = true;

            if (!match)
                continue;

            // Find opening brace '{' starting from this line; if not found search forward
            int braceLine = -1;
            size_t bracePos = std::wstring::npos;
            for (int s = li; s < (int)lines.size(); ++s)
            {
                size_t p = lines[s].find(L'{');
                if (p != std::wstring::npos)
                {
                    braceLine = s;
                    bracePos = p;
                    break;
                }
            }

            if (braceLine == -1)
                continue; // no block found

            // ✅ Correct X position: based on the visual column of '{' (tabs expanded)
            int braceVisualCol = VisualColFromCharIndex(lines[braceLine], bracePos);
            float guideX = renderCtx.contentLeft + (braceVisualCol * spaceWidth) - renderCtx.scrollOffsetX;

            // From braceLine, find matching closing brace '}'
            int depth = 0;
            int endLine = -1;
            for (int s = braceLine; s < (int)lines.size(); ++s)
            {
                const std::wstring &L = lines[s];
                for (size_t k = 0; k < L.size(); ++k)
                {
                    if (L[k] == L'{')
                        depth++;
                    else if (L[k] == L'}')
                    {
                        depth--;
                        if (depth == 0)
                        {
                            endLine = s;
                            break;
                        }
                    }
                }
                if (endLine != -1)
                    break;
            }

            if (endLine == -1)
                endLine = (std::min)(renderCtx.lastVisibleLine - 1, (int)lines.size() - 1);

            float topY = renderCtx.topEdge + (li * renderCtx.lineHeight) - renderCtx.scrollOffsetY +
                         (renderCtx.lineHeight * style_.topMargin);

            float bottomY = renderCtx.topEdge + (endLine * renderCtx.lineHeight) - renderCtx.scrollOffsetY +
                            renderCtx.lineHeight - (renderCtx.lineHeight * style_.bottomMargin);

            DrawGuideSegment(ctx, guideX, topY, bottomY, false);
        }
    }

    void GuideRenderer::DrawGuideSegment(
        ID2D1RenderTarget *ctx,
        float x,
        float topY,
        float bottomY,
        bool isActive)
    {
        if (!ctx)
            return;

        D2D1_COLOR_F color = isActive ? style_.activeColor : style_.normalColor;

        ID2D1SolidColorBrush *brush = nullptr;
        if (SUCCEEDED(ctx->CreateSolidColorBrush(color, &brush)) && brush)
        {
            D2D1_RECT_F rect = D2D1::RectF(x, topY, x + style_.lineWidth, bottomY);
            ctx->FillRectangle(rect, brush);
            brush->Release();
        }
    }

} // namespace Orion::Rendering