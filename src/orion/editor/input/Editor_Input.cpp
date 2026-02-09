#include "orion/editor/Editor.h"

#include "../../completion/popup/Popup.h"
#include "orion/caret/Caret.h"
#include "lsp/LspManager.h"

#include <algorithm>
#include <cmath>

// Ensure Windows min/max macros don't interfere with std::min/std::max
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

namespace Orion
{
    bool Editor::GetWordAtColumn(const std::wstring &line, int column, std::wstring &outWord, int &startCol, int &endCol) const
    {
        outWord.clear();
        startCol = -1;
        endCol = -1;
        if (line.empty())
            return false;

        auto isWordChar = [](wchar_t c)
        {
            return (iswalnum(c) != 0) || (c == L'_');
        };

        int col = column;
        if (col < 0)
            col = 0;
        if (col >= (int)line.size())
            col = (int)line.size() - 1;

        if (col < 0 || col >= (int)line.size())
            return false;

        if (!isWordChar(line[col]))
            return false;

        int left = col;
        while (left > 0 && isWordChar(line[left - 1]))
            --left;
        int right = col;
        while (right + 1 < (int)line.size() && isWordChar(line[right + 1]))
            ++right;

        if (right < left)
            return false;

        outWord = line.substr(left, right - left + 1);
        startCol = left;
        endCol = right + 1;
        return !outWord.empty();
    }

    static std::wstring StripLineCommentsAndStrings(const std::wstring &line)
    {
        std::wstring out = line;
        bool inString = false;
        bool inChar = false;
        bool escape = false;
        for (size_t i = 0; i < out.size(); ++i)
        {
            wchar_t c = out[i];
            if (escape)
            {
                escape = false;
                out[i] = L' ';
                continue;
            }
            if (c == L'\\')
            {
                if (inString || inChar)
                {
                    escape = true;
                    out[i] = L' ';
                }
                continue;
            }
            if (!inChar && c == L'"')
            {
                inString = !inString;
                out[i] = L' ';
                continue;
            }
            if (!inString && c == L'\'')
            {
                inChar = !inChar;
                out[i] = L' ';
                continue;
            }
            if (!inString && !inChar && c == L'/' && i + 1 < out.size() && out[i + 1] == L'/')
            {
                for (size_t k = i; k < out.size(); ++k)
                    out[k] = L' ';
                break;
            }
            if (inString || inChar)
                out[i] = L' ';
        }
        return out;
    }

    static bool IsIdentifierChar(wchar_t c)
    {
        return (iswalnum(c) != 0) || (c == L'_');
    }

