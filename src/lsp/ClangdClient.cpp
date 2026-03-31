#include "ClangdClient.h"
#include "utils/logger/Logger.h"
#include <sstream>
#include <cassert>
#include <cctype>
#include <regex>
#include <filesystem>
#include <functional>
#include <cstdlib>
#include <set>
#include <fstream>

// Suppress "function not used" warnings for static helpers
#pragma warning(disable: 4505)

namespace Lsp {

namespace {
std::wstring GetNebulaClangdDbDir(const std::wstring &projectRoot)
{
    if (projectRoot.empty())
        return {};

    wchar_t localAppData[MAX_PATH] = {};
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return {};

    size_t key = std::hash<std::wstring>{}(projectRoot);
    std::filesystem::path dir = std::filesystem::path(localAppData) /
                                L"Nebula" /
                                L"clangd-db" /
                                std::to_wstring(static_cast<unsigned long long>(key));
    return dir.wstring();
}

std::wstring NormalizePathKey(std::wstring path)
{
    for (wchar_t &c : path) {
        if (c == L'/')
            c = L'\\';
        else
            c = (wchar_t)towlower(c);
    }
    return path;
}

std::wstring NormalizePathForClangArg(std::wstring path)
{
    for (wchar_t &c : path)
        if (c == L'\\')
            c = L'/';

    // Collapse duplicated separators (e.g. C://Users//...)
    std::wstring collapsed;
    collapsed.reserve(path.size());
    bool prevSlash = false;
    for (wchar_t c : path)
    {
        bool isSlash = (c == L'/');
        if (isSlash && prevSlash)
            continue;
        collapsed.push_back(c);
        prevSlash = isSlash;
    }
    return collapsed;
}

std::string UnescapeJsonStringMinimal(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size())
        {
            char n = s[i + 1];
            switch (n)
            {
            case '\\': out.push_back('\\'); ++i; continue;
            case '"': out.push_back('"'); ++i; continue;
            case '/': out.push_back('/'); ++i; continue;
            case 'n': out.push_back('\n'); ++i; continue;
            case 'r': out.push_back('\r'); ++i; continue;
            case 't': out.push_back('\t'); ++i; continue;
            default: break;
            }
        }
        out.push_back(c);
    }
    return out;
}

std::wstring NormalizeWindowsPathForFs(std::wstring path)
{
    std::wstring out;
    out.reserve(path.size());
    bool prevSlash = false;
    for (wchar_t c : path)
    {
        bool isSlash = (c == L'\\' || c == L'/');
        if (isSlash)
        {
            if (!prevSlash)
                out.push_back(L'\\');
            prevSlash = true;
        }
        else
        {
            out.push_back(c);
            prevSlash = false;
        }
    }
    return out;
}

std::wstring Utf8ToWideLocal(const std::string& s)
{
    if (s.empty())
        return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (size <= 0)
        return {};
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), size);
    return out;
}

std::wstring ExtractCompilerFromCommand(const std::wstring& cmd)
{
    if (cmd.empty())
        return {};

    size_t i = 0;
    while (i < cmd.size() && iswspace(cmd[i]))
        ++i;
    if (i >= cmd.size())
        return {};

    if (cmd[i] == L'"')
    {
        size_t q = cmd.find(L'"', i + 1);
        if (q == std::wstring::npos)
            return {};
        return cmd.substr(i + 1, q - (i + 1));
    }

    size_t j = i;
    while (j < cmd.size() && !iswspace(cmd[j]))
        ++j;
    return cmd.substr(i, j - i);
}

void CollectQueryDriverGlobsFromCompileCommands(const std::filesystem::path& ccdbFile,
                                                std::set<std::wstring>& globs)
{
    std::error_code ec;
    if (!std::filesystem::exists(ccdbFile, ec))
        return;

    std::ifstream ifs(ccdbFile);
    if (!ifs)
        return;

    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    if (content.empty())
        return;

    std::regex commandRe("\\\"command\\\"\\s*:\\s*\\\"([^\\\"]+)\\\"");
    auto begin = std::sregex_iterator(content.begin(), content.end(), commandRe);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it)
    {
        if (it->size() < 2)
            continue;
        std::string cmdUtf8 = UnescapeJsonStringMinimal((*it)[1].str());
        std::wstring cmd = Utf8ToWideLocal(cmdUtf8);
        std::wstring compiler = ExtractCompilerFromCommand(cmd);
        if (compiler.empty())
            continue;
        compiler = NormalizeWindowsPathForFs(compiler);
        std::filesystem::path compilerPath(compiler);
        if (!compilerPath.has_parent_path())
            continue;

        std::error_code canonEc;
        std::filesystem::path parent = compilerPath.parent_path();
        std::filesystem::path normalizedParent = std::filesystem::weakly_canonical(parent, canonEc);
        if (canonEc)
            normalizedParent = parent;

        std::wstring dir = NormalizePathForClangArg(normalizedParent.wstring());
        if (!dir.empty())
            globs.insert(dir + L"/*");
    }
}

std::wstring JoinQueryDriverGlobs(const std::set<std::wstring>& globs)
{
    std::wstring joined;
    bool first = true;
    for (const auto& g : globs)
    {
        if (!first)
            joined += L",";
        joined += g;
        first = false;
    }
    return joined;
}

bool IsLspTraceEnabled()
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

void TraceLsp(const std::wstring& msg)
{
    if (IsLspTraceEnabled())
        Logger::Instance().Log(L"[LSP-TRACE] " + msg);
}
}

// ---------------------------------------------------------------------------
// Public static — accessible from outside (e.g. Explorer_ExtLibs)
// ---------------------------------------------------------------------------
std::wstring ClangdClient::GetCompileCommandsDir(const std::wstring& projectRoot)
{
    return GetNebulaClangdDbDir(projectRoot);
}

bool ClangdClient::RestartForProject(const std::wstring& projectRoot)
{
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    {
        std::lock_guard<std::mutex> lk(docsMutex_);
        replayOpenDocumentsOnInit_ = true;
    }
    TraceLsp(L"RestartForProject root=" + projectRoot);
    StopLocked();
    return StartLocked(projectRoot);
}

