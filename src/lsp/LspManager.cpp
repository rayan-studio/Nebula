#include "LspManager.h"
#include "core/window/Window.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <thread>
#include <cwctype>

namespace Lsp
{
    static std::wstring ToLower(std::wstring v)
    {
        for (auto &c : v)
            c = (wchar_t)towlower(c);
        return v;
    }

    static std::wstring NormalizePath(const std::wstring &path)
    {
        try
        {
            std::filesystem::path p(path);
            std::error_code ec;
            auto norm = std::filesystem::weakly_canonical(p, ec);
            if (!ec)
                return norm.wstring();
        }
        catch (...)
        {
        }
        return path;
    }

    static bool IsCppFile(const std::filesystem::path &p)
    {
        auto ext = ToLower(p.extension().wstring());
        return ext == L".cpp" || ext == L".c" || ext == L".hpp" || ext == L".h" || ext == L".hh" || ext == L".inc";
    }

    static std::vector<std::wstring> ReadFileLinesUtf8(const std::filesystem::path &path)
    {
        std::vector<std::wstring> lines;
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
            return lines;

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

        if (data.empty())
            return lines;

        // UTF-8 decode (fallback: ACP)
        int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data.data(), (int)data.size(), nullptr, 0);
        std::wstring w;
        if (wlen > 0)
        {
            w.resize((size_t)wlen);
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data.data(), (int)data.size(), w.data(), wlen);
        }
        else
        {
            wlen = MultiByteToWideChar(CP_ACP, 0, data.data(), (int)data.size(), nullptr, 0);
            w.resize((size_t)wlen);
            MultiByteToWideChar(CP_ACP, 0, data.data(), (int)data.size(), w.data(), wlen);
        }

        size_t start = 0;
        for (size_t i = 0; i < w.size(); ++i)
        {
            if (w[i] == L'\r' || w[i] == L'\n')
            {
                lines.push_back(w.substr(start, i - start));
                if (w[i] == L'\r' && i + 1 < w.size() && w[i + 1] == L'\n')
                    i++;
                start = i + 1;
            }
        }
        lines.push_back(w.substr(start));
        return lines;
    }

    LspManager &LspManager::Instance()
    {
        static LspManager inst;
        return inst;
    }

    void LspManager::SetProjectRoot(const std::wstring &rootPath)
    {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            projectRoot_ = NormalizePath(rootPath);
        }
        StartProjectIndexAsync();
    }

    void LspManager::StartProjectIndexAsync()
    {
        if (indexing_)
            return;
        indexing_ = true;

        std::wstring root = projectRoot_;
        if (root.empty())
        {
            indexing_ = false;
            return;
        }

        std::thread([this, root]()
                    {
                        try
                        {
                            std::filesystem::path base(root);
                            if (!std::filesystem::exists(base))
                            {
                                indexing_ = false;
                                return;
                            }
                            for (auto &entry : std::filesystem::recursive_directory_iterator(base))
                            {
                                if (!entry.is_regular_file())
                                    continue;
                                if (!IsCppFile(entry.path()))
                                    continue;
                                auto lines = ReadFileLinesUtf8(entry.path());
                                if (!lines.empty())
                                    UpdateFile(entry.path().wstring(), lines);
                            }
                        }
                        catch (...)
                        {
                        }
                        indexing_ = false;
                    })
            .detach();
    }

    void LspManager::IndexFileSymbols(const std::wstring &filePath, const std::vector<std::wstring> &lines)
    {
        std::vector<std::wstring> newSymbols;
        int lineIndex = 0;
        for (const auto &line : lines)
        {
            std::wstring ln = line;
            if (ln.find(L"//") != std::wstring::npos)
                ln = ln.substr(0, ln.find(L"//"));

            auto trim = [](const std::wstring &s)
            {
                size_t a = s.find_first_not_of(L" \t");
                size_t b = s.find_last_not_of(L" \t");
                if (a == std::wstring::npos || b == std::wstring::npos)
                    return std::wstring();
                return s.substr(a, b - a + 1);
            };
            std::wstring t = trim(ln);
            if (t.empty())
            {
                lineIndex++;
                continue;
            }

            auto addSymbol = [&](const std::wstring &name, int col)
            {
                if (name.empty())
                    return;
                Location loc{NormalizePath(filePath), lineIndex, col};
                symbolIndex_[name] = loc;
                newSymbols.push_back(name);
            };

            auto isIdentChar = [](wchar_t c)
            {
                return iswalnum(c) || c == L'_';
            };

            // class/struct/enum
            {
                std::wstring lowered = ToLower(t);
                const std::wstring keys[] = {L"class ", L"struct ", L"enum "};
                for (const auto &k : keys)
                {
                    size_t pos = lowered.find(k);
                    if (pos != std::wstring::npos)
                    {
                        size_t start = pos + k.size();
                        while (start < t.size() && iswspace(t[start]))
                            start++;
                        size_t end = start;
                        while (end < t.size() && isIdentChar(t[end]))
                            end++;
                        if (end > start)
                        {
                            addSymbol(t.substr(start, end - start), (int)start);
                        }
                    }
                }
            }

            // function definitions (simple heuristic)
            if (t.find(L'(') != std::wstring::npos && t.find(L')') != std::wstring::npos)
            {
                static const std::wstring keywords[] = {L"if", L"for", L"while", L"switch", L"return", L"catch"};
                size_t lparen = t.find(L'(');
                if (lparen != std::wstring::npos && lparen > 0)
                {
                    size_t end = lparen;
                    while (end > 0 && iswspace(t[end - 1]))
                        end--;
                    size_t start = end;
                    while (start > 0 && isIdentChar(t[start - 1]))
                        start--;
                    if (end > start)
                    {
                        std::wstring name = t.substr(start, end - start);
                        bool isKeyword = false;
                        for (const auto &k : keywords)
                        {
                            if (name == k)
                            {
                                isKeyword = true;
                                break;
                            }
                        }
                        if (!isKeyword)
                            addSymbol(name, (int)start);
                    }
                }
            }

            lineIndex++;
        }

        // remove old symbols from this file
        auto it = fileSymbols_.find(filePath);
        if (it != fileSymbols_.end())
        {
            for (const auto &sym : it->second)
            {
                auto sit = symbolIndex_.find(sym);
                if (sit != symbolIndex_.end() && sit->second.filePath == NormalizePath(filePath))
                    symbolIndex_.erase(sit);
            }
            it->second = newSymbols;
        }
        else
        {
            fileSymbols_[filePath] = newSymbols;
        }
    }

    void LspManager::UpdateFile(const std::wstring &filePath, const std::vector<std::wstring> &lines)
    {
        if (!IsCppFile(std::filesystem::path(filePath)))
            return;
        std::lock_guard<std::mutex> lk(mutex_);
        IndexFileSymbols(filePath, lines);
    }

    std::vector<Diagnostic> LspManager::AnalyzeDiagnostics(const std::wstring &filePath, const std::vector<std::wstring> &lines) const
    {
        std::vector<Diagnostic> out;
        bool inBlockComment = false;
        bool inString = false;
        bool inChar = false;
        bool escape = false;
        bool inRawString = false;
        std::wstring rawDelimiter;

        struct StackItem
        {
            wchar_t ch;
            int line;
            int col;
        };
        std::vector<StackItem> stack;

        auto push = [&](wchar_t c, int l, int col)
        {
            stack.push_back({c, l, col});
        };
        auto popMatch = [&](wchar_t c) -> bool
        {
            if (stack.empty())
                return false;
            wchar_t top = stack.back().ch;
            if ((c == L')' && top == L'(') ||
                (c == L']' && top == L'[') ||
                (c == L'}' && top == L'{'))
            {
                stack.pop_back();
                return true;
            }
            return false;
        };

        for (int line = 0; line < (int)lines.size(); ++line)
        {
            const std::wstring &ln = lines[line];
            for (int col = 0; col < (int)ln.size(); ++col)
            {
                wchar_t c = ln[col];

                if (inRawString)
                {
                    if (c == L')')
                    {
                        size_t closePos = (size_t)col + 1;
                        bool match = true;
                        size_t delimSize = rawDelimiter.size();
                        if (closePos + delimSize < ln.size())
                        {
                            for (size_t k = 0; k < delimSize; ++k)
                            {
                                if (ln[closePos + k] != rawDelimiter[k])
                                {
                                    match = false;
                                    break;
                                }
                            }
                            if (match && closePos + delimSize < ln.size() && ln[closePos + delimSize] == L'"')
                            {
                                inRawString = false;
                                rawDelimiter.clear();
                                col = (int)(closePos + delimSize); // will be incremented by loop
                                continue;
                            }
                        }
                    }
                    continue;
                }

                if (inBlockComment)
                {
                    if (c == L'*' && col + 1 < (int)ln.size() && ln[col + 1] == L'/')
                    {
                        inBlockComment = false;
                        col++;
                    }
                    continue;
                }

                if (!inString && !inChar && c == L'/' && col + 1 < (int)ln.size() && ln[col + 1] == L'/')
                    break;

                if (!inString && !inChar && c == L'/' && col + 1 < (int)ln.size() && ln[col + 1] == L'*')
                {
                    inBlockComment = true;
                    col++;
                    continue;
                }

                if (!inString && !inChar && c == L'R' && col + 1 < (int)ln.size() && ln[col + 1] == L'"')
                {
                    size_t delimStart = (size_t)col + 2;
                    size_t parenPos = ln.find(L'(', delimStart);
                    if (parenPos != std::wstring::npos)
                    {
                        rawDelimiter = ln.substr(delimStart, parenPos - delimStart);
                        inRawString = true;
                        col = (int)parenPos;
                        continue;
                    }
                }

                if (inString)
                {
                    if (escape)
                    {
                        escape = false;
                    }
                    else if (c == L'\\')
                    {
                        escape = true;
                    }
                    else if (c == L'"')
                    {
                        inString = false;
                    }
                    continue;
                }

                if (inChar)
                {
                    if (escape)
                    {
                        escape = false;
                    }
                    else if (c == L'\\')
                    {
                        escape = true;
                    }
                    else if (c == L'\'')
                    {
                        inChar = false;
                    }
                    continue;
                }

                if (c == L'"')
                {
                    inString = true;
                    continue;
                }
                if (c == L'\'')
                {
                    inChar = true;
                    continue;
                }

                if (c == L'(' || c == L'{' || c == L'[')
                    push(c, line, col);
                else if (c == L')' || c == L'}' || c == L']')
                {
                    if (!popMatch(c))
                    {
                        Diagnostic d;
                        d.line = line;
                        d.startCol = col;
                        d.endCol = col + 1;
                        d.severity = DiagnosticSeverity::Error;
                        d.message = L"Unmatched closing bracket";
                        d.suggestion = L"Check missing opener";
                        out.push_back(d);
                    }
                }
            }

            // include diagnostics
            size_t incPos = ln.find(L"#include");
            if (incPos != std::wstring::npos)
            {
                size_t q1 = ln.find(L'"', incPos);
                size_t q2 = std::wstring::npos;
                if (q1 != std::wstring::npos)
                    q2 = ln.find(L'"', q1 + 1);

                // Only validate quoted includes (system includes require include paths)
                if (q1 != std::wstring::npos && q2 != std::wstring::npos && q2 > q1 + 1)
                {
                    size_t start = q1 + 1;
                    size_t end = q2;
                    std::wstring includePath = ln.substr(start, end - start);
                    auto loc = ResolveIncludeAtCursor(filePath, ln, (int)start);
                    if (!loc.has_value())
                    {
                        Diagnostic d;
                        d.line = line;
                        d.startCol = (int)start;
                        d.endCol = (int)end;
                        d.severity = DiagnosticSeverity::Error;
                        d.message = L"Include not found: " + includePath;
                        d.suggestion = L"Check file path or include directories";
                        out.push_back(d);
                    }
                }
            }

            // Naive missing semicolon check (simple statements) with comment stripping
            auto stripComments = [](const std::wstring &s) -> std::wstring
            {
                std::wstring out;
                out.reserve(s.size());
                bool inStringLocal = false;
                bool inCharLocal = false;
                bool escapeLocal = false;
                bool inBlock = false;
                for (size_t i = 0; i < s.size(); ++i)
                {
                    wchar_t c = s[i];
                    if (inBlock)
                    {
                        if (c == L'*' && i + 1 < s.size() && s[i + 1] == L'/')
                        {
                            inBlock = false;
                            i++;
                        }
                        continue;
                    }
                    if (!inStringLocal && !inCharLocal && c == L'/' && i + 1 < s.size())
                    {
                        if (s[i + 1] == L'/')
                            break;
                        if (s[i + 1] == L'*')
                        {
                            inBlock = true;
                            i++;
                            continue;
                        }
                    }
                    if (inStringLocal)
                    {
                        if (escapeLocal)
                            escapeLocal = false;
                        else if (c == L'\\')
                            escapeLocal = true;
                        else if (c == L'"')
                            inStringLocal = false;
                        out.push_back(c);
                        continue;
                    }
                    if (inCharLocal)
                    {
                        if (escapeLocal)
                            escapeLocal = false;
                        else if (c == L'\\')
                            escapeLocal = true;
                        else if (c == L'\'')
                            inCharLocal = false;
                        out.push_back(c);
                        continue;
                    }
                    if (c == L'"')
                        inStringLocal = true;
                    else if (c == L'\'')
                        inCharLocal = true;
                    out.push_back(c);
                }
                return out;
            };

            auto trim = [](const std::wstring &s, size_t &startOut)
            {
                size_t a = s.find_first_not_of(L" \t");
                size_t b = s.find_last_not_of(L" \t");
                startOut = (a == std::wstring::npos) ? 0 : a;
                if (a == std::wstring::npos || b == std::wstring::npos)
                    return std::wstring();
                return s.substr(a, b - a + 1);
            };

            size_t startCol = 0;
            std::wstring cleaned = stripComments(ln);
            std::wstring t = trim(cleaned, startCol);
            if (!t.empty())
            {
                if (t[0] != L'#')
                {
                    wchar_t last = t.back();
                    bool endsOk = (last == L';' || last == L'{' || last == L'}' || last == L':' || last == L',');

                    auto startsWithWord = [&](const std::wstring &kw)
                    {
                        if (t.size() < kw.size())
                            return false;
                        if (ToLower(t.substr(0, kw.size())) != kw)
                            return false;
                        if (t.size() == kw.size())
                            return true;
                        wchar_t c = t[kw.size()];
                        return iswspace(c) || c == L'(';
                    };

                    bool isControl = startsWithWord(L"if") ||
                                     startsWithWord(L"for") ||
                                     startsWithWord(L"while") ||
                                     startsWithWord(L"switch") ||
                                     startsWithWord(L"catch") ||
                                     startsWithWord(L"else") ||
                                     startsWithWord(L"do");

                    if (!endsOk && !isControl)
                    {
                        bool looksLikeStmt = (t.find(L"=") != std::wstring::npos) ||
                                             (t.find(L"(") != std::wstring::npos) ||
                                             (t.find(L")") != std::wstring::npos);
                        bool looksLikeDecl = (t.find(L"(") != std::wstring::npos && last == L')');
                        bool looksLikeScope = (t.find(L"{") != std::wstring::npos || t.find(L"}") != std::wstring::npos);
                        if (looksLikeStmt)
                        {
                            if (!looksLikeDecl && !looksLikeScope)
                            {
                                Diagnostic d;
                                d.line = line;
                                d.startCol = (int)startCol;
                                d.endCol = (int)ln.size();
                                d.severity = DiagnosticSeverity::Warning;
                                d.message = L"Possible missing semicolon";
                                d.suggestion = L"Add ';' at end of line";
                                out.push_back(d);
                            }
                        }
                    }
                }
            }
        }

        for (const auto &it : stack)
        {
            Diagnostic d;
            d.line = it.line;
            d.startCol = it.col;
            d.endCol = it.col + 1;
            d.severity = DiagnosticSeverity::Error;
            d.message = L"Unmatched opening bracket";
            d.suggestion = L"Add matching closing bracket";
            out.push_back(d);
        }

        return out;
    }

    void LspManager::RequestDiagnosticsAsync(const std::wstring &filePath,
                                             const std::vector<std::wstring> &lines,
                                             HWND hwnd,
                                             int tabIndex)
    {
        if (!IsCppFile(std::filesystem::path(filePath)))
            return;
        if (!hwnd)
            return;

        DWORD now = GetTickCount();
        {
            std::lock_guard<std::mutex> lk(mutex_);
            DWORD &last = lastDiagTick_[filePath];
            if (now - last < 200)
                return;
            last = now;
        }

        std::wstring pathCopy = filePath;
        std::vector<std::wstring> linesCopy = lines;

        std::thread([this, hwnd, tabIndex, pathCopy, linesCopy]()
                    {
                        auto diags = AnalyzeDiagnostics(pathCopy, linesCopy);
                        {
                            std::lock_guard<std::mutex> lk(mutex_);
                            diagnostics_[pathCopy] = diags;
                        }

                        auto *payload = new LspDiagnosticsResult();
                        payload->tabIndex = tabIndex;
                        payload->filePath = pathCopy;
                        payload->diagnostics = std::move(diags);
                        PostMessageW(hwnd, WM_LSP_DIAGNOSTICS, 0, (LPARAM)payload);
                    })
            .detach();
    }

    std::optional<Location> LspManager::ResolveIncludeAtCursor(const std::wstring &filePath,
                                                               const std::wstring &lineText,
                                                               int column) const
    {
        std::wstring projectRootCopy;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            projectRootCopy = projectRoot_;
        }

        size_t incPos = lineText.find(L"#include");
        if (incPos == std::wstring::npos)
            return std::nullopt;

        size_t q1 = lineText.find(L'"', incPos);
        size_t q2 = (q1 != std::wstring::npos) ? lineText.find(L'"', q1 + 1) : std::wstring::npos;
        size_t a1 = lineText.find(L'<', incPos);
        size_t a2 = (a1 != std::wstring::npos) ? lineText.find(L'>', a1 + 1) : std::wstring::npos;

        size_t start = std::wstring::npos;
        size_t end = std::wstring::npos;
        bool isAngle = false;
        if (q1 != std::wstring::npos && q2 != std::wstring::npos)
        {
            start = q1 + 1;
            end = q2;
        }
        else if (a1 != std::wstring::npos && a2 != std::wstring::npos)
        {
            start = a1 + 1;
            end = a2;
            isAngle = true;
        }

        if (start == std::wstring::npos || end == std::wstring::npos)
            return std::nullopt;

        if (column < (int)start || column > (int)end)
            return std::nullopt;

        std::wstring inc = lineText.substr(start, end - start);
        if (inc.empty())
            return std::nullopt;

        std::vector<std::filesystem::path> searchRoots;
        std::filesystem::path currentFile(filePath);
        if (!isAngle && currentFile.has_parent_path())
            searchRoots.push_back(currentFile.parent_path());

        if (!projectRootCopy.empty())
        {
            searchRoots.push_back(projectRootCopy);
            searchRoots.push_back(std::filesystem::path(projectRootCopy) / "src");
            searchRoots.push_back(std::filesystem::path(projectRootCopy) / "external");
        }

        for (const auto &root : searchRoots)
        {
            std::filesystem::path candidate = root / inc;
            if (std::filesystem::exists(candidate))
            {
                Location loc;
                loc.filePath = NormalizePath(candidate.wstring());
                loc.line = 0;
                loc.column = 0;
                return loc;
            }
        }

        return std::nullopt;
    }

    std::optional<Location> LspManager::GoToDefinition(const std::wstring &filePath,
                                                       const std::wstring &lineText,
                                                       int line,
                                                       int column,
                                                       const std::wstring &word)
    {
        (void)line;
        auto inc = ResolveIncludeAtCursor(filePath, lineText, column);
        if (inc.has_value())
            return inc;

        std::lock_guard<std::mutex> lk(mutex_);
        auto it = symbolIndex_.find(word);
        if (it != symbolIndex_.end())
            return it->second;
        return std::nullopt;
    }

    std::vector<Diagnostic> LspManager::GetDiagnostics(const std::wstring &filePath) const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = diagnostics_.find(filePath);
        if (it != diagnostics_.end())
            return it->second;
        return {};
    }

    std::wstring LspManager::GetProjectRoot() const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        return projectRoot_;
    }
}
