#include "orion/editor/Editor.h"

#include "../../completion/popup/Popup.h"
#include "orion/caret/Caret.h"

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
    void Editor::OnLeftButtonDown(HWND hwnd, POINT pt)
    {
        (void)hwnd;

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
                searchBox_.OnLeftButtonDown(pt);
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

        // ✅ click must be inside editor content area (after gutter + left padding)
        if (pt.x < state_.leftEdge + metrics_.gutterWidth + metrics_.leftPadding)
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

        if (completionPopup_ && completionPopup_->IsVisible())
        {
            completionPopup_->OnMouseMove(pt);
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
    }

    void Editor::OnLeftButtonUp(HWND hwnd, POINT pt)
    {
        (void)hwnd;
        (void)pt;

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

        if (scrollbar_.IsDragging())
        {
            scrollbar_.OnLeftButtonUp();
            ReleaseCapture();
        }
    }
} // namespace Orion
