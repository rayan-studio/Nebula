#include "orion/editor/Editor.h"
#include "orion/caret/Caret.h"
#include <algorithm>
#include <string>
#include <vector>
#include <thread>
#include <fstream>
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
        state_.filePath = std::move(filePath);
        state_.encoding = std::move(encoding);

        state_.lines = std::move(lines);
        if (state_.lines.empty())
            state_.lines.push_back(L"");

        state_.caret = {0, 0};
        state_.scrollOffsetX = 0.0f;
        state_.scrollOffsetY = 0.0f;

    }
    void Orion::Editor::LoadFileAsync(HWND hwnd, const std::wstring &filePath, int tabIndex)
    {
        // UI : afficher un "loading" instantané (optionnel)
        state_.filePath = filePath;
        state_.lines.clear();
        state_.lines.push_back(L"// Loading...");
        state_.encoding = L"";
        state_.caret = {0, 0};
        state_.scrollOffsetX = 0.0f;
        state_.scrollOffsetY = 0.0f;

        // Lancer le travail lourd en background
        std::wstring filePathCopy = filePath;

        std::thread([hwnd, tabIndex, filePathCopy]()
                    {
        auto* result = new Window::EditorFileLoadResult();
        result->tabIndex = tabIndex;
        result->filePath = filePathCopy;

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
        Orion::Caret::EnsureCaretVisible(state_, metrics_, scrollbar_);
    }
}