    bool Editor::FindLocalDefinition(const std::wstring &word, int fromLine, int &outLine, int &outCol) const
    {
        outLine = -1;
        outCol = -1;
        if (word.empty())
            return false;

        const std::wstring modifiers[] = {
            L"const", L"static", L"constexpr", L"volatile", L"extern", L"register",
            L"mutable", L"inline", L"typedef", L"using"};
        const std::wstring keywords[] = {
            L"if", L"for", L"while", L"switch", L"return", L"case", L"catch",
            L"sizeof", L"new", L"delete"};

        auto isModifier = [&](const std::wstring &tok) -> bool
        {
            for (const auto &m : modifiers)
                if (tok == m)
                    return true;
            return false;
        };
        auto isKeyword = [&](const std::wstring &tok) -> bool
        {
            for (const auto &k : keywords)
                if (tok == k)
                    return true;
            return false;
        };
        auto isTypeToken = [&](const std::wstring &tok) -> bool
        {
            if (tok.empty())
                return false;
            if (tok == L"auto" || tok == L"int" || tok == L"float" || tok == L"double" ||
                tok == L"char" || tok == L"bool" || tok == L"short" || tok == L"long" ||
                tok == L"unsigned" || tok == L"signed" || tok == L"size_t")
                return true;
            if (tok.find(L"::") != std::wstring::npos)
                return true;
            if (iswupper(tok[0]))
                return true;
            return false;
        };

        for (int li = fromLine; li >= 0; --li)
        {
            if (li < 0 || li >= (int)state_.lines.size())
                continue;
            std::wstring raw = state_.lines[li];
            if (raw.find(L"#include") != std::wstring::npos)
                continue;
            std::wstring line = StripLineCommentsAndStrings(raw);

            size_t pos = 0;
            while (true)
            {
                pos = line.find(word, pos);
                if (pos == std::wstring::npos)
                    break;
                size_t end = pos + word.size();
                bool leftOk = (pos == 0) || !IsIdentifierChar(line[pos - 1]);
                bool rightOk = (end >= line.size()) || !IsIdentifierChar(line[end]);
                if (!leftOk || !rightOk)
                {
                    pos = end;
                    continue;
                }

                // avoid member access or scope resolution
                size_t p = pos;
                while (p > 0 && iswspace(line[p - 1]))
                    --p;
                if (p > 0)
                {
                    wchar_t prev = line[p - 1];
                    if (prev == L'.' || prev == L':' || prev == L'>')
                    {
                        pos = end;
                        continue;
                    }
                }

                // must look like a declaration: needs '=' or ';' after the word
                size_t semi = line.find(L';', end);
                size_t eq = line.find(L'=', end);
                if (semi == std::wstring::npos && eq == std::wstring::npos)
                {
                    pos = end;
                    continue;
                }

                // find token before word (skip pointers/refs/spaces)
                size_t j = pos;
                while (j > 0 && (iswspace(line[j - 1]) || line[j - 1] == L'*' || line[j - 1] == L'&'))
                    --j;
                if (j == 0)
                {
                    pos = end;
                    continue;
                }

                size_t tokEnd = j;
                size_t tokStart = tokEnd;
                while (tokStart > 0 && (IsIdentifierChar(line[tokStart - 1]) || line[tokStart - 1] == L':'))
                    --tokStart;
                std::wstring tok = line.substr(tokStart, tokEnd - tokStart);
                if (tok.size() >= 2 && tok.back() == L':' && tok[tok.size() - 2] == L':')
                    tok.pop_back();

                // handle modifiers like "const", "static"
                while (isModifier(tok) && tokStart > 0)
                {
                    size_t k = tokStart;
                    while (k > 0 && (iswspace(line[k - 1]) || line[k - 1] == L'*' || line[k - 1] == L'&'))
                        --k;
                    if (k == 0)
                        break;
                    size_t tEnd = k;
                    size_t tStart = tEnd;
                    while (tStart > 0 && (IsIdentifierChar(line[tStart - 1]) || line[tStart - 1] == L':'))
                        --tStart;
                    tok = line.substr(tStart, tEnd - tStart);
                    tokStart = tStart;
                }

                if (!tok.empty() && !isKeyword(tok) && isTypeToken(tok))
                {
                    outLine = li;
                    outCol = (int)pos;
                    return true;
                }

                pos = end;
            }
        }
        return false;
    }

    static bool GetIncludePathRange(const std::wstring &line, int column, int &startCol, int &endCol)
    {
        startCol = -1;
        endCol = -1;

        size_t incPos = line.find(L"#include");
        if (incPos == std::wstring::npos)
            return false;

        size_t q1 = line.find(L'"', incPos);
        size_t q2 = (q1 != std::wstring::npos) ? line.find(L'"', q1 + 1) : std::wstring::npos;
        size_t a1 = line.find(L'<', incPos);
        size_t a2 = (a1 != std::wstring::npos) ? line.find(L'>', a1 + 1) : std::wstring::npos;

        size_t start = std::wstring::npos;
        size_t end = std::wstring::npos;
        if (q1 != std::wstring::npos && q2 != std::wstring::npos && q2 > q1 + 1)
        {
            start = q1 + 1;
            end = q2;
        }
        else if (a1 != std::wstring::npos && a2 != std::wstring::npos && a2 > a1 + 1)
        {
            start = a1 + 1;
            end = a2;
        }

        if (start == std::wstring::npos || end == std::wstring::npos)
            return false;

        if (column < (int)start || column > (int)end)
            return false;

        startCol = (int)start;
        endCol = (int)end;
        return true;
    }

