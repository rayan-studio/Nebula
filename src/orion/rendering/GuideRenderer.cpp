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

        // Calculer la position X du caret pour le highlight
        float caretX = -10000.0f;
        if (renderCtx.caretLine >= renderCtx.firstVisibleLine &&
            renderCtx.caretLine < renderCtx.lastVisibleLine &&
            renderCtx.caretLine < (int)lines.size())
        {
            const std::wstring &caretLine = lines[renderCtx.caretLine];
            auto caretIndent = indentHelper_.GetLineIndent(caretLine);

            if (caretIndent.level > 0 && !caretIndent.isWhitespaceOnly)
            {
                // Trouver la colonne du premier caractère non-blanc
                int codeColumn = 0;
                for (size_t i = 0; i < caretLine.size(); ++i)
                {
                    if (caretLine[i] != L' ' && caretLine[i] != L'\t')
                    {
                        codeColumn = (int)i;
                        break;
                    }
                }

                IDWriteTextLayout *layout = nullptr;
                if (SUCCEEDED(renderCtx.dwriteFactory->CreateTextLayout(
                        caretLine.c_str(),
                        (UINT32)caretLine.size(),
                        renderCtx.textFormat,
                        10000.0f,
                        renderCtx.lineHeight,
                        &layout)) &&
                    layout)
                {
                    // Calculer la position de base
                    float baseX = 0.0f;
                    DWRITE_OVERHANG_METRICS om = {};
                    if (SUCCEEDED(layout->GetOverhangMetrics(&om)))
                    {
                        baseX -= om.left;
                    }

                    FLOAT x = 0, y = 0;
                    DWRITE_HIT_TEST_METRICS htm = {};
                    layout->HitTestTextPosition(codeColumn, FALSE, &x, &y, &htm);

                    caretX = renderCtx.contentLeft + baseX + x - renderCtx.scrollOffsetX;
                    layout->Release();
                }
            }
        }

        // ✅ ÉTAPE 1 : Trouver tous les niveaux d'indentation présents
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

        // ✅ ÉTAPE 2 : Pour chaque niveau, mesurer sur une VRAIE ligne de code
        std::map<int, float> guidePositions;

        for (int targetLevel : indentLevels)
        {
            std::vector<float> measurements;

            // Chercher des lignes qui ont au moins ce niveau d'indentation
            for (int li = renderCtx.firstVisibleLine;
                 li < renderCtx.lastVisibleLine && li < (int)lines.size(); ++li)
            {
                const std::wstring &line = lines[li];
                auto lineIndent = indentHelper_.GetLineIndent(line);

                if (lineIndent.isWhitespaceOnly || lineIndent.level < targetLevel)
                    continue;

                // ✅ Parcourir l'indentation de cette ligne pour trouver où on atteint targetLevel
                int currentLevel = 0;
                int targetColumn = -1;

                // ✅ Au lieu de chercher targetColumn, chercher où finit l'indentation
                for (size_t i = 0; i < line.size(); ++i)
                {
                    if (line[i] == L' ')
                    {
                        currentLevel++;
                    }
                    else if (line[i] == L'\t')
                    {
                        int nextStop = ((currentLevel / tabSize) + 1) * tabSize;
                        currentLevel = nextStop;
                    }
                    else
                    {
                        // ✅ On vient d'atteindre le premier caractère de code
                        // Si le niveau précédent est >= targetLevel, mesurer ICI
                        if (currentLevel >= targetLevel)
                        {
                            targetColumn = (int)i;
                            break;
                        }
                    }
                }

                // ✅ Si on a trouvé où commence le code ET qu'on est au bon niveau
                if (targetColumn >= 0 && currentLevel >= targetLevel)
                {
                    // Créer une string qui contient SEULEMENT l'indentation
                    std::wstring indentOnly = line.substr(0, targetColumn);

                    // Maintenant, trouver quelle est la position du caractère qui correspond
                    // EXACTEMENT à targetLevel espaces
                    int measuredLevel = 0;
                    int measureColumn = 0;

                    for (size_t i = 0; i < indentOnly.size(); ++i)
                    {
                        int prevLevel = measuredLevel;

                        if (indentOnly[i] == L' ')
                        {
                            measuredLevel++;
                        }
                        else if (indentOnly[i] == L'\t')
                        {
                            int nextStop = ((measuredLevel / tabSize) + 1) * tabSize;
                            measuredLevel = nextStop;
                        }

                        // Dès qu'on atteint targetLevel, c'est ICI
                        if (measuredLevel >= targetLevel && prevLevel < targetLevel)
                        {
                            measureColumn = (int)i;
                            break;
                        }
                    }

                    // ✅ Maintenant mesurer à cette position EXACTE
                    IDWriteTextLayout *layout = nullptr;
                    if (SUCCEEDED(renderCtx.dwriteFactory->CreateTextLayout(
                            line.c_str(),
                            (UINT32)line.size(),
                            renderCtx.textFormat,
                            10000.0f,
                            renderCtx.lineHeight,
                            &layout)) &&
                        layout)
                    {
                        float baseX = 0.0f;

                        DWRITE_OVERHANG_METRICS om = {};
                        if (SUCCEEDED(layout->GetOverhangMetrics(&om)))
                        {
                            baseX -= om.left;
                        }

                        FLOAT x = 0, y = 0;
                        DWRITE_HIT_TEST_METRICS htm = {};
                        // ✅ Mesurer APRÈS ce caractère (trailing edge = TRUE)
                        layout->HitTestTextPosition(measureColumn, TRUE, &x, &y, &htm);

                        float finalX = baseX + x;

                        measurements.push_back(finalX);
                        layout->Release();

                        if (measurements.size() >= 5)
                            break;
                    }
                }
            }

            // Prendre la médiane
            if (!measurements.empty())
            {
                std::sort(measurements.begin(), measurements.end());
                guidePositions[targetLevel] = measurements[measurements.size() / 2];
            }
        }

        // ✅ ÉTAPE 3 : Dessiner les guides
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