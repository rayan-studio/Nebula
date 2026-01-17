#include "../Editor.h"

#include "orion/completion/popup/Popup.h"
#include "utils/logger/Logger.h"
#include "orion/caret/Caret.h"

#include <algorithm>

namespace Orion
{
    // --- OnChar (complete) ---
    void Editor::OnChar(wchar_t ch)
    {
        if (searchBox_.IsVisible() && searchBox_.IsInputFocused())
        {
            searchBox_.OnChar(ch);
            // Re-effectuer la recherche après chaque caractère
            searchBox_.PerformSearch(state_.lines);

            // Scroller vers le match courant si disponible
            if (!searchBox_.GetMatches().empty())
            {
                int idx = searchBox_.GetCurrentMatchIndex();
                if (idx >= 0 && idx < (int)searchBox_.GetMatches().size())
                {
                    const auto &match = searchBox_.GetMatches()[idx];
                    state_.caret.line = match.line;
                    state_.caret.column = match.startColumn;
                    Orion::Caret::EnsureCaretVisible(state_, metrics_, scrollbar_);
                }
            }
            return;
        }

        // If user typed '!' and the file is HTML, prepare an HTML template completion
        if (ch == L'!')
        {
            std::wstring ext;
            if (!state_.filePath.empty())
            {
                size_t pos = state_.filePath.find_last_of(L'.');
                if (pos != std::wstring::npos)
                {
                    ext = state_.filePath.substr(pos);
                    for (auto &c : ext)
                        c = towlower(c);
                }
            }

            if (ext == L".html" || ext == L".htm")
            {
                // Do not block insertion of the '!' — instead store a pending
                // short label and the full template to insert when accepted.
                pendingCompletionLabel_ = L"HTML5 boilerplate";
                pendingCompletionTemplate_ = L"<!DOCTYPE html>\n<html>\n<head>\n  <meta charset=\"utf-8\">\n  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n  <title>Document</title>\n</head>\n<body>\n\n</body>\n</html>";
                pendingCompletionShow_ = true;
                // allow normal insertion of '!'
            }
        }

        // If a previous OnKeyDown consumed this char (e.g. Ctrl+Space), suppress it
        if (suppressNextChar_ && ch == L' ')
        {
            suppressNextChar_ = false;
            return;
        }

        if (ch < 32 && ch != L'\t' && ch != L'\r' && ch != L'\n')
            return;

        // Push undo snapshot before any mutation
        if (undoStack_.empty() || undoStack_.back().lines != state_.lines ||
            undoStack_.back().caret.line != state_.caret.line ||
            undoStack_.back().caret.column != state_.caret.column)
        {
            undoStack_.push_back(state_);
            if (undoStack_.size() > maxUndoEntries_)
                undoStack_.erase(undoStack_.begin());
        }

        // Supprimer la sélection si elle existe
        if (state_.hasSelection)
        {
            DeleteSelection();
        }

        // Compute current file extension for language-specific behaviors
        std::wstring ext;
        if (!state_.filePath.empty())
        {
            size_t pos = state_.filePath.find_last_of(L'.');
            if (pos != std::wstring::npos)
            {
                ext = state_.filePath.substr(pos);
                for (auto &c : ext)
                    c = towlower(c);
            }
        }

        // If completion popup visible and typing, hide it
        if (completionPopup_ && completionPopup_->IsVisible())
        {
            completionPopup_->Hide();
        }

        if (ch == L'\r' || ch == L'\n')
        {
            // HTML: auto-indent and optionally insert closing tag when pressing Enter
            if ((ext == L".html" || ext == L".htm") && state_.caret.line >= 0 && state_.caret.line < (int)state_.lines.size())
            {
                std::wstring &currentLine = state_.lines[state_.caret.line];
                int col = state_.caret.column;
                // find last non-space before caret
                int idx = col - 1;
                while (idx >= 0 && iswspace(currentLine[idx]))
                    idx--;
                if (idx >= 0 && currentLine[idx] == L'>')
                {
                    // ignore self-closing like />
                    if (idx - 1 >= 0 && currentLine[idx - 1] == L'/')
                    {
                        // fallback to normal newline
                    }
                    else
                    {
                        // find '<' that starts the tag
                        int lt = idx - 1;
                        while (lt >= 0 && currentLine[lt] != L'<')
                            lt--;
                        if (lt >= 0 && lt + 1 < (int)currentLine.size() && currentLine[lt + 1] != L'/')
                        {
                            // extract tag name
                            int tstart = lt + 1;
                            int tpos = tstart;
                            while (tpos < (int)currentLine.size() && (iswalpha(currentLine[tpos]) || iswdigit(currentLine[tpos]) || currentLine[tpos] == L':' || currentLine[tpos] == L'-'))
                                tpos++;
                            if (tpos > tstart)
                            {
                                std::wstring tag = currentLine.substr(tstart, tpos - tstart);

                                // compute base indent
                                std::wstring baseIndent;
                                for (size_t i = 0; i < currentLine.size(); ++i)
                                {
                                    if (!iswspace(currentLine[i]))
                                        break;
                                    baseIndent.push_back(currentLine[i]);
                                }
                                int tabSize = GetIndentConfig().tabSize;
                                std::wstring innerIndent = baseIndent + std::wstring(tabSize, L' ');

                                // split line at caret
                                std::wstring before = currentLine.substr(0, col);
                                std::wstring after = currentLine.substr(col);

                                // replace current line with before
                                state_.lines[state_.caret.line] = before;
                                // insert inner blank line
                                state_.lines.insert(state_.lines.begin() + state_.caret.line + 1, innerIndent);

                                // Determine whether a matching closing tag already exists later in the buffer
                                std::wstring closing = L"</" + tag + L">", trimmed;
                                bool closingExists = false;
                                // check 'after' on same line
                                size_t kk = 0;
                                while (kk < after.size() && iswspace(after[kk]))
                                    kk++;
                                if (after.size() >= kk + closing.size() && after.substr(kk, closing.size()) == closing)
                                    closingExists = true;

                                // search next lines for an existing closing tag
                                if (!closingExists)
                                {
                                    for (int ln = state_.caret.line + 1; ln < (int)state_.lines.size(); ++ln)
                                    {
                                        const std::wstring &l = state_.lines[ln];
                                        size_t p = 0;
                                        while (p < l.size() && iswspace(l[p]))
                                            p++;
                                        if (l.size() >= p + closing.size() && l.substr(p, closing.size()) == closing)
                                        {
                                            closingExists = true;
                                            break;
                                        }
                                    }
                                }

                                if (!closingExists)
                                {
                                    // insert closing tag line and preserve trailing text
                                    state_.lines.insert(state_.lines.begin() + state_.caret.line + 2, baseIndent + closing + after);
                                }
                                else
                                {
                                    // closing already exists; preserve any trailing 'after' text on a new content line
                                    if (!after.empty())
                                    {
                                        state_.lines.insert(state_.lines.begin() + state_.caret.line + 2, baseIndent + after);
                                    }
                                }

                                // move caret to inner indent
                                state_.caret.line++;
                                state_.caret.column = (int)innerIndent.size();
                                state_.caretVisible = true;
                                Orion::Caret::EnsureCaretVisible(state_, metrics_, scrollbar_);
                                state_.lastBlinkTime = GetTickCount();
                                return;
                            }
                        }
                    }
                }
                // Nouvelle ligne handling (kept identical to original)
                std::wstring curLine = state_.lines[state_.caret.line];
                std::wstring before = curLine.substr(0, state_.caret.column);
                std::wstring after = curLine.substr(state_.caret.column);

                // Special-case: if caret is between matching braces/paren/brackets
                // (e.g. "{    }" or "(|)" with only whitespace between),
                // insert an indented blank line between them like VSCode.
                auto matching = [](wchar_t open) -> wchar_t
                {
                    switch (open)
                    {
                    case L'(':
                        return L')';
                    case L'{':
                        return L'}';
                    case L'[':
                        return L']';
                    default:
                        return 0;
                    }
                };

                // Find last non-space in before
                int lb = (int)before.size() - 1;
                while (lb >= 0 && iswspace(before[lb]))
                    lb--;

                bool didSpecial = false;
                if (lb >= 0)
                {
                    wchar_t openChar = before[lb];
                    wchar_t expectedClose = matching(openChar);
                    if (expectedClose != 0)
                    {
                        // find first non-space in after
                        int fa = 0;
                        while (fa < (int)after.size() && iswspace(after[fa]))
                            fa++;
                        if (fa < (int)after.size() && after[fa] == expectedClose)
                        {
                            // compute base indent (leading whitespace of the current line)
                            std::wstring baseIndent;
                            for (size_t i = 0; i < currentLine.size(); ++i)
                            {
                                if (!iswspace(currentLine[i]))
                                    break;
                                baseIndent.push_back(currentLine[i]);
                            }

                            // indent unit = 4 spaces (same as Tab behavior)
                            int tabSize = GetIndentConfig().tabSize;
                            std::wstring innerIndent = baseIndent + std::wstring(tabSize, L' ');

                            // left content: before up to openChar (trim trailing spaces)
                            std::wstring left = before.substr(0, lb + 1);

                            // right content: after from first non-space (keep rest)
                            std::wstring right = after.substr(fa);

                            // Replace current line and insert two new lines
                            state_.lines[state_.caret.line] = left;
                            state_.lines.insert(state_.lines.begin() + state_.caret.line + 1, innerIndent);
                            state_.lines.insert(state_.lines.begin() + state_.caret.line + 2, baseIndent + right);

                            state_.caret.line++;
                            state_.caret.column = (int)innerIndent.size();
                            didSpecial = true;
                        }
                    }
                }

                if (!didSpecial)
                {
                    state_.lines[state_.caret.line] = before;
                    state_.lines.insert(state_.lines.begin() + state_.caret.line + 1, after);

                    state_.caret.line++;
                    state_.caret.column = 0;
                }
            }
            else
            {
                // Non-HTML newline handling -- attempt special-case between braces like VSCode
                std::wstring curLine = state_.lines[state_.caret.line];
                std::wstring before = curLine.substr(0, state_.caret.column);
                std::wstring after = curLine.substr(state_.caret.column);

                // Special-case: if caret is between matching braces/paren/brackets
                auto matching = [](wchar_t open) -> wchar_t
                {
                    switch (open)
                    {
                    case L'(':
                        return L')';
                    case L'{':
                        return L'}';
                    case L'[':
                        return L']';
                    default:
                        return 0;
                    }
                };

                // Find last non-space in before
                int lb = (int)before.size() - 1;
                while (lb >= 0 && iswspace(before[lb]))
                    lb--;

                bool didSpecial = false;
                if (lb >= 0)
                {
                    wchar_t openChar = before[lb];
                    wchar_t expectedClose = matching(openChar);
                    if (expectedClose != 0)
                    {
                        // find first non-space in after
                        int fa = 0;
                        while (fa < (int)after.size() && iswspace(after[fa]))
                            fa++;

                        // Determine if a closing char exists immediately after, later on this line,
                        // or on subsequent lines. We'll insert one if none found.
                        bool closingExists = false;
                        if (fa < (int)after.size() && after[fa] == expectedClose)
                        {
                            closingExists = true;
                        }
                        else
                        {
                            // search rest of current line
                            for (int k = fa; k < (int)after.size(); ++k)
                            {
                                if (after[k] == expectedClose)
                                {
                                    closingExists = true;
                                    break;
                                }
                            }
                        }

                        if (!closingExists)
                        {
                            // search subsequent lines for an early closing at line start
                            for (int ln = state_.caret.line + 1; ln < (int)state_.lines.size(); ++ln)
                            {
                                const std::wstring &l = state_.lines[ln];
                                int p = 0;
                                while (p < (int)l.size() && iswspace(l[p])) p++;
                                if (p < (int)l.size() && l[p] == expectedClose)
                                {
                                    closingExists = true;
                                    break;
                                }
                                // if non-empty line without close, stop searching (no early close)
                                if (p < (int)l.size())
                                    break;
                            }
                        }

                        // compute base indent (leading whitespace of the current line)
                        std::wstring baseIndent;
                        for (size_t i = 0; i < curLine.size(); ++i)
                        {
                            if (!iswspace(curLine[i]))
                                break;
                            baseIndent.push_back(curLine[i]);
                        }

                        int tabSize = GetIndentConfig().tabSize;
                        std::wstring innerIndent = baseIndent + std::wstring(tabSize, L' ');

                        // Determine whether the open char should be moved to its own line.
                        bool splitOpenToOwnLine = false;
                        if (openChar == L'(' || openChar == L'{' || openChar == L'[')
                        {
                            int k = lb - 1;
                            while (k >= 0 && iswspace(before[k]))
                                k--;
                            if (k >= 0)
                            {
                                wchar_t beforeOpen = before[k];
                                if (iswalnum(beforeOpen) || beforeOpen == L'_' || beforeOpen == L')')
                                    splitOpenToOwnLine = true;
                            }
                        }

                        std::wstring left;
                        std::wstring right = after.substr(fa);

                        if (splitOpenToOwnLine)
                        {
                            left = before.substr(0, lb);
                            state_.lines[state_.caret.line] = left;

                            std::wstring openLine = baseIndent + std::wstring(1, openChar);
                            state_.lines.insert(state_.lines.begin() + state_.caret.line + 1, openLine);

                            state_.lines.insert(state_.lines.begin() + state_.caret.line + 2, innerIndent);

                            std::wstring finalLine = baseIndent + right;
                            if (!closingExists)
                                finalLine += std::wstring(1, expectedClose);
                            state_.lines.insert(state_.lines.begin() + state_.caret.line + 3, finalLine);

                            state_.caret.line += 2;
                            state_.caret.column = (int)innerIndent.size();
                            didSpecial = true;
                        }
                        else
                        {
                            left = before.substr(0, lb + 1);
                            state_.lines[state_.caret.line] = left;
                            state_.lines.insert(state_.lines.begin() + state_.caret.line + 1, innerIndent);

                            std::wstring finalLine = baseIndent + right;
                            if (!closingExists)
                                finalLine += std::wstring(1, expectedClose);
                            state_.lines.insert(state_.lines.begin() + state_.caret.line + 2, finalLine);

                            state_.caret.line++;
                            state_.caret.column = (int)innerIndent.size();
                            didSpecial = true;
                        }
                    }
                }

                if (!didSpecial)
                {
                    state_.lines[state_.caret.line] = before;
                    state_.lines.insert(state_.lines.begin() + state_.caret.line + 1, after);

                    state_.caret.line++;
                    state_.caret.column = 0;
                }
            }
        }
        else if (ch == L'\t')
        {
            // Tab = indent size from configuration
            int tabSize = GetIndentConfig().tabSize;
            std::wstring spaces = std::wstring(tabSize, L' ');
            state_.lines[state_.caret.line].insert(state_.caret.column, spaces);
            state_.caret.column += tabSize;
        }
        else
        {
            // Auto-pairing for brackets and quotes, and skip-over for closing chars
            auto matching = [](wchar_t c) -> wchar_t
            {
                switch (c)
                {
                case L'(':
                    return L')';
                case L'{':
                    return L'}';
                case L'[':
                    return L']';
                case L'"':
                    return L'"';
                case L'\'':
                    return L'\'';
                default:
                    return 0;
                }
            };

            wchar_t closeForOpen = matching(ch);
            bool isQuote = (ch == L'"' || ch == L'\'');

            if (closeForOpen != 0)
            {
                // Special handling for quotes because opening and closing are the same
                if (isQuote)
                {
                    std::wstring &line = state_.lines[state_.caret.line];

                    // If there's a single-line selection, wrap it with the quotes
                    if (state_.hasSelection)
                    {
                        CaretPosition a = state_.selectionStart;
                        CaretPosition b = state_.caret;
                        if (a.line > b.line || (a.line == b.line && a.column > b.column))
                            std::swap(a, b);

                        if (a.line == b.line)
                        {
                            std::wstring &selLine = state_.lines[a.line];
                            selLine.insert(b.column, 1, ch);
                            selLine.insert(a.column, 1, ch);
                            state_.hasSelection = false;
                            state_.caret.line = b.line;
                            state_.caret.column = b.column + 1;
                        }
                        else
                        {
                            // Fallback for multi-line selection: insert an empty pair at caret
                            std::wstring pairStr;
                            pairStr.push_back(ch);
                            pairStr.push_back(ch);
                            state_.lines[state_.caret.line].insert(state_.caret.column, pairStr);
                            state_.caret.column += 1;
                        }
                    }
                    else
                    {
                        // No selection: if next char is the same quote and not escaped, skip over it
                        if (state_.caret.column < (int)line.size() && line[state_.caret.column] == ch)
                        {
                            bool escaped = false;
                            if (state_.caret.column > 0 && line[state_.caret.column - 1] == L'\\')
                                escaped = true;

                            if (!escaped)
                            {
                                state_.caret.column++;
                            }
                            else
                            {
                                state_.lines[state_.caret.line].insert(state_.caret.column, 1, ch);
                                state_.caret.column++;
                            }
                        }
                        else
                        {
                            std::wstring pairStr;
                            pairStr.push_back(ch);
                            pairStr.push_back(ch);
                            state_.lines[state_.caret.line].insert(state_.caret.column, pairStr);
                            state_.caret.column += 1;
                        }
                    }
                }
                else
                {
                    // opening bracket behavior (wrap single-line selection or insert pair)
                    if (state_.hasSelection)
                    {
                        CaretPosition a = state_.selectionStart;
                        CaretPosition b = state_.caret;
                        if (a.line > b.line || (a.line == b.line && a.column > b.column))
                            std::swap(a, b);

                        if (a.line == b.line)
                        {
                            std::wstring &line = state_.lines[a.line];
                            line.insert(b.column, 1, closeForOpen);
                            line.insert(a.column, 1, ch);
                            state_.hasSelection = false;
                            state_.caret.line = b.line;
                            state_.caret.column = b.column + 1;
                        }
                        else
                        {
                            std::wstring pairStr;
                            pairStr.push_back(ch);
                            pairStr.push_back(closeForOpen);
                            state_.lines[state_.caret.line].insert(state_.caret.column, pairStr);
                            state_.caret.column += 1;
                        }
                    }
                    else
                    {
                        std::wstring pairStr;
                        pairStr.push_back(ch);
                        pairStr.push_back(closeForOpen);
                        state_.lines[state_.caret.line].insert(state_.caret.column, pairStr);
                        state_.caret.column += 1;
                    }
                }
            }
            else if (ch == L')' || ch == L'}' || ch == L']')
            {
                // If the next character is the same closing char, skip over it
                std::wstring &line = state_.lines[state_.caret.line];
                if (state_.caret.column < (int)line.size() && line[state_.caret.column] == ch)
                {
                    state_.caret.column++;
                }
                else
                {
                    state_.lines[state_.caret.line].insert(state_.caret.column, 1, ch);
                    state_.caret.column++;
                }
            }
            else
            {
                // Caractère normal
                state_.lines[state_.caret.line].insert(state_.caret.column, 1, ch);
                state_.caret.column++;

                // HTML: after typing '>' on an opening tag, auto-insert closing tag
                if ((ext == L".html" || ext == L".htm") && ch == L'>')
                {
                    std::wstring &line = state_.lines[state_.caret.line];
                    int col = state_.caret.column;
                    int lt = col - 2;
                    while (lt >= 0 && line[lt] != L'<')
                        lt--;
                    if (lt >= 0 && lt + 1 < (int)line.size() && line[lt + 1] != L'/' &&
                        !(lt + 3 < (int)line.size() && line.substr(lt + 1, 3) == L"!--"))
                    {
                        int tstart = lt + 1;
                        int tpos = tstart;
                        while (tpos < (int)line.size() && (iswalpha(line[tpos]) || iswdigit(line[tpos]) ||
                                                           line[tpos] == L':' || line[tpos] == L'-'))
                            tpos++;
                        if (tpos > tstart)
                        {
                            std::wstring tag = line.substr(tstart, tpos - tstart);
                            int beforeGt = col - 2;
                            while (beforeGt > lt && iswspace(line[beforeGt]))
                                beforeGt--;
                            if (beforeGt >= lt && line[beforeGt] != L'/')
                            {
                                std::wstring closing = L"</" + tag + L">";
                                state_.lines[state_.caret.line].insert(state_.caret.column, closing);
                            }
                        }
                    }
                }
            }

            // show pending completion label
            if (pendingCompletionShow_ && completionPopup_)
            {
                std::vector<std::wstring> items;
                items.push_back(pendingCompletionLabel_.empty() ? L"Snippet" : pendingCompletionLabel_);
                completionPopup_->SetItems(items);
                D2D1_POINT_2F p = TextToScreenPosition(state_.caret);
                completionPopup_->UpdateLayout(p.x, p.y + metrics_.lineHeight, 520.0f, metrics_.lineHeight);
                completionPopup_->Show();
                pendingCompletionShow_ = false;
            }
        }

        state_.caretVisible = true;
        Orion::Caret::EnsureCaretVisible(state_, metrics_, scrollbar_);
        state_.lastBlinkTime = GetTickCount();
    } // end OnChar

