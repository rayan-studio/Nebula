#include "CppBraceGuides.h"
#include "utils/logger/Logger.h"
#include <sstream>
#include <chrono>
#include <algorithm>

namespace Orion::Geometry
{
    struct OpenBrace
    {
        int line;
        int visualCol;
    };

    std::vector<IndentGuide> ComputeCppBraceGuides(
        const std::vector<std::wstring> &lines,
        int tabSize,
        int activeLine,
        int firstLine,
        int lastLine)
    {
        std::vector<IndentGuide> out;
        if (lines.empty())
            return out;

        if (tabSize <= 0)
            tabSize = 4;

        firstLine = (std::max)(0, firstLine);
        lastLine = (std::min)((int)lines.size(), lastLine);
        if (lastLine <= firstLine)
            return out;

        bool inLineComment = false;
        bool inBlockComment = false;
        bool inString = false;
        bool inChar = false;
        bool escape = false;

        std::vector<OpenBrace> stack;
        stack.reserve(128);

        auto AdvanceVisual = [&](int vc, wchar_t ch) -> int
        {
            if (ch == L'\t')
            {
                int ts = tabSize;
                int next = ((vc / ts) + 1) * ts;
                return next;
            }
            return vc + 1;
        };

        auto ScanRange = [&](int a, int b, bool emitGuides)
        {
            a = (std::max)(0, a);
            b = (std::min)((int)lines.size(), b);

            for (int li = a; li < b; ++li)
            {
                const std::wstring &L = lines[li];
                inLineComment = false;

                for (size_t i = 0; i < L.size(); ++i)
                {
                    wchar_t c = L[i];
                    wchar_t n = (i + 1 < L.size()) ? L[i + 1] : 0;

                    if (inLineComment)
                        break;

                    if (inBlockComment)
                    {
                        if (c == L'*' && n == L'/')
                        {
                            inBlockComment = false;
                            ++i;
                        }
                        continue;
                    }

                    if (inString)
                    {
                        if (escape) { escape = false; continue; }
                        if (c == L'\\') { escape = true; continue; }
                        if (c == L'"') { inString = false; continue; }
                        continue;
                    }

                    if (inChar)
                    {
                        if (escape) { escape = false; continue; }
                        if (c == L'\\') { escape = true; continue; }
                        if (c == L'\'') { inChar = false; continue; }
                        continue;
                    }

                    if (c == L'/' && n == L'/')
                    {
                        inLineComment = true;
                        break;
                    }
                    if (c == L'/' && n == L'*')
                    {
                        inBlockComment = true;
                        ++i;
                        continue;
                    }

                    if (c == L'"')
                    {
                        inString = true;
                        escape = false;
                        continue;
                    }
                    if (c == L'\'')
                    {
                        inChar = true;
                        escape = false;
                        continue;
                    }

                    if (c == L'{')
                    {
                        int vc = 0;
                        for (size_t k = 0; k < i; ++k)
                            vc = AdvanceVisual(vc, L[k]);

                        stack.push_back(OpenBrace{li, vc});
                    }
                    else if (c == L'}')
                    {
                        if (!stack.empty())
                        {
                            OpenBrace ob = stack.back();
                            stack.pop_back();

                            if (emitGuides)
                            {
                                IndentGuide g;
                                g.startLine = ob.line;
                                g.endLine = li;
                                g.visualCol = ob.visualCol;
                                g.active = (activeLine >= g.startLine && activeLine <= g.endLine);
                                out.push_back(g);
                            }
                        }
                    }
                }
            }
        };

        ScanRange(0, firstLine, false);
        ScanRange(firstLine, lastLine, true);

        // Guides pour blocs ouverts non refermés dans la fenêtre
        for (const auto &ob : stack)
        {
            IndentGuide g;
            g.startLine = ob.line;
            g.endLine = lastLine - 1;
            g.visualCol = ob.visualCol;
            g.active = (activeLine >= g.startLine && activeLine <= g.endLine);
            out.push_back(g);
        }

        out.erase(
            std::remove_if(out.begin(), out.end(),
                [](const IndentGuide &g){ return g.endLine < g.startLine; }),
            out.end());

        // no logging here (avoid spamming logs on every render)

        return out;
    }
}
