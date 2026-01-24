#include "GuideRenderer.h"
#include <algorithm>
#include <cmath>

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

namespace Orion::Rendering
{
    GuideRenderer::GuideRenderer(const Geometry::IndentConfig& indentConfig, const GuideStyle& style)
        : style_(style), tabSize_(indentConfig.tabSize)
    {
        if (tabSize_ <= 0) tabSize_ = 4;
    }

    GuideRenderer::~GuideRenderer()
    {
        if (normalBrush_) { normalBrush_->Release(); normalBrush_ = nullptr; }
        if (activeBrush_) { activeBrush_->Release(); activeBrush_ = nullptr; }
    }

    void GuideRenderer::EnsureBrushes(ID2D1RenderTarget* ctx)
    {
        if (!ctx) return;

        if (!normalBrush_) ctx->CreateSolidColorBrush(style_.normalColor, &normalBrush_);
        else normalBrush_->SetColor(style_.normalColor);

        if (!activeBrush_) ctx->CreateSolidColorBrush(style_.activeColor, &activeBrush_);
        else activeBrush_->SetColor(style_.activeColor);
    }

    void GuideRenderer::DrawGuideSegment(ID2D1RenderTarget* ctx, float x, float topY, float bottomY, bool isActive)
    {
        if (!ctx) return;

        ID2D1SolidColorBrush* b = isActive ? activeBrush_ : normalBrush_;
        if (!b) return;
        if (bottomY <= topY) return;

        D2D1_RECT_F rect = D2D1::RectF(x, topY, x + style_.lineWidth, bottomY);
        ctx->FillRectangle(rect, b);
    }

    void GuideRenderer::DrawGuides(
        ID2D1RenderTarget* ctx,
        const std::vector<Geometry::IndentGuide>& guides,
        const GuideRenderContext& renderCtx)
    {
        if (!ctx || guides.empty()) return;
        EnsureBrushes(ctx);

        float cw = renderCtx.charWidth;
        if (cw <= 0.1f) cw = 8.0f;

        int vFirst = renderCtx.firstVisibleLine;
        int vLast = renderCtx.lastVisibleLine - 1;

        for (const auto& g : guides)
        {
            int start = (std::max)(g.startLine, vFirst);
            int end   = (std::min)(g.endLine, vLast);
            if (end < start) continue;

            // Center guides within the indentation column, not on the left edge.
            float x = renderCtx.contentLeft + (g.visualCol * cw) - renderCtx.scrollOffsetX + (cw * 0.5f);
            x = std::floor(x) + 0.5f;
            float topY = renderCtx.topEdge + (start * renderCtx.lineHeight) - renderCtx.scrollOffsetY
                       + (renderCtx.lineHeight * style_.topMargin);
            float bottomY = renderCtx.topEdge + ((end + 1) * renderCtx.lineHeight) - renderCtx.scrollOffsetY
                          - (renderCtx.lineHeight * style_.bottomMargin);

            // Pixel-align to reduce 1px wobble.
            topY = std::floor(topY) + 0.5f;
            bottomY = std::floor(bottomY) + 0.5f;

            DrawGuideSegment(ctx, x, topY, bottomY, g.active);
        }
    }
}
