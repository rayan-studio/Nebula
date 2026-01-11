#include "GuideRenderer.h"
#include <dwrite.h>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <map>
#include <set>
#include "utils/logger/Logger.h"

// Ensure Windows min/max macros don't interfere with std::min/std::max
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

        int tabSize = indentHelper_.GetConfig().tabSize;

        // ✅ ÉTAPE 1 : Calculer la largeur moyenne d'un caractère
        float charWidth = 0.0f;
        {
            // Mesurer avec une string de test d'espaces (plus fiable que "MMMM")
            std::wstring testStr = L"          "; // 10 espaces
            IDWriteTextLayout *layout = nullptr;
            if (SUCCEEDED(renderCtx.dwriteFactory->CreateTextLayout(
                    testStr.c_str(),
                    (UINT32)testStr.size(),
                    renderCtx.textFormat,
                    10000.0f,
                    renderCtx.lineHeight,
                    &layout)) &&
                layout)
            {
                DWRITE_TEXT_METRICS metrics = {};
                if (SUCCEEDED(layout->GetMetrics(&metrics)))
                {
                    charWidth = metrics.width / 10.0f; // Moyenne sur 10 espaces
                }

                // ✅ APPLIQUER la correction de l'overhang
                DWRITE_OVERHANG_METRICS om = {};
                if (SUCCEEDED(layout->GetOverhangMetrics(&om)))
                {
                    charWidth -= om.left / 10.0f;
                }

                layout->Release();
            }
        }

        if (charWidth <= 0.0f)
        {
            charWidth = 8.0f; // Fallback
        }

        // ✅ ÉTAPE 2 : Trouver tous les niveaux d'indentation présents
        std::set<int> indentLevels;

        for (int li = renderCtx.firstVisibleLine; li < renderCtx.lastVisibleLine && li < (int)lines.size(); ++li)
        {
            const std::wstring &line = lines[li];
            auto lineIndent = indentHelper_.GetLineIndent(line);

            if (lineIndent.level > 0 && !lineIndent.isWhitespaceOnly)
            {
                for (int lvl = tabSize; lvl <= lineIndent.level; lvl += tabSize)
                {
                    indentLevels.insert(lvl);
                }
            }
        }

        if (indentLevels.empty())
            return;

        // ✅ ÉTAPE 3 : Calculer les positions des guides (position mathématique)
        std::map<int, float> guidePositions;

        for (int targetLevel : indentLevels)
        {
            // Position = nombre d'espaces × largeur d'un caractère
            guidePositions[targetLevel] = targetLevel * charWidth;
        }

        // ✅ ÉTAPE 4 : Calculer caretX pour le highlight
        float caretX = -10000.0f;
        if (renderCtx.caretLine >= renderCtx.firstVisibleLine &&
            renderCtx.caretLine < renderCtx.lastVisibleLine &&
            renderCtx.caretLine < (int)lines.size())
        {
            const std::wstring &caretLine = lines[renderCtx.caretLine];
            auto caretIndent = indentHelper_.GetLineIndent(caretLine);

            if (caretIndent.level > 0 && !caretIndent.isWhitespaceOnly)
            {
                // Arrondir au niveau d'indentation le plus proche
                int caretLevel = (caretIndent.level / tabSize) * tabSize;
                if (caretLevel > 0 && guidePositions.count(caretLevel) > 0)
                {
                    caretX = renderCtx.contentLeft + guidePositions[caretLevel] - renderCtx.scrollOffsetX;
                }
            }
        }

        // ✅ ÉTAPE 5 : Dessiner les guides
        for (const auto &[levelSpaces, posX] : guidePositions)
        {
            float guideX = renderCtx.contentLeft + posX - renderCtx.scrollOffsetX;
            bool isActive = std::fabs(guideX - caretX) < 5.0f;

            bool inRange = false;
            int rangeStart = -1;

            for (int li = renderCtx.firstVisibleLine; li < renderCtx.lastVisibleLine; ++li)
            {
                if (li >= (int)lines.size())
                    break;

                auto lineIndent = indentHelper_.GetLineIndent(lines[li]);
                bool shouldDraw = false;

                if (lineIndent.isWhitespaceOnly)
                {
                    shouldDraw = inRange;
                }
                else
                {
                    shouldDraw = (lineIndent.level >= levelSpaces);
                }

                if (shouldDraw)
                {
                    if (!inRange)
                    {
                        inRange = true;
                        rangeStart = li;
                    }
                }
                else if (inRange)
                {
                    float topY = renderCtx.topEdge + (rangeStart * renderCtx.lineHeight) -
                                 renderCtx.scrollOffsetY + (renderCtx.lineHeight * style_.topMargin);
                    float bottomY = renderCtx.topEdge + ((li - 1) * renderCtx.lineHeight) -
                                    renderCtx.scrollOffsetY + renderCtx.lineHeight -
                                    (renderCtx.lineHeight * style_.bottomMargin);

                    DrawGuideSegment(ctx, guideX, topY, bottomY, isActive);
                    inRange = false;
                    rangeStart = -1;
                }
            }

            if (inRange && rangeStart >= 0)
            {
                int lastLine = std::min(renderCtx.lastVisibleLine - 1, (int)lines.size() - 1);
                float topY = renderCtx.topEdge + (rangeStart * renderCtx.lineHeight) -
                             renderCtx.scrollOffsetY + (renderCtx.lineHeight * style_.topMargin);
                float bottomY = renderCtx.topEdge + (lastLine * renderCtx.lineHeight) -
                                renderCtx.scrollOffsetY + renderCtx.lineHeight -
                                (renderCtx.lineHeight * style_.bottomMargin);

                DrawGuideSegment(ctx, guideX, topY, bottomY, isActive);
            }
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