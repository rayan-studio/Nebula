#pragma once
#include <vector>
#include <d2d1.h>
#include <dwrite.h>

#include "../geometry/IndentGuides.h"
#include "../geometry/IndentationHelper.h"

namespace Orion::Rendering
{
    struct GuideStyle
    {
        D2D1_COLOR_F normalColor = D2D1::ColorF(0.3f, 0.3f, 0.35f, 0.5f);
        D2D1_COLOR_F activeColor = D2D1::ColorF(0.4f, 0.4f, 0.5f, 0.7f);
        float lineWidth = 1.0f;
        float topMargin = 0.15f;
        float bottomMargin = 0.15f;
    };

    struct GuideRenderContext
    {
        float contentLeft = 0.0f;
        float topEdge = 0.0f;
        float scrollOffsetX = 0.0f;
        float scrollOffsetY = 0.0f;
        float lineHeight = 0.0f;

        // ✅ largeur char utilisée par ton rendu texte
        float charWidth = 0.0f;

        // Optional: lines for per-line indent clipping (avoid drawing over text)
        const std::vector<std::wstring>* lines = nullptr;

        int firstVisibleLine = 0;
        int lastVisibleLine = 0; // exclusif

        // ✅ GARDÉS pour compat avec ton Editor_TextRender.cpp
        IDWriteFactory* dwriteFactory = nullptr;
        IDWriteTextFormat* textFormat = nullptr;
    };

    class GuideRenderer
    {
    public:
        GuideRenderer(const Geometry::IndentConfig& indentConfig, const GuideStyle& style);
        ~GuideRenderer();

        void DrawGuides(
            ID2D1RenderTarget* ctx,
            const std::vector<Geometry::IndentGuide>& guides,
            const GuideRenderContext& renderCtx);

    private:
        void EnsureBrushes(ID2D1RenderTarget* ctx);
        void DrawGuideSegment(ID2D1RenderTarget* ctx, float x, float topY, float bottomY, bool isActive);

    private:
        GuideStyle style_;
        int tabSize_ = 4;

        ID2D1SolidColorBrush* normalBrush_ = nullptr;
        ID2D1SolidColorBrush* activeBrush_ = nullptr;
    };
}