void ClangdClient::EnsureCompileCommandsAsync(const std::wstring& projectRoot)
{
    if (projectRoot.empty()) return;

    std::error_code ec;
    std::wstring ccdbDir  = GetNebulaClangdDbDir(projectRoot);
    std::filesystem::path ccdbFile = std::filesystem::path(ccdbDir) / L"compile_commands.json";

    // If cache exists, check whether CMakeLists.txt is newer — if so, regenerate.
    if (std::filesystem::exists(ccdbFile, ec)) {
        std::filesystem::path cmakeLists =
            std::filesystem::path(projectRoot) / L"CMakeLists.txt";
        bool stale = false;
        if (std::filesystem::exists(cmakeLists, ec)) {
            auto ccdbTime  = std::filesystem::last_write_time(ccdbFile,   ec);
            auto cmakeTime = std::filesystem::last_write_time(cmakeLists, ec);
            if (!ec && cmakeTime > ccdbTime)
                stale = true;
        }
        if (!stale) return; // cache is up-to-date, nothing to do
    }

    // Find existing build dir (must already have CMakeCache.txt)
    static const wchar_t* kBuildDirs[] = {
        L"build", L"cmake-build-debug", L"cmake-build-release",
        L"cmake-build-relwithdebinfo", L"out\\build", L"_build",
        L"Release", L"Debug"
    };
    std::wstring buildDir;
    for (auto* sub : kBuildDirs) {
        std::wstring candidate = projectRoot + L"\\" + sub;
        if (std::filesystem::exists(candidate + L"\\CMakeCache.txt", ec)) {
            buildDir = candidate;
            break;
        }
    }
    // No existing build dir: if the project has a CMakeLists.txt, bootstrap a
    // minimal cmake configure into build/ just to get compile_commands.json.
    bool bootstrapConfigure = false;
    if (buildDir.empty()) {
        std::filesystem::path cmakeLists = std::filesystem::path(projectRoot) / L"CMakeLists.txt";
        if (!std::filesystem::exists(cmakeLists, ec)) return;
        buildDir = projectRoot + L"\\build";
        bootstrapConfigure = true;
    }

    std::thread([this, projectRoot, buildDir, ccdbDir, ccdbFile, bootstrapConfigure]() {
        std::wstring cmdLine;
        if (bootstrapConfigure) {
            // Fresh configure: cmake -S <root> -B <buildDir> -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
            cmdLine = L"cmake -S \"" + projectRoot + L"\" -B \"" + buildDir +
                      L"\" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON";
        } else {
            // Re-run existing configure to refresh compile_commands.json
            cmdLine = L"cmake \"" + buildDir + L"\" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON";
        }

        STARTUPINFOW si{ sizeof(si) };
        si.dwFlags     = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};

        if (!CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, projectRoot.c_str(), &si, &pi))
            return;

        WaitForSingleObject(pi.hProcess, 60000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        // Copy compile_commands.json to the clangd-db dir
        std::filesystem::path src = std::filesystem::path(buildDir) / L"compile_commands.json";
        std::error_code ec2;
        if (std::filesystem::exists(src, ec2)) {
            std::filesystem::create_directories(ccdbDir, ec2);
            std::filesystem::copy_file(src, ccdbFile,
                std::filesystem::copy_options::overwrite_existing, ec2);

            // Notify clangd that compile_commands.json changed so it reindexes
            // without requiring a full restart. clangd 14+ responds to this event.
            bool stillCurrentProject = false;
            {
                std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
                stillCurrentProject = !projectRoot_.empty() &&
                                     NormalizePathKey(projectRoot_) == NormalizePathKey(projectRoot);
            }
            if (stillCurrentProject && initialized_) {
                std::string ccdbUri = FilePathToUri(ccdbFile.wstring());
                std::string notif =
                    "{\"jsonrpc\":\"2.0\",\"method\":\"workspace/didChangeWatchedFiles\","
                    "\"params\":{\"changes\":[{\"uri\":\"" + ccdbUri + "\",\"type\":2}]}}";
                Send(notif);
            }
        }
    }).detach();
}

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

ClangdClient& ClangdClient::Instance() {
    static ClangdClient inst;
    return inst;
}

// ---------------------------------------------------------------------------
// FindClangd
// ---------------------------------------------------------------------------

std::wstring ClangdClient::FindClangd() {
    // 0. Bundled clangd in external/clangd (Nebula's embedded copy)
    {
        // Get the app directory (where the executable is)
        wchar_t exePath[MAX_PATH]{};
        DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (len > 0) {
            std::wstring appDir(exePath);
            size_t lastSlash = appDir.find_last_of(L"\\/");
            if (lastSlash != std::wstring::npos) {
                appDir = appDir.substr(0, lastSlash);
                // Assume the structure is: app.exe is at root or build/Release/
                // external/clangd should be relative to the project root
                std::wstring candidates[] = {
                    appDir + L"\\..\\..\\external\\clangd\\bin\\clangd.exe",  // From build/Release
                    appDir + L"\\..\\external\\clangd\\bin\\clangd.exe",      // From build
                    appDir + L"\\external\\clangd\\bin\\clangd.exe",          // From root
                };
                for (const auto& candidate : candidates) {
                    wchar_t resolved[MAX_PATH]{};
                    if (GetFullPathNameW(candidate.c_str(), MAX_PATH, resolved, nullptr) > 0) {
                        if (GetFileAttributesW(resolved) != INVALID_FILE_ATTRIBUTES) {
                            return resolved;
                        }
                    }
                }
            }
        }
    }

    // 1. Specific CLion 2025.3.2 path
    {
        const wchar_t* exact = L"C:\\Program Files\\JetBrains\\CLion 2025.3.2\\bin\\clang\\win\\x64\\bin\\clangd.exe";
        if (GetFileAttributesW(exact) != INVALID_FILE_ATTRIBUTES)
            return exact;
    }

    // 2. Any CLion version under JetBrains
    {
        WIN32_FIND_DATAW fd{};
        HANDLE hFind = FindFirstFileW(L"C:\\Program Files\\JetBrains\\CLion *", &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    std::wstring candidate = std::wstring(L"C:\\Program Files\\JetBrains\\")
                        + fd.cFileName
                        + L"\\bin\\clang\\win\\x64\\bin\\clangd.exe";
                    if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
                        FindClose(hFind);
                        return candidate;
                    }
                }
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
    }

    // 3. LLVM standard install
    {
        const wchar_t* llvm = L"C:\\Program Files\\LLVM\\bin\\clangd.exe";
        if (GetFileAttributesW(llvm) != INVALID_FILE_ATTRIBUTES)
            return llvm;
    }

    // 4. LLVM x86 install
    {
        const wchar_t* llvm86 = L"C:\\Program Files (x86)\\LLVM\\bin\\clangd.exe";
        if (GetFileAttributesW(llvm86) != INVALID_FILE_ATTRIBUTES)
            return llvm86;
    }

    // 5. PATH
    {
        wchar_t buf[MAX_PATH]{};
        DWORD len = SearchPathW(nullptr, L"clangd", L".exe", MAX_PATH, buf, nullptr);
        if (len > 0 && len < MAX_PATH)
            return buf;
    }

    return L"";
}

// ---------------------------------------------------------------------------
// FindMinGW
// ---------------------------------------------------------------------------

std::wstring ClangdClient::FindMinGW() {
    wchar_t exePath[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0)
        return L"";

    std::wstring appDir(exePath);
    size_t lastSlash = appDir.find_last_of(L"\\/");
    if (lastSlash == std::wstring::npos)
        return L"";
    appDir = appDir.substr(0, lastSlash);

    std::wstring candidates[] = {
        appDir + L"\\..\\..\\external\\mingw\\bin",  // From build/Release
        appDir + L"\\..\\external\\mingw\\bin",      // From build
        appDir + L"\\external\\mingw\\bin",          // From root
    };
    for (const auto& candidate : candidates) {
        wchar_t resolved[MAX_PATH]{};
        if (GetFullPathNameW(candidate.c_str(), MAX_PATH, resolved, nullptr) > 0) {
            if (GetFileAttributesW(resolved) != INVALID_FILE_ATTRIBUTES)
                return std::wstring(resolved);
        }
    }
    return L"";
}

// ---------------------------------------------------------------------------
// Start / Stop / IsRunning
// ---------------------------------------------------------------------------

bool ClangdClient::Start(const std::wstring& projectRoot) {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    return StartLocked(projectRoot);
}

