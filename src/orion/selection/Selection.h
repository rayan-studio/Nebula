#pragma once

#include <d2d1.h>
#include <dwrite.h>
#include <vector>
#include <string>

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
            Rectangle = 0,
            Rounded = 1,
            RoundedSmart = 2
        };

        struct SelectionConfig
        {
            D2D1_COLOR_F color = D2D1::ColorF(0.2f, 0.4f, 0.8f, 0.3f);
            float cornerRadius = 3.0f;
            SelectionStyle style = SelectionStyle::RoundedSmart;
        };

        class Selection
        {
        public:
            explicit Selection(const SelectionConfig &cfg);
            ~Selection();

            // Rounded rects are used to allow both rectangular and rounded drawing
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
        };

    } // namespace Rendering
} // namespace Orion
