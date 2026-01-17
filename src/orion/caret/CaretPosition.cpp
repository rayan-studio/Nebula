#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <d2d1.h>
#include <d2d1helper.h>

#include "CaretPosition.h"
#include "orion/editor/Editor.h"

#include "../geometry/TextColumns.h" // pour Orion::Geometry::AdvanceVisualCol

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace Orion
{
    // =========================
    // Tabs: logique <-> visuel
    // =========================

    static int LogicalToVisualCol(const std::wstring& line, int logicalCol, int tabSize)
    {
        if (logicalCol <= 0 || line.empty())
            return 0;

        int col = logicalCol;
        if (col > (int)line.size())
            col = (int)line.size();

        int visual = 0;
        for (int i = 0; i < col; ++i)
            visual = Orion::Geometry::AdvanceVisualCol(visual, line[i], tabSize);

        return visual;
    }

    static int VisualToLogicalCol(const std::wstring& line, int targetVisual, int tabSize)
    {
        if (line.empty() || targetVisual <= 0)
            return 0;

        int visual = 0;
        for (int i = 0; i < (int)line.size(); ++i)
        {
            const int nextVisual = Orion::Geometry::AdvanceVisualCol(visual, line[i], tabSize);

            // "caret style": si on vise une cellule au milieu d’un tab,
            // on choisit début/fin selon la moitié.
            const int mid = (visual + nextVisual) / 2;
            if (targetVisual < mid)
                return i;

            visual = nextVisual;
        }

        return (int)line.size();
    }

    // =========================
    // API principale (propre)
    // =========================

    D2D1_POINT_2F CaretPositionToScreen(
        const CaretPosition& pos,
        const EditorState& state,
        const EditorMetrics& metrics,
        int tabSize)
    {
        const float contentLeft = state.leftEdge + metrics.gutterWidth + metrics.leftPadding;
        const float baseX = contentLeft - state.scrollOffsetX;

        if (state.lines.empty())
            return D2D1::Point2F(baseX, state.topEdge - state.scrollOffsetY);

        int line = pos.line;
        if (line < 0) line = 0;
        if (line >= (int)state.lines.size()) line = (int)state.lines.size() - 1;

        const float y = state.topEdge + (line * metrics.lineHeight) - state.scrollOffsetY;

        const std::wstring& ln = state.lines[line];

        int col = pos.column;
        if (col < 0) col = 0;
        if (col > (int)ln.size()) col = (int)ln.size();

        const int visualCol = LogicalToVisualCol(ln, col, tabSize);
        const float x = baseX + (visualCol * metrics.characterWidth);

        return D2D1::Point2F(x, y);
    }

    CaretPosition ScreenToCaretPosition(
        POINT pt,
        const EditorState& state,
        const EditorMetrics& metrics,
        int tabSize)
    {
        if (state.lines.empty())
            return { 0, 0 };

        const float contentLeft = state.leftEdge + metrics.gutterWidth + metrics.leftPadding;
        const float baseX = contentLeft - state.scrollOffsetX;

        // Ligne
        const float adjustedY = (float)pt.y + state.scrollOffsetY;
        int line = (int)((adjustedY - state.topEdge) / metrics.lineHeight);
        line = (std::max)(0, (std::min)(line, (int)state.lines.size() - 1));

        const std::wstring& ln = state.lines[line];

        // Colonne visuelle
        float localX = (float)pt.x - baseX;
        if (localX < 0.0f) localX = 0.0f;

        const int targetVisual = (int)std::floor((localX / metrics.characterWidth) + 0.5f);
        int col = VisualToLogicalCol(ln, targetVisual, tabSize);

        col = (std::max)(0, (std::min)(col, (int)ln.size()));
        return { line, col };
    }

    // =========================
    // Compat pour ton code existant
    // (Selection.cpp etc.)
    // =========================

    // Ancien nom très courant : "GetXPositionForColumn"
    // -> retourne juste la coordonnée X pour (line, column).
    float GetXPositionForColumn(
        int line,
        int column,
        const EditorState& state,
        const EditorMetrics& metrics,
        int tabSize)
    {
        CaretPosition p{ line, column };
        return CaretPositionToScreen(p, state, metrics, tabSize).x;
    }

    // Autre helper fréquent: depuis un point écran -> caret.
    CaretPosition GetCaretPositionFromPoint(
        POINT pt,
        const EditorState& state,
        const EditorMetrics& metrics,
        int tabSize)
    {
        return ScreenToCaretPosition(pt, state, metrics, tabSize);
    }
}