bool ClangdClient::StartLocked(const std::wstring& projectRoot) {
    if (IsRunningLocked())
        return true;

    // If clangd exited unexpectedly, the previous reader thread may still be
    // joinable. Reassigning std::thread without joining would call
    // std::terminate (observed as abort() in MSVC runtime).
    if (readerThread_.joinable())
        readerThread_.join();

    {
        std::lock_guard<std::mutex> lk(docsMutex_);
        openedInSession_.clear();
        if (!projectRoot_.empty() && projectRoot_ != projectRoot) {
            openDocuments_.clear();
            replayOpenDocumentsOnInit_ = false;
        }
    }

    std::wstring clangdPath = FindClangd();
    if (clangdPath.empty()) {
        Logger::Instance().Log(L"[LSP-CORE] Start failed: clangd not found");
        return false;
    }

    Logger::Instance().Log(L"[LSP-CORE] Start requested root=" + projectRoot + L" clangd=" + clangdPath);

    TraceLsp(L"Start root=" + projectRoot + L" clangd=" + clangdPath);

    projectRoot_ = projectRoot;
    initialized_ = false;

    // Create pipes
    SECURITY_ATTRIBUTES sa{};
    sa.nLength              = sizeof(sa);
    sa.bInheritHandle       = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE hStdinRd  = nullptr;
    HANDLE hStdoutWr = nullptr;

    if (!CreatePipe(&hStdinRd, &hStdinWr_, &sa, 0))
    {
        Logger::Instance().Log(L"[LSP-CORE] Start failed: CreatePipe(stdin) err=" + std::to_wstring(GetLastError()));
        return false;
    }
    // Write end must NOT be inherited by the child
    SetHandleInformation(hStdinWr_, HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&hStdoutRd_, &hStdoutWr, &sa, 0)) {
        Logger::Instance().Log(L"[LSP-CORE] Start failed: CreatePipe(stdout) err=" + std::to_wstring(GetLastError()));
        CloseHandle(hStdinRd);
        CloseHandle(hStdinWr_);
        hStdinWr_ = nullptr;
        return false;
    }
    // Read end must NOT be inherited by the child
    SetHandleInformation(hStdoutRd_, HANDLE_FLAG_INHERIT, 0);

    // Build command line
    std::wstring cmdLine = L"\"" + clangdPath + L"\" --log=error --clang-tidy=false";
    bool hasCompileCommands = false;
    std::wstring compileCommandsDir;

    // Ensure compile_commands.json is in the Nebula clangd-db dir.
    // Priority: clangd-db (already there) > build sub-dirs > project root (legacy).
    if (!projectRoot.empty()) {
        std::error_code ec;
        std::wstring ccdbDir = GetNebulaClangdDbDir(projectRoot);
        std::filesystem::path ccdbFile = std::filesystem::path(ccdbDir) / L"compile_commands.json";

        if (!std::filesystem::exists(ccdbFile, ec)) {
            // Search common build directories for compile_commands.json
            static const wchar_t* kBuildDirs[] = {
                L"build", L"cmake-build-debug", L"cmake-build-release",
                L"cmake-build-relwithdebinfo", L"out\\build", L"_build",
                L"Release", L"Debug"
            };
            std::filesystem::path src;
            for (auto* sub : kBuildDirs) {
                std::filesystem::path candidate =
                    std::filesystem::path(projectRoot) / sub / L"compile_commands.json";
                if (std::filesystem::exists(candidate, ec)) { src = candidate; break; }
            }
            // Fallback: legacy location at project root
            if (src.empty()) {
                std::filesystem::path legacy =
                    std::filesystem::path(projectRoot) / L"compile_commands.json";
                if (std::filesystem::exists(legacy, ec)) src = legacy;
            }
            // Copy to clangd-db dir
            if (!src.empty() && !ccdbDir.empty()) {
                try {
                    std::filesystem::create_directories(ccdbDir, ec);
                    std::filesystem::copy_file(src, ccdbFile,
                        std::filesystem::copy_options::overwrite_existing, ec);
                } catch (...) {}
            }
        }

        if (std::filesystem::exists(ccdbFile, ec)) {
            hasCompileCommands = true;
            compileCommandsDir = ccdbDir;
            cmdLine += L" \"--compile-commands-dir=" + ccdbDir + L"\"";
        }
    }

    std::set<std::wstring> queryDriverGlobs;
    std::wstring mingwBin = FindMinGW();
    if (!mingwBin.empty()) {
        queryDriverGlobs.insert(NormalizePathForClangArg(mingwBin) + L"/*");
    }

    if (!projectRoot.empty()) {
        std::error_code ec;
        std::wstring ccdbDir = GetNebulaClangdDbDir(projectRoot);
        std::filesystem::path ccdbFile = std::filesystem::path(ccdbDir) / L"compile_commands.json";
        CollectQueryDriverGlobsFromCompileCommands(ccdbFile, queryDriverGlobs);
    }

    std::wstring queryDriverArg = JoinQueryDriverGlobs(queryDriverGlobs);
    if (!queryDriverArg.empty()) {
        cmdLine += L" \"--query-driver=" + queryDriverArg + L"\"";
    }

    // Always inject project include paths via --extra-arg as a safety net.
    // This covers the window between startup and compile_commands.json being
    // ready, and also makes clangd resilient against stale compile_commands.
    if (!projectRoot.empty()) {
        namespace fs = std::filesystem;
        std::error_code ec2;
        fs::path root(projectRoot);

        std::set<std::wstring> addedPaths;
        auto addExtraArg = [&](const fs::path& p) {
            if (!fs::is_directory(p, ec2)) return;
            std::wstring key = p.lexically_normal().wstring();
            if (!addedPaths.insert(key).second) return;
            std::wstring pathStr = key;
            std::replace(pathStr.begin(), pathStr.end(), L'\\', L'/');
            cmdLine += L" \"--extra-arg=-I" + pathStr + L"\"";
        };

        // Common project dirs
        addExtraArg(root / L"include");
        addExtraArg(root / L"src");

        // external/<name>/include  and  external/<name>  (for libs like imgui)
        fs::path externalDir = root / L"external";
        if (fs::is_directory(externalDir, ec2)) {
            for (const auto& entry : fs::directory_iterator(externalDir, ec2)) {
                if (!entry.is_directory(ec2)) continue;
                addExtraArg(entry.path() / L"include");
                addExtraArg(entry.path());
            }
        }

        // Parse add_subdirectory() paths from CMakeLists.txt and inject their
        // include/ dirs — covers libs placed outside of external/ (e.g. libs/, vendor/).
        fs::path cmakeLists = root / L"CMakeLists.txt";
        std::ifstream cmakeFile(cmakeLists);
        if (cmakeFile.is_open()) {
            std::string line;
            while (std::getline(cmakeFile, line)) {
                // Strip comments
                auto hash = line.find('#');
                if (hash != std::string::npos) line = line.substr(0, hash);

                // Match add_subdirectory(<path> ...) — case-insensitive
                std::string low = line;
                std::transform(low.begin(), low.end(), low.begin(),
                               [](unsigned char c){ return (char)std::tolower(c); });
                auto pos = low.find("add_subdirectory");
                if (pos == std::string::npos) continue;
                auto open  = line.find('(', pos);
                auto close = line.find(')', open != std::string::npos ? open : 0);
                if (open == std::string::npos || close == std::string::npos) continue;
                std::string args = line.substr(open + 1, close - open - 1);
                // First token is the path
                std::istringstream ss(args);
                std::string subdir;
                if (!(ss >> subdir) || subdir.empty()) continue;
                // Convert to wide and resolve
                int wn = MultiByteToWideChar(CP_UTF8, 0, subdir.c_str(), -1, nullptr, 0);
                if (wn <= 1) continue;
                std::wstring wsubdir(wn - 1, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, subdir.c_str(), -1, wsubdir.data(), wn);
                fs::path libPath = root / wsubdir;
                addExtraArg(libPath / L"include");
                addExtraArg(libPath);
            }
        }
    }

    TraceLsp(L"Start options compile_commands=" + std::wstring(hasCompileCommands ? L"yes" : L"no") +
             (compileCommandsDir.empty() ? L"" : (L" dir=" + compileCommandsDir)) +
             (queryDriverArg.empty() ? L" query_driver=no" : (L" query_driver=" + queryDriverArg)));

    STARTUPINFOW si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = hStdinRd;
    si.hStdOutput = hStdoutWr;
    HANDLE hStderrWr = CreateFileW(L"NUL",
                                   GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   &sa,
                                   OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
    if (hStderrWr == INVALID_HANDLE_VALUE) {
        Logger::Instance().Log(L"[LSP-CORE] Start failed: CreateFile(NUL) err=" + std::to_wstring(GetLastError()));
        CloseHandle(hStdinRd);
        CloseHandle(hStdoutWr);
        CloseHandle(hStdinWr_); hStdinWr_ = nullptr;
        CloseHandle(hStdoutRd_); hStdoutRd_ = nullptr;
        return false;
    }
    si.hStdError  = hStderrWr;

    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessW(
        nullptr,
        cmdLine.data(),
        nullptr,
        nullptr,
        TRUE,           // inherit handles
        CREATE_NO_WINDOW,
        nullptr,
        projectRoot.empty() ? nullptr : projectRoot.c_str(),
        &si,
        &pi
    );

    // Child now owns its ends — close our copies
    CloseHandle(hStdinRd);
    CloseHandle(hStdoutWr);
    CloseHandle(hStderrWr);

    if (!ok) {
        Logger::Instance().Log(L"[LSP-CORE] Start failed: CreateProcess err=" + std::to_wstring(GetLastError()) +
                               L" root=" + projectRoot + L" cmd=" + cmdLine);
        CloseHandle(hStdinWr_); hStdinWr_ = nullptr;
        CloseHandle(hStdoutRd_); hStdoutRd_ = nullptr;
        return false;
    }

    hProcess_ = pi.hProcess;
    Logger::Instance().Log(L"[LSP-CORE] Start ok: pid=" + std::to_wstring((unsigned long long)pi.dwProcessId) +
                           L" root=" + projectRoot);
    CloseHandle(pi.hThread);

    running_ = true;
    readerThread_ = std::thread([this] { ReaderLoop(); });

    SendInitialize();
    return true;
}

