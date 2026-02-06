#include "orion/editor/Editor.h"
#include <algorithm>
#include <cwctype>

namespace Orion
{
    // ---- petits helpers ----
    static std::wstring TrimLeftCopy(const std::wstring& s)
    {
        size_t i = 0;
        while (i < s.size() && (s[i] == L' ' || s[i] == L'\t')) ++i;
        return s.substr(i);
    }

    static bool IsCppLikeExt(const std::wstring& ext)
    {
        return (ext == L".c" || ext == L".cpp" || ext == L".h" || ext == L".hpp");
    }

    struct ScanState
    {
        bool inBlockComment = false;
        bool inString = false;
        bool inChar = false;
        bool escape = false;
    };

    static void CountBracesIgnoringStringsComments(
        const std::wstring& line,
        ScanState& st,
        int& opens,
        int& closes)
    {
        opens = 0;
        closes = 0;

        for (size_t i = 0; i < line.size(); ++i)
        {
            wchar_t c = line[i];
            wchar_t n = (i + 1 < line.size()) ? line[i + 1] : 0;

            if (st.inBlockComment)
            {
                if (c == L'*' && n == L'/')
                {
                    st.inBlockComment = false;
                    ++i;
                }
                continue;
            }

            if (st.inString)
            {
                if (st.escape) { st.escape = false; continue; }
                if (c == L'\\') { st.escape = true; continue; }
                if (c == L'"')  { st.inString = false; }
                continue;
            }

            if (st.inChar)
            {
                if (st.escape) { st.escape = false; continue; }
                if (c == L'\\') { st.escape = true; continue; }
                if (c == L'\'') { st.inChar = false; }
                continue;
            }

            // line comment //
            if (c == L'/' && n == L'/')
                break;

            // block comment /*
            if (c == L'/' && n == L'*')
            {
                st.inBlockComment = true;
                ++i;
                continue;
            }

            if (c == L'"') { st.inString = true; st.escape = false; continue; }
            if (c == L'\'') { st.inChar = true; st.escape = false; continue; }

            if (c == L'{') ++opens;
            else if (c == L'}') ++closes;
        }
    }

    static bool StartsWithCloseBrace(const std::wstring& trimmed)
    {
        return !trimmed.empty() && trimmed[0] == L'}';
    }

    static std::wstring TrimRightCopy(const std::wstring& s)
    {
        if (s.empty()) return s;
        size_t i = s.size();
        while (i > 0 && (s[i - 1] == L' ' || s[i - 1] == L'\t')) --i;
        return s.substr(0, i);
    }

    static std::wstring ReplaceTabsWithSpaces(const std::wstring &s, int tabSize)
    {
        if (s.find(L'\t') == std::wstring::npos) return s;
        std::wstring out;
        out.reserve(s.size());
        for (wchar_t c : s)
        {
            if (c == L'\t') out.append(tabSize, L' ');
            else out.push_back(c);
        }
        return out;
    }

    // Collapse multiple spaces into one, remove spaces before some punctuation,
    // and ensure a single space after commas — but ignore strings and comments.
    static std::wstring CleanSpacesOutsideStringsComments(const std::wstring &line)
    {
        std::wstring out;
        out.reserve(line.size());
        ScanState st{};
        bool prevSpace = false;

        auto peekNextNonSpace = [&](size_t idx) -> wchar_t {
            for (size_t j = idx + 1; j < line.size(); ++j)
            {
                wchar_t nc = line[j];
                if (nc != L' ' && nc != L'\t') return nc;
            }
            return 0;
        };

        for (size_t i = 0; i < line.size(); ++i)
        {
            wchar_t c = line[i];
            wchar_t n = (i + 1 < line.size()) ? line[i + 1] : 0;

            if (st.inBlockComment)
            {
                out.push_back(c);
                if (c == L'*' && n == L'/') { st.inBlockComment = false; ++i; out.push_back(L'/'); }
                continue;
            }

            if (st.inString)
            {
                out.push_back(c);
                if (st.escape) { st.escape = false; continue; }
                if (c == L'\\') { st.escape = true; continue; }
                if (c == L'"') { st.inString = false; }
                continue;
            }

            if (st.inChar)
            {
                out.push_back(c);
                if (st.escape) { st.escape = false; continue; }
                if (c == L'\\') { st.escape = true; continue; }
                if (c == L'\'') { st.inChar = false; }
                continue;
            }

            // line comment start -> copy rest as-is
            if (c == L'/' && n == L'/')
            {
                out.append(line.substr(i));
                break;
            }

            // block comment start
            if (c == L'/' && n == L'*')
            {
                st.inBlockComment = true;
                out.push_back(c);
                continue;
            }

            if (c == L'"') { out.push_back(c); st.inString = true; st.escape = false; continue; }
            if (c == L'\'') { out.push_back(c); st.inChar = true; st.escape = false; continue; }

            // spaces handling outside strings/comments
            if (c == L' ' || c == L'\t')
            {
                prevSpace = true;
                continue;
            }

            if (prevSpace)
            {
                // If next non-space is punctuation that should not be preceded by space, skip space
                wchar_t next = c; // current non-space char
                bool skipSpaceBefore = (next == L';' || next == L',' || next == L')' || next == L']' || next == L'.' || next == L':' );
                if (!skipSpaceBefore)
                {
                    // If previous output ends with certain tokens like '(' we don't add space
                    wchar_t prevOut = out.empty() ? 0 : out.back();
                    if (prevOut != L'(' && prevOut != L'/' && prevOut != L'+' && prevOut != L'-')
                        out.push_back(L' ');
                }
                prevSpace = false;
            }

            // Handle comma -> ensure single space after comma (we will suppress later spaces by prevSpace logic)
            if (c == L',')
            {
                out.push_back(c);
                // peek next non-space char and if it's not punctuation or end, add single space
                wchar_t nxt = peekNextNonSpace(i);
                if (nxt != 0 && nxt != L';' && nxt != L',' && nxt != L')' && nxt != L']' && nxt != L'.')
                    out.push_back(L' ');
                continue;
            }

            out.push_back(c);
        }

        return out;
    }

