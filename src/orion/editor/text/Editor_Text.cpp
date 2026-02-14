#include "orion/editor/Editor.h"
#include "orion/caret/Caret.h"
#include <algorithm>
#include <string>
#include <vector>
#include <thread>
#include <fstream>
#include <cwctype>
// For EditorFileLoadResult and WM_EDITOR_FILE_LOADED
#include "core/window/Window.h"

// Ensure Windows min/max macros don't interfere with std::min/std::max
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

namespace Orion
{
    namespace
    {
        static bool IsWordChar(wchar_t ch)
        {
            return std::iswalnum(ch) != 0 || ch == L'_';
        }

        static void NormalizeRange(CaretPosition &a, CaretPosition &b)
        {
            if (a.line > b.line || (a.line == b.line && a.column > b.column))
                std::swap(a, b);
        }

        static std::wstring LeadingIndent(const std::wstring &line)
        {
            size_t i = 0;
            while (i < line.size() && (line[i] == L' ' || line[i] == L'\t'))
                ++i;
            return line.substr(0, i);
        }

        static bool StartsWith(const std::wstring &text, const std::wstring &prefix)
        {
            return prefix.size() <= text.size() && text.compare(0, prefix.size(), prefix) == 0;
        }
    }

    std::wstring Editor::GetSelectionText() const
    {
        if (!state_.hasSelection)
            return L"";

        CaretPosition a = state_.selectionStart;
        CaretPosition b = state_.caret;
        if (a.line > b.line || (a.line == b.line && a.column > b.column))
            std::swap(a, b);

        std::wstring out;
        if (a.line == b.line)
        {
            if (a.line >= 0 && a.line < (int)state_.lines.size())
            {
                const std::wstring &line = state_.lines[a.line];
                int start = std::min<int>(std::max<int>(0, a.column), (int)line.size());
                int end = std::min<int>(std::max<int>(0, b.column), (int)line.size());
                if (end > start)
                    out = line.substr(start, end - start);
            }
            return out;
        }

        for (int L = a.line; L <= b.line && L < (int)state_.lines.size(); ++L)
        {
            const std::wstring &line = state_.lines[L];
            if (L == a.line)
            {
                int start = std::min<int>(std::max<int>(0, a.column), (int)line.size());
                out += line.substr(start);
            }
            else if (L == b.line)
            {
                int end = std::min<int>(std::max<int>(0, b.column), (int)line.size());
                out += line.substr(0, end);
            }
            else
            {
                out += line;
            }

            if (L < b.line)
                out.push_back(L'\n');
        }

        return out;
    }

    void Editor::CopySelectionToClipboard()
    {
        std::wstring sel = GetSelectionText();
        if (sel.empty())
            return;

        if (OpenClipboard(NULL))
        {
            EmptyClipboard();
            SIZE_T bytes = (sel.size() + 1) * sizeof(wchar_t);
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (hMem)
            {
                void *ptr = GlobalLock(hMem);
                if (ptr)
                {
                    memcpy(ptr, sel.c_str(), bytes);
                    GlobalUnlock(hMem);
                    SetClipboardData(CF_UNICODETEXT, hMem);
                }
                else
                {
                    GlobalFree(hMem);
                }
            }
            CloseClipboard();
        }
    }

    void Editor::CutSelectionToClipboard()
    {
        std::wstring sel = GetSelectionText();
        if (sel.empty())
            return;

        if (OpenClipboard(NULL))
        {
            EmptyClipboard();
            SIZE_T bytes = (sel.size() + 1) * sizeof(wchar_t);
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (hMem)
            {
                void *ptr = GlobalLock(hMem);
                if (ptr)
                {
                    memcpy(ptr, sel.c_str(), bytes);
                    GlobalUnlock(hMem);
                    SetClipboardData(CF_UNICODETEXT, hMem);
                }
                else
                {
                    GlobalFree(hMem);
                }
            }
            CloseClipboard();
        }

        DeleteSelection();
    }
    // Helper
    static bool DecodeUtf8Strict(const char *src, int len, std::wstring &out)
    {
        out.clear();
        if (len <= 0)
            return true;

        int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, len, nullptr, 0);
        if (wlen <= 0)
            return false;