bool ClangdClient::IsRunning() const {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    return IsRunningLocked();
}

bool ClangdClient::IsReady() const {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    return IsRunningLocked() && initialized_.load();
}

void ClangdClient::GetRunningState(bool& running, bool& ready) const {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    running = IsRunningLocked();
    ready   = running && initialized_.load();
}

bool ClangdClient::IsRunningLocked() const {
    if (!running_.load())
        return false;

    HANDLE process = hProcess_;
    if (!process)
    {
        Logger::Instance().Log(L"[LSP-CORE] IsRunning=false: process handle null");
        const_cast<ClangdClient*>(this)->running_ = false;
        const_cast<ClangdClient*>(this)->initialized_ = false;
        return false;
    }

    DWORD code = 0;
    if (!GetExitCodeProcess(process, &code) || code != STILL_ACTIVE)
    {
        Logger::Instance().Log(L"[LSP-CORE] IsRunning=false: process exited code=" + std::to_wstring((unsigned long long)code));
        const_cast<ClangdClient*>(this)->running_ = false;
        const_cast<ClangdClient*>(this)->initialized_ = false;
        return false;
    }

    return true;
}

void ClangdClient::Stop() {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    StopLocked();
}

void ClangdClient::StopLocked() {
    TraceLsp(L"Stop");
    running_ = false;

    if (hStdinWr_)  { CloseHandle(hStdinWr_);  hStdinWr_  = nullptr; }
    if (hStdoutRd_) { CloseHandle(hStdoutRd_); hStdoutRd_ = nullptr; }
    if (hProcess_)  {
        TerminateProcess(hProcess_, 0);
        CloseHandle(hProcess_);
        hProcess_ = nullptr;
    }

    if (readerThread_.joinable())
        readerThread_.join();

    {
        std::lock_guard<std::mutex> lk(docsMutex_);
        openedInSession_.clear();
    }

    initialized_ = false;
}

// ---------------------------------------------------------------------------
// SetContext / SetDiagnosticsCallback
// ---------------------------------------------------------------------------

void ClangdClient::SetContext(const std::wstring& filePath, HWND hwnd, int tabIndex) {
    std::lock_guard<std::mutex> lk(ctxMutex_);
    fileContexts_[NormalizePathKey(filePath)] = { hwnd, tabIndex, filePath };
    TraceLsp(L"SetContext tab=" + std::to_wstring(tabIndex) + L" file=" + filePath);
}

