#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <vector>
#include <string>
#include "../geometry/IndentationHelper.h"

namespace Orion::Rendering
{
    struct GuideStyle
    {
        D2D1_COLOR_F normalColor;
        D2D1_COLOR_F activeColor;
        float lineWidth = 1.0f;
        float topMargin = 0.0f;
        float bottomMargin = 0.0f;
    };

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
        IDWriteFactory* dwriteFactory;
        IDWriteTextFormat* textFormat;
    };

    class GuideRenderer
    {
    public:
        GuideRenderer(const Geometry::IndentConfig& indentConfig, const GuideStyle& style);

        void DrawCppGuides(
            ID2D1RenderTarget* ctx,
            const std::vector<std::wstring>& lines,
            const GuideRenderContext& renderCtx);

    private:
        void DrawGuideSegment(
            ID2D1RenderTarget* ctx,
            float x,
            float topY,
            float bottomY,
            bool isActive);

        Geometry::IndentationHelper indentHelper_;
        GuideStyle style_;
        int tabSize_;
    };

} // namespace Orion::Rendering