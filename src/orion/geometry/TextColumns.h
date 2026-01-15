#pragma once
#include <string>

namespace Orion::Geometry
{
    inline int AdvanceVisualCol(int visualCol, wchar_t ch, int tabSize)
    {
        if (ch == L'\t')
        {
            int next = tabSize - (visualCol % tabSize);
            return visualCol + next;
        }
        return visualCol + 1;
    }

    inline int LogicalToVisualCol(const std::wstring& line, int logicalCol, int tabSize)
    {
        if (logicalCol <= 0 || line.empty())
            return 0;

        int col = logicalCol;
        if (col > (int)line.size())
            col = (int)line.size();

        int visual = 0;
        for (int i = 0; i < col; ++i)
            visual = AdvanceVisualCol(visual, line[i], tabSize);

        return visual;
    }

    inline int VisualToLogicalCol(const std::wstring& line, int targetVisual, int tabSize)
    {
        if (line.empty() || targetVisual <= 0)
            return 0;

        int visual = 0;
        for (int i = 0; i < (int)line.size(); ++i)
        {
            int nextVisual = AdvanceVisualCol(visual, line[i], tabSize);
            int mid = (visual + nextVisual) / 2;
            if (targetVisual < mid)
                return i;
            visual = nextVisual;
        }
        return (int)line.size();
    }
}