void ClangdClient::SetDiagnosticsCallback(DiagCallback cb) {
    diagCb_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// DidOpen / DidChange
// ---------------------------------------------------------------------------

void ClangdClient::DidOpen(const std::wstring& filePath, const std::string& utf8Content, int version) {
    {
        std::lock_guard<std::mutex> lk(docsMutex_);
        openDocuments_[filePath] = OpenDocumentState{utf8Content, version};
        openedInSession_.insert(NormalizePathKey(filePath));
    }
    TraceLsp(L"DidOpen v=" + std::to_wstring(version) + L" file=" + filePath);

    std::string uri = FilePathToUri(filePath);
    std::string escaped = EscapeJsonString(utf8Content);

    std::string json =
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{"
        "\"textDocument\":{"
        "\"uri\":\"" + uri + "\","
        "\"languageId\":\"cpp\","
        "\"version\":" + std::to_string(version) + ","
        "\"text\":\"" + escaped + "\""
        "}}}";

    SendOrBuffer(json);
}

void ClangdClient::DidChange(const std::wstring& filePath, const std::string& utf8Content, int version) {
    bool mustSendDidOpen = false;
    {
        std::lock_guard<std::mutex> lk(docsMutex_);
        openDocuments_[filePath] = OpenDocumentState{utf8Content, version};
        const std::wstring key = NormalizePathKey(filePath);
        if (openedInSession_.find(key) == openedInSession_.end())
            mustSendDidOpen = true;
    }

    if (mustSendDidOpen) {
        TraceLsp(L"DidChange->DidOpen upgrade v=" + std::to_wstring(version) + L" file=" + filePath);
        DidOpen(filePath, utf8Content, version);
        return;
    }

    TraceLsp(L"DidChange v=" + std::to_wstring(version) + L" file=" + filePath);

    std::string uri = FilePathToUri(filePath);
    std::string escaped = EscapeJsonString(utf8Content);

    std::string json =
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{"
        "\"textDocument\":{"
        "\"uri\":\"" + uri + "\","
        "\"version\":" + std::to_string(version) +
        "},"
        "\"contentChanges\":[{\"text\":\"" + escaped + "\"}]"
        "}}";

    SendOrBuffer(json);
}

void ClangdClient::DidSave(const std::wstring& filePath) {
    TraceLsp(L"DidSave file=" + filePath);

    std::string uri = FilePathToUri(filePath);
    std::string json =
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didSave\",\"params\":{"
        "\"textDocument\":{"
        "\"uri\":\"" + uri + "\""
        "}}}";

    SendOrBuffer(json);
}

// ---------------------------------------------------------------------------
// Send / SendOrBuffer / FlushPending
// ---------------------------------------------------------------------------

void ClangdClient::Send(const std::string& json) {
    // IMPORTANT: avoid calling IsRunning() here.
    // Send() can be reached while StartLocked() holds lifecycleMutex_
    // (through SendInitialize), and IsRunning() would try to lock it again.
    if (!running_.load() || !hStdinWr_)
        return;

    std::string msg = "Content-Length: " + std::to_string(json.size()) + "\r\n\r\n" + json;

    std::lock_guard<std::mutex> lk(sendMutex_);
    DWORD written = 0;
    if (!WriteFile(hStdinWr_, msg.data(), static_cast<DWORD>(msg.size()), &written, nullptr))
    {
        running_ = false;
        initialized_ = false;
    }
}

void ClangdClient::SendOrBuffer(const std::string& json) {
    if (initialized_) {
        Send(json);
    } else {
        std::lock_guard<std::mutex> lk(pendingMutex_);
        pendingNotifications_.push_back({ json });
    }
}

void ClangdClient::FlushPending() {
    std::vector<PendingNotification> pending;
    {
        std::lock_guard<std::mutex> lk(pendingMutex_);
        pending.swap(pendingNotifications_);
    }
    for (auto& n : pending)
        Send(n.json);
}

void ClangdClient::ReplayOpenDocuments()
{
    std::vector<std::pair<std::wstring, OpenDocumentState>> docs;
    {
        std::lock_guard<std::mutex> lk(docsMutex_);
        docs.reserve(openDocuments_.size());
        for (const auto& kv : openDocuments_)
            docs.push_back(kv);
    }

    for (const auto& kv : docs)
        DidOpen(kv.first, kv.second.content, kv.second.version);

    TraceLsp(L"ReplayOpenDocuments count=" + std::to_wstring((unsigned long long)docs.size()));
}

// ---------------------------------------------------------------------------
// ReaderLoop
// ---------------------------------------------------------------------------

void ClangdClient::ReaderLoop() {
    std::string buf;
    buf.reserve(65536);

    char tmp[4096];

    while (running_) {
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(hStdoutRd_, tmp, sizeof(tmp), &bytesRead, nullptr);
        if (!ok || bytesRead == 0)
            break; // pipe closed or process died

        buf.append(tmp, bytesRead);

        // Parse as many complete messages as are in the buffer
        while (true) {
            // Look for "Content-Length: <N>\r\n\r\n"
            const std::string header_key = "Content-Length: ";
            size_t hpos = buf.find(header_key);
            if (hpos == std::string::npos)
                break;

            size_t numStart = hpos + header_key.size();
            size_t crpos = buf.find("\r\n\r\n", numStart);
            if (crpos == std::string::npos)
                break;

            // Parse the content length
            std::string numStr = buf.substr(numStart, crpos - numStart);
            // numStr may contain extra headers between \r\n; grab only first line
            size_t nlpos = numStr.find('\r');
            if (nlpos != std::string::npos)
                numStr = numStr.substr(0, nlpos);

            int contentLength = 0;
            try { contentLength = std::stoi(numStr); } catch (...) { break; }
            if (contentLength <= 0)
                break;

            size_t bodyStart = crpos + 4; // skip \r\n\r\n
            if (buf.size() < bodyStart + static_cast<size_t>(contentLength))
                break; // incomplete body, wait for more data

            std::string body = buf.substr(bodyStart, contentLength);
            buf.erase(0, bodyStart + contentLength);

            HandleMessage(body);
        }
    }

    // If the reader loop exits unexpectedly, ensure the client state reflects
    // that clangd is no longer available.
    running_ = false;
    initialized_ = false;
}

// ---------------------------------------------------------------------------
// TranslateClangdMessage  (clangd → CLion style)
// ---------------------------------------------------------------------------

static std::wstring TranslateClangdMessage(const std::wstring& msg)
{
    // Helper lambdas
    auto startsWith = [&](const wchar_t* prefix) -> bool {
        return msg.compare(0, wcslen(prefix), prefix) == 0;
    };
    auto extractQuoted = [&](size_t from) -> std::wstring {
        size_t q1 = msg.find(L'\'', from);
        if (q1 == std::wstring::npos) return L"";
        size_t q2 = msg.find(L'\'', q1 + 1);
        if (q2 == std::wstring::npos) return L"";
        return msg.substr(q1 + 1, q2 - q1 - 1);
    };

    // "Included header 'X' is not used directly"  →  clearer hint
    if (startsWith(L"Included header") && msg.find(L"not used directly") != std::wstring::npos) {
        std::wstring sym = extractQuoted(0);
        if (!sym.empty()) return L"'" + sym + L"' is included but not used";
        return L"Included but not used";
    }

    // "Use of undeclared identifier 'X'"  →  "Cannot resolve symbol 'X'"
    if (startsWith(L"Use of undeclared identifier")) {
        std::wstring sym = extractQuoted(0);
        if (!sym.empty()) return L"Cannot resolve symbol '" + sym + L"'";
    }

    // "Unknown type name 'X'"  →  "Cannot resolve symbol 'X'"
    if (startsWith(L"Unknown type name")) {
        std::wstring sym = extractQuoted(0);
        if (!sym.empty()) return L"Cannot resolve symbol '" + sym + L"'";
    }

    // "No member named 'X' in 'Y'"  →  "Cannot resolve member 'X'"
    if (startsWith(L"No member named")) {
        std::wstring sym = extractQuoted(0);
        if (!sym.empty()) return L"Cannot resolve member '" + sym + L"'";
    }

    // "No matching function for call to 'X'"  →  "Cannot resolve overloaded function 'X'"
    if (startsWith(L"No matching function for call to")) {
        std::wstring sym = extractQuoted(0);
        if (!sym.empty()) return L"Cannot resolve overloaded function '" + sym + L"'";
        return L"Cannot resolve overloaded function";
    }

    // "Reference to overloaded function could not be resolved; did you mean to call it?"
    // → clearer message explaining the likely cause
    if (startsWith(L"Reference to overloaded function could not be resolved")) {
        std::wstring sym = extractQuoted(0);
        if (!sym.empty()) return L"'" + sym + L"' is ambiguous — wrong name or argument types?";
        return L"Ambiguous call — wrong name or argument types?";
    }

    // "Too many arguments to function call, expected N, have M"  →  "Too many arguments"
    if (startsWith(L"Too many arguments to function call"))
        return L"Too many arguments";

    // "Too few arguments to function call"  →  "Too few arguments"
    if (startsWith(L"Too few arguments to function call"))
        return L"Too few arguments";

    // "Redefinition of 'X'"  →  "'X' is already defined in this scope"
    if (startsWith(L"Redefinition of")) {
        std::wstring sym = extractQuoted(0);
        if (!sym.empty()) return L"'" + sym + L"' is already defined in this scope";
    }

    // "Unused variable 'X'"  →  "Variable 'X' is never used"
    if (startsWith(L"Unused variable")) {
        std::wstring sym = extractQuoted(0);
        if (!sym.empty()) return L"Variable '" + sym + L"' is never used";
    }

    // "'return' with a value, in function returning void"
    if (startsWith(L"'return' with a value, in function returning void"))
        return L"Return with a value in void function";

    // "'return' with no value"  →  "Missing return value"
    if (startsWith(L"'return' with no value"))
        return L"Missing return value";

    // "Control reaches end of non-void function"
    if (startsWith(L"Control reaches end of non-void function"))
        return L"Not all code paths return a value";

    // "Expected ';'"  →  "';' expected"
    if (startsWith(L"Expected ';'"))
        return L"';' expected";

    // "Expected '}'"  →  "'}' expected"
    if (startsWith(L"Expected '}'"))
        return L"'}' expected";

    // "Expected '{'"  →  "'{' expected"
    if (startsWith(L"Expected '{'"))
        return L"'{' expected";

    // "Expected expression"
    if (startsWith(L"Expected expression"))
        return L"Expression expected";

    // "Implicit instantiation of undefined template 'X'"
    if (startsWith(L"Implicit instantiation of undefined template")) {
        std::wstring sym = extractQuoted(0);
        if (!sym.empty()) return L"Template '" + sym + L"' has no definition";
    }

    // "Variable length array of non-POD element type"
    if (startsWith(L"Variable length array of non-POD"))
        return L"Variable-length array of non-trivial element type";

    // "Member access into incomplete type 'X'"
    if (startsWith(L"Member access into incomplete type")) {
        std::wstring sym = extractQuoted(0);
        if (!sym.empty()) return L"Incomplete type '" + sym + L"'";
    }

    // No translation found — return original
    return msg;
}

// ---------------------------------------------------------------------------
// HandleMessage
// ---------------------------------------------------------------------------

void ClangdClient::HandleMessage(const std::string& json) {
    // Check if this is a response to our initialize request (has "id" and "result")
    if (!initialized_) {
        std::string result = JsonGetObject(json, "result");
        int id = JsonGetInt(json, "id", -1);
        bool looksLikeInitResponse = (json.find("\"id\":1") != std::string::npos &&
                                      json.find("\"result\"") != std::string::npos);
        if ((id == 1 && !result.empty()) || looksLikeInitResponse) {
            // Initialize response received
            SendInitialized();
            initialized_ = true;
            FlushPending();

            bool replayOpenDocuments = false;
            {
                std::lock_guard<std::mutex> lk(docsMutex_);
                replayOpenDocuments = replayOpenDocumentsOnInit_;
                replayOpenDocumentsOnInit_ = false;
            }
            if (replayOpenDocuments)
                ReplayOpenDocuments();

            return;
        }
    }

    // Dispatch response to a pending request callback (definition, completion, etc.)
    int respId = JsonGetInt(json, "id", -1);
    if (respId >= 2) {
        std::function<void(const std::string&)> cb;
        {
            std::lock_guard<std::mutex> lk(requestsMutex_);
            auto it = pendingRequests_.find(respId);
            if (it != pendingRequests_.end()) {
                cb = std::move(it->second.callback);
                pendingRequests_.erase(it);
            }
        }
        if (cb) { cb(json); return; }
    }

    // Notification: textDocument/publishDiagnostics
    std::string method = JsonGetString(json, "method");
    if (method == "textDocument/publishDiagnostics") {
        std::string params = JsonGetObject(json, "params");
        if (params.empty())
            return;

        std::string uriStr = JsonGetString(params, "uri");
        std::wstring filePath = UriToFilePath(uriStr);

        std::string diagArray = JsonGetArray(params, "diagnostics");
        std::vector<std::string> diagObjs = JsonSplitObjects(diagArray);

        std::vector<Diagnostic> diags;
        diags.reserve(diagObjs.size());

        for (const auto& diagJson : diagObjs) {
            // range.start
            std::string range  = JsonGetObject(diagJson, "range");
            if (range.empty()) continue;
            std::string start  = JsonGetObject(range, "start");
            std::string end_   = JsonGetObject(range, "end");

            int line     = JsonGetInt(start, "line", 0);
            int startCol = JsonGetInt(start, "character", 0);
            int endCol   = JsonGetInt(end_,  "character", startCol);

            int severityInt = JsonGetInt(diagJson, "severity", 1);
            DiagnosticSeverity sev = DiagnosticSeverity::Error;
            if (severityInt == 2)      sev = DiagnosticSeverity::Warning;
            else if (severityInt >= 3) sev = DiagnosticSeverity::Info;

            std::string msgUtf8 = JsonGetString(diagJson, "message");
            std::wstring msg    = TranslateClangdMessage(Utf8ToWide(msgUtf8));

            // Demote stylistic hints to Info so they render in gray
            if (sev == DiagnosticSeverity::Warning) {
                if (msgUtf8.find("not used directly") != std::string::npos ||
                    msgUtf8.find("is never used")     != std::string::npos)
                    sev = DiagnosticSeverity::Info;
            }

            Diagnostic d;
            d.line     = line;
            d.startCol = startCol;
            d.endCol   = endCol;
            d.severity = sev;
            d.message  = msg;
            diags.push_back(std::move(d));
        }

        // Look up context and fire callback
        if (diagCb_) {
            HWND hwnd     = nullptr;
            int  tabIndex = -1;
            std::wstring payloadPath = filePath;
            {
                std::lock_guard<std::mutex> lk(ctxMutex_);
                auto it = fileContexts_.find(NormalizePathKey(filePath));
                if (it != fileContexts_.end()) {
                    hwnd     = it->second.hwnd;
                    tabIndex = it->second.tabIndex;
                    payloadPath = it->second.originalPath;
                }
            }
            if (hwnd)
                diagCb_(payloadPath, tabIndex, hwnd, diags);

            std::wstring firstDiag = diags.empty() ? L"" : diags.front().message;
            TraceLsp(L"publishDiagnostics file=" + filePath +
                     L" mapped=" + payloadPath +
                     L" tab=" + std::to_wstring(tabIndex) +
                     L" count=" + std::to_wstring((unsigned long long)diags.size()) +
                     (hwnd ? L" ctx=ok" : L" ctx=missing") +
                     (firstDiag.empty() ? L"" : (L" first=\"" + firstDiag + L"\"")));
        }
    }
}

// ---------------------------------------------------------------------------
// FindMinGWIncludes
// ---------------------------------------------------------------------------

std::vector<std::string> ClangdClient::FindMinGWIncludes() {
    std::wstring mingwBin = FindMinGW();
    if (mingwBin.empty())
        return {};

    // Strip "bin" to get root
    std::wstring root = mingwBin;
    size_t sep = root.find_last_of(L"\\/");
    if (sep != std::wstring::npos)
        root = root.substr(0, sep);

    std::vector<std::wstring> candidates = {
        root + L"\\include\\c++\\v1",   // libc++ (iostream, etc.)
        root + L"\\include",            // MinGW / Windows SDK headers
    };

    // Clang builtin includes — find versioned subdirectory dynamically
    {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((root + L"\\lib\\clang\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') {
                    candidates.push_back(root + L"\\lib\\clang\\" + fd.cFileName + L"\\include");
                    break;
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }

    std::vector<std::string> result;
    for (const auto& w : candidates) {
        wchar_t resolved[MAX_PATH]{};
        if (GetFullPathNameW(w.c_str(), MAX_PATH, resolved, nullptr) > 0 &&
            GetFileAttributesW(resolved) != INVALID_FILE_ATTRIBUTES) {
            std::string path = WideToUtf8(resolved);
            for (char& c : path)
                if (c == '\\') c = '/';
            result.push_back(std::move(path));
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// RequestDefinitionSync / RequestCompletionsSync
// ---------------------------------------------------------------------------

std::optional<Location> ClangdClient::RequestDefinitionSync(
    const std::wstring& filePath, int line, int col, int timeoutMs)
{
    if (!IsReady()) return std::nullopt;

    struct State {
        std::mutex mtx;
        std::condition_variable cv;
        std::optional<Location> result;
        bool done = false;
    };
    auto state = std::make_shared<State>();

    int id = nextRequestId_++;
    {
        std::lock_guard<std::mutex> lk(requestsMutex_);
        pendingRequests_[id] = { [state, this](const std::string& json) {
            std::string resultArr = JsonGetArray(json, "result");
            if (!resultArr.empty()) {
                auto objs = JsonSplitObjects(resultArr);
                if (!objs.empty()) {
                    std::string uri   = JsonGetString(objs[0], "uri");
                    std::string range = JsonGetObject(objs[0], "range");
                    std::string start = JsonGetObject(range, "start");
                    Location loc;
                    loc.filePath = UriToFilePath(uri);
                    loc.line     = JsonGetInt(start, "line", 0);
                    loc.column   = JsonGetInt(start, "character", 0);
                    std::lock_guard<std::mutex> lk2(state->mtx);
                    state->result = loc;
                }
            }
            std::lock_guard<std::mutex> lk2(state->mtx);
            state->done = true;
            state->cv.notify_one();
        }};
    }

    std::string uri = FilePathToUri(filePath);
    std::string req =
        "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
        ",\"method\":\"textDocument/definition\",\"params\":{"
        "\"textDocument\":{\"uri\":\"" + uri + "\"},"
        "\"position\":{\"line\":" + std::to_string(line) +
        ",\"character\":" + std::to_string(col) + "}}}";
    Send(req);

    std::unique_lock<std::mutex> lk(state->mtx);
    state->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return state->done; });

    if (!state->done) {
        std::lock_guard<std::mutex> lk2(requestsMutex_);
        pendingRequests_.erase(id);
    }
    return state->result;
}

std::vector<CompletionItem> ClangdClient::RequestCompletionsSync(
    const std::wstring& filePath, int line, int col, int timeoutMs)
{
    if (!IsReady()) return {};

    struct State {
        std::mutex mtx;
        std::condition_variable cv;
        std::vector<CompletionItem> result;
        bool done = false;
    };
    auto state = std::make_shared<State>();

    int id = nextRequestId_++;
    {
        std::lock_guard<std::mutex> lk(requestsMutex_);
        pendingRequests_[id] = { [state, this](const std::string& json) {
            // Response is either an array or {"isIncomplete":...,"items":[...]}
            std::string itemsArr = JsonGetArray(json, "result");
            if (itemsArr.empty()) {
                std::string result = JsonGetObject(json, "result");
                if (!result.empty())
                    itemsArr = JsonGetArray(result, "items");
            }
            std::vector<CompletionItem> items;
            for (const auto& obj : JsonSplitObjects(itemsArr)) {
                CompletionItem item;
                item.label       = Utf8ToWide(JsonGetString(obj, "label"));
                item.description = Utf8ToWide(JsonGetString(obj, "detail"));
                item.category    = L"lsp";
                items.push_back(std::move(item));
            }
            std::lock_guard<std::mutex> lk2(state->mtx);
            state->result = std::move(items);
            state->done   = true;
            state->cv.notify_one();
        }};
    }

    std::string uri = FilePathToUri(filePath);
    std::string req =
        "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
        ",\"method\":\"textDocument/completion\",\"params\":{"
        "\"textDocument\":{\"uri\":\"" + uri + "\"},"
        "\"position\":{\"line\":" + std::to_string(line) +
        ",\"character\":" + std::to_string(col) + "}}}";
    Send(req);

    std::unique_lock<std::mutex> lk(state->mtx);
    state->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return state->done; });

    if (!state->done) {
        std::lock_guard<std::mutex> lk2(requestsMutex_);
        pendingRequests_.erase(id);
    }
    return state->result;
}

// ---------------------------------------------------------------------------
// SendInitialize / SendInitialized
// ---------------------------------------------------------------------------

void ClangdClient::SendInitialize() {
    DWORD pid = GetCurrentProcessId();
    std::string rootUri = FilePathToUri(projectRoot_);

    // Build fallbackFlags from bundled MinGW includes
    std::string initOptions;
    std::vector<std::string> includes = FindMinGWIncludes();
    if (!includes.empty()) {
        std::string flags = "[";
        for (const auto& inc : includes) {
            if (flags.size() > 1) flags += ',';
            flags += "\"-isystem\",\"" + EscapeJsonString(inc) + "\"";
        }
        flags += "]";
        initOptions = "\"initializationOptions\":{\"fallbackFlags\":" + flags + "},";
    }

    std::string json =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"processId\":" + std::to_string(pid) + ","
        "\"rootUri\":\"" + rootUri + "\","
        + initOptions +
        "\"capabilities\":{"
        "\"workspace\":{\"didChangeWatchedFiles\":{\"dynamicRegistration\":false}},"
        "\"textDocument\":{"
        "\"publishDiagnostics\":{\"relatedInformation\":false}"
        "}},"
        "\"trace\":\"off\""
        "}}";

    Send(json);
}

void ClangdClient::SendInitialized() {
    Send("{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}");
}

// ---------------------------------------------------------------------------
// FilePathToUri / UriToFilePath
// ---------------------------------------------------------------------------

std::string ClangdClient::FilePathToUri(const std::wstring& path) {
    // Convert to UTF-8 first
    std::string utf8 = WideToUtf8(path);

    // Replace backslashes with forward slashes
    for (char& c : utf8)
        if (c == '\\') c = '/';

    // Percent-encode ':' as %3A (for drive letter like C:)
    std::string encoded;
    encoded.reserve(utf8.size() + 4);
    for (char c : utf8) {
        if (c == ':')
            encoded += "%3A";
        else
            encoded += c;
    }

    return "file:///" + encoded;
}

std::wstring ClangdClient::UriToFilePath(const std::string& uri) {
    std::string s = uri;

    // Remove "file:///" prefix
    const std::string prefix = "file:///";
    if (s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix)
        s = s.substr(prefix.size());

    // Decode %3A -> ':'
    std::string decoded;
    decoded.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            char hi = s[i+1];
            char lo = s[i+2];
            // Check for %3A or %3a
            if ((hi == '3' || hi == '3') && (lo == 'A' || lo == 'a')) {
                decoded += ':';
                i += 2;
                continue;
            }
            // Generic percent-decode
            auto hexVal = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return -1;
            };
            int h = hexVal(hi), l = hexVal(lo);
            if (h >= 0 && l >= 0) {
                decoded += static_cast<char>(h * 16 + l);
                i += 2;
                continue;
            }
        }
        decoded += s[i];
    }

    // Convert forward slashes to backslashes on Windows
    for (char& c : decoded)
        if (c == '/') c = '\\';

    return Utf8ToWide(decoded);
}

// ---------------------------------------------------------------------------
// WideToUtf8 / Utf8ToWide
// ---------------------------------------------------------------------------

std::string ClangdClient::WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring ClangdClient::Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), result.data(), size);
    return result;
}

