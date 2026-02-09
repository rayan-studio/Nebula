#include "Caret.h"
#include <algorithm>

namespace Orion
{
    namespace Caret
    {
        void UpdateCaretBlink(EditorState &state)
        {
            DWORD current = GetTickCount();
            if (current - state.lastBlinkTime > 500)
            {
                state.caretVisible = !state.caretVisible;
                state.lastBlinkTime = current;
            }
        }

        void EnsureCaretVisible(EditorState &state, const EditorMetrics &metrics, Scrollbar &scrollbar)
        {
            float viewportHeight = state.bottomEdge - state.topEdge;
            int caretVisualLine = state.caret.line;
            if (!state.visualLineByActual.empty() &&
                state.caret.line >= 0 &&
                state.caret.line < (int)state.visualLineByActual.size())
            {
                caretVisualLine = state.visualLineByActual[(size_t)state.caret.line];
            }
            float caretTop = caretVisualLine * metrics.lineHeight;
            float caretBottom = caretTop + metrics.lineHeight;
            float pad = metrics.lineHeight * 0.25f;

            float current = state.scrollOffsetY;
            if (caretTop < current + pad)
            {
                scrollbar.SetScrollOffset((std::max)(0.0f, caretTop - pad));
                state.scrollOffsetY = scrollbar.GetScrollOffset();
            }
            else if (caretBottom > current + viewportHeight - pad)
            {
                float desired = caretBottom - viewportHeight + pad;
                scrollbar.SetScrollOffset(desired);
                state.scrollOffsetY = scrollbar.GetScrollOffset();
            }
        }

        void SetCaret(EditorState &state, const EditorMetrics &metrics, Scrollbar &scrollbar, int line, int column)
        {
            // Clamp line to valid range
            if (state.lines.empty())
            {
                state.caret.line = 0;
                state.caret.column = 0;
                return;
            }

            state.caret.line = (std::max)(0, (std::min)(line, (int)state.lines.size() - 1));

            // Clamp column to valid range for the line
            int maxCol = (int)state.lines[state.caret.line].length();
            state.caret.column = (std::max)(0, (std::min)(column, maxCol));

            // Clear any selection
            state.hasSelection = false;

            // Make sure caret is visible
            EnsureCaretVisible(state, metrics, scrollbar);
        }

        void SetCaret(Editor &editor, int line, int column)
        {
            ::Orion::Caret::SetCaret(editor.state_, editor.metrics_, editor.scrollbar_, line, column);
            editor.state_.caretVisible = true;
        }
    }
} // namespace Orion
