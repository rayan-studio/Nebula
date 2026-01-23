#include "orion/editor/Editor.h"
#include "orion/caret/Caret.h"

// Ensure Windows min/max macros don't interfere with std::min/std::max
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

namespace Orion
{
    bool Editor::ReplaceCurrentMatch()
    {
        if (!searchBox_.IsVisible())
            return false;

        const auto &matches = searchBox_.GetMatches();
        int idx = searchBox_.GetCurrentMatchIndex();
        if (idx < 0 || idx >= static_cast<int>(matches.size()))
            return false;

        const SearchMatch &match = matches[idx];
        if (match.line < 0 || match.line >= static_cast<int>(state_.lines.size()))
            return false;

        const std::wstring &searchText = searchBox_.GetSearchText();
        if (searchText.empty())
            return false;

        std::wstring replaceText = searchBox_.GetReplaceText();
        std::wstring &line = state_.lines[match.line];

        int lineLength = static_cast<int>(line.size());
        int startCol = std::max(0, std::min(match.startColumn, lineLength));
        int endCol = std::max(startCol, std::min(match.endColumn, lineLength));
        if (endCol <= startCol)
            return false;

        // Push undo snapshot before any mutation
        if (undoStack_.empty() || undoStack_.back().lines != state_.lines ||
            undoStack_.back().caret.line != state_.caret.line ||
            undoStack_.back().caret.column != state_.caret.column)
        {
            undoStack_.push_back(state_);
            if (undoStack_.size() > maxUndoEntries_)
                undoStack_.erase(undoStack_.begin());
        }

        line.replace(startCol, endCol - startCol, replaceText);

        state_.caret.line = match.line;
        state_.caret.column = startCol + static_cast<int>(replaceText.size());
        state_.hasSelection = false;
        MarkDirty();

        searchBox_.PerformSearch(state_.lines);

        const auto &updatedMatches = searchBox_.GetMatches();
        if (!updatedMatches.empty())
        {
            int targetLine = match.line;
            int targetColumn = startCol + static_cast<int>(replaceText.size());
            int nextIndex = 0;
            for (size_t i = 0; i < updatedMatches.size(); ++i)
            {
                const auto &candidate = updatedMatches[i];
                if (candidate.line > targetLine ||
                    (candidate.line == targetLine && candidate.startColumn >= targetColumn))
                {
                    nextIndex = static_cast<int>(i);
                    break;
                }
            }

            searchBox_.SetCurrentMatchIndex(nextIndex);
            const auto &nextMatch = updatedMatches[nextIndex];
            state_.caret.line = nextMatch.line;
            state_.caret.column = nextMatch.startColumn;
            Orion::Caret::EnsureCaretVisible(state_, metrics_, scrollbar_);
        }

        return true;
    }
} // namespace Orion