// ---------------------------------------------------------------------------
// EscapeJsonString
// ---------------------------------------------------------------------------

std::string ClangdClient::EscapeJsonString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

// Skip whitespace
static size_t SkipWs(const std::string& s, size_t pos) {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\r' || s[pos] == '\n'))
        ++pos;
    return pos;
}

// Skip over a JSON string starting at the opening quote; returns position after closing quote
static size_t SkipString(const std::string& s, size_t pos) {
    // pos points at the opening '"'
    if (pos >= s.size() || s[pos] != '"') return pos;
    ++pos;
    while (pos < s.size()) {
        if (s[pos] == '\\') { pos += 2; continue; }
        if (s[pos] == '"')  { ++pos; return pos; }
        ++pos;
    }
    return pos;
}

// Skip a JSON value (object, array, string, number, bool, null)
static size_t SkipValue(const std::string& s, size_t pos);

static size_t SkipObject(const std::string& s, size_t pos) {
    // pos points at '{'
    if (pos >= s.size() || s[pos] != '{') return pos;
    ++pos;
    pos = SkipWs(s, pos);
    if (pos < s.size() && s[pos] == '}') return pos + 1;
    while (pos < s.size()) {
        pos = SkipWs(s, pos);
        if (s[pos] == '}') return pos + 1;
        // key
        pos = SkipString(s, pos);
        pos = SkipWs(s, pos);
        if (pos < s.size() && s[pos] == ':') ++pos;
        pos = SkipWs(s, pos);
        pos = SkipValue(s, pos);
        pos = SkipWs(s, pos);
        if (pos < s.size() && s[pos] == ',') ++pos;
    }
    return pos;
}