        out.resize((size_t)wlen);
        int wlen2 = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, len, out.data(), wlen);
        return wlen2 == wlen;
    }

    static void DecodeAnsi(const char *src, int len, std::wstring &out)
    {
        out.clear();
        if (len <= 0)
            return;

        int wlen = MultiByteToWideChar(CP_ACP, 0, src, len, nullptr, 0);
        if (wlen <= 0)
            return;

        out.resize((size_t)wlen);
        MultiByteToWideChar(CP_ACP, 0, src, len, out.data(), wlen);
    }

    static void SplitWLines(const std::wstring &w, std::vector<std::wstring> &outLines)
    {
        outLines.clear();
        size_t start = 0;
        size_t i = 0;
        const size_t n = w.size();

        while (i < n)
        {
            wchar_t c = w[i];
            if (c == L'\r' || c == L'\n')
            {
                outLines.emplace_back(w.substr(start, i - start));

                if (c == L'\r' && (i + 1) < n && w[i + 1] == L'\n')
                    i++;

                i++;
                start = i;
            }
            else
            {
                i++;
            }
        }

        outLines.emplace_back(w.substr(start));
        if (outLines.empty())
            outLines.push_back(L"");
    }

    void Orion::Editor::ApplyLoadedFile(std::wstring filePath,
                                        std::wstring encoding,
                                        std::vector<std::wstring> lines)
    {
        ResetPreview();
        ClearGitSplitDiffView();
        collapsedFolds_.clear();
        foldLineMapsDirty_ = true;
        gutterHoverLine_ = -1;
        state_.filePath = std::move(filePath);
        state_.encoding = std::move(encoding);

        state_.lines = std::move(lines);
        if (state_.lines.empty())
            state_.lines.push_back(L"");

        if (restoreViewAfterNextFileLoad_)
        {
            int maxLine = (int)state_.lines.size() - 1;
            int line = (std::max)(0, (std::min)(restoreCaretAfterNextFileLoad_.line, maxLine));
            int maxCol = (int)state_.lines[(size_t)line].size();
            int col = (std::max)(0, (std::min)(restoreCaretAfterNextFileLoad_.column, maxCol));
            state_.caret = {line, col};
            state_.hasSelection = false;

            state_.scrollOffsetX = (std::max)(0.0f, restoreScrollXAfterNextFileLoad_);
            scrollbar_.SetScrollOffset((std::max)(0.0f, restoreScrollYAfterNextFileLoad_));
            state_.scrollOffsetY = scrollbar_.GetScrollOffset();

            restoreViewAfterNextFileLoad_ = false;
        }
        else
        {
            state_.caret = {0, 0};
            state_.scrollOffsetX = 0.0f;
            state_.scrollOffsetY = 0.0f;
        }
        UpdateKnownFileWriteTime(state_.filePath);

    }
    void Orion::Editor::LoadFileAsync(HWND hwnd, const std::wstring &filePath, int tabIndex, bool preserveView)
    {
        if (!preserveView)
        {
            restoreViewAfterNextFileLoad_ = false;
            ResetPreview();
            ClearGitSplitDiffView();
            // UI : afficher un "loading" instantan?? (optionnel)
            state_.filePath = filePath;
            state_.lines.clear();
            state_.lines.push_back(L"// Loading...");
            state_.encoding = L"";
            state_.caret = {0, 0};
            state_.scrollOffsetX = 0.0f;
            state_.scrollOffsetY = 0.0f;
        }

        // Lancer le travail lourd en background
        std::wstring filePathCopy = filePath;

        std::thread([hwnd, tabIndex, filePathCopy]()
                    {
        auto* result = new Window::EditorFileLoadResult();
        result->tabIndex = tabIndex;
        result->filePath = filePathCopy;
        result->isPreview = false;

        // --- Convert path to UTF-8 (safe, with null terminator) ---
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, filePathCopy.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string pathUtf8;
        if (size_needed > 0)
        {
            pathUtf8.resize((size_t)size_needed);
            WideCharToMultiByte(CP_UTF8, 0, filePathCopy.c_str(), -1, pathUtf8.data(), size_needed, nullptr, nullptr);
            if (!pathUtf8.empty() && pathUtf8.back() == '\0')
                pathUtf8.pop_back();
        }

        std::ifstream file(pathUtf8, std::ios::binary);
        if (!file.is_open())
        {
            result->encoding = L"";
            result->lines = {L"// Could not open file"};
            PostMessageW(hwnd, WM_EDITOR_FILE_LOADED, 0, (LPARAM)result);
            return;
        }

        std::string data;
        file.seekg(0, std::ios::end);
        std::streamoff fsize = file.tellg();
        if (fsize > 0)
        {
            file.seekg(0, std::ios::beg);
            data.resize((size_t)fsize);
            file.read(&data[0], fsize);
        }
        file.close();

        const unsigned char* p = reinterpret_cast<const unsigned char*>(data.data());
        const size_t len = data.size();
        std::wstring decoded;

        if (len >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF)
        {
            result->encoding = L"UTF-8";
            const char* src = data.data() + 3;
            const int slen = (int)(len - 3);

            if (!DecodeUtf8Strict(src, slen, decoded))
            {
                int wlen = MultiByteToWideChar(CP_UTF8, 0, src, slen, nullptr, 0);
                decoded.resize((size_t)wlen);
                MultiByteToWideChar(CP_UTF8, 0, src, slen, decoded.data(), wlen);
            }
            SplitWLines(decoded, result->lines);
        }
        else if (len >= 2 && p[0] == 0xFF && p[1] == 0xFE)
        {
            result->encoding = L"UTF-16 LE";
            const size_t byteCount = len - 2;
            const size_t wcharCount = byteCount / 2;

            std::wstring w;
            w.resize(wcharCount);
            for (size_t i = 0; i < wcharCount; ++i)
            {
                unsigned char lo = p[2 + i * 2];
                unsigned char hi = p[2 + i * 2 + 1];
                w[i] = (wchar_t)((hi << 8) | lo);
            }
            SplitWLines(w, result->lines);
        }
        else if (len >= 2 && p[0] == 0xFE && p[1] == 0xFF)
        {
            result->encoding = L"UTF-16 BE";
            const size_t byteCount = len - 2;
            const size_t wcharCount = byteCount / 2;

            std::wstring w;
            w.resize(wcharCount);
            for (size_t i = 0; i < wcharCount; ++i)
            {
                unsigned char hi = p[2 + i * 2];
                unsigned char lo = p[2 + i * 2 + 1];
                w[i] = (wchar_t)((hi << 8) | lo);
            }
            SplitWLines(w, result->lines);
        }
        else
        {
            if (DecodeUtf8Strict(data.data(), (int)len, decoded))
            {
                result->encoding = L"UTF-8";
                SplitWLines(decoded, result->lines);
            }
            else
            {
                result->encoding = L"ANSI";
                DecodeAnsi(data.data(), (int)len, decoded);
                SplitWLines(decoded, result->lines);
            }
        }

        if (result->lines.empty())
            result->lines.push_back(L"");

        PostMessageW(hwnd, WM_EDITOR_FILE_LOADED, 0, (LPARAM)result); })
            .detach();
    }

    void Editor::PasteFromClipboard()
    {
        if (OpenClipboard(NULL))
        {
            HANDLE hData = GetClipboardData(CF_UNICODETEXT);
            if (hData)
            {
                wchar_t *clip = static_cast<wchar_t *>(GlobalLock(hData));
                if (clip)
                {
                    std::wstring text(clip);
                    GlobalUnlock(hData);

                    std::wstring tmp;
                    tmp.reserve(text.size());
                    for (size_t i = 0; i < text.size(); ++i)
                    {
                        if (text[i] == L'\r')
                            continue;
                        tmp.push_back(text[i]);
                    }

                    if (state_.hasSelection)
                    {
                        DeleteSelection();
                    }

                    std::vector<std::wstring> parts;
                    size_t start = 0;
                    while (start <= tmp.size())
                    {
                        size_t pos = tmp.find(L'\n', start);
                        if (pos == std::wstring::npos)
                        {
                            parts.push_back(tmp.substr(start));
                            break;
                        }
                        parts.push_back(tmp.substr(start, pos - start));
                        start = pos + 1;
                    }

                    if (parts.size() == 1)
                    {
                        state_.lines[state_.caret.line].insert(state_.caret.column, parts[0]);
                        state_.caret.column += (int)parts[0].size();
                    }
                    else if (!parts.empty())
                    {
                        std::wstring currentLine = state_.lines[state_.caret.line];
                        std::wstring before = currentLine.substr(0, state_.caret.column);
                        std::wstring after = currentLine.substr(state_.caret.column);

                        state_.lines[state_.caret.line] = before + parts[0];

                        int insertAt = state_.caret.line + 1;
                        for (size_t i = 1; i < parts.size(); ++i)
                        {
                            state_.lines.insert(state_.lines.begin() + insertAt, parts[i]);
                            insertAt++;
                        }

                        state_.lines[insertAt - 1] += after;
                        state_.caret.line = insertAt - 1;
                        state_.caret.column = (int)(state_.lines[state_.caret.line].size() - after.size());
                    }
                }
            }
            CloseClipboard();
        }
        // Mark document dirty after paste
        MarkDirty();
    }

    void Editor::DeleteSelection()
    {
        if (!state_.hasSelection)
            return;

        if (undoStack_.empty() || undoStack_.back().lines != state_.lines ||
            undoStack_.back().caret.line != state_.caret.line ||
            undoStack_.back().caret.column != state_.caret.column)
        {
            undoStack_.push_back(state_);
            if (undoStack_.size() > maxUndoEntries_)
                undoStack_.erase(undoStack_.begin());
        }

        CaretPosition start = state_.selectionStart;
        CaretPosition end = state_.caret;
        if (start.line > end.line || (start.line == end.line && start.column > end.column))
            std::swap(start, end);

        if (start.line == end.line)
        {
            state_.lines[start.line].erase(start.column, end.column - start.column);
        }
        else
        {
            std::wstring prefix = state_.lines[start.line].substr(0, start.column);
            std::wstring suffix;
            if (end.column < (int)state_.lines[end.line].size())
                suffix = state_.lines[end.line].substr(end.column);

            state_.lines[start.line] = prefix + suffix;

            if (end.line > start.line)
            {
                state_.lines.erase(state_.lines.begin() + start.line + 1,
                                   state_.lines.begin() + end.line + 1);
            }
        }

        state_.caret = start;
        state_.hasSelection = false;
        selectionExpandHistory_.clear();
        // Mark document dirty after deletion
        MarkDirty();
    }

    void Editor::DeleteSelectionPublic()
    {
        DeleteSelection();
    }

    bool Editor::HasNonEmptyContent() const
    {
        for (const auto &ln : state_.lines)
        {
            if (!ln.empty())
                return true;
        }
        return false;
    }

    bool Editor::Undo()
    {
        if (undoStack_.empty())
            return false;

        EditorState prev = undoStack_.back();
        undoStack_.pop_back();

        state_ = prev;
        state_.caretVisible = true;
        state_.lastBlinkTime = GetTickCount();
        collapsedFolds_.clear();
        foldLineMapsDirty_ = true;
        gutterHoverLine_ = -1;

        return true;
    }

    void Editor::SelectAll()
    {
        if (state_.lines.empty())
            return;

        state_.selectionStart = {0, 0};
        int lastLine = (int)state_.lines.size() - 1;
        int lastCol = (int)state_.lines[lastLine].size();
        state_.caret.line = lastLine;
        state_.caret.column = lastCol;
        state_.hasSelection = true;
        selectionExpandHistory_.clear();
        Orion::Caret::EnsureCaretVisible(state_, metrics_, scrollbar_);
    }

    void Editor::SelectCurrentLine()
    {
        if (state_.lines.empty())
            return;

        int line = (std::min)((std::max)(state_.caret.line, 0), (int)state_.lines.size() - 1);
        int len = (int)state_.lines[line].size();
        state_.selectionStart = {line, 0};
        state_.caret = {line, len};
        state_.hasSelection = true;
        selectionExpandHistory_.clear();
        Orion::Caret::EnsureCaretVisible(state_, metrics_, scrollbar_);
    }

    void Editor::ExpandSelection()
    {
        if (state_.lines.empty())
            return;

        auto clampPos = [this](CaretPosition p) -> CaretPosition
        {
            int line = (std::min)((std::max)(p.line, 0), (int)state_.lines.size() - 1);
            int col = (std::min)((std::max)(p.column, 0), (int)state_.lines[line].size());
            return {line, col};
        };

        CaretPosition curStart = state_.hasSelection ? state_.selectionStart : state_.caret;
        CaretPosition curEnd = state_.hasSelection ? state_.caret : state_.caret;
        curStart = clampPos(curStart);
        curEnd = clampPos(curEnd);
        NormalizeRange(curStart, curEnd);

        CaretPosition nextStart = curStart;
        CaretPosition nextEnd = curEnd;
        bool hasNonEmptySelection = !(curStart.line == curEnd.line && curStart.column == curEnd.column);

        if (!hasNonEmptySelection)
        {
            int line = curStart.line;
            const std::wstring &text = state_.lines[line];
            int len = (int)text.size();
            int anchorCol = (std::min)((std::max)(curStart.column, 0), len);

            if (len > 0)
            {
                int probe = anchorCol;
                if (probe >= len)
                    probe = len - 1;

                if (!IsWordChar(text[probe]) && probe > 0 && IsWordChar(text[probe - 1]))
                    probe -= 1;

                if (IsWordChar(text[probe]))
                {
                    int left = probe;
                    int right = probe + 1;
                    while (left > 0 && IsWordChar(text[left - 1]))
                        --left;
                    while (right < len && IsWordChar(text[right]))
                        ++right;
                    nextStart = {line, left};
                    nextEnd = {line, right};
                }
                else if (anchorCol < len)
                {
                    nextStart = {line, anchorCol};
                    nextEnd = {line, anchorCol + 1};
                }
                else
                {
                    nextStart = {line, 0};
                    nextEnd = {line, len};
                }
            }
        }
        else
        {
            bool isSingleLine = curStart.line == curEnd.line;
            bool isFullLine = false;
            if (isSingleLine)
            {
                int len = (int)state_.lines[curStart.line].size();
                isFullLine = (curStart.column == 0 && curEnd.column == len);
            }

            int lastLine = (int)state_.lines.size() - 1;
            int lastCol = (int)state_.lines[lastLine].size();
            bool isWholeDocument = (curStart.line == 0 && curStart.column == 0 &&
                                    curEnd.line == lastLine && curEnd.column == lastCol);

            if (!isFullLine && isSingleLine)
            {
                int len = (int)state_.lines[curStart.line].size();
                nextStart = {curStart.line, 0};
                nextEnd = {curStart.line, len};
            }
            else if (!isWholeDocument)
            {
                nextStart = {0, 0};
                nextEnd = {lastLine, lastCol};
            }
        }

        NormalizeRange(nextStart, nextEnd);
        if (nextStart.line == curStart.line && nextStart.column == curStart.column &&
            nextEnd.line == curEnd.line && nextEnd.column == curEnd.column)
        {
            return;
        }

        selectionExpandHistory_.push_back({curStart, curEnd});
        state_.selectionStart = nextStart;
        state_.caret = nextEnd;
        state_.hasSelection = !(nextStart.line == nextEnd.line && nextStart.column == nextEnd.column);
        Orion::Caret::EnsureCaretVisible(state_, metrics_, scrollbar_);
    }

    void Editor::ShrinkSelection()
    {
        if (!selectionExpandHistory_.empty())
        {
            auto clampPos = [this](CaretPosition p) -> CaretPosition
            {
                if (state_.lines.empty())
                    return {0, 0};
                int line = (std::min)((std::max)(p.line, 0), (int)state_.lines.size() - 1);
                int col = (std::min)((std::max)(p.column, 0), (int)state_.lines[line].size());
                return {line, col};
            };

            auto prev = selectionExpandHistory_.back();
            selectionExpandHistory_.pop_back();

            CaretPosition a = clampPos(prev.first);
            CaretPosition b = clampPos(prev.second);
            NormalizeRange(a, b);
            state_.selectionStart = a;
            state_.caret = b;
            state_.hasSelection = !(a.line == b.line && a.column == b.column);
            Orion::Caret::EnsureCaretVisible(state_, metrics_, scrollbar_);
            return;
        }

        if (state_.hasSelection)
        {
            CaretPosition a = state_.selectionStart;
            CaretPosition b = state_.caret;
            NormalizeRange(a, b);
            state_.caret = a;
            state_.hasSelection = false;
            Orion::Caret::EnsureCaretVisible(state_, metrics_, scrollbar_);
        }
    }

    bool Editor::MoveSelectionToFunction()
    {
        if (!state_.hasSelection || state_.lines.empty())
            return false;

        CaretPosition start = state_.selectionStart;
        CaretPosition end = state_.caret;
        NormalizeRange(start, end);

        int startLine = (std::max)(0, (std::min)(start.line, (int)state_.lines.size() - 1));
        int endLine = (std::max)(0, (std::min)(end.line, (int)state_.lines.size() - 1));
        if (end.column == 0 && endLine > startLine)
            endLine -= 1;
        if (endLine < startLine)
            return false;

        if (undoStack_.empty() || undoStack_.back().lines != state_.lines ||
            undoStack_.back().caret.line != state_.caret.line ||
            undoStack_.back().caret.column != state_.caret.column)
        {
            undoStack_.push_back(state_);
            if (undoStack_.size() > maxUndoEntries_)
                undoStack_.erase(undoStack_.begin());
        }

        std::vector<std::wstring> selectedLines;
        selectedLines.reserve((size_t)(endLine - startLine + 1));
        for (int line = startLine; line <= endLine; ++line)
            selectedLines.push_back(state_.lines[(size_t)line]);
        if (selectedLines.empty())
            return false;

        std::wstring baseIndent = LeadingIndent(state_.lines[(size_t)startLine]);
        std::wstring bodyIndent = baseIndent + L"    ";

        std::wstring functionName = L"extractedFunction";
        int suffix = 1;
        auto nameInUse = [&](const std::wstring &candidate) -> bool
        {
            for (const auto &line : state_.lines)
            {
                if (line.find(candidate) != std::wstring::npos)
                    return true;
            }
            return false;
        };
        while (nameInUse(functionName))
        {
            functionName = L"extractedFunction" + std::to_wstring(suffix++);
        }

        std::vector<std::wstring> replacement;
        replacement.reserve(selectedLines.size() + 4);
        replacement.push_back(baseIndent + L"auto " + functionName + L" = [&]()");
        replacement.push_back(baseIndent + L"{");
        for (const auto &raw : selectedLines)
        {
            std::wstring body = raw;
            if (!baseIndent.empty() && StartsWith(body, baseIndent))
                body = body.substr(baseIndent.size());
            replacement.push_back(bodyIndent + body);
        }
        replacement.push_back(baseIndent + L"};");
        replacement.push_back(baseIndent + functionName + L"();");

        state_.lines.erase(state_.lines.begin() + startLine, state_.lines.begin() + endLine + 1);
        state_.lines.insert(state_.lines.begin() + startLine, replacement.begin(), replacement.end());

        int callLine = startLine + (int)replacement.size() - 1;
        state_.selectionStart = {startLine, 0};
        state_.caret = {callLine, (int)state_.lines[(size_t)callLine].size()};
        state_.hasSelection = false;
        selectionExpandHistory_.clear();
        secondaryCarets_.clear();
        state_.caretVisible = true;
        state_.lastBlinkTime = GetTickCount();
        Orion::Caret::EnsureCaretVisible(state_, metrics_, scrollbar_);
        MarkDirty();
        return true;
    }
}