    // --- OnKeyDown (complete) ---
    void Editor::OnKeyDown(WPARAM key)
    {
        // Save initial caret to detect whether a key actually moved it.
        CaretPosition prevCaret = state_.caret;

        {
            wchar_t buf[128];
            bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            swprintf_s(buf, L"Editor::OnKeyDown - key=%d ctrl=%d shift=%d", (int)key, ctrl ? 1 : 0, shift ? 1 : 0);
            Logger::Instance().Log(std::wstring(buf));
        }

        bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

        // determine file extension for language-specific behavior
        std::wstring ext;
        if (!state_.filePath.empty())
        {
            size_t pos = state_.filePath.find_last_of(L'.');
            if (pos != std::wstring::npos)
            {
                ext = state_.filePath.substr(pos);
                for (auto &c : ext)
                    c = towlower(c);
            }
        }

        // Ctrl+Space -> trigger completion via CompletionService
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 && key == VK_SPACE && completionService_)
        {
            Completion::CompletionContext ctx{state_.filePath, state_.lines, state_.caret.line, state_.caret.column, ext};

            auto items = completionService_->GetCompletions(ctx);
            if (!items.empty())
            {
                std::vector<std::wstring> labels;
                labels.reserve(items.size());
                for (const auto &it : items)
                    labels.push_back(it.label);

                if (items[0].isSnippet)
                {
                    pendingCompletionLabel_ = items[0].label;
                    pendingCompletionTemplate_ = items[0].insertText;
                }

                completionPopup_->SetItems(labels);
                D2D1_POINT_2F p = TextToScreenPosition(state_.caret);
                completionPopup_->UpdateLayout(p.x, p.y + metrics_.lineHeight, 400.0f, metrics_.lineHeight);
                completionPopup_->Show();
                suppressNextChar_ = true;
            }
            return;
        }

