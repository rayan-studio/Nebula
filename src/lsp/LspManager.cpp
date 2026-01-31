#include "LspManager.h"
#include "core/window/Window.h"
#include "utils/logger/Logger.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <thread>
#include <cwctype>
#include <unordered_set>

namespace Lsp
{
    static std::vector<std::wstring> ReadFileLinesUtf8(const std::filesystem::path &path);

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

    static std::wstring GetExeDir()
    {
        wchar_t buf[MAX_PATH] = {0};
        DWORD len = GetModuleFileNameW(NULL, buf, MAX_PATH);
        if (len == 0)
            return L"";
        std::wstring path(buf, buf + len);
        size_t pos = path.find_last_of(L"\\/");
        if (pos == std::wstring::npos)
            return L"";
        return path.substr(0, pos);
    }

    static void AddPathIfExists(std::vector<std::filesystem::path> &out, const std::filesystem::path &p)
    {
        std::error_code ec;
        if (std::filesystem::exists(p, ec))
            out.push_back(p);
    }

    static std::optional<std::filesystem::path> FindStdHeaderFile(const std::wstring &header, const std::wstring &projectRoot)
    {
        std::vector<std::filesystem::path> roots;

        if (!projectRoot.empty())
        {
            AddPathIfExists(roots, std::filesystem::path(projectRoot) / "external" / "Nebula Studio 2026" / "toolchains" / "mingw64" / "include" / "c++");
            AddPathIfExists(roots, std::filesystem::path(projectRoot) / "external" / "Nebula Studio 2026" / "toolchains" / "mingw64" / "include");
            AddPathIfExists(roots, std::filesystem::path(projectRoot) / "external" / "toolchains" / "mingw64" / "include" / "c++");
            AddPathIfExists(roots, std::filesystem::path(projectRoot) / "external" / "toolchains" / "mingw64" / "include");
        }

        std::wstring exeDir = GetExeDir();
        for (int i = 0; i < 5 && !exeDir.empty(); ++i)
        {
            std::filesystem::path base(exeDir);
            AddPathIfExists(roots, base / "external" / "Nebula Studio 2026" / "toolchains" / "mingw64" / "include" / "c++");
            AddPathIfExists(roots, base / "external" / "Nebula Studio 2026" / "toolchains" / "mingw64" / "include");
            AddPathIfExists(roots, base / "toolchains" / "mingw64" / "include" / "c++");
            AddPathIfExists(roots, base / "toolchains" / "mingw64" / "include");
            size_t pos = exeDir.find_last_of(L"\\/");
            if (pos == std::wstring::npos)
                break;
            exeDir = exeDir.substr(0, pos);
        }

        const std::wstring vsBases[] = {
            L"C:\\Program Files\\Microsoft Visual Studio\\2026\\Community\\VC\\Tools\\MSVC",
            L"C:\\Program Files\\Microsoft Visual Studio\\2026\\BuildTools\\VC\\Tools\\MSVC",
            L"C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Tools\\MSVC",
            L"C:\\Program Files\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC",
            L"C:\\Program Files\\Microsoft Visual Studio\\2022\\Professional\\VC\\Tools\\MSVC",
            L"C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise\\VC\\Tools\\MSVC"};
        for (const auto &base : vsBases)
        {
            std::error_code ec;
            std::filesystem::path root(base);
            if (!std::filesystem::exists(root, ec))
                continue;
            for (const auto &entry : std::filesystem::directory_iterator(root, ec))
            {
                if (ec)
                    break;
                if (!entry.is_directory(ec))
                    continue;
                AddPathIfExists(roots, entry.path() / "include");
            }
        }

        std::error_code ec;
        for (const auto &root : roots)
        {
            if (root.filename() == L"c++")
            {
                for (const auto &entry : std::filesystem::directory_iterator(root, ec))
                {
                    if (ec)
                        break;
                    if (!entry.is_directory(ec))
                        continue;
                    std::filesystem::path cand = entry.path() / header;
                    if (std::filesystem::exists(cand, ec))
                        return cand;
                }
                continue;
            }

            std::filesystem::path cand = root / header;
            if (std::filesystem::exists(cand, ec))
                return cand;
        }

        return std::nullopt;
    }