    std::optional<Lsp::Location> Editor::TryGoToDefinitionAtCaret()
    {
        if (isPreview_ || state_.lines.empty())
            return std::nullopt;

        CaretPosition caretPos = state_.caret;
        if (caretPos.line < 0 || caretPos.line >= (int)state_.lines.size())
            return std::nullopt;
        if (caretPos.column < 0)
            caretPos.column = 0;
        if (caretPos.column > (int)state_.lines[caretPos.line].size())
            caretPos.column = (int)state_.lines[caretPos.line].size();

        const std::wstring &ln = state_.lines[caretPos.line];

        int incStart = -1;
        int incEnd = -1;
        if (GetIncludePathRange(ln, caretPos.column, incStart, incEnd))
            return Lsp::LspManager::Instance().GoToDefinition(state_.filePath, ln, caretPos.line, caretPos.column, L"");

        std::wstring word;
        int startCol = -1;
        int endCol = -1;
        if (!GetWordAtColumn(ln, caretPos.column, word, startCol, endCol))
            return std::nullopt;

        int defLine = -1;
        int defCol = -1;
        if (FindLocalDefinition(word, caretPos.line, defLine, defCol))
            return Lsp::Location{state_.filePath, defLine, defCol};

        return Lsp::LspManager::Instance().GoToDefinition(state_.filePath, ln, caretPos.line, caretPos.column, word);
    }

    std::optional<Lsp::Location> Editor::TryGoToDefinitionAtPoint(POINT pt)
    {
        if (isPreview_)
            return std::nullopt;

        float contentLeft = state_.leftEdge + metrics_.gutterWidth + metrics_.leftPadding;
        if (pt.x < (int)contentLeft || pt.x > (int)state_.rightEdge || pt.y < (int)state_.topEdge || pt.y > (int)state_.bottomEdge)
            return std::nullopt;

        if (state_.lines.empty())
            return std::nullopt;

        CaretPosition clickedPos = ScreenToTextPosition(pt);
        if (clickedPos.line < 0 || clickedPos.line >= (int)state_.lines.size())
            return std::nullopt;
        if (clickedPos.column < 0)
            clickedPos.column = 0;
        if (clickedPos.column > (int)state_.lines[clickedPos.line].size())
            clickedPos.column = (int)state_.lines[clickedPos.line].size();

        const std::wstring &ln = state_.lines[clickedPos.line];

        int incStart = -1;
        int incEnd = -1;
        if (GetIncludePathRange(ln, clickedPos.column, incStart, incEnd))
        {
            return Lsp::LspManager::Instance().GoToDefinition(state_.filePath, ln, clickedPos.line, clickedPos.column, L"");
        }

        std::wstring word;
        int startCol = -1;
        int endCol = -1;
        if (!GetWordAtColumn(ln, clickedPos.column, word, startCol, endCol))
            return std::nullopt;

        int defLine = -1;
        int defCol = -1;
        if (FindLocalDefinition(word, clickedPos.line, defLine, defCol))
        {
            return Lsp::Location{state_.filePath, defLine, defCol};
        }

        return Lsp::LspManager::Instance().GoToDefinition(state_.filePath, ln, clickedPos.line, clickedPos.column, word);
    }