        // If completion popup is visible, forward navigation/accept/escape keys to it
        if (completionPopup_ && completionPopup_->IsVisible())
        {
            completionPopup_->OnKeyDown(key);

            if (key == VK_RETURN)
            {
                std::wstring chosen = completionPopup_->GetSelectedItem();
                if (!chosen.empty())
                {
                    std::wstring toInsert = chosen;
                    bool usePendingTemplate = false;
                    if (!pendingCompletionTemplate_.empty() && !pendingCompletionLabel_.empty() && chosen == pendingCompletionLabel_)
                    {
                        toInsert = pendingCompletionTemplate_;
                        usePendingTemplate = true;
                    }

                    if (undoStack_.empty() || undoStack_.back().lines != state_.lines ||
                        undoStack_.back().caret.line != state_.caret.line ||
                        undoStack_.back().caret.column != state_.caret.column)
                    {
                        undoStack_.push_back(state_);
                        if (undoStack_.size() > maxUndoEntries_)
                            undoStack_.erase(undoStack_.begin());
                    }

                    std::wstring tmp;
                    tmp.reserve(toInsert.size());
                    for (wchar_t c : toInsert)
                        if (c != L'\r')
                            tmp.push_back(c);

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

                    if (state_.hasSelection)
                        DeleteSelection();

                    if (usePendingTemplate)
                    {
                        if (state_.caret.column > 0)
                        {
                            std::wstring &line = state_.lines[state_.caret.line];
                            if (state_.caret.column - 1 < (int)line.size() && line[state_.caret.column - 1] == L'!')
                            {
                                line.erase(state_.caret.column - 1, 1);
                                state_.caret.column--;
                            }
                        }
                    }

                    if (!usePendingTemplate && (ext == L".html" || ext == L".htm") && parts.size() == 1)
                    {
                        std::wstring &line = state_.lines[state_.caret.line];
                        int caretCol = state_.caret.column;
                        int lt = caretCol - 1;
                        while (lt >= 0 && line[lt] != L'<')
                            lt--;
                        if (lt >= 0 && lt + 1 < caretCol)
                        {
                            if (line[lt + 1] != L'/')
                            {
                                int startPos = lt + 1;
                                if (caretCol > startPos)
                                {
                                    line.erase(startPos, caretCol - startPos);
                                    state_.caret.column = startPos;
                                }
                            }
                        }
                    }

                    if (parts.size() == 1)
                    {
                        state_.lines[state_.caret.line].insert(state_.caret.column, parts[0]);
                        state_.caret.column += (int)parts[0].size();

                        if (ext == L".html" || ext == L".htm")
                        {
                            std::wstring &line = state_.lines[state_.caret.line];
                            int nextPos = state_.caret.column;
                            if (nextPos >= (int)line.size() || line[nextPos] != L'>')
                            {
                                line.insert(nextPos, 1, L'>');
                                state_.caret.column = nextPos + 1;
                            }
                            else
                            {
                                state_.caret.column = nextPos + 1;
                            }
                        }
                    }
                    else if (!parts.empty())
                    {
                        std::wstring curLine = state_.lines[state_.caret.line];
                        std::wstring before = curLine.substr(0, state_.caret.column);
                        std::wstring after = curLine.substr(state_.caret.column);

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

                    if (usePendingTemplate)
                    {
                        pendingCompletionTemplate_.clear();
                        pendingCompletionLabel_.clear();
                    }
                }
            }

            if (key == VK_RETURN || key == VK_ESCAPE || key == VK_UP || key == VK_DOWN)
                return;
        }

        if (ctrl && (key == 'F' || key == 'f'))
        {
            ShowSearch();
            if (!searchBox_.GetSearchText().empty())
            {
                searchBox_.PerformSearch(state_.lines);
            }
            return;
        }

        if (searchBox_.IsVisible() && searchBox_.IsInputFocused())
        {
            searchBox_.OnKeyDown(key);

            if (key == VK_BACK || key == VK_DELETE || key == VK_RETURN)
            {
                searchBox_.PerformSearch(state_.lines);

                if (!searchBox_.GetMatches().empty())
                {
                    int idx = searchBox_.GetCurrentMatchIndex();
                    if (idx >= 0 && idx < (int)searchBox_.GetMatches().size())
                    {
                        const auto &match = searchBox_.GetMatches()[idx];
                        state_.caret.line = match.line;
                        state_.caret.column = match.startColumn;
                        Orion::Caret::EnsureCaretVisible(state_, metrics_, scrollbar_);
                    }
                }
            }

            if (!searchBox_.IsVisible())
                return;

            return;
        }

        if (ctrl && (key == 'Z' || key == 'z'))
        {
            Undo();
            return;
        }

        // NOTE: don't start a selection just by pressing Shift alone.
        // We'll enable selection only if Shift is held while a movement actually occurs.
        if (!shift && state_.hasSelection &&
            key != VK_BACK && key != VK_DELETE &&
            key != 'C' && key != 'X' && key != 'V' &&
            key != VK_CONTROL && key != VK_MENU)
        {
            state_.hasSelection = false;
        }

        switch (key)
        {
        case VK_LEFT:
            if (state_.caret.column > 0)
                state_.caret.column--;
            else if (state_.caret.line > 0)
            {
                state_.caret.line--;
                state_.caret.column = (int)state_.lines[state_.caret.line].size();
            }
            break;

        case VK_RIGHT:
            if (state_.caret.column < (int)state_.lines[state_.caret.line].size())
                state_.caret.column++;
            else if (state_.caret.line < (int)state_.lines.size() - 1)
            {
                state_.caret.line++;
                state_.caret.column = 0;
            }
            break;

        case VK_UP:
            if (state_.caret.line > 0)
            {
                state_.caret.line--;
                state_.caret.column = (std::min)(state_.caret.column, (int)state_.lines[state_.caret.line].size());
            }
            break;

        case VK_DOWN:
            if (state_.caret.line < (int)state_.lines.size() - 1)
            {
                state_.caret.line++;
                state_.caret.column = (std::min)(state_.caret.column, (int)state_.lines[state_.caret.line].size());
            }
            break;

        case VK_HOME:
            state_.caret.column = 0;
            break;

        case VK_END:
            state_.caret.column = (int)state_.lines[state_.caret.line].size();
            break;

        case VK_BACK:
            if (undoStack_.empty() || undoStack_.back().lines != state_.lines ||
                undoStack_.back().caret.line != state_.caret.line ||
                undoStack_.back().caret.column != state_.caret.column)
            {
                undoStack_.push_back(state_);
                if (undoStack_.size() > maxUndoEntries_)
                    undoStack_.erase(undoStack_.begin());
            }

            if (state_.hasSelection)
                DeleteSelection();
            else if (state_.caret.column > 0)
            {
                state_.lines[state_.caret.line].erase(state_.caret.column - 1, 1);
                state_.caret.column--;
            }
            else if (state_.caret.line > 0)
            {
                int prevLineLen = (int)state_.lines[state_.caret.line - 1].size();
                state_.lines[state_.caret.line - 1] += state_.lines[state_.caret.line];
                state_.lines.erase(state_.lines.begin() + state_.caret.line);
                state_.caret.line--;
                state_.caret.column = prevLineLen;
            }
            break;

        case VK_DELETE:
            if (undoStack_.empty() || undoStack_.back().lines != state_.lines ||
                undoStack_.back().caret.line != state_.caret.line ||
                undoStack_.back().caret.column != state_.caret.column)
            {
                undoStack_.push_back(state_);
                if (undoStack_.size() > maxUndoEntries_)
                    undoStack_.erase(undoStack_.begin());
            }

            if (state_.hasSelection)
                DeleteSelection();
            else if (state_.caret.column < (int)state_.lines[state_.caret.line].size())
                state_.lines[state_.caret.line].erase(state_.caret.column, 1);
            else if (state_.caret.line < (int)state_.lines.size() - 1)
            {
                state_.lines[state_.caret.line] += state_.lines[state_.caret.line + 1];
                state_.lines.erase(state_.lines.begin() + state_.caret.line + 1);
            }
            break;

        case 'A':
            if (ctrl)
            {
                state_.selectionStart = {0, 0};
                state_.caret = {(int)state_.lines.size() - 1, (int)state_.lines.back().size()};
                state_.hasSelection = true;
            }
            break;

        case 'C':
            if (ctrl && state_.hasSelection)
            {
                auto getSelectionText = [this]() -> std::wstring
                {
                    CaretPosition start = state_.selectionStart;
                    CaretPosition end = state_.caret;
                    if (start.line > end.line || (start.line == end.line && start.column > end.column))
                        std::swap(start, end);

                    std::wstring out;
                    if (start.line == end.line)
                    {
                        out = state_.lines[start.line].substr(start.column, end.column - start.column);
                        return out;
                    }

                    out += state_.lines[start.line].substr(start.column);
                    out += L"\r\n";
                    for (int L = start.line + 1; L < end.line; ++L)
                    {
                        out += state_.lines[L];
                        out += L"\r\n";
                    }
                    out += state_.lines[end.line].substr(0, end.column);
                    return out;
                };

                std::wstring sel = getSelectionText();
                if (!sel.empty())
                {
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
            }
            break;

        case 'X':
            if (ctrl && state_.hasSelection)
            {
                auto getSelectionText = [this]() -> std::wstring
                {
                    CaretPosition start = state_.selectionStart;
                    CaretPosition end = state_.caret;
                    if (start.line > end.line || (start.line == end.line && start.column > end.column))
                        std::swap(start, end);

                    std::wstring out;
                    if (start.line == end.line)
                    {
                        out = state_.lines[start.line].substr(start.column, end.column - start.column);
                        return out;
                    }

                    out += state_.lines[start.line].substr(start.column);
                    out += L"\r\n";
                    for (int L = start.line + 1; L < end.line; ++L)
                    {
                        out += state_.lines[L];
                        out += L"\r\n";
                    }
                    out += state_.lines[end.line].substr(0, end.column);
                    return out;
                };

                std::wstring sel = getSelectionText();
                if (!sel.empty())
                {
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

                DeleteSelection();
            }
            break;

        case 'V':
            if (ctrl)
            {
                if (undoStack_.empty() || undoStack_.back().lines != state_.lines ||
                    undoStack_.back().caret.line != state_.caret.line ||
                    undoStack_.back().caret.column != state_.caret.column)
                {
                    undoStack_.push_back(state_);
                    if (undoStack_.size() > maxUndoEntries_)
                        undoStack_.erase(undoStack_.begin());
                }

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
                                DeleteSelection();

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
            break;
        }

        // If caret moved while Shift is held, start (or update) selection
        if (shift)
        {
            if (!state_.hasSelection && (prevCaret.line != state_.caret.line || prevCaret.column != state_.caret.column))
            {
                state_.selectionStart = prevCaret;
                state_.hasSelection = true;
            }
        }

        if (prevCaret.line != state_.caret.line || prevCaret.column != state_.caret.column)
        {
            Orion::Caret::EnsureCaretVisible(state_, metrics_, scrollbar_);
        }
        state_.caretVisible = true;
        state_.lastBlinkTime = GetTickCount();
    }
} // namespace Orion
