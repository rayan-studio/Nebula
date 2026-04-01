#include "LspManager.h"
#include "ClangdClient.h"
#include "core/window/Window.h"
#include "utils/logger/Logger.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <thread>
#include <chrono>
#include <cwctype>
#include <unordered_set>
#include <cstdlib>

namespace Lsp
{
    static constexpr DWORD CLANGD_RESTART_TIMEOUT_MS = 10000;
    static constexpr DWORD DIAG_REQUEST_THROTTLE_MS = 120;
    static constexpr int CLANGD_DIDCHANGE_DEBOUNCE_MS = 120;

    static bool IsLspTraceEnabled()
    {
        static int cached = -1;
        if (cached == -1)
        {
            char* env = nullptr;
            size_t envLen = 0;
            errno_t err = _dupenv_s(&env, &envLen, "NEBULA_LSP_TRACE");
            cached = (err == 0 && env && env[0] != '\0' && env[0] != '0') ? 1 : 0;
            if (env)
                free(env);
        }
        return cached == 1;
    }

    static void TraceLsp(const std::wstring& msg)
    {
        if (IsLspTraceEnabled())
            Logger::Instance().Log(L"[LSP-TRACE] " + msg);
    }

    static std::string WideToUtf8(const std::wstring& w)
    {
        if (w.empty()) return {};
        int size = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        if (size <= 0) return {};
        std::string result(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), result.data(), size, nullptr, nullptr);
        return result;
    }

    // Standard Library completions database
    static const std::vector<CompletionItem> GetStdCompletions()
    {
        return {
            // Containers
            {L"std::vector",  L"Dynamic array",  L"std_container"},
            {L"std::array",  L"Fixed-size array",  L"std_container"},
            {L"std::deque",  L"Double-ended queue",  L"std_container"},
            {L"std::list",  L"Doubly-linked list",  L"std_container"},
            {L"std::forward_list",  L"Singly-linked list",  L"std_container"},
            {L"std::set",  L"Unique ordered elements",  L"std_container"},
            {L"std::multiset",  L"Multiple ordered elements",  L"std_container"},
            {L"std::map",  L"Key-value pairs (ordered)",  L"std_container"},
            {L"std::multimap",  L"Multiple key-value pairs",  L"std_container"},
            {L"std::unordered_set",  L"Unique unordered elements",  L"std_container"},
            {L"std::unordered_map",  L"Key-value pairs (unordered)",  L"std_container"},
            {L"std::stack",  L"LIFO container adapter",  L"std_container"},
            {L"std::queue",  L"FIFO container adapter",  L"std_container"},
            {L"std::priority_queue",  L"Priority queue adapter",  L"std_container"},
            {L"std::string",  L"Character sequence",  L"std_string"},
            {L"std::wstring",  L"Wide character sequence",  L"std_string"},

            // Algorithms
            {L"std::sort",  L"Sort elements",  L"std_algorithm"},
            {L"std::find",  L"Find element",  L"std_algorithm"},
            {L"std::find_if",  L"Find with condition",  L"std_algorithm"},
            {L"std::count",  L"Count occurrences",  L"std_algorithm"},
            {L"std::copy",  L"Copy elements",  L"std_algorithm"},
            {L"std::transform",  L"Apply function to range",  L"std_algorithm"},
            {L"std::for_each",  L"Apply function to each element",  L"std_algorithm"},
            {L"std::reverse",  L"Reverse elements",  L"std_algorithm"},
            {L"std::unique",  L"Remove duplicates",  L"std_algorithm"},
            {L"std::binary_search",  L"Binary search",  L"std_algorithm"},
            {L"std::lower_bound",  L"Find lower bound",  L"std_algorithm"},
            {L"std::upper_bound",  L"Find upper bound",  L"std_algorithm"},

            // Utilities
            {L"std::pair",  L"Two-element tuple",  L"std_utility"},
            {L"std::tuple",  L"Multiple-element tuple",  L"std_utility"},
            {L"std::optional",  L"Value that may not exist",  L"std_utility"},
            {L"std::variant",  L"Type-safe union",  L"std_utility"},
            {L"std::unique_ptr",  L"Unique ownership pointer",  L"std_memory"},
            {L"std::shared_ptr",  L"Shared ownership pointer",  L"std_memory"},
            {L"std::make_unique",  L"Create unique_ptr",  L"std_memory"},
            {L"std::make_shared",  L"Create shared_ptr",  L"std_memory"},

            // I/O
            {L"std::cout",  L"Standard output stream",  L"std_io"},
            {L"std::cin",  L"Standard input stream",  L"std_io"},
            {L"std::cerr",  L"Standard error stream",  L"std_io"},
            {L"std::endl",  L"Newline and flush",  L"std_io"},
            {L"std::ifstream",  L"Input file stream",  L"std_io"},
            {L"std::ofstream",  L"Output file stream",  L"std_io"},
            {L"std::stringstream",  L"String stream",  L"std_io"},

            // Threading
            {L"std::thread",  L"Thread of execution",  L"std_threading"},
            {L"std::mutex",  L"Mutual exclusion lock",  L"std_threading"},
            {L"std::lock_guard",  L"Automatic lock holder",  L"std_threading"},
            {L"std::unique_lock",  L"Exclusive lock holder",  L"std_threading"},
            {L"std::condition_variable",  L"Condition variable",  L"std_threading"},

            // Other common
            {L"std::function",  L"Function wrapper",  L"std_utility"},
            {L"std::chrono",  L"Time utilities",  L"std_chrono"},
            {L"std::random",  L"Random number generation",  L"std_random"},
            {L"std::exception",  L"Exception base class",  L"std_exception"},
        };
    }

    static std::vector<std::wstring> ReadFileLinesUtf8(const std::filesystem::path &path);

    // Map std:: symbols to their required headers
    static std::wstring GetStdHeaderFor(const std::wstring &symbol)
    {
        static const std::unordered_map<std::wstring,  std::wstring> map = {
            // Containers
            {L"std::vector",  L"<vector>"},
            {L"std::array",  L"<array>"},
            {L"std::deque",  L"<deque>"},
            {L"std::list",  L"<list>"},
            {L"std::forward_list",  L"<forward_list>"},
            {L"std::set",  L"<set>"},
            {L"std::multiset",  L"<set>"},
            {L"std::map",  L"<map>"},
            {L"std::multimap",  L"<map>"},
            {L"std::unordered_set",  L"<unordered_set>"},
            {L"std::unordered_map",  L"<unordered_map>"},
            {L"std::stack",  L"<stack>"},
            {L"std::queue",  L"<queue>"},
            {L"std::priority_queue",  L"<queue>"},
            {L"std::string",  L"<string>"},
            {L"std::wstring",  L"<string>"},

            // Algorithms
            {L"std::sort",  L"<algorithm>"},
            {L"std::find",  L"<algorithm>"},
            {L"std::find_if",  L"<algorithm>"},
            {L"std::count",  L"<algorithm>"},
            {L"std::copy",  L"<algorithm>"},
            {L"std::transform",  L"<algorithm>"},
            {L"std::for_each",  L"<algorithm>"},
            {L"std::reverse",  L"<algorithm>"},
            {L"std::unique",  L"<algorithm>"},
            {L"std::binary_search",  L"<algorithm>"},
            {L"std::lower_bound",  L"<algorithm>"},
            {L"std::upper_bound",  L"<algorithm>"},

            // Utilities
            {L"std::pair",  L"<utility>"},
            {L"std::tuple",  L"<tuple>"},
            {L"std::optional",  L"<optional>"},
            {L"std::variant",  L"<variant>"},
            {L"std::unique_ptr",  L"<memory>"},
            {L"std::shared_ptr",  L"<memory>"},
            {L"std::make_unique",  L"<memory>"},
            {L"std::make_shared",  L"<memory>"},

            // I/O
            {L"std::cout",  L"<iostream>"},
            {L"std::cin",  L"<iostream>"},
            {L"std::cerr",  L"<iostream>"},
            {L"std::endl",  L"<iostream>"},
            {L"std::ifstream",  L"<fstream>"},
            {L"std::ofstream",  L"<fstream>"},
            {L"std::stringstream",  L"<sstream>"},

            // Threading
            {L"std::thread",  L"<thread>"},
            {L"std::mutex",  L"<mutex>"},
            {L"std::lock_guard",  L"<mutex>"},
            {L"std::unique_lock",  L"<mutex>"},
            {L"std::condition_variable",  L"<condition_variable>"},

            // Other
            {L"std::function",  L"<functional>"},
            {L"std::chrono",  L"<chrono>"},
            {L"std::random",  L"<random>"},
            {L"std::exception",  L"<exception>"},
        };

        auto it = map.find(symbol);
        if (it != map.end())
        return it->second;
        return L"";
    }

    static std::vector<std::wstring> ReadFileLinesUtf8(const std::filesystem::path &path);

    static std::wstring ToLower(std::wstring v)
    {
        for (auto &c: v)
        c = (wchar_t)towlower(c);
        return v;
    }

    static std::wstring NormalizePath(const std::wstring &path)
    {
        try
        {
            std::filesystem::path p(path);
            std::error_code ec;
            auto norm = std::filesystem::weakly_canonical(p,  ec);
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
        DWORD len = GetModuleFileNameW(NULL,  buf,  MAX_PATH);
        if (len == 0)
        return L"";
        std::wstring path(buf,  buf +len);
        size_t pos = path.find_last_of(L"\\/");
        if (pos == std::wstring::npos)
        return L"";
        return path.substr(0,  pos);
    }

    static void AddPathIfExists(std::vector<std::filesystem::path> &out,  const std::filesystem::path &p)
    {
        std::error_code ec;
        if (std::filesystem::exists(p,  ec))
        out.push_back(p);
    }

    static std::optional<std::filesystem::path> FindStdHeaderFile(const std::wstring &header,  const std::wstring &projectRoot)
    {
        std::vector<std::filesystem::path> roots;

        if (!projectRoot.empty())
        {
            AddPathIfExists(roots,  std::filesystem::path(projectRoot) /"external" /"mingw" /"include" /"c++");
            AddPathIfExists(roots,  std::filesystem::path(projectRoot) /"external" /"mingw" /"include");
        }

        std::wstring exeDir = GetExeDir();
        for (int i = 0; i < 5 && !exeDir.empty(); ++i)
        {
            std::filesystem::path base(exeDir);
            AddPathIfExists(roots,  base /"external" /"mingw" /"include" /"c++");
            AddPathIfExists(roots,  base /"external" /"mingw" /"include");
            AddPathIfExists(roots,  base /"mingw" /"include" /"c++");
            AddPathIfExists(roots,  base /"mingw" /"include");
            size_t pos = exeDir.find_last_of(L"\\/");
            if (pos == std::wstring::npos)
            break;
            exeDir = exeDir.substr(0,  pos);
        }

        const std::wstring vsBases[] = {
            L"C:\\Program Files\\Microsoft Visual Studio\\2026\\Community\\VC\\Tools\\MSVC",
            L"C:\\Program Files\\Microsoft Visual Studio\\2026\\BuildTools\\VC\\Tools\\MSVC",
            L"C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Tools\\MSVC",
            L"C:\\Program Files\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC",
            L"C:\\Program Files\\Microsoft Visual Studio\\2022\\Professional\\VC\\Tools\\MSVC",
            L"C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise\\VC\\Tools\\MSVC"};
        for (const auto &base: vsBases)
        {
            std::error_code ec;
            std::filesystem::path root(base);
            if (!std::filesystem::exists(root,  ec))
            continue;
            for (const auto &entry: std::filesystem::directory_iterator(root,  ec))
            {
                if (ec)
                break;
                if (!entry.is_directory(ec))
                continue;
                AddPathIfExists(roots,  entry.path() /"include");
            }
        }

        std::error_code ec;
        for (const auto &root: roots)
        {
            if (root.filename() == L"c++")
            {
                for (const auto &entry: std::filesystem::directory_iterator(root,  ec))
                {
                    if (ec)
                    break;
                    if (!entry.is_directory(ec))
                    continue;
                    std::filesystem::path cand = entry.path() /header;
                    if (std::filesystem::exists(cand,  ec))
                    return cand;
                }
                continue;
            }

            std::filesystem::path cand = root /header;
            if (std::filesystem::exists(cand,  ec))
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
            searchRoots.push_back(std::filesystem::path(projectRootCopy) /"src");
            searchRoots.push_back(std::filesystem::path(projectRootCopy) /"external");
        }

        for (const auto &root: searchRoots)
        {
            std::filesystem::path candidate = root /inc;
            std::error_code ec;
            if (std::filesystem::exists(candidate,  ec))
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
        while (end > 0 && iswspace(lineText[end -1]))
        end--;
        size_t start = end;
        auto isIdentChar = [](wchar_t c)
        {
            return iswalnum(c) || c == L'_';
        };
        while (start > 0 && isIdentChar(lineText[start -1]))
        start--;
        if (end > start)
        return lineText.substr(start,  end -start);
        return std::nullopt;
    }

    static std::wstring MapStdSymbolToHeader(const std::wstring &sym)
    {
        static const std::unordered_map<std::wstring,  std::wstring> map = {
            {L"string",  L"string"},
            {L"wstring",  L"string"},
            {L"string_view",  L"string_view"},
            {L"vector",  L"vector"},
            {L"array",  L"array"},
            {L"deque",  L"deque"},
            {L"list",  L"list"},
            {L"forward_list",  L"forward_list"},
            {L"map",  L"map"},
            {L"multimap",  L"map"},
            {L"set",  L"set"},
            {L"multiset",  L"set"},
            {L"unordered_map",  L"unordered_map"},
            {L"unordered_set",  L"unordered_set"},
            {L"unordered_multimap",  L"unordered_map"},
            {L"unordered_multiset",  L"unordered_set"},
            {L"pair",  L"utility"},
            {L"tuple",  L"tuple"},
            {L"optional",  L"optional"},
            {L"variant",  L"variant"},
            {L"any",  L"any"},
            {L"regex",  L"regex"},
            {L"smatch",  L"regex"},
            {L"wregex",  L"regex"},
            {L"basic_regex",  L"regex"},
            {L"stringstream",  L"sstream"},
            {L"istringstream",  L"sstream"},
            {L"ostringstream",  L"sstream"},
            {L"ifstream",  L"fstream"},
            {L"ofstream",  L"fstream"},
            {L"fstream",  L"fstream"},
            {L"cin",  L"iostream"},
            {L"cout",  L"iostream"},
            {L"cerr",  L"iostream"},
            {L"clog",  L"iostream"},
            {L"chrono",  L"chrono"},
            {L"time_point",  L"chrono"},
            {L"duration",  L"chrono"},
            {L"filesystem",  L"filesystem"},
            {L"path",  L"filesystem"},
            {L"unique_ptr",  L"memory"},
            {L"shared_ptr",  L"memory"},
            {L"weak_ptr",  L"memory"},
            {L"make_unique",  L"memory"},
            {L"make_shared",  L"memory"},
            {L"function",  L"functional"},
            {L"bind",  L"functional"},
            {L"move",  L"utility"},
            {L"forward",  L"utility"},
            {L"thread",  L"thread"},
            {L"mutex",  L"mutex"},
            {L"lock_guard",  L"mutex"},
            {L"unique_lock",  L"mutex"}};

        auto it = map.find(sym);
        if (it != map.end())
        return it->second;
        return sym;
    }

    static bool GetWordRangeAtColumn(const std::wstring &line,  int column,  int &start,  int &end)
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
        col = (int)line.size() -1;
        if (col < 0 || col >= (int)line.size())
        return false;
        if (!isWordChar(line[col]))
        return false;
        int left = col;
        while (left > 0 && isWordChar(line[left -1]))
        --left;
        int right = col;
        while (right +1 < (int)line.size() && isWordChar(line[right +1]))
        ++right;
        if (right < left)
        return false;
        start = left;
        end = right +1;
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
        if (!GetWordRangeAtColumn(lineText,  column,  start,  end))
        return std::nullopt;
        if (start < 5)
        return std::nullopt;
        if (lineText.substr(start -5,  5) != L"std::")
        return std::nullopt;

        std::wstring header = MapStdSymbolToHeader(word);
        auto headerPath = FindStdHeaderFile(header,  projectRoot);
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
                if (ln.find(L"class " +word) != std::wstring::npos ||
                ln.find(L"struct " +word) != std::wstring::npos ||
                ln.find(L"using " +word) != std::wstring::npos ||
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
        std::ifstream file(path,  std::ios::binary);
        if (!file.is_open())
        return lines;

        std::string data;
        file.seekg(0,  std::ios::end);
        std::streamoff fsize = file.tellg();
        if (fsize > 0)
        {
            file.seekg(0,  std::ios::beg);
            data.resize((size_t)fsize);
            file.read(&data[0],  fsize);
        }
        file.close();

        if (data.empty())
        return lines;

        // UTF-8 decode (fallback: ACP)
        int wlen = MultiByteToWideChar(CP_UTF8,  MB_ERR_INVALID_CHARS,  data.data(),  (int)data.size(),  nullptr,  0);
        std::wstring w;
        if (wlen > 0)
        {
            w.resize((size_t)wlen);
            MultiByteToWideChar(CP_UTF8,  MB_ERR_INVALID_CHARS,  data.data(),  (int)data.size(),  w.data(),  wlen);
        }
        else
        {
            wlen = MultiByteToWideChar(CP_ACP,  0,  data.data(),  (int)data.size(),  nullptr,  0);
            w.resize((size_t)wlen);
            MultiByteToWideChar(CP_ACP,  0,  data.data(),  (int)data.size(),  w.data(),  wlen);
        }

        size_t start = 0;
        for (size_t i = 0; i < w.size(); ++i)
        {
            if (w[i] == L'\r' || w[i] == L'\n')
            {
                lines.push_back(w.substr(start,  i -start));
                if (w[i] == L'\r' && i +1 < w.size() && w[i +1] == L'\n')
                i++;
                start = i +1;
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

    void LspManager::ApplyClangdRunningState(bool running, bool ready)
    {
        // Must be called with mutex_ held.
        if (ready)
        {
            clangdState_ = ClangdRuntimeState::Running;
            clangdReason_.clear();
            clangdRestartingUntilTick_ = 0;
        }
        else if (running)
        {
            clangdState_ = ClangdRuntimeState::Restarting;
            clangdReason_ = L"synchronisation clangd";
            clangdRestartingUntilTick_ = GetTickCount() + CLANGD_RESTART_TIMEOUT_MS;
        }
    }

    ClangdUiStatus LspManager::GetClangdUiStatus() const
    {
        bool runningNow = false, readyNow = false;
        std::wstring failureReason;
        ClangdClient::Instance().GetRunningState(runningNow, readyNow, failureReason);

        ClangdUiStatus status;
        std::lock_guard<std::mutex> lk(mutex_);
        status.state = clangdState_;
        status.reason = clangdReason_;
        ClangdRuntimeState previousState = status.state;
        DWORD now = GetTickCount();
        if (readyNow)
        {
            status.state = ClangdRuntimeState::Running;
            status.reason.clear();
        }
        else if (runningNow)
        {
            if (clangdRestartingUntilTick_ != 0 && now >= clangdRestartingUntilTick_)
            {
                status.state = ClangdRuntimeState::Failed;
                status.reason = failureReason.empty() ? L"initialize timeout" : failureReason;
            }
            else
            {
                status.state = ClangdRuntimeState::Restarting;
                status.reason = failureReason.empty() ? L"synchronisation clangd" : failureReason;
            }
        }
        else
        {
            status.state = ClangdRuntimeState::Failed;
            if (!failureReason.empty())
                status.reason = failureReason;
            else if (previousState == ClangdRuntimeState::Restarting &&
                     clangdRestartingUntilTick_ != 0 && now >= clangdRestartingUntilTick_)
                status.reason = L"restart timeout";
            else if (status.reason.empty() || status.reason == L"synchronisation clangd")
                status.reason = L"clangd arrete";
        }
        return status;
    }

    std::unordered_map<std::wstring, std::vector<std::wstring>> LspManager::GetFileIncludes() const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        return fileIncludes_;
    }

    void LspManager::SetProjectRoot(const std::wstring &rootPath)
    {
        const std::wstring normalizedRoot = NormalizePath(rootPath);
        bool rootChanged = false;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            rootChanged = (projectRoot_ != normalizedRoot);
            if (rootChanged)
            {
                diagnostics_.clear();
                fileSymbols_.clear();
                fileSymbolDefLocs_.clear();
                fileSymbolDeclLocs_.clear();
                fileIncludes_.clear();
                symbolIndexDef_.clear();
                symbolIndexDecl_.clear();
                lastDiagTick_.clear();
                clangdLastDiagTickByFile_.clear();
                clangdPendingVer_.clear();
            }
            projectRoot_ = normalizedRoot;
        }
        if (rootChanged)
            StartProjectIndexAsync();

        // Start the real clangd LSP client for this project.
        // If clangd is already running (project switch), restart it for the new root.
        bool running = false, ready = false;
        std::wstring failureReason;
        ClangdClient::Instance().GetRunningState(running, ready, failureReason);

        bool started = false;
        if (running)
            started = rootChanged ? ClangdClient::Instance().RestartForProject(normalizedRoot) : true;
        else
            started = ClangdClient::Instance().Start(normalizedRoot);

        ClangdClient::Instance().GetRunningState(running, ready, failureReason);
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (started)
                ApplyClangdRunningState(running, ready);
            else
            {
                clangdState_ = ClangdRuntimeState::Failed;
                clangdReason_ = failureReason.empty() ? L"clangd introuvable ou lancement echoue" : failureReason;
                clangdRestartingUntilTick_ = 0;
            }
        }

        // If there's no compile_commands.json yet, regenerate it in the background
        // and restart clangd once it's ready — fixes "iostream not found" on first launch.
        ClangdClient::Instance().EnsureCompileCommandsAsync(normalizedRoot);

        ClangdClient::Instance().SetDiagnosticsCallback(
            [this](const std::wstring& filePath, int tabIndex, HWND hwnd, const std::vector<Diagnostic>& diags) {
                {
                    std::lock_guard<std::mutex> lk(mutex_);
                    diagnostics_[filePath] = diags;
                    clangdLastDiagTickByFile_[filePath] = GetTickCount();
                }

                auto* payload = new LspDiagnosticsResult();
                payload->tabIndex   = tabIndex;
                payload->filePath   = filePath;
                payload->diagnostics = diags;
                PostMessageW(hwnd, WM_LSP_DIAGNOSTICS, 0, (LPARAM)payload);
            });
    }

    void LspManager::StartProjectIndexAsync()
    {
        bool expected = false;
        if (!indexing_.compare_exchange_strong(expected, true))
            return;

        std::wstring root;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            root = projectRoot_;
        }
        if (root.empty())
        {
            indexing_ = false;
            return;
        }

        std::thread([this,  root]()
        {
            try
            {
                std::filesystem::path base(root);
                if (!std::filesystem::exists(base))
                {
                    indexing_ = false;
                    return;
                }

                const std::unordered_set<std::wstring> skipDirs = {
                    L".git",  L".idea",  L".vs",  L".vscode",
                    L"build",  L"build-ninja",  L"dist",  L"external",
                    L"logs",  L"out",  L"bin",  L"obj"
                };
                const uintmax_t maxIndexBytes = 2 * 1024 * 1024;// skip huge files

                auto lower = [](std::wstring s)
                {
                    for (auto &ch: s)
                    ch = (wchar_t)towlower(ch);
                    return s;
                };

                std::filesystem::recursive_directory_iterator it(
                base,
                std::filesystem::directory_options::skip_permission_denied);
                std::filesystem::recursive_directory_iterator end;
                for (; it != end; ++it)
                {
                    const auto &entry = *it;
                    if (entry.is_directory())
                    {
                        std::wstring name = lower(entry.path().filename().wstring());
                        if (skipDirs.find(name) != skipDirs.end())
                        {
                            it.disable_recursion_pending();
                            continue;
                        }
                        continue;
                    }
                    if (!entry.is_regular_file())
                    continue;
                    if (!IsCppFile(entry.path()))
                    continue;
                    std::error_code ec;
                    uintmax_t fsize = entry.file_size(ec);
                    if (!ec && fsize > maxIndexBytes)
                    continue;

                    auto lines = ReadFileLinesUtf8(entry.path());
                    if (!lines.empty())
                    UpdateFile(entry.path().wstring(),  lines);
                }
            }
            catch (...)
            {
            }
            indexing_ = false;
        })
        .detach();
    }

    void LspManager::IndexFileSymbols(const std::wstring &filePath,  const std::vector<std::wstring> &lines)
    {
        std::vector<std::wstring> newSymbols;
        std::unordered_map<std::wstring,  std::vector<Location>> newDefLocs;
        std::unordered_map<std::wstring,  std::vector<Location>> newDeclLocs;
        std::vector<std::wstring> newIncludes;
        int lineIndex = 0;
        for (const auto &line: lines)
        {
            std::wstring ln = line;
            if (ln.find(L"//") != std::wstring::npos)
            ln = ln.substr(0,  ln.find(L"//"));

            auto trim = [](const std::wstring &s)
            {
                size_t a = s.find_first_not_of(L" \t");
                size_t b = s.find_last_not_of(L" \t");
                if (a == std::wstring::npos || b == std::wstring::npos)
                return std::wstring();
                return s.substr(a,  b -a +1);
            };
            std::wstring t = trim(ln);
            if (t.empty())
            {
                lineIndex++;
                continue;
            }

            if (t.rfind(L"#include",  0) == 0)
            {
                bool isAngle = false;
                size_t q1 = t.find(L'"');
                size_t q2 = std::wstring::npos;
                if (q1 != std::wstring::npos)
                {
                    q2 = t.find(L'"',  q1 +1);
                }
                else
                {
                    size_t a1 = t.find(L'<');
                    size_t a2 = std::wstring::npos;
                    if (a1 != std::wstring::npos)
                    {
                        a2 = t.find(L'>',  a1 +1);
                        if (a2 != std::wstring::npos)
                        {
                            q1 = a1;
                            q2 = a2;
                            isAngle = true;
                        }
                    }
                }

                if (q1 != std::wstring::npos && q2 != std::wstring::npos && q2 > q1 +1)
                {
                    std::wstring inc = t.substr(q1 +1,  q2 -q1 -1);
                    auto resolved = ResolveIncludePath(filePath,  inc,  isAngle,  projectRoot_);
                    if (resolved.has_value())
                    newIncludes.push_back(NormalizePath(resolved->wstring()));
                }
            }

            auto addSymbol = [&](const std::wstring &name,  int col,  bool isDefinition)
            {
                if (name.empty())
                return;
                Location loc{NormalizePath(filePath),  lineIndex,  col};
                if (isDefinition)
                symbolIndexDef_[name] = loc;
                else
                symbolIndexDecl_[name] = loc;
                if (isDefinition)
                newDefLocs[name].push_back(loc);
                else
                newDeclLocs[name].push_back(loc);
                newSymbols.push_back(name);
            };

            auto isIdentChar = [](wchar_t c)
            {
                return iswalnum(c) || c == L'_';
            };

            // class/struct/enum (treat as definition for navigation)
            {
                std::wstring lowered = ToLower(t);
                const std::wstring keys[] = {L"class ",  L"struct ",  L"enum "};
                for (const auto &k: keys)
                {
                    size_t pos = lowered.find(k);
                    if (pos != std::wstring::npos)
                    {
                        size_t start = pos +k.size();
                        while (start < t.size() && iswspace(t[start]))
                        start++;
                        size_t end = start;
                        while (end < t.size() && isIdentChar(t[end]))
                        end++;
                        if (end > start)
                        {
                            addSymbol(t.substr(start,  end -start),  (int)start,  true);
                        }
                    }
                }
            }

            // function definitions (simple heuristic)
            if (t.find(L'(') != std::wstring::npos && t.find(L')') != std::wstring::npos)
            {
                static const std::wstring keywords[] = {L"if",  L"for",  L"while",  L"switch",  L"return",  L"catch"};
                size_t lparen = t.find(L'(');
                if (lparen != std::wstring::npos && lparen > 0)
                {
                    size_t end = lparen;
                    while (end > 0 && iswspace(t[end -1]))
                    end--;
                    size_t start = end;
                    while (start > 0 && isIdentChar(t[start -1]))
                    start--;
                    if (end > start)
                    {
                        std::wstring name = t.substr(start,  end -start);
                        bool isKeyword = false;
                        for (const auto &k: keywords)
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
                                return s.substr(a,  b -a +1);
                            };

                            auto isDefinition = [&]() -> bool
                            {
                                size_t rparen = t.find(L')',  lparen);
                                if (rparen == std::wstring::npos)
                                return false;
                                size_t brace = t.find(L'{',  rparen);
                                if (brace != std::wstring::npos)
                                return true;
                                size_t semi = t.find(L';',  rparen);
                                if (semi != std::wstring::npos)
                                return false;

                                for (int i = lineIndex +1; i < (int)lines.size(); ++i)
                                {
                                    std::wstring next = lines[i];
                                    size_t cpos = next.find(L"//");
                                    if (cpos != std::wstring::npos)
                                    next = next.substr(0,  cpos);
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

                            auto isLikelyDeclaration = [&]() -> bool
                            {
                                if (isDefinition)
                                    return true;

                                size_t rparen = t.find(L')', lparen);
                                if (rparen == std::wstring::npos)
                                    return false;
                                if (t.find(L';', rparen) == std::wstring::npos)
                                    return false;

                                std::wstring prefix = trimLine(t.substr(0, start));
                                if (prefix.empty())
                                    return false;

                                if (prefix.find(L'.') != std::wstring::npos || prefix.find(L"->") != std::wstring::npos)
                                    return false;
                                if (prefix.find(L'=') != std::wstring::npos)
                                    return false;

                                std::wstring lowerPrefix = ToLower(prefix);
                                if (lowerPrefix.rfind(L"return", 0) == 0 ||
                                    lowerPrefix.rfind(L"co_return", 0) == 0 ||
                                    lowerPrefix.rfind(L"throw", 0) == 0)
                                {
                                    return false;
                                }

                                // Reject standalone namespace-qualified calls like ns::foo();
                                bool hasSpace = prefix.find_first_of(L" \t") != std::wstring::npos;
                                bool hasPtrRef = prefix.find(L'*') != std::wstring::npos || prefix.find(L'&') != std::wstring::npos;
                                bool hasTilde = !prefix.empty() && prefix.back() == L'~';
                                if (!hasSpace && !hasPtrRef && !hasTilde &&
                                    prefix.size() >= 2 && prefix[prefix.size() - 1] == L':' && prefix[prefix.size() - 2] == L':')
                                {
                                    return false;
                                }

                                return true;
                            }();

                            if (isDefinition || isLikelyDeclaration)
                                addSymbol(name,  (int)start,  isDefinition);
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
            for (const auto &sym: it->second)
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

    void LspManager::UpdateFile(const std::wstring &filePath,  const std::vector<std::wstring> &lines)
    {
        if (!IsCppFile(std::filesystem::path(filePath)))
        return;
        std::lock_guard<std::mutex> lk(mutex_);
        IndexFileSymbols(filePath,  lines);
    }

    std::vector<Diagnostic> LspManager::AnalyzeDiagnostics(const std::wstring &filePath,  const std::vector<std::wstring> &lines) const
    {
        std::vector<Diagnostic> out;
        bool hasWindowsHeader = false;
        std::unordered_set<std::wstring> knownSymbols;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            for (const auto &it: symbolIndexDef_)
            knownSymbols.insert(it.first);
            for (const auto &it: symbolIndexDecl_)
            knownSymbols.insert(it.first);
        }
        for (const auto &line: lines)
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

        auto push = [&](wchar_t c,  int l,  int col)
        {
            stack.push_back({c,  l,  col});
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

        int parenDepth = 0;
        for (int line = 0; line < (int)lines.size(); ++line)
        {
            int parenDepthAtLineStart = parenDepth;
            const std::wstring &ln = lines[line];
            for (int col = 0; col < (int)ln.size(); ++col)
            {
                wchar_t c = ln[col];

                if (inRawString)
                {
                    if (c == L')')
                    {
                        size_t closePos = (size_t)col +1;
                        bool match = true;
                        size_t delimSize = rawDelimiter.size();
                        if (closePos +delimSize < ln.size())
                        {
                            for (size_t k = 0; k < delimSize; ++k)
                            {
                                if (ln[closePos +k] != rawDelimiter[k])
                                {
                                    match = false;
                                    break;
                                }
                            }
                            if (match && closePos +delimSize < ln.size() && ln[closePos +delimSize] == L'"')
                            {
                                inRawString = false;
                                rawDelimiter.clear();
                                col = (int)(closePos +delimSize);// will be incremented by loop
                                continue;
                            }
                        }
                    }
                    continue;
                }

                if (inBlockComment)
                {
                    if (c == L'*' && col +1 < (int)ln.size() && ln[col +1] == L'/')
                    {
                        inBlockComment = false;
                        col++;
                    }
                    continue;
                }

                if (!inString && !inChar && c == L'/' && col +1 < (int)ln.size() && ln[col +1] == L'/')
                break;

                if (!inString && !inChar && c == L'/' && col +1 < (int)ln.size() && ln[col +1] == L'*')
                {
                    inBlockComment = true;
                    col++;
                    continue;
                }

                if (!inString && !inChar && c == L'R' && col +1 < (int)ln.size() && ln[col +1] == L'"')
                {
                    size_t delimStart = (size_t)col +2;
                    size_t parenPos = ln.find(L'(',  delimStart);
                    if (parenPos != std::wstring::npos)
                    {
                        rawDelimiter = ln.substr(delimStart,  parenPos -delimStart);
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
                {
                    if (c == L'(' || c == L'[')
                    parenDepth++;
                    push(c,  line,  col);
                }
                else if (c == L')' || c == L'}' || c == L']')
                {
                    if (c == L')' || c == L']')
                    {
                        if (parenDepth > 0)
                        parenDepth--;
                    }
                    if (!popMatch(c))
                    {
                        Diagnostic d;
                        d.line = line;
                        d.startCol = col;
                        d.endCol = col +1;
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
                size_t q1 = ln.find(L'"',  incPos);
                size_t q2 = std::wstring::npos;
                if (q1 != std::wstring::npos)
                q2 = ln.find(L'"',  q1 +1);

                // Only validate quoted includes (system includes require include paths)
                if (q1 != std::wstring::npos && q2 != std::wstring::npos && q2 > q1 +1)
                {
                    size_t start = q1 +1;
                    size_t end = q2;
                    std::wstring includePath = ln.substr(start,  end -start);
                    auto loc = ResolveIncludeAtCursor(filePath,  ln,  (int)start);
                    if (!loc.has_value())
                    {
                        Diagnostic d;
                        d.line = line;
                        d.startCol = (int)start;
                        d.endCol = (int)end;
                        d.severity = DiagnosticSeverity::Error;
                        d.message = L"Include not found: " +includePath;
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
                        if (c == L'*' && i +1 < s.size() && s[i +1] == L'/')
                        {
                            inBlock = false;
                            i++;
                        }
                        continue;
                    }
                    if (!inStringLocal && !inCharLocal && c == L'/' && i +1 < s.size())
                    {
                        if (s[i +1] == L'/')
                        break;
                        if (s[i +1] == L'*')
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

            auto trim = [](const std::wstring &s,  size_t &startOut)
            {
                size_t a = s.find_first_not_of(L" \t");
                size_t b = s.find_last_not_of(L" \t");
                startOut = (a == std::wstring::npos) ? 0: a;
                if (a == std::wstring::npos || b == std::wstring::npos)
                return std::wstring();
                return s.substr(a,  b -a +1);
            };

            size_t startCol = 0;
            std::wstring cleaned = stripComments(ln);
            std::wstring t = trim(cleaned,  startCol);
            if (!t.empty())
            {
                if (t[0] != L'#')
                {
                    auto isOpChar = [](wchar_t c)
                    {
                        switch (c)
                        {
                            case L'+':
                            case L'-':
                            case L'*':
                            case L'/':
                            case L'%':
                            case L'|':
                            case L'&':
                            case L'^':
                            case L'=':
                            case L'<':
                            case L'>':
                            case L'?':
                            case L':':
                            case L',':
                            case L'.':
                            case L'(':
                            case L'[':
                            return true;
                            default:
                            return false;
                        }
                    };
                    auto startsWithOp = [&](const std::wstring &s)
                    {
                        if (s.size() >= 2)
                        {
                            std::wstring p2 = s.substr(0,  2);
                            if (p2 == L"||" || p2 == L"&&" || p2 == L"<<" || p2 == L">>" || p2 == L"->" || p2 == L"::")
                            return true;
                        }
                        return !s.empty() && isOpChar(s[0]);
                    };
                    auto endsWithOp = [&](const std::wstring &s)
                    {
                        if (s.empty())
                        return false;
                        if (s.size() >= 2)
                        {
                            std::wstring t2 = s.substr(s.size() -2);
                            if (t2 == L"||" || t2 == L"&&" || t2 == L"<<" || t2 == L">>" || t2 == L"->" || t2 == L"::")
                            return true;
                        }
                        wchar_t last = s.back();
                        if (last == L'\\')
                        return true;
                        return isOpChar(last);
                    };

                    bool continuedFromPrev = (parenDepthAtLineStart > 0);
                    bool startsWithOperator = startsWithOp(t);
                    bool endsWithOperator = endsWithOp(t);
                    bool nextStartsWithOperator = false;
                    if (!continuedFromPrev && !startsWithOperator)
                    {
                        for (int n = line +1; n < (int)lines.size(); ++n)
                        {
                            size_t nextStart = 0;
                            std::wstring nextClean = stripComments(lines[n]);
                            std::wstring nextTrim = trim(nextClean,  nextStart);
                            if (nextTrim.empty())
                            continue;
                            if (nextTrim[0] == L'#')
                            break;
                            if (startsWithOp(nextTrim))
                            nextStartsWithOperator = true;
                            break;
                        }
                    }
                    bool skipSemicolonCheck = (continuedFromPrev || startsWithOperator || endsWithOperator || nextStartsWithOperator);
                    if (!skipSemicolonCheck)
                    {
                        wchar_t last = t.back();
                        bool endsOk = (last == L';' || last == L'{' || last == L'}' || last == L':' || last == L',');

                        auto startsWithWord = [&](const std::wstring &kw)
                        {
                            if (t.size() < kw.size())
                            return false;
                            if (ToLower(t.substr(0,  kw.size())) != kw)
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

                        auto isIdentifierOnly = [](const std::wstring &s)
                        {
                            if (s.empty())
                                return false;
                            if (!(iswalpha(s[0]) || s[0] == L'_'))
                                return false;
                            for (wchar_t c : s)
                            {
                                if (!((iswalnum(c) != 0) || c == L'_'))
                                    return false;
                            }
                            return true;
                        };

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

                            // Bare token inside code (e.g. random identifier line)
                            // should still produce a visible local error when clangd is down.
                            if (isIdentifierOnly(t))
                            {
                                Diagnostic d;
                                d.line = line;
                                d.startCol = (int)startCol;
                                d.endCol = (int)(startCol + t.size());
                                d.severity = DiagnosticSeverity::Error;
                                d.message = L"Unknown identifier: " + t;
                                d.suggestion = L"Did you mean to declare/call something, or remove this token?";
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
                auto checkTypo = [&](const std::wstring &bad,  const std::wstring &good)
                {
                    size_t pos = cleaned.find(bad);
                    while (pos != std::wstring::npos)
                    {
                        bool leftOk = (pos == 0) || !isIdentChar(cleaned[pos -1]);
                        bool rightOk = (pos +bad.size() >= cleaned.size()) || !isIdentChar(cleaned[pos +bad.size()]);
                        if (leftOk && rightOk)
                        {
                            Diagnostic d;
                            d.line = line;
                            d.startCol = (int)pos;
                            d.endCol = (int)(pos +bad.size());
                            d.severity = DiagnosticSeverity::Error;
                            d.message = L"Unknown identifier: " +bad;
                            d.suggestion = L"Did you mean " +good +L"?";
                            out.push_back(d);
                            break;
                        }
                        pos = cleaned.find(bad,  pos +1);
                    }
                };

                checkTypo(L"DefWowProcW",  L"DefWindowProcW");
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
                    if (pos == 0 || !isIdentChar(cleaned[pos -1]))
                    {
                        size_t end = pos +3;
                        while (end < cleaned.size() && isIdentChar(cleaned[end]))
                        ++end;
                        std::wstring ident = cleaned.substr(pos,  end -pos);
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
                                d.message = L"Unknown WinAPI function: " +ident;
                                d.suggestion = (last == L'W') ? L"Use DefWindowProcW": L"Use DefWindowProcA";
                                out.push_back(d);
                                break;
                            }
                        }
                    }
                    pos = cleaned.find(L"Def",  pos +1);
                }
            }

            // Heuristic: unknown WinAPI-like function calls (windows.h included)
            if (hasWindowsHeader)
            {
                static const std::unordered_set<std::wstring> kAllow = {
                    L"DefWindowProcW",  L"DefWindowProcA",
                    L"PostQuitMessage",
                    L"LoadCursor",  L"LoadCursorW",  L"LoadCursorA",
                    L"RegisterClassW",  L"RegisterClassA",  L"RegisterClassExW",  L"RegisterClassExA",
                    L"CreateWindowExW",  L"CreateWindowExA",
                    L"ShowWindow",  L"UpdateWindow",
                    L"GetMessageW",  L"GetMessageA",
                    L"TranslateMessage",  L"DispatchMessageW",  L"DispatchMessageA",
                    L"BeginPaint",  L"EndPaint",
                    L"InvalidateRect",  L"GetClientRect",  L"GetWindowRect",
                    L"GetModuleHandleW",  L"GetModuleHandleA",
                    L"SetWindowLongPtrW",  L"SetWindowLongPtrA",
                    L"GetWindowLongPtrW",  L"GetWindowLongPtrA",
                    L"SetWindowPos",  L"SendMessageW",  L"SendMessageA",
                    L"PeekMessageW",  L"PeekMessageA",
                    L"MessageBoxW",  L"MessageBoxA"
                };
                static const std::wstring kPrefixes[] = {
                    L"Get",  L"Set",  L"Post",  L"Create",  L"Register",
                    L"Load",  L"Show",  L"Dispatch",  L"Translate",  L"Def",
                    L"Peek",  L"Send",  L"Destroy",  L"Update",  L"Begin",  L"End",
                    L"Invalidate",  L"MessageBox"
                };
                auto isIdentChar = [](wchar_t c)
                {
                    return (iswalnum(c) != 0) || (c == L'_');
                };
                auto isKeyword = [](const std::wstring &s)
                {
                    static const std::unordered_set<std::wstring> k = {
                        L"if",  L"for",  L"while",  L"switch",  L"case",  L"default",
                        L"return",  L"sizeof",  L"typedef",  L"catch",  L"else",
                        L"do",  L"static",  L"const",  L"inline",  L"struct",  L"class",
                        L"enum",  L"namespace",  L"using",  L"new",  L"delete"
                    };
                    return k.find(ToLower(s)) != k.end();
                };
                auto isAllCaps = [](const std::wstring &s)
                {
                    bool any = false;
                    for (wchar_t c: s)
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
                    if (!isIdentChar(cleaned[i]) || (i > 0 && isIdentChar(cleaned[i -1])))
                    continue;
                    size_t start = i;
                    size_t end = i +1;
                    while (end < cleaned.size() && isIdentChar(cleaned[end]))
                    ++end;
                    std::wstring ident = cleaned.substr(start,  end -start);
                    if (ident.empty())
                    continue;
                    if (isKeyword(ident) || isAllCaps(ident))
                    continue;
                    // Skip member/namespace calls (obj.Func(), ns::Func())
                    if (start > 1)
                    {
                        wchar_t prev = cleaned[start -1];
                        wchar_t prev2 = cleaned[start -2];
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
                    for (const auto &p: kPrefixes)
                    {
                        if (ident.rfind(p,  0) == 0)
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
                    d.message = L"Unknown WinAPI function: " +ident;
                    d.suggestion = L"Check spelling or include the correct header";
                    out.push_back(d);
                    added = true;
                    break;
                }
                (void)added;
            }
        }

        for (const auto &it: stack)
        {
            Diagnostic d;
            d.line = it.line;
            d.startCol = it.col;
            d.endCol = it.col +1;
            d.severity = DiagnosticSeverity::Error;
            d.message = L"Unmatched opening bracket";
            d.suggestion = L"Add matching closing bracket";
            out.push_back(d);
        }

        // Check for std:: symbols and verify they are included
        {
            std::unordered_set<std::wstring> includedHeaders;
            // Collect all includes from the file
            for (const auto &line: lines)
            {
                std::wstring lower = ToLower(line);
                size_t inclPos = lower.find(L"#include");
                if (inclPos != std::wstring::npos)
                {
                    size_t ltPos = line.find(L'<',  inclPos);
                    size_t rtPos = line.find(L'>',  ltPos);
                    if (ltPos != std::wstring::npos && rtPos != std::wstring::npos)
                    {
                        std::wstring header = line.substr(ltPos,  rtPos -ltPos +1);
                        includedHeaders.insert(header);
                    }
                }
            }

            // Check for std:: usage in code
            for (int lineIdx = 0; lineIdx < (int)lines.size(); ++lineIdx)
            {
                const std::wstring &ln = lines[lineIdx];

                // Skip if line is a comment
                size_t commentPos = ln.find(L"//");

                size_t pos = 0;
                while ((pos = ln.find(L"std::",  pos)) != std::wstring::npos)
                {
                    // Skip if this occurrence is after // comment
                    if (commentPos != std::wstring::npos && pos > commentPos)
                    {
                        break;
                    }

                    // Extract the full std:: symbol
                    size_t symStart = pos +5;// after "std::"
                    size_t symEnd = symStart;
                    auto isIdentChar = [](wchar_t c) { return (iswalnum(c) != 0) || (c == L'_'); };

                    while (symEnd < ln.size() && isIdentChar(ln[symEnd]))
                    symEnd++;

                    if (symEnd > symStart)
                    {
                        std::wstring symbol = L"std::" +ln.substr(symStart,  symEnd -symStart);
                        std::wstring requiredHeader = GetStdHeaderFor(symbol);

                        // Only check if we have a mapping for this symbol
                        if (!requiredHeader.empty() && includedHeaders.find(requiredHeader) == includedHeaders.end())
                        {
                            Diagnostic d;
                            d.line = lineIdx;
                            d.startCol = (int)pos;
                            d.endCol = (int)symEnd;
                            d.severity = DiagnosticSeverity::Error;
                            d.message = symbol +L" requires " +requiredHeader;
                            d.suggestion = L"Add #include " +requiredHeader +L" at the top";
                            out.push_back(d);
                        }
                    }

                    pos++;
                }
            }
        }

        return out;
    }

    void LspManager::RequestDiagnosticsAsync(const std::wstring &filePath,
    const std::vector<std::wstring> &lines,
    HWND hwnd,
    int tabIndex,
    bool documentSaved)
    {
        if (!IsCppFile(std::filesystem::path(filePath)))
        return;
        if (!hwnd)
        return;

        TraceLsp(L"RequestDiagnosticsAsync tab=" + std::to_wstring(tabIndex) +
             L" file=" + filePath +
             L" lines=" + std::to_wstring((unsigned long long)lines.size()) +
             (documentSaved ? L" saved=yes" : L" saved=no") +
             (ClangdClient::Instance().IsRunning() ? L" clangd=running" : L" clangd=stopped"));

        // Check if an #include line was just added/modified
        bool hasIncludeChange = false;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            auto it = fileIncludes_.find(filePath);
            std::unordered_set<std::wstring> oldIncludes;
            if (it != fileIncludes_.end())
            {
                oldIncludes.insert(it->second.begin(),  it->second.end());
            }

            // Check current includes
            std::unordered_set<std::wstring> newIncludes;
            for (const auto& line: lines)
            {
                if (line.find(L"#include") != std::wstring::npos)
                {
                    size_t lt = line.find(L'<');
                    size_t rt = line.find(L'>');
                    if (lt != std::wstring::npos && rt != std::wstring::npos)
                    {
                        newIncludes.insert(line.substr(lt,  rt -lt +1));
                    }
                }
            }

            // If includes changed, force immediate re-analysis
            if (oldIncludes != newIncludes)
            {
                hasIncludeChange = true;
                fileIncludes_[filePath].assign(newIncludes.begin(),  newIncludes.end());
                TraceLsp(L"IncludesChanged file=" + filePath +
                         L" includeCount=" + std::to_wstring((unsigned long long)newIncludes.size()));
            }
        }

        DWORD now = GetTickCount();
        {
            std::lock_guard<std::mutex> lk(mutex_);
            DWORD &last = lastDiagTick_[filePath];
            // Skip throttle if includes changed (force immediate update)
            if (!hasIncludeChange && now -last < DIAG_REQUEST_THROTTLE_MS)
            return;
            last = now;
        }

        std::wstring pathCopy = filePath;
        std::vector<std::wstring> linesCopy = lines;

        // Register context so clangd callback can post WM_LSP_DIAGNOSTICS
        ClangdClient::Instance().SetContext(pathCopy, hwnd, tabIndex);

        // Sync document state with clangd; lifecycle is managed by SetProjectRoot.
        bool clangdRunning = false, clangdReady = false;
        std::wstring failureReason;
        ClangdClient::Instance().GetRunningState(clangdRunning, clangdReady, failureReason);
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (clangdReady || clangdRunning)
                ApplyClangdRunningState(clangdRunning, clangdReady);
            else
            {
                clangdState_ = ClangdRuntimeState::Failed;
                clangdReason_ = failureReason.empty() ? L"clangd arrete" : failureReason;
                clangdRestartingUntilTick_ = 0;
            }
        }

        // Always send (or buffer) file content to clangd.
        // This avoids first-open stale diagnostics when clangd starts slightly later.
        std::string content;
        content.reserve(linesCopy.size() * 40);
        for (size_t i = 0; i < linesCopy.size(); i++) {
            content += WideToUtf8(linesCopy[i]);
            if (i + 1 < linesCopy.size()) content += "\n";
        }
        static std::unordered_map<std::wstring, int> s_versions;
        static std::mutex s_versionsMutex;
        int ver = 0;
        {
            std::lock_guard<std::mutex> lk(s_versionsMutex);
            ver = ++s_versions[pathCopy];
        }
        if (ver == 1) {
            ClangdClient::Instance().DidOpen(pathCopy, content, 1);
        } else if (documentSaved) {
            {
                std::lock_guard<std::mutex> lk(mutex_);
                clangdPendingVer_.erase(pathCopy);
            }
            ClangdClient::Instance().DidChange(pathCopy, content, ver);
        } else {
            // Debounce: envoyer à clangd 300ms après le dernier keystroke
            {
                std::lock_guard<std::mutex> lk(mutex_);
                clangdPendingVer_[pathCopy] = ver;
            }
            std::thread([this, pathCopy, content, ver]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(CLANGD_DIDCHANGE_DEBOUNCE_MS));
                std::lock_guard<std::mutex> lk(mutex_);
                auto it = clangdPendingVer_.find(pathCopy);
                if (it != clangdPendingVer_.end() && it->second == ver) {
                    ClangdClient::Instance().DidChange(pathCopy, content, ver);
                    clangdPendingVer_.erase(it);
                }
            }).detach();
        }

        if (!clangdReady)
            TraceLsp(L"ClangdQueued file=" + pathCopy + L" ver=" + std::to_wstring(ver));

        if (documentSaved)
            ClangdClient::Instance().DidSave(pathCopy);
    }

    std::optional<Location> LspManager::ResolveIncludeAtCursor(const std::wstring &filePath,
    const std::wstring &lineText,
    int /*column*/) const
    {
        std::wstring projectRootCopy;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            projectRootCopy = projectRoot_;
        }

        size_t incPos = lineText.find(L"#include");
        if (incPos == std::wstring::npos)
        return std::nullopt;

        size_t q1 = lineText.find(L'"',  incPos);
        size_t q2 = (q1 != std::wstring::npos) ? lineText.find(L'"',  q1 +1): std::wstring::npos;
        size_t a1 = lineText.find(L'<',  incPos);
        size_t a2 = (a1 != std::wstring::npos) ? lineText.find(L'>',  a1 +1): std::wstring::npos;

        size_t start = std::wstring::npos;
        size_t end = std::wstring::npos;
        bool isAngle = false;
        if (q1 != std::wstring::npos && q2 != std::wstring::npos)
        {
            start = q1 +1;
            end = q2;
        }
        else if (a1 != std::wstring::npos && a2 != std::wstring::npos)
        {
            start = a1 +1;
            end = a2;
            isAngle = true;
        }

        if (start == std::wstring::npos || end == std::wstring::npos)
        return std::nullopt;

        // Be lenient: if the user clicks anywhere on the include line, try to resolve.
        // This avoids false negatives when the column mapping is slightly off.

        std::wstring inc = lineText.substr(start,  end -start);
        if (inc.empty())
        return std::nullopt;

        Logger::Instance().Log(L"LSP include resolve: " +inc +
        L" (angle=" +std::wstring(isAngle ? L"true": L"false") +L")");

        auto resolved = ResolveIncludePath(filePath,  inc,  isAngle,  projectRootCopy);
        if (resolved.has_value())
        {
            Location loc;
            loc.filePath = NormalizePath(resolved->wstring());
            loc.line = 0;
            loc.column = 0;
            Logger::Instance().Log(L"LSP include resolved: " +loc.filePath);
            return loc;
        }

        if (isAngle)
        {
            auto stdHeader = FindStdHeaderFile(inc,  projectRootCopy);
            if (stdHeader.has_value())
            {
                Location loc;
                loc.filePath = NormalizePath(stdHeader->wstring());
                loc.line = 0;
                loc.column = 0;
                Logger::Instance().Log(L"LSP std include resolved: " +loc.filePath);
                return loc;
            }
        }

        Logger::Instance().Log(L"LSP include NOT found: " +inc +L" (root=" +projectRootCopy +L")");
        return std::nullopt;
    }

    std::optional<Location> LspManager::GoToDefinition(const std::wstring &filePath,
    const std::wstring &lineText,
    int line,
    int column,
    const std::wstring &word)
    {
        auto inc = ResolveIncludeAtCursor(filePath,  lineText,  column);
        if (inc.has_value())
        return inc;

        std::wstring lookupWord = word;
        if (!lookupWord.empty())
        {
            static const std::unordered_set<std::wstring> kTypeKeywords = {
                L"void",  L"int",  L"float",  L"double",  L"char",  L"wchar_t",  L"bool",
                L"short",  L"long",  L"signed",  L"unsigned",  L"auto",  L"const",  L"static"};
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
            auto stdLoc = ResolveStdSymbolAtCursor(rootCopy,  lineText,  column,  lookupWord);
            if (stdLoc.has_value())
            return stdLoc;
        }

        std::lock_guard<std::mutex> lk(mutex_);
        std::wstring normFile = NormalizePath(filePath);

        auto pickCandidate = [&](const std::vector<Location> &cands, bool preferNearCursor) -> std::optional<Location>
        {
            if (cands.empty())
                return std::nullopt;
            if (!preferNearCursor)
                return cands.front();

            const Location *bestBefore = nullptr;
            const Location *bestAfter = nullptr;
            for (const auto &cand : cands)
            {
                if (cand.line <= line)
                {
                    if (!bestBefore || cand.line > bestBefore->line ||
                        (cand.line == bestBefore->line && cand.column >= bestBefore->column))
                    {
                        bestBefore = &cand;
                    }
                }
                else
                {
                    if (!bestAfter || cand.line < bestAfter->line ||
                        (cand.line == bestAfter->line && cand.column < bestAfter->column))
                    {
                        bestAfter = &cand;
                    }
                }
            }

            if (bestBefore)
                return *bestBefore;
            if (bestAfter)
                return *bestAfter;
            return cands.front();
        };

        auto findInFile = [&](const std::wstring &path,  bool allowDecl) -> std::optional<Location>
        {
            bool preferNearCursor = (path == normFile);
            auto dIt = fileSymbolDefLocs_.find(path);
            if (dIt != fileSymbolDefLocs_.end())
            {
                auto it = dIt->second.find(lookupWord);
                if (it != dIt->second.end())
                    if (auto pick = pickCandidate(it->second, preferNearCursor))
                        return pick;
            }
            if (allowDecl)
            {
                auto cIt = fileSymbolDeclLocs_.find(path);
                if (cIt != fileSymbolDeclLocs_.end())
                {
                    auto it = cIt->second.find(lookupWord);
                    if (it != cIt->second.end())
                        if (auto pick = pickCandidate(it->second, preferNearCursor))
                            return pick;
                }
            }
            return std::nullopt;
        };

        if (auto loc = findInFile(normFile,  false))
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
            for (const auto &incPath: queue)
            {
                if (visited.find(incPath) != visited.end())
                continue;
                visited.insert(incPath);

                if (auto loc = findInFile(incPath,  true))
                return loc;

                auto it = fileIncludes_.find(incPath);
                if (it != fileIncludes_.end())
                {
                    for (const auto &p: it->second)
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

        // Fallback: ask clangd directly (authoritative, handles templates/macros/STL).
        return ClangdClient::Instance().RequestDefinitionSync(filePath, line, column);
    }

    std::vector<Diagnostic> LspManager::GetDiagnostics(const std::wstring &filePath) const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = diagnostics_.find(filePath);
        if (it != diagnostics_.end())
        return it->second;
        return {};
    }

    std::vector<CompletionItem> LspManager::GetCompletions(const std::wstring &filePath,
    const std::wstring &lineText,
    int column) const
    {
        // Try clangd first — it provides accurate, context-aware completions.
        auto clangdItems = ClangdClient::Instance().RequestCompletionsSync(filePath, column > 0 ? column - 1 : 0, column);
        if (!clangdItems.empty())
            return clangdItems;

        // Fallback: filtered static std:: list when clangd is unavailable.
        std::vector<CompletionItem> result;

        std::wstring prefix;
        int pos = column -1;

        while (pos >= 0 && (iswalnum(lineText[pos]) || lineText[pos] == L'_' || lineText[pos] == L':'))
        {
            prefix = lineText[pos] +prefix;
            pos--;
        }

        bool inStdContext = false;

        if (prefix.find(L"std::") != std::wstring::npos)
        {
            inStdContext = true;
        }
        else if (pos >= 0)
        {
            // Look backwards for context keywords
            std::wstring context = lineText.substr(0,  column);
            std::wstring contextLower = ToLower(context);

            if (contextLower.find(L"using") != std::wstring::npos ||
            contextLower.find(L"auto") != std::wstring::npos ||
            contextLower.find(L"new") != std::wstring::npos ||
            context.find(L"=") != std::wstring::npos ||
            context.find(L"<") != std::wstring::npos ||
            context.find(L"(") != std::wstring::npos ||
            context.find(L":") != std::wstring::npos)
            {
                inStdContext = true;
            }
        }

        if (inStdContext)
        {
            auto allCompletions = GetStdCompletions();

            // Collect included headers from the file
            std::unordered_set<std::wstring> includedHeaders;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                auto it = fileIncludes_.find(filePath);
                if (it != fileIncludes_.end())
                {
                    for (const auto &inc: it->second)
                    includedHeaders.insert(inc);
                }
            }

            // Filter by current prefix
            std::wstring lowerPrefix = ToLower(prefix);

            for (const auto &item: allCompletions)
            {
                std::wstring lowerLabel = ToLower(item.label);

                // Match prefix
                if (lowerLabel.find(lowerPrefix) == 0 ||
                lowerPrefix.empty() ||
                lowerLabel.find(lowerPrefix) != std::wstring::npos)
                {
                    // Enhance description with required header info
                    CompletionItem enhanced = item;
                    std::wstring header = GetStdHeaderFor(item.label);
                    if (!header.empty())
                    {
                        bool isIncluded = includedHeaders.find(header) != includedHeaders.end();
                        if (!isIncluded)
                        {
                            enhanced.description += L" [Requires: " +header +L"]";
                        }
                    }
                    result.push_back(enhanced);
                }
            }

            // Sort: exact prefix matches first, then by alphabetical order
            std::sort(result.begin(),  result.end(),
            [lowerPrefix](const CompletionItem &a,  const CompletionItem &b)
            {
                std::wstring aLower = ToLower(a.label);
                std::wstring bLower = ToLower(b.label);

                bool aStartsWith = aLower.find(lowerPrefix) == 0;
                bool bStartsWith = bLower.find(lowerPrefix) == 0;

                if (aStartsWith != bStartsWith)
                return aStartsWith;
                return aLower < bLower;
            });

            // Limit results
            if (result.size() > 20)
            result.resize(20);
        }

        return result;
    }

    std::wstring LspManager::GetProjectRoot() const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        return projectRoot_;
    }
}
