#pragma once

#include <windows.h>
#include "orion/editor/Editor.h"
#include "CaretPosition.h"

namespace Orion
{
    namespace Caret
    {
        void UpdateCaretBlink(EditorState &state);
        void EnsureCaretVisible(EditorState &state, const EditorMetrics &metrics, Scrollbar &scrollbar);
        void SetCaret(EditorState &state, const EditorMetrics &metrics, Scrollbar &scrollbar, int line, int column);
        void SetCaret(Editor &editor, int line, int column);
    }
}
