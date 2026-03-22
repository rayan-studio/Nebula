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
        wchar_t kind;
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
        bool inRawString = false;
        std::wstring rawDelim;
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

                    if (inRawString)
                    {
                        if (c == L')')
                        {
                            bool match = true;
                            for (size_t k = 0; k < rawDelim.size(); ++k)
                            {
                                if ((i + 1 + k) >= L.size() || L[i + 1 + k] != rawDelim[k])
                                {
                                    match = false;
                                    break;
                                }
                            }
                            size_t endIdx = i + 1 + rawDelim.size();
                            if (match && endIdx < L.size() && L[endIdx] == L'"')
                            {
                                inRawString = false;
                                rawDelim.clear();
                                i = endIdx;
                            }
                        }
                        continue;
                    }

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

                    if (c == L'R' && n == L'"')
                    {
                        size_t start = i + 2;
                        size_t openPos = L.find(L'(', start);
                        if (openPos != std::wstring::npos)
                        {
                            rawDelim = L.substr(start, openPos - start);
                            inRawString = true;
                            i = openPos;
                            continue;
                        }
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
                        int braceVc = 0;
                        for (size_t k = 0; k < i; ++k)
                            braceVc = AdvanceVisual(braceVc, L[k]);

                        // Determine leading indent of the line (visual columns).
                        int indentVc = 0;
                        for (size_t k = 0; k < L.size(); ++k)
                        {
                            if (!iswspace(L[k]))
                                break;
                            indentVc = AdvanceVisual(indentVc, L[k]);
                        }

                        // Place guide at the block's base indent (VS-like for multi-line pairs).
                        int guideVc = indentVc;
                        if (guideVc < 0)
                            guideVc = 0;

                        stack.push_back(OpenBrace{li, guideVc, c});
                    }
                    else if (c == L'}')
                    {
                        if (!stack.empty())
                        {
                            // Pop until we find a matching kind
                            wchar_t expected = L'{';
                            OpenBrace ob = {};
                            bool found = false;
                            for (int s = (int)stack.size() - 1; s >= 0; --s)
                            {
                                if (stack[s].kind == expected)
                                {
                                    ob = stack[s];
                                    stack.erase(stack.begin() + s);
                                    found = true;
                                    break;
                                }
                            }
                            if (!found)
                                continue;

                            if (emitGuides)
                            {
                                if (li == ob.line)
                                    continue; // same-line pairs don't get guides
                                if (ob.visualCol <= 0)
                                    continue; // root-level block guide would duplicate the main left guide
                                IndentGuide g;
                                g.startLine = ob.line + 1; // start after opening brace line
                                g.endLine = li - 1; // stop before closing brace line
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
            if (ob.visualCol <= 0)
                continue; // keep root-level block out of the indentation guides
            IndentGuide g;
            g.startLine = ob.line + 1; // start after opening brace line
            g.endLine = lastLine - 1;
            g.visualCol = ob.visualCol;
            g.active = (activeLine >= g.startLine && activeLine <= g.endLine);
            out.push_back(g);
        }

        out.erase(
            std::remove_if(out.begin(), out.end(),
                [](const IndentGuide &g){ return g.endLine < g.startLine; }),
            out.end());

        // Only the deepest containing block should be active.
        int activeGuideIndex = -1;
        for (size_t i = 0; i < out.size(); ++i)
        {
            const IndentGuide &g = out[i];
            if (activeLine < g.startLine || activeLine > g.endLine)
                continue;

            if (activeGuideIndex < 0)
            {
                activeGuideIndex = (int)i;
                continue;
            }

            const IndentGuide &current = out[(size_t)activeGuideIndex];
            if (g.visualCol > current.visualCol ||
                (g.visualCol == current.visualCol && g.startLine > current.startLine) ||
                (g.visualCol == current.visualCol && g.startLine == current.startLine &&
                 (g.endLine - g.startLine) < (current.endLine - current.startLine)))
            {
                activeGuideIndex = (int)i;
            }
        }

        for (size_t i = 0; i < out.size(); ++i)
            out[i].active = ((int)i == activeGuideIndex);

        // no logging here (avoid spamming logs on every render)

        return out;
    }
}