    void Editor::OnLeftButtonDown(HWND hwnd, POINT pt)
    {
        (void)hwnd;
        if (isPreview_)
        {
            if (previewMode_ == PreviewMode::Markdown)
            {
                if (scrollbar_.OnLeftButtonDown(pt))
                    SetCapture(hwnd);
            }
            return;
        }

        dragStartPos_ = pt;
        dragSelecting_ = false;

        // Check scrollbar interaction first
        if (scrollbar_.OnLeftButtonDown(pt))
        {
            SetCapture(hwnd);
            return;
        }

        // Horizontal scrollbar hit test
        if (hScrollbarVisible_)
        {
            float hLeft = state_.leftEdge + metrics_.gutterWidth;
            float hRight = state_.rightEdge - (scrollbar_.IsVisible() ? 14.0f : 0.0f);
            float hTop = state_.bottomEdge - 14.0f;
            float hBottom = state_.bottomEdge;
            float thumbLeft = hLeft + hThumbPos_;
            float thumbRight = thumbLeft + hThumbWidth_;

            if (pt.x >= (int)thumbLeft && pt.x <= (int)thumbRight && pt.y >= (int)hTop && pt.y <= (int)hBottom)
            {
                hIsDragging_ = true;
                hDragStartX_ = pt.x;
                hDragStartOffset_ = state_.scrollOffsetX;
                SetCapture(hwnd);
                return;
            }

            if (pt.x >= (int)hLeft && pt.x <= (int)hRight && pt.y >= (int)hTop && pt.y <= (int)hBottom)
            {
                // Click on track -> page left/right
                if (pt.x < thumbLeft)
                    state_.scrollOffsetX = (std::max)(0.0f, state_.scrollOffsetX - hViewportWidth_ * 0.8f);
                else
                    state_.scrollOffsetX = (std::min)(hContentWidth_ - hViewportWidth_, state_.scrollOffsetX + hViewportWidth_ * 0.8f);

                // update thumb
                float maxScroll = hContentWidth_ - hViewportWidth_;
                float availableTrack = hViewportWidth_ - hThumbWidth_;
                if (maxScroll > 0.0f)
                    hThumbPos_ = (state_.scrollOffsetX / maxScroll) * availableTrack;
                else
                    hThumbPos_ = 0.0f;

                InvalidateRect(hwnd, nullptr, FALSE);
                return;
            }
        }

        // SearchBox handling
        if (searchBox_.IsVisible())
        {
            bool inside = searchBox_.IsPointInSearchBox(pt);
            if (inside)
            {
                searchBox_.OnLeftButtonDown(hwnd, pt);
                if (searchBox_.ConsumeReplaceRequest())
                {
                    ReplaceCurrentMatch();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return;
            }
        }

        if (searchBox_.IsVisible())
        {
            searchBox_.SetInputFocused(false);
        }

        // Completion popup
        if (completionPopup_ && completionPopup_->IsVisible())
        {
            if (completionPopup_->IsPointInPopup(pt))
            {
                completionPopup_->OnLeftButtonDown(pt);
                std::wstring chosen = completionPopup_->GetSelectedItem();
                if (!chosen.empty())
                {
                    std::wstring &line = state_.lines[state_.caret.line];
                    size_t pos = line.rfind(L"#include");
                    if (pos != std::wstring::npos)
                    {
                        size_t lt = line.find_last_of(L"<", state_.caret.column - 1);
                        size_t qt = line.find_last_of(L'"', state_.caret.column - 1);
                        size_t start = std::wstring::npos;
                        if (lt != std::wstring::npos && lt > pos)
                            start = lt + 1;
                        if (qt != std::wstring::npos && qt > pos)
                            start = (start == std::wstring::npos) ? qt + 1 : std::min(start, qt + 1);
                        if (start == std::wstring::npos)
                            start = state_.caret.column;

                        size_t caretPos = (size_t)state_.caret.column;
                        if (caretPos > start)
                            line.erase(start, caretPos - start);

                        line.insert(start, chosen);
                        state_.caret.column = (int)(start + chosen.size());
                    }
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return;
            }
            else
            {
                completionPopup_->Hide();
            }
        }

        float contentLeft = state_.leftEdge + metrics_.gutterWidth + metrics_.leftPadding;
        bool isInEditorArea = !(pt.x < contentLeft || pt.x > state_.rightEdge || pt.y < state_.topEdge || pt.y > state_.bottomEdge);
        if (!isInEditorArea)
            return;

        // safety
        if (state_.lines.empty())
        {
            state_.caret = {0, 0};
            state_.hasSelection = false;
            return;
        }

        // Determine clicked text position
        CaretPosition clickedPos = ScreenToTextPosition(pt);

        // Clamp
        if (clickedPos.line < 0 || clickedPos.line >= (int)state_.lines.size())
            clickedPos.line = (std::max)(0, (std::min)(clickedPos.line, (int)state_.lines.size() - 1));
        if (clickedPos.column < 0)
            clickedPos.column = 0;
        if (clickedPos.line >= 0 && clickedPos.line < (int)state_.lines.size())
            clickedPos.column = (std::min)(clickedPos.column, (int)state_.lines[clickedPos.line].size());

        bool altPressed = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        bool ctrlPressed = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        if (altPressed && !ctrlPressed)
        {
            state_.hasSelection = false;

            // Move current primary caret to secondary list, then set new primary at clicked position.
            if (!(state_.caret.line == clickedPos.line && state_.caret.column == clickedPos.column))
            {
                secondaryCarets_.push_back(state_.caret);
                state_.caret = clickedPos;
            }

            NormalizeSecondaryCarets();
            state_.caretVisible = true;
            state_.lastBlinkTime = GetTickCount();
            Orion::Caret::EnsureCaretVisible(state_, metrics_, scrollbar_);
            if (hwnd)
                InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }

        // Any non-Alt click exits multi-caret mode.
        secondaryCarets_.clear();

        if (ctrlPressed && clickedPos.line >= 0 && clickedPos.line < (int)state_.lines.size())
        {
            const std::wstring &ln = state_.lines[clickedPos.line];
            std::wstring word;
            int startCol = -1;
            int endCol = -1;
            GetWordAtColumn(ln, clickedPos.column, word, startCol, endCol);

            auto loc = Lsp::LspManager::Instance().GoToDefinition(state_.filePath, ln, clickedPos.line, clickedPos.column, word);
            if (loc.has_value())
            {
                auto *heapPath = new std::wstring(loc->filePath);
                PostMessageW(hwnd, WM_USER + 100, (WPARAM)loc->line, (LPARAM)heapPath);
                return;
            }
        }

        // Click count logic
        DWORD now = GetTickCount();
        UINT dblTime = GetDoubleClickTime();
        int dblWidth = GetSystemMetrics(SM_CXDOUBLECLK);
        int dblHeight = GetSystemMetrics(SM_CYDOUBLECLK);
        bool withinDoubleClickArea = std::abs(pt.x - lastClickPos_.x) <= dblWidth
            && std::abs(pt.y - lastClickPos_.y) <= dblHeight;
        if (now - lastClickTime_ <= dblTime && withinDoubleClickArea)
            clickCount_ = (clickCount_ < 3) ? clickCount_ + 1 : 1;
        else
            clickCount_ = 1;

        lastClickTime_ = now;
        lastClickPos_ = pt;

        bool shiftPressed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

        if (shiftPressed)
        {
            if (!state_.hasSelection)
                state_.selectionStart = state_.caret;

            state_.caret = clickedPos;
            state_.hasSelection = true;
            state_.caretVisible = true;
            state_.lastBlinkTime = GetTickCount();
            Orion::Caret::EnsureCaretVisible(state_, metrics_, scrollbar_);
            if (hwnd)
                InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }

        // double/triple click selection
        if (clickCount_ == 2 || clickCount_ == 3)
        {
            int line = clickedPos.line;
            if (line < 0 || line >= (int)state_.lines.size())
                return;

            const std::wstring &ln = state_.lines[line];

            if (clickCount_ == 3)
            {
                state_.selectionStart = {line, 0};
                state_.caret = {line, (int)ln.size()};
                state_.hasSelection = true;
                state_.caretVisible = true;
                state_.lastBlinkTime = GetTickCount();
                if (hwnd)
                    InvalidateRect(hwnd, nullptr, FALSE);
                return;
            }

            if (ln.empty())
            {
                state_.selectionStart = {line, 0};
                state_.caret = {line, 0};
                state_.hasSelection = true;
                state_.caretVisible = true;
                state_.lastBlinkTime = GetTickCount();
                if (hwnd)
                    InvalidateRect(hwnd, nullptr, FALSE);
                return;
            }

            auto isWordChar = [](wchar_t c)
            {
                return (iswalnum(c) != 0) || (c == L'_');
            };

            int col = clickedPos.column;
            if (col < 0) col = 0;
            if (col > (int)ln.size()) col = (int)ln.size();

            int idx = col;
            if (idx == (int)ln.size())
                idx = (int)ln.size() - 1;

            if (idx < 0 || idx >= (int)ln.size())
            {
                state_.selectionStart = {line, 0};
                state_.caret = {line, (int)ln.size()};
                state_.hasSelection = true;
                state_.caretVisible = true;
                state_.lastBlinkTime = GetTickCount();
                Orion::Caret::EnsureCaretVisible(state_, metrics_, scrollbar_);
                if (hwnd)
                    InvalidateRect(hwnd, nullptr, FALSE);
                return;
            }

            if (!isWordChar(ln[idx]))
            {
                if (idx > 0 && isWordChar(ln[idx - 1]))
                    idx = idx - 1;
            }

            int left = idx;
            while (left > 0 && isWordChar(ln[left - 1]))
                --left;
            int right = idx;
            while (right + 1 < (int)ln.size() && isWordChar(ln[right + 1]))
                ++right;

            if (left > right)
            {
                left = idx;
                right = idx;
            }

            state_.selectionStart = {line, left};
            state_.caret = {line, right + 1};
            state_.hasSelection = true;
            state_.caretVisible = true;
            state_.lastBlinkTime = GetTickCount();
            Orion::Caret::EnsureCaretVisible(state_, metrics_, scrollbar_);
            if (hwnd)
                InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }

        // Normal click
        state_.caret = clickedPos;
        Orion::Caret::EnsureCaretVisible(state_, metrics_, scrollbar_);
        state_.hasSelection = false;
        state_.caretVisible = true;
        state_.lastBlinkTime = GetTickCount();
    }

    void Editor::OnMouseMove(HWND hwnd, POINT pt)
    {
        if (isPreview_)
        {
            if (previewMode_ == PreviewMode::Markdown)
            {
                if (scrollbar_.OnMouseMove(pt))
                {
                    state_.scrollOffsetY = scrollbar_.GetScrollOffset();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return;
        }
        (void)hwnd;
        (void)hwnd;

        // vertical scrollbar
        if (scrollbar_.OnMouseMove(pt))
        {
            state_.scrollOffsetY = scrollbar_.GetScrollOffset();
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }

        // horizontal scrollbar drag
        if (hIsDragging_)
        {
            float availableTrack = hViewportWidth_ - hThumbWidth_;
            int deltaX = pt.x - hDragStartX_;
            float maxScroll = hContentWidth_ - hViewportWidth_;

            if (availableTrack > 0.0f && maxScroll > 0.0f)
            {
                float scrollDelta = (deltaX / availableTrack) * maxScroll;
                state_.scrollOffsetX = hDragStartOffset_ + scrollDelta;

                if (state_.scrollOffsetX < 0.0f) state_.scrollOffsetX = 0.0f;
                if (state_.scrollOffsetX > maxScroll) state_.scrollOffsetX = maxScroll;

                hThumbPos_ = (state_.scrollOffsetX / maxScroll) * availableTrack;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return;
        }

        // editor area check
        float contentLeft = state_.leftEdge + metrics_.gutterWidth + metrics_.leftPadding;
        bool isInEditorArea = !(pt.x < contentLeft || pt.x > state_.rightEdge || pt.y < state_.topEdge || pt.y > state_.bottomEdge);

        if (!isInEditorArea && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000))
            return;

        if (GetAsyncKeyState(VK_LBUTTON) & 0x8000)
        {
            if (!dragSelecting_)
            {
                int dx = std::abs(pt.x - dragStartPos_.x);
                int dy = std::abs(pt.y - dragStartPos_.y);
                int threshX = GetSystemMetrics(SM_CXDRAG);
                int threshY = GetSystemMetrics(SM_CYDRAG);
                if (dx < threshX && dy < threshY)
                    return;
                dragSelecting_ = true;
            }

            if (pt.x < (int)(state_.leftEdge + metrics_.gutterWidth + metrics_.leftPadding))
                return;

            if (!state_.hasSelection)
            {
                state_.selectionStart = state_.caret;
                state_.hasSelection = true;
            }

            bool needsRedraw = false;

            // auto-scroll selection drag
            if (pt.y < (int)state_.topEdge + 2)
            {
                float over = ((float)state_.topEdge + 2.0f) - (float)pt.y;
                float speed = (std::max)(4.0f, over * 0.5f);
                scrollbar_.ScrollBy(-speed);
                state_.scrollOffsetY = scrollbar_.GetScrollOffset();
                needsRedraw = true;
            }
            else if (pt.y > (int)state_.bottomEdge - 2)
            {
                float over = (float)pt.y - ((float)state_.bottomEdge - 2.0f);
                float speed = (std::max)(4.0f, over * 0.5f);
                scrollbar_.ScrollBy(speed);
                state_.scrollOffsetY = scrollbar_.GetScrollOffset();
                needsRedraw = true;
            }

            CaretPosition newPos = ScreenToTextPosition(pt);

            if (newPos.line < 0) newPos.line = 0;
            if (newPos.line >= (int)state_.lines.size()) newPos.line = (int)state_.lines.size() - 1;

            if (newPos.line >= 0 && newPos.line < (int)state_.lines.size())
            {
                int maxCol = (int)state_.lines[newPos.line].size();
                if (newPos.column < 0) newPos.column = 0;
                if (newPos.column > maxCol) newPos.column = maxCol;
            }

            if (newPos.line != state_.caret.line || newPos.column != state_.caret.column)
            {
                state_.caret = newPos;
                state_.caretVisible = true;
                state_.lastBlinkTime = GetTickCount();
                needsRedraw = true;
            }

            if (needsRedraw)
                InvalidateRect(hwnd, nullptr, FALSE);
        }

        // Diagnostics hover tooltip (only when not dragging)
        if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000))
        {
            bool prevVisible = diagHoverVisible_;
            std::wstring newText;

            CaretPosition hoverPos = ScreenToTextPosition(pt);
            if (hoverPos.line >= 0 && hoverPos.line < (int)state_.lines.size())
            {
                auto diagnostics = GetDiagnostics();
                for (const auto &d : diagnostics)
                {
                    if (d.line != hoverPos.line)
                        continue;
                    int start = d.startCol;
                    int end = d.endCol;
                    if (end <= start)
                        end = start + 1;

                    float x1 = contentLeft + (float)start * metrics_.characterWidth - state_.scrollOffsetX;
                    float x2 = contentLeft + (float)end * metrics_.characterWidth - state_.scrollOffsetX;
                    float y1 = state_.topEdge + (float)hoverPos.line * metrics_.lineHeight - state_.scrollOffsetY;
                    float y2 = y1 + metrics_.lineHeight;

                    if (pt.x >= (int)x1 && pt.x <= (int)x2 && pt.y >= (int)y1 && pt.y <= (int)y2)
                    {
                        newText = d.message;
                        if (!d.suggestion.empty())
                        {
                            newText += L"\nSuggestion: " + d.suggestion;
                        }
                        break;
                    }
                }
            }

            diagHoverVisible_ = !newText.empty();
            diagHoverText_ = newText;
            diagHoverPos_ = pt;

            if (prevVisible != diagHoverVisible_)
                InvalidateRect(hwnd, nullptr, FALSE);
        }

        // Ctrl+hover definition underline
        {
            bool ctrlPressed = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool updateHover = false;

            if (!ctrlPressed || !isInEditorArea || (GetAsyncKeyState(VK_LBUTTON) & 0x8000))
            {
                if (defHoverActive_)
                {
                    defHoverActive_ = false;
                    defHoverLine_ = -1;
                    defHoverStart_ = -1;
                    defHoverEnd_ = -1;
                    defHoverWord_.clear();
                    defHoverLocation_.reset();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            else
            {
                CaretPosition hoverPos = ScreenToTextPosition(pt);
                if (hoverPos.line >= 0 && hoverPos.line < (int)state_.lines.size())
                {
                    const std::wstring &ln = state_.lines[hoverPos.line];
                    std::wstring word;
                    int startCol = -1;
                    int endCol = -1;
                    int incStart = -1;
                    int incEnd = -1;
                    if (GetIncludePathRange(ln, hoverPos.column, incStart, incEnd))
                    {
                        auto loc = Lsp::LspManager::Instance().GoToDefinition(state_.filePath, ln, hoverPos.line, hoverPos.column, L"");
                        if (loc.has_value())
                        {
                            defHoverActive_ = true;
                            defHoverLine_ = hoverPos.line;
                            defHoverStart_ = incStart;
                            defHoverEnd_ = incEnd;
                            defHoverWord_.clear();
                            defHoverLocation_ = loc;
                        }
                        else
                        {
                            defHoverActive_ = false;
                            defHoverLine_ = -1;
                            defHoverStart_ = -1;
                            defHoverEnd_ = -1;
                            defHoverWord_.clear();
                            defHoverLocation_.reset();
                        }
                        updateHover = true;
                    }
                    else if (GetWordAtColumn(ln, hoverPos.column, word, startCol, endCol))
                    {
                        bool foundLocal = false;
                        int defLine = -1;
                        int defCol = -1;
                        if (FindLocalDefinition(word, hoverPos.line, defLine, defCol))
                        {
                            defHoverActive_ = true;
                            defHoverLine_ = hoverPos.line;
                            defHoverStart_ = startCol;
                            defHoverEnd_ = endCol;
                            defHoverWord_ = word;
                            defHoverLocation_ = Lsp::Location{state_.filePath, defLine, defCol};
                            updateHover = true;
                            foundLocal = true;
                        }

                        if (!foundLocal && (hoverPos.line != defHoverLine_ || startCol != defHoverStart_ || endCol != defHoverEnd_ || word != defHoverWord_))
                        {
                            auto loc = Lsp::LspManager::Instance().GoToDefinition(state_.filePath, ln, hoverPos.line, hoverPos.column, word);
                            if (loc.has_value())
                            {
                                defHoverActive_ = true;
                                defHoverLine_ = hoverPos.line;
                                defHoverStart_ = startCol;
                                defHoverEnd_ = endCol;
                                defHoverWord_ = word;
                                defHoverLocation_ = loc;
                            }
                            else
                            {
                                defHoverActive_ = false;
                                defHoverLine_ = -1;
                                defHoverStart_ = -1;
                                defHoverEnd_ = -1;
                                defHoverWord_.clear();
                                defHoverLocation_.reset();
                            }
                            updateHover = true;
                        }
                    }
                    else if (defHoverActive_)
                    {
                        defHoverActive_ = false;
                        defHoverLine_ = -1;
                        defHoverStart_ = -1;
                        defHoverEnd_ = -1;
                        defHoverWord_.clear();
                        defHoverLocation_.reset();
                        updateHover = true;
                    }
                }
            }

            if (updateHover && hwnd)
                InvalidateRect(hwnd, nullptr, FALSE);
        }

        if (completionPopup_ && completionPopup_->IsVisible())
        {
            completionPopup_->OnMouseMove(pt);
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
    }

    void Editor::OnLeftButtonUp(HWND hwnd, POINT pt)
    {
        if (isPreview_)
        {
            if (previewMode_ == PreviewMode::Markdown)
            {
                if (scrollbar_.OnLeftButtonUp())
                    ReleaseCapture();
            }
            return;
        }
        (void)hwnd;
        (void)pt;

        dragSelecting_ = false;

        if (scrollbar_.OnLeftButtonUp())
        {
            ReleaseCapture();
            return;
        }

        if (hIsDragging_)
        {
            hIsDragging_ = false;
            ReleaseCapture();
            return;
        }

        if (completionPopup_ && completionPopup_->IsVisible())
        {
            completionPopup_->OnLeftButtonUp();
        }
    }

    void Editor::OnMouseWheel(HWND hwnd, int delta, bool ctrlPressed)
    {
        if (isPreview_)
        {
            if (previewMode_ == PreviewMode::Markdown)
            {
                if (scrollbar_.OnMouseWheel(delta))
                {
                    state_.scrollOffsetY = scrollbar_.GetScrollOffset();
                    if (hwnd)
                        InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return;
        }
        (void)hwnd;

        bool shiftPressed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (shiftPressed && hScrollbarVisible_)
        {
            float scrollAmount = (delta / 120.0f) * 40.0f;
            state_.scrollOffsetX += scrollAmount;

            float maxScroll = hContentWidth_ - hViewportWidth_;
            if (state_.scrollOffsetX < 0.0f) state_.scrollOffsetX = 0.0f;
            if (state_.scrollOffsetX > maxScroll) state_.scrollOffsetX = maxScroll;

            float availableTrack = hViewportWidth_ - hThumbWidth_;
            if (maxScroll > 0.0f)
                hThumbPos_ = (state_.scrollOffsetX / maxScroll) * availableTrack;

            if (hwnd)
                InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }

        (void)ctrlPressed;

        if (completionPopup_ && completionPopup_->IsVisible())
        {
            POINT cur;
            GetCursorPos(&cur);
            ScreenToClient(hwnd, &cur);
            if (completionPopup_->IsPointInPopup(cur))
            {
                if (completionPopup_->OnMouseWheel(delta))
                {
                    if (hwnd)
                        InvalidateRect(hwnd, nullptr, FALSE);
                    return;
                }
            }
        }

        if (scrollbar_.OnMouseWheel(delta))
        {
            state_.scrollOffsetY = scrollbar_.GetScrollOffset();
            if (hwnd)
                InvalidateRect(hwnd, nullptr, FALSE);
        }
    }

    void Editor::OnHorizontalWheel(HWND hwnd, int delta)
    {
        if (isPreview_)
            return;
        if (!hScrollbarVisible_)
            return;

        float scrollAmount = (delta / 120.0f) * 40.0f;
        state_.scrollOffsetX += scrollAmount;

        float maxScroll = hContentWidth_ - hViewportWidth_;
        if (state_.scrollOffsetX < 0.0f) state_.scrollOffsetX = 0.0f;
        if (state_.scrollOffsetX > maxScroll) state_.scrollOffsetX = maxScroll;

        float availableTrack = hViewportWidth_ - hThumbWidth_;
        if (maxScroll > 0.0f)
            hThumbPos_ = (state_.scrollOffsetX / maxScroll) * availableTrack;

        if (hwnd)
            InvalidateRect(hwnd, nullptr, FALSE);
    }

    void Editor::CancelInteraction()
    {
        state_.hasSelection = false;
        state_.caretVisible = true;
        state_.lastBlinkTime = GetTickCount();
        dragSelecting_ = false;
        secondaryCarets_.clear();

        if (scrollbar_.IsDragging())
        {
            scrollbar_.OnLeftButtonUp();
            ReleaseCapture();
        }
    }
} // namespace Orion