    void Editor::FormatDocument()
    {
        const std::wstring ext = GetFileExtension();
        if (!IsCppLikeExt(ext))
            return;

        if (state_.lines.empty())
            return;

        // Undo snapshot
        undoStack_.push_back(state_);
        if (undoStack_.size() > maxUndoEntries_)
            undoStack_.erase(undoStack_.begin());

        const int tabSize = GetIndentConfig().tabSize;

        int indent = 0;
        ScanState scan{};

        std::vector<std::wstring> newLines;
        newLines.reserve(state_.lines.size());
        bool prevEmpty = false;

        for (size_t li = 0; li < state_.lines.size(); ++li)
        {
            const std::wstring &rawLine = state_.lines[li];
            std::wstring leftTrimmed = TrimLeftCopy(rawLine);

            // Keep preprocessor lines intact (do not reindent or re-space includes).
            if (!leftTrimmed.empty() && leftTrimmed[0] == L'#')
            {
                std::wstring keep = TrimRightCopy(rawLine);
                newLines.push_back(keep);
                prevEmpty = false;
                continue;
            }
            // Remove trailing whitespace
            leftTrimmed = TrimRightCopy(leftTrimmed);
            // Convert tabs inside the content to spaces
            leftTrimmed = ReplaceTabsWithSpaces(leftTrimmed, tabSize);
            // Collapse/clean internal spacing while respecting strings/comments
            leftTrimmed = CleanSpacesOutsideStringsComments(leftTrimmed);

            // Keep a single empty line only
            if (leftTrimmed.empty())
            {
                if (!prevEmpty)
                {
                    newLines.push_back(L"");
                    prevEmpty = true;
                }
                // don't update scan/indent state on empty lines
                continue;
            }

            prevEmpty = false;

            int lineIndent = indent;
            if (StartsWithCloseBrace(leftTrimmed))
                lineIndent = ((lineIndent - 1) > 0) ? (lineIndent - 1) : 0;

            // Apply indentation (spaces)
            std::wstring outLine = std::wstring(lineIndent * tabSize, L' ') + leftTrimmed;
            newLines.push_back(outLine);

            // Count braces (ignoring strings/comments)
            int opens = 0, closes = 0;
            CountBracesIgnoringStringsComments(leftTrimmed, scan, opens, closes);

            indent += opens;
            indent -= closes;
            if (indent < 0) indent = 0;
        }

        state_.lines = std::move(newLines);

        // Clamp caret
        if (state_.caret.line < 0) state_.caret.line = 0;
        if (state_.caret.line >= (int)state_.lines.size())
            state_.caret.line = (int)state_.lines.size() - 1;

        int maxCol = (int)state_.lines[state_.caret.line].size();
        if (state_.caret.column < 0) state_.caret.column = 0;
        if (state_.caret.column > maxCol) state_.caret.column = maxCol;

        // Note: ton render calcule contentWidth via maxLen à UpdateLayout,
        // donc pas besoin de recalcul “caché” ici.
        // Window invalide déjà => redraw + UpdateLayout au prochain WM_SIZE/Draw.
    }
}
