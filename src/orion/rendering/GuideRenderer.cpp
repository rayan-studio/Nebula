#include "GuideRenderer.h"
#include <cmath>
#include <algorithm>
#include <sstream>
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
    GuideRenderer::GuideRenderer(const Geometry::IndentConfig& indentConfig, const GuideStyle& style)
        : indentHelper_(indentConfig)
        , style_(style)
        , tabSize_(indentConfig.tabSize)
    {
    }

    void GuideRenderer::DrawCppGuides(
        ID2D1RenderTarget* ctx,
        const std::vector<std::wstring>& lines,
        const GuideRenderContext& renderCtx)
    {
        static bool debugLogs = true;

        if (!ctx || lines.empty())
            return;

        // Calculer la position X du caret (pour highlight)
        float caretX = -10000.0f;
        if (renderCtx.caretLine >= renderCtx.firstVisibleLine &&
            renderCtx.caretLine < renderCtx.lastVisibleLine &&
            renderCtx.caretLine < (int)lines.size())
        {
            auto caretIndent = indentHelper_.GetLineIndent(lines[renderCtx.caretLine]);
            caretX = indentHelper_.GetIndentScreenX(
                caretIndent.level,
                renderCtx.contentLeft,
                renderCtx.scrollOffsetX);
            
            if (debugLogs)
            {
                std::wstringstream ss;
                ss << L"[GuideRenderer::DrawCppGuides] caretLine=" << renderCtx.caretLine 
                   << L" caretIndent=" << caretIndent.level << L" caretX=" << caretX;
                Logger::Instance().Log(ss.str());
            }
        }

        // Trouver tous les niveaux d'indentation présents
        auto levels = indentHelper_.GetVisibleIndentLevels(
            lines,
            renderCtx.firstVisibleLine,
            renderCtx.lastVisibleLine);

        if (levels.empty())
            return;

        if (debugLogs)
        {
            std::wstringstream ss;
            ss << L"[GuideRenderer::DrawCppGuides] levelsCount=" << levels.size();
            Logger::Instance().Log(ss.str());
        }

        // Pour chaque niveau d'indentation
        for (int levelSpaces : levels)
        {
            // Utiliser le même calcul que pour le caret pour avoir le bon alignement
            float guideX = indentHelper_.GetIndentScreenX(
                levelSpaces,
                renderCtx.contentLeft,
                renderCtx.scrollOffsetX);

            // Déterminer si ce guide est actif (près du caret)
            bool isActive = std::fabs(guideX - caretX) < 5.0f;

            // Parcourir les lignes et créer des plages continues
            bool inRange = false;
            int rangeStart = -1;

            for (int li = renderCtx.firstVisibleLine; li < renderCtx.lastVisibleLine; ++li)
            {
                bool shouldDraw = false;
                bool isEmptyLine = false;
                
                // Vérifier si on est dans les limites
                if (li < (int)lines.size())
                {
                    auto lineIndent = indentHelper_.GetLineIndent(lines[li]);
                    
                    // Une ligne est considérée vide si elle n'a que des espaces/tabs
                    isEmptyLine = lineIndent.isWhitespaceOnly;
                    
                    // Dessiner si la ligne a assez d'indentation OU si c'est une ligne vide
                    // (les lignes vides ne doivent pas interrompre les guides)
                    if (isEmptyLine)
                    {
                        // Si on est déjà dans une plage, continuer
                        shouldDraw = inRange;
                    }
                    else
                    {
                        shouldDraw = (lineIndent.level > 0 && 
                                     indentHelper_.ShouldDrawGuide(lineIndent.level, levelSpaces));
                    }
                }

                if (shouldDraw)
                {
                    if (!inRange)
                    {
                        // Début d'une nouvelle plage
                        inRange = true;
                        rangeStart = li;
                    }
                }
                else if (inRange && !isEmptyLine)
                {
                    // Fin de plage - dessiner le segment continu
                    // Seulement si c'est une ligne non-vide qui n'a pas assez d'indentation
                    float topY = renderCtx.topEdge + 
                                (rangeStart * renderCtx.lineHeight) - 
                                renderCtx.scrollOffsetY + 
                                (renderCtx.lineHeight * style_.topMargin);

                    float bottomY = renderCtx.topEdge + 
                                   ((li - 1) * renderCtx.lineHeight) - 
                                   renderCtx.scrollOffsetY + 
                                   renderCtx.lineHeight - 
                                   (renderCtx.lineHeight * style_.bottomMargin);

                    if (debugLogs)
                    {
                        std::wstringstream ss;
                        ss << L"[GuideRenderer::DrawCppGuides] RANGE levelSpaces=" << levelSpaces 
                           << L" lines=" << rangeStart << L"-" << (li-1)
                           << L" guideX=" << guideX 
                           << L" topY=" << topY 
                           << L" bottomY=" << bottomY 
                           << L" isActive=" << (isActive ? 1 : 0);
                        Logger::Instance().Log(ss.str());
                    }

                    DrawGuideSegment(ctx, guideX, topY, bottomY, isActive);
                    inRange = false;
                    rangeStart = -1;
                }
            }

            // Si une plage est toujours ouverte à la fin, la fermer
            if (inRange && rangeStart >= 0)
            {
                int lastLine = std::min(renderCtx.lastVisibleLine - 1, (int)lines.size() - 1);
                
                float topY = renderCtx.topEdge + 
                            (rangeStart * renderCtx.lineHeight) - 
                            renderCtx.scrollOffsetY + 
                            (renderCtx.lineHeight * style_.topMargin);

                float bottomY = renderCtx.topEdge + 
                               (lastLine * renderCtx.lineHeight) - 
                               renderCtx.scrollOffsetY + 
                               renderCtx.lineHeight - 
                               (renderCtx.lineHeight * style_.bottomMargin);

                if (debugLogs)
                {
                    std::wstringstream ss;
                    ss << L"[GuideRenderer::DrawCppGuides] RANGE (final) levelSpaces=" << levelSpaces 
                       << L" lines=" << rangeStart << L"-" << lastLine
                       << L" guideX=" << guideX 
                       << L" topY=" << topY 
                       << L" bottomY=" << bottomY 
                       << L" isActive=" << (isActive ? 1 : 0);
                    Logger::Instance().Log(ss.str());
                }

                DrawGuideSegment(ctx, guideX, topY, bottomY, isActive);
            }
        }
    }

   

    void GuideRenderer::DrawGuideSegment(
        ID2D1RenderTarget* ctx,
        float x,
        float topY,
        float bottomY,
        bool isActive)
    {
        if (!ctx)
            return;

        D2D1_COLOR_F color = isActive ? style_.activeColor : style_.normalColor;
        
        ID2D1SolidColorBrush* brush = nullptr;
        if (SUCCEEDED(ctx->CreateSolidColorBrush(color, &brush)) && brush)
        {
            D2D1_RECT_F rect = D2D1::RectF(x, topY, x + style_.lineWidth, bottomY);
            ctx->FillRectangle(rect, brush);
            brush->Release();
        }
    }

} // namespace Orion::Rendering