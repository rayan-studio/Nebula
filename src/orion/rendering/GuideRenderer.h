#pragma once
#include <d2d1.h>
#include <string>
#include <vector>
#include "../geometry/IndentationHelper.h"

namespace Orion::Rendering
{
    // Configuration visuelle des guides
    struct GuideStyle
    {
        D2D1_COLOR_F normalColor = D2D1::ColorF(0.3f, 0.3f, 0.35f, 0.5f);
        D2D1_COLOR_F activeColor = D2D1::ColorF(0.4f, 0.4f, 0.5f, 0.7f);
        float lineWidth = 1.0f;
        float topMargin = 0.12f;    // Proportion de lineHeight
        float bottomMargin = 0.12f; // Proportion de lineHeight
    };

    // Informations de contexte pour le rendu
    struct GuideRenderContext
    {
        float contentLeft;
        float topEdge;
        float scrollOffsetY;
        float scrollOffsetX;
        float lineHeight;
        int firstVisibleLine;
        int lastVisibleLine;
        int caretLine;
    };

    class GuideRenderer
    {
    public:
        GuideRenderer(const Geometry::IndentConfig& indentConfig, const GuideStyle& style);

        // Dessine les guides d'indentation pour C/C++
        void DrawCppGuides(
            ID2D1RenderTarget* ctx,
            const std::vector<std::wstring>& lines,
            const GuideRenderContext& renderCtx);

        void SetStyle(const GuideStyle& style) { style_ = style; }

    private:
        // Dessine un segment vertical de guide
        void DrawGuideSegment(
            ID2D1RenderTarget* ctx,
            float x,
            float topY,
            float bottomY,
            bool isActive);

        Geometry::IndentationHelper indentHelper_;
        int tabSize_ = 4;
        GuideStyle style_;
    };

} // namespace Orion::Rendering