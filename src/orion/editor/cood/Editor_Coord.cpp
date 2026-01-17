#include "orion/editor/Editor.h"

#include <algorithm>
#include <cmath>

#include "orion/geometry/TextColumns.h"

// Ensure Windows min/max macros don't interfere with std::min/std::max
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

namespace Orion
{
    D2D1_POINT_2F Editor::TextToScreenPosition(CaretPosition pos)
    {
        const float contentLeft = state_.leftEdge + metrics_.gutterWidth + metrics_.leftPadding;
        const float baseX = contentLeft - state_.scrollOffsetX;

        if (state_.lines.empty())
            return D2D1::Point2F(baseX, state_.topEdge - state_.scrollOffsetY);

        int line = pos.line;
        if (line < 0) line = 0;
        if (line >= (int)state_.lines.size()) line = (int)state_.lines.size() - 1;

        const float y = state_.topEdge + (line * metrics_.lineHeight) - state_.scrollOffsetY;

        const std::wstring &ln = state_.lines[line];
        int col = pos.column;
        if (col < 0) col = 0;
        if (col > (int)ln.size()) col = (int)ln.size();

        int visualCol = 0;
        const int tabSize = 4;
        for (int i = 0; i < col; ++i)
            visualCol = Orion::Geometry::AdvanceVisualCol(visualCol, ln[i], tabSize);

        const float x = baseX + (visualCol * metrics_.characterWidth);
        return D2D1::Point2F(x, y);
    }

    CaretPosition Editor::ScreenToTextPosition(POINT screenPoint)
    {
        CaretPosition out{0, 0};
        if (state_.lines.empty())
            return out;

        const float contentLeft = state_.leftEdge + metrics_.gutterWidth + metrics_.leftPadding;
        const float baseX = contentLeft - state_.scrollOffsetX;

        const float adjustedY = (float)screenPoint.y + state_.scrollOffsetY;
        int line = (int)((adjustedY - state_.topEdge) / metrics_.lineHeight);
        line = (std::max)(0, (std::min)(line, (int)state_.lines.size() - 1));

        const std::wstring &ln = state_.lines[line];

        float localX = (float)screenPoint.x - baseX;
        if (localX < 0.0f) localX = 0.0f;

        int targetVisual = (int)std::floor((localX / metrics_.characterWidth) + 0.5f);
        if (targetVisual < 0) targetVisual = 0;

        const int tabSize = 4;

        int visual = 0;
        int col = 0;
        for (int i = 0; i < (int)ln.size(); ++i)
        {
            int nextVisual = Orion::Geometry::AdvanceVisualCol(visual, ln[i], tabSize);

            int mid = (visual + nextVisual) / 2;
            if (targetVisual < mid)
            {
                col = i;
                break;
            }

            visual = nextVisual;
            col = i + 1;
        }

        if (col < 0) col = 0;
        if (col > (int)ln.size()) col = (int)ln.size();

        out.line = line;
        out.column = col;
        return out;
    }
}