static size_t SkipArray(const std::string& s, size_t pos) {
    // pos points at '['
    if (pos >= s.size() || s[pos] != '[') return pos;
    ++pos;
    pos = SkipWs(s, pos);
    if (pos < s.size() && s[pos] == ']') return pos + 1;
    while (pos < s.size()) {
        pos = SkipWs(s, pos);
        if (s[pos] == ']') return pos + 1;
        pos = SkipValue(s, pos);
        pos = SkipWs(s, pos);
        if (pos < s.size() && s[pos] == ',') ++pos;
    }
    return pos;
}

static size_t SkipValue(const std::string& s, size_t pos) {
    pos = SkipWs(s, pos);
    if (pos >= s.size()) return pos;
    char c = s[pos];
    if (c == '"')  return SkipString(s, pos);
    if (c == '{')  return SkipObject(s, pos);
    if (c == '[')  return SkipArray(s, pos);
    // number, bool, null — scan until delimiter
    while (pos < s.size()) {
        char ch = s[pos];
        if (ch == ',' || ch == '}' || ch == ']' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
            break;
        ++pos;
    }
    return pos;
}

// Find key in a JSON object string and return position right after the colon
// Returns std::string::npos if not found
static size_t FindKey(const std::string& json, const std::string& key) {
    size_t pos = 0;
    while (pos < json.size()) {
        // Scan for '"'
        pos = SkipWs(json, pos);
        if (pos >= json.size()) break;
        if (json[pos] != '"') { ++pos; continue; }

        // Read the key string
        size_t keyStart = pos + 1;
        size_t keyEnd   = SkipString(json, pos); // keyEnd is after closing '"'
        std::string k   = json.substr(keyStart, keyEnd - keyStart - 1);

        pos = SkipWs(json, keyEnd);
        if (pos < json.size() && json[pos] == ':') {
            ++pos; // skip ':'
            if (k == key)
                return pos; // pos is right after ':'
            // else skip the value
            pos = SkipValue(json, pos);
            pos = SkipWs(json, pos);
            if (pos < json.size() && json[pos] == ',') ++pos;
        } else {
            ++pos;
        }
    }
    return std::string::npos;
}

// Decode a JSON string value (handles basic escape sequences)
static std::string DecodeJsonString(const std::string& s, size_t start, size_t end) {
    // start points at the character AFTER opening '"', end points AT closing '"'
    std::string out;
    out.reserve(end - start);
    for (size_t i = start; i < end; ) {
        if (s[i] == '\\' && i + 1 < end) {
            char esc = s[i+1];
            switch (esc) {
                case '"':  out += '"';  i += 2; break;
                case '\\': out += '\\'; i += 2; break;
                case '/':  out += '/';  i += 2; break;
                case 'n':  out += '\n'; i += 2; break;
                case 'r':  out += '\r'; i += 2; break;
                case 't':  out += '\t'; i += 2; break;
                case 'u': {
                    // \uXXXX
                    if (i + 5 < end) {
                        char hex[5] = { s[i+2], s[i+3], s[i+4], s[i+5], 0 };
                        unsigned int cp = static_cast<unsigned int>(strtoul(hex, nullptr, 16));
                        // Simple UTF-8 encoding
                        if (cp < 0x80) {
                            out += static_cast<char>(cp);
                        } else if (cp < 0x800) {
                            out += static_cast<char>(0xC0 | (cp >> 6));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            out += static_cast<char>(0xE0 | (cp >> 12));
                            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        i += 6;
                    } else {
                        out += esc; i += 2;
                    }
                    break;
                }
                default: out += esc; i += 2; break;
            }
        } else {
            out += s[i++];
        }
    }
    return out;
}

std::string ClangdClient::JsonGetString(const std::string& json, const std::string& key) {
    size_t pos = FindKey(json, key);
    if (pos == std::string::npos) return {};
    pos = SkipWs(json, pos);
    if (pos >= json.size() || json[pos] != '"') return {};
    size_t contentStart = pos + 1;
    size_t endPos = SkipString(json, pos);  // endPos is after closing '"'
    size_t contentEnd = endPos - 1;         // points AT closing '"'
    return DecodeJsonString(json, contentStart, contentEnd);
}

int ClangdClient::JsonGetInt(const std::string& json, const std::string& key, int def) {
    size_t pos = FindKey(json, key);
    if (pos == std::string::npos) return def;
    pos = SkipWs(json, pos);
    if (pos >= json.size()) return def;

    // Handle null / bool
    if (json[pos] == 'n' || json[pos] == 't' || json[pos] == 'f') return def;

    // Parse sign + digits
    bool negative = false;
    if (json[pos] == '-') { negative = true; ++pos; }
    if (pos >= json.size() || !isdigit(static_cast<unsigned char>(json[pos]))) return def;

    int val = 0;
    while (pos < json.size() && isdigit(static_cast<unsigned char>(json[pos])))
        val = val * 10 + (json[pos++] - '0');

    return negative ? -val : val;
}

std::string ClangdClient::JsonGetObject(const std::string& json, const std::string& key) {
    size_t pos = FindKey(json, key);
    if (pos == std::string::npos) return {};
    pos = SkipWs(json, pos);
    if (pos >= json.size() || json[pos] != '{') return {};
    size_t objStart = pos;
    size_t objEnd   = SkipObject(json, pos);
    return json.substr(objStart, objEnd - objStart);
}

std::string ClangdClient::JsonGetArray(const std::string& json, const std::string& key) {
    size_t pos = FindKey(json, key);
    if (pos == std::string::npos) return {};
    pos = SkipWs(json, pos);
    if (pos >= json.size() || json[pos] != '[') return {};
    size_t arrStart = pos;
    size_t arrEnd   = SkipArray(json, pos);
    return json.substr(arrStart, arrEnd - arrStart);
}

std::vector<std::string> ClangdClient::JsonSplitObjects(const std::string& arrayContent) {
    // arrayContent may be "[{...},{...}]" or just the content between brackets
    std::vector<std::string> result;
    size_t pos = 0;
    // Skip leading '['
    if (!arrayContent.empty() && arrayContent[pos] == '[') ++pos;

    while (pos < arrayContent.size()) {
        pos = SkipWs(arrayContent, pos);
        if (pos >= arrayContent.size()) break;
        if (arrayContent[pos] == ']') break;
        if (arrayContent[pos] == '{') {
            size_t objStart = pos;
            size_t objEnd   = SkipObject(arrayContent, pos);
            result.push_back(arrayContent.substr(objStart, objEnd - objStart));
            pos = objEnd;
            pos = SkipWs(arrayContent, pos);
            if (pos < arrayContent.size() && arrayContent[pos] == ',') ++pos;
        } else {
            ++pos;
        }
    }
    return result;
}

} // namespace Lsp