    static std::optional<std::filesystem::path> ResolveIncludePath(const std::wstring &filePath,
                                                                   const std::wstring &inc,
                                                                   bool isAngle,
                                                                   const std::wstring &projectRootCopy)
    {
        if (inc.empty())
            return std::nullopt;

        std::vector<std::filesystem::path> searchRoots;
        std::filesystem::path currentFile(filePath);
        if (!isAngle && currentFile.has_parent_path())
            searchRoots.push_back(currentFile.parent_path());

        if (!projectRootCopy.empty())
        {
            searchRoots.push_back(std::filesystem::path(projectRootCopy));
            searchRoots.push_back(std::filesystem::path(projectRootCopy) / "src");
            searchRoots.push_back(std::filesystem::path(projectRootCopy) / "external");
        }

        for (const auto &root : searchRoots)
        {
            std::filesystem::path candidate = root / inc;
            std::error_code ec;
            if (std::filesystem::exists(candidate, ec))
                return candidate;
        }

        return std::nullopt;
    }

    static std::optional<std::wstring> ExtractFunctionName(const std::wstring &lineText)
    {
        size_t lparen = lineText.find(L'(');
        if (lparen == std::wstring::npos || lparen == 0)
            return std::nullopt;
        size_t end = lparen;
        while (end > 0 && iswspace(lineText[end - 1]))
            end--;
        size_t start = end;
        auto isIdentChar = [](wchar_t c)
        {
            return iswalnum(c) || c == L'_';
        };
        while (start > 0 && isIdentChar(lineText[start - 1]))
            start--;
        if (end > start)
            return lineText.substr(start, end - start);
        return std::nullopt;
    }

    static std::wstring MapStdSymbolToHeader(const std::wstring &sym)
    {
        static const std::unordered_map<std::wstring, std::wstring> map = {
            {L"string", L"string"},
            {L"wstring", L"string"},
            {L"string_view", L"string_view"},
            {L"vector", L"vector"},
            {L"array", L"array"},
            {L"deque", L"deque"},
            {L"list", L"list"},
            {L"forward_list", L"forward_list"},
            {L"map", L"map"},
            {L"multimap", L"map"},
            {L"set", L"set"},
            {L"multiset", L"set"},
            {L"unordered_map", L"unordered_map"},
            {L"unordered_set", L"unordered_set"},
            {L"unordered_multimap", L"unordered_map"},
            {L"unordered_multiset", L"unordered_set"},
            {L"pair", L"utility"},
            {L"tuple", L"tuple"},
            {L"optional", L"optional"},
            {L"variant", L"variant"},
            {L"any", L"any"},
            {L"regex", L"regex"},
            {L"smatch", L"regex"},
            {L"wregex", L"regex"},
            {L"basic_regex", L"regex"},
            {L"stringstream", L"sstream"},
            {L"istringstream", L"sstream"},
            {L"ostringstream", L"sstream"},
            {L"ifstream", L"fstream"},
            {L"ofstream", L"fstream"},
            {L"fstream", L"fstream"},
            {L"cin", L"iostream"},
            {L"cout", L"iostream"},
            {L"cerr", L"iostream"},
            {L"clog", L"iostream"},
            {L"chrono", L"chrono"},
            {L"time_point", L"chrono"},
            {L"duration", L"chrono"},
            {L"filesystem", L"filesystem"},
            {L"path", L"filesystem"},
            {L"unique_ptr", L"memory"},
            {L"shared_ptr", L"memory"},
            {L"weak_ptr", L"memory"},
            {L"make_unique", L"memory"},
            {L"make_shared", L"memory"},
            {L"function", L"functional"},
            {L"bind", L"functional"},
            {L"move", L"utility"},
            {L"forward", L"utility"},
            {L"thread", L"thread"},
            {L"mutex", L"mutex"},
            {L"lock_guard", L"mutex"},
            {L"unique_lock", L"mutex"}};

        auto it = map.find(sym);
        if (it != map.end())
            return it->second;
        return sym;
    }

    static bool GetWordRangeAtColumn(const std::wstring &line, int column, int &start, int &end)
    {
        start = -1;
        end = -1;
        auto isWordChar = [](wchar_t c)
        {
            return (iswalnum(c) != 0) || (c == L'_');
        };
        if (line.empty())
            return false;
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
        start = left;
        end = right + 1;
        return true;
    }

