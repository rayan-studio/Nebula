#include "orion/editor/Editor.h"
#include "orion/caret/Caret.h"
#include <algorithm>
#include <string>
#include <vector>

// Ensure Windows min/max macros don't interfere with std::min/std::max
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

namespace Orion
{
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

    void Editor::Undo()
    {
        if (undoStack_.empty())
            return;

        EditorState prev = undoStack_.back();
        undoStack_.pop_back();
        state_ = prev;
        state_.caretVisible = true;
        state_.lastBlinkTime = GetTickCount();
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
        Orion::Caret::EnsureCaretVisible(state_, metrics_, scrollbar_);
    }
}
