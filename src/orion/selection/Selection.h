#pragma once

#include <d2d1.h>
#include <vector>
#include <string>

struct IDWriteFactory;
struct IDWriteTextFormat;

namespace Orion
{
    namespace Rendering
    {
        struct CaretPosition
        {
            int line;
            int column;
        };

        enum class SelectionStyle
        {
            Rectangle,      // Rectangles simples
            RoundedSimple,  // Tous les coins arrondis (ancien comportement)
            RoundedSmart    // Coins intelligents selon le contexte (nouveau!)
        };

        struct SelectionConfig
        {
            D2D1_COLOR_F color = D2D1::ColorF(0.2f, 0.4f, 0.8f, 0.3f);
            float cornerRadius = 3.0f;
            SelectionStyle style = SelectionStyle::RoundedSmart;
        };

        // Forward declaration pour l'analyse des coins
        enum class CornerStyle;
        struct CornerStyles;

        class Selection
        {
        public:
            explicit Selection(const SelectionConfig &cfg);
            ~Selection();

            void Draw(ID2D1RenderTarget *ctx, const std::vector<D2D1_ROUNDED_RECT> &regions);

            static std::vector<D2D1_ROUNDED_RECT> CalculateRegions(
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
                IDWriteTextFormat *textFormat);

        private:
            SelectionConfig cfg_;

            // Nouvelles méthodes pour la gestion intelligente des coins
            std::vector<CornerStyles> AnalyzeCornerStyles(const std::vector<D2D1_ROUNDED_RECT> &regions);
            
            void DrawSmartRoundedSelection(
                ID2D1RenderTarget *ctx,
                ID2D1SolidColorBrush *brush,
                const D2D1_RECT_F &rect,
                const CornerStyles &corners);

            void DrawRoundedCorner(
                ID2D1RenderTarget *ctx,
                ID2D1SolidColorBrush *brush,
                float x, float y, float radius, float angleOffset);

            void DrawInverseCorner(
                ID2D1RenderTarget *ctx,
                ID2D1Factory *factory,
                ID2D1SolidColorBrush *bgBrush,
                float x, float y, float radius, int corner);
        };

    } // namespace Rendering
} // namespace Orion