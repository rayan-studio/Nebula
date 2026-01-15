#pragma once

#include <d2d1.h>
#include <dwrite.h>
#include "../../OrionEditor.h"

namespace Orion
{
    class Gutter
    {
    public:
        Gutter();
        void DrawGutter(ID2D1RenderTarget *ctx,
                        const EditorState &state,
                        const EditorTheme &theme,
                        const EditorMetrics &metrics);

        void DrawLineNumbers(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite,
                             const EditorState &state, const EditorTheme &theme,
                             const EditorMetrics &metrics, IDWriteFontCollection *customFontCollection);
        float CalculateGutterWidth(const EditorState &state, const EditorMetrics &metrics) const;

    private:
        // À compléter plus tard (theme_, state_, metrics_, etc.)
    };
}