    static std::optional<Location> ResolveStdSymbolAtCursor(const std::wstring &projectRoot,
                                                            const std::wstring &lineText,
                                                            int column,
                                                            const std::wstring &word)
    {
        if (word.empty())
            return std::nullopt;

        int start = -1;
        int end = -1;
        if (!GetWordRangeAtColumn(lineText, column, start, end))
            return std::nullopt;
        if (start < 5)
            return std::nullopt;
        if (lineText.substr(start - 5, 5) != L"std::")
            return std::nullopt;

        std::wstring header = MapStdSymbolToHeader(word);
        auto headerPath = FindStdHeaderFile(header, projectRoot);
        if (!headerPath.has_value())
            return std::nullopt;

        Location loc;
        loc.filePath = NormalizePath(headerPath->wstring());
        loc.line = 0;
        loc.column = 0;

        auto lines = ReadFileLinesUtf8(*headerPath);
        if (!lines.empty())
        {
            for (size_t i = 0; i < lines.size(); ++i)
            {
                const std::wstring &ln = lines[i];
                if (ln.find(L"class " + word) != std::wstring::npos ||
                    ln.find(L"struct " + word) != std::wstring::npos ||
                    ln.find(L"using " + word) != std::wstring::npos ||
                    (ln.find(L"typedef") != std::wstring::npos && ln.find(word) != std::wstring::npos))
                {
                    loc.line = (int)i;
                    break;
                }
            }
        }

        return loc;
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
        std::unordered_map<std::wstring, Location> newDefLocs;
        std::unordered_map<std::wstring, Location> newDeclLocs;
        std::vector<std::wstring> newIncludes;
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

            if (t.rfind(L"#include", 0) == 0)
            {
                bool isAngle = false;
                size_t q1 = t.find(L'"');
                size_t q2 = std::wstring::npos;
                if (q1 != std::wstring::npos)
                {
                    q2 = t.find(L'"', q1 + 1);
                }
                else
                {
                    size_t a1 = t.find(L'<');
                    size_t a2 = std::wstring::npos;
                    if (a1 != std::wstring::npos)
                    {
                        a2 = t.find(L'>', a1 + 1);
                        if (a2 != std::wstring::npos)
                        {
                            q1 = a1;
                            q2 = a2;
                            isAngle = true;
                        }
                    }
                }

                if (q1 != std::wstring::npos && q2 != std::wstring::npos && q2 > q1 + 1)
                {
                    std::wstring inc = t.substr(q1 + 1, q2 - q1 - 1);
                    auto resolved = ResolveIncludePath(filePath, inc, isAngle, projectRoot_);
                    if (resolved.has_value())
                        newIncludes.push_back(NormalizePath(resolved->wstring()));
                }
            }

            auto addSymbol = [&](const std::wstring &name, int col, bool isDefinition)
            {
                if (name.empty())
                    return;
                Location loc{NormalizePath(filePath), lineIndex, col};
                if (isDefinition)
                    symbolIndexDef_[name] = loc;
                else
                    symbolIndexDecl_[name] = loc;
                if (isDefinition)
                    newDefLocs[name] = loc;
                else
                    newDeclLocs[name] = loc;
                newSymbols.push_back(name);
            };

            auto isIdentChar = [](wchar_t c)
            {
                return iswalnum(c) || c == L'_';
            };

            // class/struct/enum (treat as definition for navigation)
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
                            addSymbol(t.substr(start, end - start), (int)start, true);
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
                        {
                            auto trimLine = [&](const std::wstring &s)
                            {
                                size_t a = s.find_first_not_of(L" \t");
                                size_t b = s.find_last_not_of(L" \t");
                                if (a == std::wstring::npos || b == std::wstring::npos)
                                    return std::wstring();
                                return s.substr(a, b - a + 1);
                            };

                            auto isDefinition = [&]() -> bool
                            {
                                size_t rparen = t.find(L')', lparen);
                                if (rparen == std::wstring::npos)
                                    return false;
                                size_t brace = t.find(L'{', rparen);
                                if (brace != std::wstring::npos)
                                    return true;
                                size_t semi = t.find(L';', rparen);
                                if (semi != std::wstring::npos)
                                    return false;

                                for (int i = lineIndex + 1; i < (int)lines.size(); ++i)
                                {
                                    std::wstring next = lines[i];
                                    size_t cpos = next.find(L"//");
                                    if (cpos != std::wstring::npos)
                                        next = next.substr(0, cpos);
                                    next = trimLine(next);
                                    if (next.empty())
                                        continue;
                                    if (next[0] == L'{')
                                        return true;
                                    if (next.find(L';') != std::wstring::npos)
                                        return false;
                                    // Skip ctor init-list lines or attributes; keep scanning.
                                    if (next[0] == L':' || next.find(L"[[") != std::wstring::npos)
                                        continue;
                                    break;
                                }
                                return false;
                            }();

                            addSymbol(name, (int)start, isDefinition);
                        }
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
                auto sit = symbolIndexDef_.find(sym);
                if (sit != symbolIndexDef_.end() && sit->second.filePath == NormalizePath(filePath))
                    symbolIndexDef_.erase(sit);
                auto dit = symbolIndexDecl_.find(sym);
                if (dit != symbolIndexDecl_.end() && dit->second.filePath == NormalizePath(filePath))
                    symbolIndexDecl_.erase(dit);
            }
            it->second = newSymbols;
        }
        else
        {
            fileSymbols_[filePath] = newSymbols;
        }

        fileSymbolDefLocs_[NormalizePath(filePath)] = std::move(newDefLocs);
        fileSymbolDeclLocs_[NormalizePath(filePath)] = std::move(newDeclLocs);
        fileIncludes_[NormalizePath(filePath)] = std::move(newIncludes);
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
        bool hasWindowsHeader = false;
        std::unordered_set<std::wstring> knownSymbols;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            for (const auto &it : symbolIndexDef_)
                knownSymbols.insert(it.first);
            for (const auto &it : symbolIndexDecl_)
                knownSymbols.insert(it.first);
        }
        for (const auto &line : lines)
        {
            std::wstring lower = ToLower(line);
            if (lower.find(L"#include") != std::wstring::npos &&
                (lower.find(L"<windows.h>") != std::wstring::npos || lower.find(L"\"windows.h\"") != std::wstring::npos))
            {
                hasWindowsHeader = true;
                break;
            }
        }
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
                        bool looksLikeCall = (t.find(L"(") != std::wstring::npos);
                        bool looksLikeDecl = (t.find(L"(") != std::wstring::npos && last == L')');
                        bool looksLikeScope = (t.find(L"{") != std::wstring::npos || t.find(L"}") != std::wstring::npos);
                        if (looksLikeStmt)
                        {
                            if (!looksLikeDecl && !looksLikeScope && !looksLikeCall)
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

            // Known API typos (simple heuristic)
            {
                auto isIdentChar = [](wchar_t c)
                {
                    return (iswalnum(c) != 0) || (c == L'_');
                };
                auto checkTypo = [&](const std::wstring &bad, const std::wstring &good)
                {
                    size_t pos = cleaned.find(bad);
                    while (pos != std::wstring::npos)
                    {
                        bool leftOk = (pos == 0) || !isIdentChar(cleaned[pos - 1]);
                        bool rightOk = (pos + bad.size() >= cleaned.size()) || !isIdentChar(cleaned[pos + bad.size()]);
                        if (leftOk && rightOk)
                        {
                            Diagnostic d;
                            d.line = line;
                            d.startCol = (int)pos;
                            d.endCol = (int)(pos + bad.size());
                            d.severity = DiagnosticSeverity::Error;
                            d.message = L"Unknown identifier: " + bad;
                            d.suggestion = L"Did you mean " + good + L"?";
                            out.push_back(d);
                            break;
                        }
                        pos = cleaned.find(bad, pos + 1);
                    }
                };

                checkTypo(L"DefWowProcW", L"DefWindowProcW");
            }

            // Suspicious WinAPI fallback calls like "DefW(" or "DefA(" (likely typo)
            if (hasWindowsHeader)
            {
                auto isIdentChar = [](wchar_t c)
                {
                    return (iswalnum(c) != 0) || (c == L'_');
                };
                size_t pos = cleaned.find(L"Def");
                while (pos != std::wstring::npos)
                {
                    if (pos == 0 || !isIdentChar(cleaned[pos - 1]))
                    {
                        size_t end = pos + 3;
                        while (end < cleaned.size() && isIdentChar(cleaned[end]))
                            ++end;
                        std::wstring ident = cleaned.substr(pos, end - pos);
                        if (ident.size() <= 6)
                        {
                            wchar_t last = ident.back();
                            if ((last == L'W' || last == L'A') &&
                                ident != L"DefWindowProcW" && ident != L"DefWindowProcA")
                            {
                                Diagnostic d;
                                d.line = line;
                                d.startCol = (int)pos;
                                d.endCol = (int)end;
                                d.severity = DiagnosticSeverity::Error;
                                d.message = L"Unknown WinAPI function: " + ident;
                                d.suggestion = (last == L'W') ? L"Use DefWindowProcW" : L"Use DefWindowProcA";
                                out.push_back(d);
                                break;
                            }
                        }
                    }
                        pos = cleaned.find(L"Def", pos + 1);
                }
            }

            // Heuristic: unknown WinAPI-like function calls (windows.h included)
            if (hasWindowsHeader)
            {
                static const std::unordered_set<std::wstring> kAllow = {
                    L"DefWindowProcW", L"DefWindowProcA",
                    L"PostQuitMessage",
                    L"LoadCursor", L"LoadCursorW", L"LoadCursorA",
                    L"RegisterClassW", L"RegisterClassA", L"RegisterClassExW", L"RegisterClassExA",
                    L"CreateWindowExW", L"CreateWindowExA",
                    L"ShowWindow", L"UpdateWindow",
                    L"GetMessageW", L"GetMessageA",
                    L"TranslateMessage", L"DispatchMessageW", L"DispatchMessageA",
                    L"BeginPaint", L"EndPaint",
                    L"InvalidateRect", L"GetClientRect", L"GetWindowRect",
                    L"GetModuleHandleW", L"GetModuleHandleA",
                    L"SetWindowLongPtrW", L"SetWindowLongPtrA",
                    L"GetWindowLongPtrW", L"GetWindowLongPtrA",
                    L"SetWindowPos", L"SendMessageW", L"SendMessageA",
                    L"PeekMessageW", L"PeekMessageA",
                    L"MessageBoxW", L"MessageBoxA"
                };
                static const std::wstring kPrefixes[] = {
                    L"Get", L"Set", L"Post", L"Create", L"Register",
                    L"Load", L"Show", L"Dispatch", L"Translate", L"Def",
                    L"Peek", L"Send", L"Destroy", L"Update", L"Begin", L"End",
                    L"Invalidate", L"MessageBox"
                };
                auto isIdentChar = [](wchar_t c)
                {
                    return (iswalnum(c) != 0) || (c == L'_');
                };
                auto isKeyword = [](const std::wstring &s)
                {
                    static const std::unordered_set<std::wstring> k = {
                        L"if", L"for", L"while", L"switch", L"case", L"default",
                        L"return", L"sizeof", L"typedef", L"catch", L"else",
                        L"do", L"static", L"const", L"inline", L"struct", L"class",
                        L"enum", L"namespace", L"using", L"new", L"delete"
                    };
                    return k.find(ToLower(s)) != k.end();
                };
                auto isAllCaps = [](const std::wstring &s)
                {
                    bool any = false;
                    for (wchar_t c : s)
                    {
                        if (iswalpha(c))
                        {
                            any = true;
                            if (!iswupper(c))
                                return false;
                        }
                    }
                    return any;
                };

                bool added = false;
                for (size_t i = 0; i < cleaned.size(); ++i)
                {
                    if (!isIdentChar(cleaned[i]) || (i > 0 && isIdentChar(cleaned[i - 1])))
                        continue;
                    size_t start = i;
                    size_t end = i + 1;
                    while (end < cleaned.size() && isIdentChar(cleaned[end]))
                        ++end;
                    std::wstring ident = cleaned.substr(start, end - start);
                    if (ident.empty())
                        continue;
                    if (isKeyword(ident) || isAllCaps(ident))
                        continue;
                    // Skip member/namespace calls (obj.Func(), ns::Func())
                    if (start > 1)
                    {
                        wchar_t prev = cleaned[start - 1];
                        wchar_t prev2 = cleaned[start - 2];
                        if (prev == L'.' || (prev == L'>' && prev2 == L'-') || prev == L':')
                            continue;
                    }
                    // Lookahead for a call
                    size_t j = end;
                    while (j < cleaned.size() && iswspace(cleaned[j]))
                        ++j;
                    if (j >= cleaned.size() || cleaned[j] != L'(')
                        continue;
                    if (knownSymbols.find(ident) != knownSymbols.end())
                        continue;
                    if (kAllow.find(ident) != kAllow.end())
                        continue;
                    bool prefixMatch = false;
                    for (const auto &p : kPrefixes)
                    {
                        if (ident.rfind(p, 0) == 0)
                        {
                            prefixMatch = true;
                            break;
                        }
                    }
                    if (!prefixMatch)
                        continue;

                    Diagnostic d;
                    d.line = line;
                    d.startCol = (int)start;
                    d.endCol = (int)end;
                    d.severity = DiagnosticSeverity::Error;
                    d.message = L"Unknown WinAPI function: " + ident;
                    d.suggestion = L"Check spelling or include the correct header";
                    out.push_back(d);
                    added = true;
                    break;
                }
                (void)added;
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

        // Be lenient: if the user clicks anywhere on the include line, try to resolve.
        // This avoids false negatives when the column mapping is slightly off.

        std::wstring inc = lineText.substr(start, end - start);
        if (inc.empty())
            return std::nullopt;

        Logger::Instance().Log(L"LSP include resolve: " + inc +
                               L" (angle=" + std::wstring(isAngle ? L"true" : L"false") + L")");

        auto resolved = ResolveIncludePath(filePath, inc, isAngle, projectRootCopy);
        if (resolved.has_value())
        {
            Location loc;
            loc.filePath = NormalizePath(resolved->wstring());
            loc.line = 0;
            loc.column = 0;
            Logger::Instance().Log(L"LSP include resolved: " + loc.filePath);
            return loc;
        }

        if (isAngle)
        {
            auto stdHeader = FindStdHeaderFile(inc, projectRootCopy);
            if (stdHeader.has_value())
            {
                Location loc;
                loc.filePath = NormalizePath(stdHeader->wstring());
                loc.line = 0;
                loc.column = 0;
                Logger::Instance().Log(L"LSP std include resolved: " + loc.filePath);
                return loc;
            }
        }

        Logger::Instance().Log(L"LSP include NOT found: " + inc + L" (root=" + projectRootCopy + L")");
        return std::nullopt;
    }

    std::optional<Location> LspManager::GoToDefinition(const std::wstring &filePath,
                                                       const std::wstring &lineText,
                                                       int line,
                                                       int column,
                                                       const std::wstring &word)
    {
        auto inc = ResolveIncludeAtCursor(filePath, lineText, column);
        if (inc.has_value())
            return inc;

        std::wstring lookupWord = word;
        if (!lookupWord.empty())
        {
            static const std::unordered_set<std::wstring> kTypeKeywords = {
                L"void", L"int", L"float", L"double", L"char", L"wchar_t", L"bool",
                L"short", L"long", L"signed", L"unsigned", L"auto", L"const", L"static"};
            if (kTypeKeywords.find(lookupWord) != kTypeKeywords.end())
            {
                auto fn = ExtractFunctionName(lineText);
                if (fn.has_value())
                    lookupWord = *fn;
            }
        }

        {
            std::wstring rootCopy;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                rootCopy = projectRoot_;
            }
            auto stdLoc = ResolveStdSymbolAtCursor(rootCopy, lineText, column, lookupWord);
            if (stdLoc.has_value())
                return stdLoc;
        }

        std::lock_guard<std::mutex> lk(mutex_);
        std::wstring normFile = NormalizePath(filePath);

        auto findInFile = [&](const std::wstring &path, bool allowDecl) -> std::optional<Location>
        {
            auto dIt = fileSymbolDefLocs_.find(path);
            if (dIt != fileSymbolDefLocs_.end())
            {
                auto it = dIt->second.find(lookupWord);
                if (it != dIt->second.end())
                    return it->second;
            }
            if (allowDecl)
            {
                auto cIt = fileSymbolDeclLocs_.find(path);
                if (cIt != fileSymbolDeclLocs_.end())
                {
                    auto it = cIt->second.find(lookupWord);
                    if (it != cIt->second.end())
                        return it->second;
                }
            }
            return std::nullopt;
        };

        if (auto loc = findInFile(normFile, false))
            return loc;

        std::vector<std::wstring> queue;
        std::unordered_set<std::wstring> visited;
        visited.insert(normFile);

        auto incIt = fileIncludes_.find(normFile);
        if (incIt != fileIncludes_.end())
            queue = incIt->second;

        for (int depth = 0; depth < 4; ++depth)
        {
            if (queue.empty())
                break;
            std::vector<std::wstring> next;
            for (const auto &incPath : queue)
            {
                if (visited.find(incPath) != visited.end())
                    continue;
                visited.insert(incPath);

                if (auto loc = findInFile(incPath, true))
                    return loc;

                auto it = fileIncludes_.find(incPath);
                if (it != fileIncludes_.end())
                {
                    for (const auto &p : it->second)
                        next.push_back(p);
                }
            }
            queue.swap(next);
        }

        auto it = symbolIndexDef_.find(lookupWord);
        if (it != symbolIndexDef_.end())
            return it->second;
        auto dit = symbolIndexDecl_.find(lookupWord);
        if (dit != symbolIndexDecl_.end())
            return dit->second;
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
