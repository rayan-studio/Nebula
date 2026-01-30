#include "core/window/Window.h"
#include "utils/logger/Logger.h"
#include "core/explorer/Explorer.h"
#include "ui/panels/terminal/TerminalPanel.h"
#include <windows.h>
#include <shellapi.h>
#include <filesystem>
#include <optional>
#include <sstream>
#include <fstream>
#include <unordered_map>
#include <vector>
#include <string>
#include <thread>
#include <cwctype>
#include <cctype>

static std::wstring QuotePath(const std::filesystem::path &path)
{
    return L"\"" + path.wstring() + L"\"";
}

static bool RunCommandAndWait(const std::wstring &command, const std::filesystem::path &workingDir, DWORD &exitCode)
{
    std::wstring cmdLine = L"cmd.exe /C " + command;
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    std::wstring workdirStr = workingDir.wstring();
    wchar_t *mutableCmd = _wcsdup(cmdLine.c_str());
    BOOL ok = CreateProcessW(
        nullptr,
        mutableCmd,
        nullptr,
        nullptr,
        FALSE,
        CREATE_NEW_CONSOLE,
        nullptr,
        workdirStr.empty() ? nullptr : workdirStr.c_str(),
        &si,
        &pi);
    free(mutableCmd);

    if (!ok)
    {
        exitCode = GetLastError();
        return false;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return exitCode == 0;
}

static bool RunCommandAndWait(const std::wstring &command,
                              const std::filesystem::path &workingDir,
                              DWORD &exitCode,
                              const std::vector<wchar_t> *envBlock)
{
    std::wstring cmdLine = L"cmd.exe /C " + command;
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    std::wstring workdirStr = workingDir.wstring();
    wchar_t *mutableCmd = _wcsdup(cmdLine.c_str());
    BOOL ok = CreateProcessW(
        nullptr,
        mutableCmd,
        nullptr,
        nullptr,
        FALSE,
        CREATE_NEW_CONSOLE,
        envBlock && !envBlock->empty() ? (LPVOID)envBlock->data() : nullptr,
        workdirStr.empty() ? nullptr : workdirStr.c_str(),
        &si,
        &pi);
    free(mutableCmd);

    if (!ok)
    {
        exitCode = GetLastError();
        return false;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return exitCode == 0;
}

static std::wstring BytesToWide(const std::string& bytes)
{
    if (bytes.empty())
        return L"";

    // Heuristic: UTF-16LE often has 0x00 in every odd byte
    if (bytes.size() >= 2)
    {
        size_t zerosOdd = 0;
        for (size_t i = 1; i < bytes.size(); i += 2)
        {
            if (bytes[i] == 0)
                ++zerosOdd;
        }
        if (zerosOdd > bytes.size() / 4)
        {
            size_t wlen = bytes.size() / 2;
            std::wstring out;
            out.resize(wlen);
            memcpy(out.data(), bytes.data(), wlen * sizeof(wchar_t));
            return out;
        }
    }

    int len = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), NULL, 0);
    if (len > 0)
    {
        std::wstring out((size_t)len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), out.data(), len);
        return out;
    }

    len = MultiByteToWideChar(CP_ACP, 0, bytes.data(), (int)bytes.size(), NULL, 0);
    if (len <= 0)
        return L"";
    std::wstring out((size_t)len, L'\0');
    MultiByteToWideChar(CP_ACP, 0, bytes.data(), (int)bytes.size(), out.data(), len);
    return out;
}

static std::wstring FormatWin32Error(DWORD err)
{
    LPWSTR msg = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD len = FormatMessageW(flags, nullptr, err, 0, (LPWSTR)&msg, 0, nullptr);
    std::wstring out;
    if (len && msg)
    {
        out.assign(msg, msg + len);
        while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n'))
            out.pop_back();
    }
    if (msg)
        LocalFree(msg);
    if (out.empty())
        out = L"Win32 error " + std::to_wstring(err);
    return out;
}

static bool RunCommandAndCapture(const std::wstring &command,
                                 const std::filesystem::path &workingDir,
                                 DWORD &exitCode,
                                 const std::vector<wchar_t> *envBlock,
                                 HWND hwnd,
                                 std::wstring *lastLineOut = nullptr)
{
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE inRead = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (inRead == INVALID_HANDLE_VALUE)
        inRead = NULL;

    HANDLE outRead = NULL;
    HANDLE outWrite = NULL;
    if (!CreatePipe(&outRead, &outWrite, &sa, 0))
    {
        exitCode = GetLastError();
        std::wstring msg = L"[run] CreatePipe failed: " + FormatWin32Error(exitCode) + L"\n";
        GetTerminalPanel().AppendOutputChunk(msg);
        if (hwnd)
            InvalidateRect(hwnd, nullptr, FALSE);
        if (inRead)
            CloseHandle(inRead);
        return false;
    }
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outWrite;
    si.hStdError = outWrite;
    si.hStdInput = inRead ? inRead : GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};

    std::wstring cmdLine = L"cmd.exe /C " + command;
    std::wstring workdirStr = workingDir.wstring();
    wchar_t *mutableCmd = _wcsdup(cmdLine.c_str());
    BOOL ok = CreateProcessW(
        nullptr,
        mutableCmd,
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        envBlock && !envBlock->empty() ? (LPVOID)envBlock->data() : nullptr,
        workdirStr.empty() ? nullptr : workdirStr.c_str(),
        &si,
        &pi);
    free(mutableCmd);

    CloseHandle(outWrite);
    if (inRead)
        CloseHandle(inRead);

    if (!ok)
    {
        exitCode = GetLastError();
        std::wstring msg = L"[run] Failed to start command.\n";
        msg += L"Command: " + command + L"\n";
        msg += L"Reason: " + FormatWin32Error(exitCode) + L"\n";
        msg += L"Hint: verify CMake is installed and available in PATH.\n";
        GetTerminalPanel().AppendOutputChunk(msg);
        GetTerminalPanel().FlushOutputBuffer();
        if (hwnd)
            InvalidateRect(hwnd, nullptr, FALSE);
        CloseHandle(outRead);
        return false;
    }

    char buffer[4096];
    DWORD read = 0;
    std::wstring lineBuf;
    std::wstring lastLine;
    while (true)
    {
        BOOL success = ReadFile(outRead, buffer, sizeof(buffer), &read, NULL);
        if (!success || read == 0)
            break;

        std::string chunk(buffer, buffer + read);
        std::wstring wide = BytesToWide(chunk);
        if (!wide.empty())
        {
            for (wchar_t c : wide)
            {
                if (c == L'\r')
                    continue;
                if (c == L'\n')
                {
                    if (!lineBuf.empty())
                        lastLine = lineBuf;
                    lineBuf.clear();
                    continue;
                }
                lineBuf.push_back(c);
            }
            GetTerminalPanel().AppendOutputChunk(wide);
            if (hwnd)
                InvalidateRect(hwnd, nullptr, FALSE);
        }
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(outRead);

    GetTerminalPanel().FlushOutputBuffer();
    if (hwnd)
        InvalidateRect(hwnd, nullptr, FALSE);
    if (lastLineOut)
    {
        if (!lineBuf.empty())
            lastLine = lineBuf;
        *lastLineOut = lastLine;
    }
    return exitCode == 0;
}

static bool RunCommandInNewConsole(const std::wstring &command,
                                   const std::filesystem::path &workingDir,
                                   DWORD &exitCode,
                                   const std::vector<wchar_t> *envBlock,
                                   bool pauseOnError)
{
    std::wstring cmdLine = L"cmd.exe /C \"";
    cmdLine += command;
    if (pauseOnError)
        cmdLine += L" & if errorlevel 1 pause";
    cmdLine += L"\"";

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    std::wstring workdirStr = workingDir.wstring();
    wchar_t *mutableCmd = _wcsdup(cmdLine.c_str());
    BOOL ok = CreateProcessW(
        nullptr,
        mutableCmd,
        nullptr,
        nullptr,
        FALSE,
        CREATE_NEW_CONSOLE,
        envBlock && !envBlock->empty() ? (LPVOID)envBlock->data() : nullptr,
        workdirStr.empty() ? nullptr : workdirStr.c_str(),
        &si,
        &pi);
    free(mutableCmd);

    if (!ok)
    {
        exitCode = GetLastError();
        return false;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return exitCode == 0;
}

static bool LaunchExecutable(const std::filesystem::path &exePath, bool keepConsoleOpen = false)
{
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    std::wstring cmdLine;
    if (keepConsoleOpen)
        cmdLine = L"cmd.exe /K " + QuotePath(exePath);
    else
        cmdLine = QuotePath(exePath);

    wchar_t *mutableCmd = _wcsdup(cmdLine.c_str());
    BOOL ok = CreateProcessW(
        nullptr,
        mutableCmd,
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        exePath.parent_path().wstring().c_str(),
        &si,
        &pi);
    free(mutableCmd);

    if (!ok)
        return false;

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

enum class SimpleCompiler
{
    None,
    MSVC,
    GPP
};

static std::optional<std::filesystem::path> FindVcVars64()
{
    std::vector<std::filesystem::path> candidates = {
        L"C:\\Program Files\\Microsoft Visual Studio\\18\\BuildTools\\VC\\Auxiliary\\Build\\vcvars64.bat",
        L"C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat",
        L"C:\\Program Files\\Microsoft Visual Studio\\18\\Professional\\VC\\Auxiliary\\Build\\vcvars64.bat",
        L"C:\\Program Files\\Microsoft Visual Studio\\18\\Enterprise\\VC\\Auxiliary\\Build\\vcvars64.bat",

        L"C:\\Program Files\\Microsoft Visual Studio\\2026\\BuildTools\\VC\\Auxiliary\\Build\\vcvars64.bat",
        L"C:\\Program Files\\Microsoft Visual Studio\\2026\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat",
        L"C:\\Program Files\\Microsoft Visual Studio\\2026\\Professional\\VC\\Auxiliary\\Build\\vcvars64.bat",
        L"C:\\Program Files\\Microsoft Visual Studio\\2026\\Enterprise\\VC\\Auxiliary\\Build\\vcvars64.bat",

        L"C:\\Program Files\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Auxiliary\\Build\\vcvars64.bat",
        L"C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat",
        L"C:\\Program Files\\Microsoft Visual Studio\\2022\\Professional\\VC\\Auxiliary\\Build\\vcvars64.bat",
        L"C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise\\VC\\Auxiliary\\Build\\vcvars64.bat",

        L"C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\BuildTools\\VC\\Auxiliary\\Build\\vcvars64.bat",
        L"C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat",
        L"C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\Professional\\VC\\Auxiliary\\Build\\vcvars64.bat",
        L"C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\Enterprise\\VC\\Auxiliary\\Build\\vcvars64.bat",

        L"C:\\Program Files (x86)\\Microsoft Visual Studio\\2017\\BuildTools\\VC\\Auxiliary\\Build\\vcvars64.bat",
        L"C:\\Program Files (x86)\\Microsoft Visual Studio\\2017\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat",
        L"C:\\Program Files (x86)\\Microsoft Visual Studio\\2017\\Professional\\VC\\Auxiliary\\Build\\vcvars64.bat",
        L"C:\\Program Files (x86)\\Microsoft Visual Studio\\2017\\Enterprise\\VC\\Auxiliary\\Build\\vcvars64.bat",

        L"C:\\Program Files (x86)\\Microsoft Visual Studio\\18\\BuildTools\\VC\\Auxiliary\\Build\\vcvars64.bat",
        L"C:\\Program Files (x86)\\Microsoft Visual Studio\\18\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat",
        L"C:\\Program Files (x86)\\Microsoft Visual Studio\\18\\Professional\\VC\\Auxiliary\\Build\\vcvars64.bat",
        L"C:\\Program Files (x86)\\Microsoft Visual Studio\\18\\Enterprise\\VC\\Auxiliary\\Build\\vcvars64.bat"};

    std::error_code ec;
    for (const auto &p : candidates)
    {
        if (std::filesystem::exists(p, ec))
            return p;
    }
    return std::nullopt;
}

static bool FindInPath(const std::wstring &exeName)
{
    wchar_t buf[MAX_PATH] = {0};
    DWORD res = SearchPathW(nullptr, exeName.c_str(), nullptr, MAX_PATH, buf, nullptr);
    return res > 0 && res < MAX_PATH;
}

static bool FileExistsW(const std::wstring &path)
{
    DWORD attrs = GetFileAttributesW(path.c_str());
    return (attrs != INVALID_FILE_ATTRIBUTES) && ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0);
}

static std::wstring ParentDirW(std::wstring path)
{
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos)
        return L"";
    path.resize(pos);
    return path;
}

static std::wstring ToLowerW(std::wstring s)
{
    for (auto &ch : s)
        ch = (wchar_t)towlower(ch);
    return s;
}

static std::wstring GetExeDir()
{
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring path = exePath;
    return ParentDirW(path);
}

static std::wstring FindToolchainConfig()
{
    std::wstring base = GetExeDir();
    for (int i = 0; i < 6 && !base.empty(); ++i)
    {
        const std::wstring cand1 = base + L"\\toolchains\\current.json";
        const std::wstring cand2 = base + L"\\dist\\toolchains\\current.json";
        const std::wstring cand3 = base + L"\\external\\Nebula Studio 2026\\toolchains\\current.json";
        const std::wstring cand4 = base + L"\\external\\Nebula Studio 2026\\dist\\toolchains\\current.json";
        if (FileExistsW(cand1)) return cand1;
        if (FileExistsW(cand2)) return cand2;
        if (FileExistsW(cand3)) return cand3;
        if (FileExistsW(cand4)) return cand4;
        base = ParentDirW(base);
    }
    return L"";
}

static std::string ReadFileUtf8(const std::wstring &path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    std::ostringstream oss;
    oss << file.rdbuf();
    return oss.str();
}

static std::string ExtractJsonString(const std::string &json, const std::string &key)
{
    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos)
        return {};
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos)
        return {};
    pos = json.find('"', pos);
    if (pos == std::string::npos)
        return {};
    ++pos;

    std::string out;
    bool esc = false;
    for (; pos < json.size(); ++pos)
    {
        char c = json[pos];
        if (esc)
        {
            out.push_back(c);
            esc = false;
            continue;
        }
        if (c == '\\')
        {
            esc = true;
            continue;
        }
        if (c == '"')
            break;
        out.push_back(c);
    }
    return out;
}

static std::wstring Utf8ToWide(const std::string &s)
{
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    if (len <= 0) return L"";
    std::wstring out((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len);
    return out;
}

static std::wstring NormalizeSlashes(std::wstring path)
{
    for (auto &ch : path)
        if (ch == L'/') ch = L'\\';
    return path;
}

static bool IsAbsolutePath(const std::wstring &path)
{
    if (path.size() >= 2 && path[1] == L':')
        return true;
    if (path.rfind(L"\\\\", 0) == 0)
        return true;
    return false;
}

static std::wstring JoinPath(const std::wstring &base, const std::wstring &rel)
{
    if (base.empty()) return rel;
    if (rel.empty()) return base;
    if (base.back() == L'\\' || base.back() == L'/')
        return base + rel;
    return base + L"\\" + rel;
}

static std::wstring ResolvePath(const std::wstring &root, const std::wstring &rel)
{
    std::wstring r = NormalizeSlashes(rel);
    if (IsAbsolutePath(r))
        return r;
    return NormalizeSlashes(JoinPath(root, r));
}

static void SplitPathList(const std::wstring &list, std::vector<std::wstring> &out)
{
    size_t start = 0;
    while (start <= list.size())
    {
        size_t pos = list.find(L';', start);
        if (pos == std::wstring::npos) pos = list.size();
        std::wstring item = list.substr(start, pos - start);
        if (!item.empty())
            out.push_back(item);
        start = pos + 1;
    }
}

static std::wstring JoinPathList(const std::vector<std::wstring> &items)
{
    std::wstring out;
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (i > 0) out.append(L";");
        out.append(items[i]);
    }
    return out;
}

static std::wstring BuildToolchainPath(const std::wstring &currentPath,
                                       const std::vector<std::wstring> &prepend)
{
    std::vector<std::wstring> existing;
    SplitPathList(currentPath, existing);

    std::vector<std::wstring> result;
    std::unordered_map<std::wstring, bool> seen;

    auto addUnique = [&](const std::wstring &p) {
        std::wstring key = ToLowerW(p);
        if (seen.find(key) != seen.end())
            return;
        seen[key] = true;
        result.push_back(p);
    };

    for (const auto &p : prepend)
        if (!p.empty())
            addUnique(p);

    for (const auto &p : existing)
        if (!p.empty())
            addUnique(p);

    return JoinPathList(result);
}

struct ToolchainConfig
{
    std::wstring toolchainBin;
    std::wstring cmakeBin;
    std::wstring ninjaDir;
    std::wstring cc;
    std::wstring cxx;
    std::wstring rootDir;
};

static bool LoadToolchainConfig(ToolchainConfig &out)
{
    std::wstring cfgPath = FindToolchainConfig();
    if (cfgPath.empty())
        return false;

    std::string json = ReadFileUtf8(cfgPath);
    if (json.empty())
        return false;

    std::wstring cfgDir = ParentDirW(cfgPath);
    std::wstring root = cfgDir;
    const std::wstring toolchainsSuffix = L"\\toolchains";
    if (cfgDir.size() >= toolchainsSuffix.size() &&
        ToLowerW(cfgDir.substr(cfgDir.size() - toolchainsSuffix.size())) == ToLowerW(toolchainsSuffix))
    {
        root = ParentDirW(cfgDir);
    }

    out.rootDir = root;
    out.toolchainBin = ResolvePath(root, Utf8ToWide(ExtractJsonString(json, "toolchainBin")));
    out.cmakeBin = ResolvePath(root, Utf8ToWide(ExtractJsonString(json, "cmakeBin")));
    out.ninjaDir = ResolvePath(root, Utf8ToWide(ExtractJsonString(json, "ninjaDir")));
    out.cc = Utf8ToWide(ExtractJsonString(json, "cc"));
    out.cxx = Utf8ToWide(ExtractJsonString(json, "cxx"));
    return true;
}

static std::vector<wchar_t> BuildEnvironmentBlock(const ToolchainConfig *tc)
{
    LPWCH env = GetEnvironmentStringsW();
    std::vector<std::wstring> entries;

    if (env)
    {
        for (LPWCH p = env; *p; )
        {
            std::wstring entry = p;
            entries.push_back(entry);
            p += entry.size() + 1;
        }
        FreeEnvironmentStringsW(env);
    }

    auto findVarIndex = [&](const std::wstring &name) -> int {
        for (size_t i = 0; i < entries.size(); ++i)
        {
            const std::wstring &e = entries[i];
            size_t eq = e.find(L'=');
            if (eq == std::wstring::npos)
                continue;
            std::wstring var = e.substr(0, eq);
            if (ToLowerW(var) == ToLowerW(name))
                return (int)i;
        }
        return -1;
    };

    auto setVar = [&](const std::wstring &name, const std::wstring &value) {
        int idx = findVarIndex(name);
        std::wstring entry = name + L"=" + value;
        if (idx >= 0)
            entries[(size_t)idx] = entry;
        else
            entries.push_back(entry);
    };

    if (tc)
    {
        std::vector<std::wstring> prepend;
        if (!tc->toolchainBin.empty()) prepend.push_back(tc->toolchainBin);
        if (!tc->cmakeBin.empty() && ToLowerW(tc->cmakeBin) != ToLowerW(tc->toolchainBin)) prepend.push_back(tc->cmakeBin);
        if (!tc->ninjaDir.empty() && ToLowerW(tc->ninjaDir) != ToLowerW(tc->toolchainBin)) prepend.push_back(tc->ninjaDir);

        int idx = findVarIndex(L"PATH");
        std::wstring currentPath;
        if (idx >= 0)
        {
            size_t eq = entries[(size_t)idx].find(L'=');
            currentPath = (eq == std::wstring::npos) ? L"" : entries[(size_t)idx].substr(eq + 1);
        }
        std::wstring newPath = BuildToolchainPath(currentPath, prepend);
        setVar(L"PATH", newPath);

        if (!tc->cc.empty())
            setVar(L"CC", tc->cc);
        if (!tc->cxx.empty())
            setVar(L"CXX", tc->cxx);
    }

    std::vector<wchar_t> block;
    for (const auto &e : entries)
    {
        block.insert(block.end(), e.begin(), e.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

static SimpleCompiler DetectSimpleCompiler()
{
    if (FindVcVars64().has_value() || FindInPath(L"cl.exe"))
        return SimpleCompiler::MSVC;
    if (FindInPath(L"g++.exe"))
        return SimpleCompiler::GPP;
    return SimpleCompiler::None;
}

static bool IsCppLikeFile(const std::wstring &path)
{
    std::wstring ext;
    size_t pos = path.find_last_of(L'.');
    if (pos != std::wstring::npos)
    {
        ext = path.substr(pos);
        for (auto &c : ext)
            c = towlower(c);
    }
    return ext == L".cpp" || ext == L".cc" || ext == L".cxx" || ext == L".c" || ext == L".h" || ext == L".hpp";
}

static bool HasSolutionFile(const std::filesystem::path &root)
{
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(root, ec))
    {
        if (ec)
            break;
        if (!entry.is_regular_file(ec))
            continue;
        auto ext = entry.path().extension().wstring();
        if (_wcsicmp(ext.c_str(), L".sln") == 0)
            return true;
    }
    return false;
}

static std::filesystem::path FindNewestExecutable(const std::filesystem::path &root)
{
    std::error_code ec;
    std::filesystem::path newest;
    std::filesystem::file_time_type newestTime{};

    if (!std::filesystem::exists(root, ec))
        return newest;

    for (const auto &entry : std::filesystem::recursive_directory_iterator(root, ec))
    {
        if (ec)
            break;
        if (!entry.is_regular_file(ec))
            continue;

        auto path = entry.path();
        if (_wcsicmp(path.extension().wstring().c_str(), L".exe") != 0)
            continue;

        auto filename = path.filename().wstring();
        if (filename.find(L"cmake") != std::wstring::npos)
            continue;

        auto time = entry.last_write_time(ec);
        if (ec)
            continue;
        if (newest.empty() || time > newestTime)
        {
            newest = path;
            newestTime = time;
        }
    }

    return newest;
}

static std::wstring EscapeForCmdQuoted(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s)
    {
        if (c == L'"')
            out.append(L"\"\"");
        else
            out.push_back(c);
    }
    return out;
}

static std::wstring BuildRunNewestExeCommand(const std::filesystem::path &root)
{
    std::wstring dir = root.wstring();
    size_t pos = 0;
    while ((pos = dir.find(L'\'', pos)) != std::wstring::npos)
    {
        dir.insert(pos, 1, L'\'');
        pos += 2;
    }

    std::wstring ps;
    ps += L"$dir='" + dir + L"'; ";
    ps += L"$exe=Get-ChildItem -Path $dir -Recurse -Filter *.exe -File | ";
    ps += L"Where-Object { $_.Name -notmatch 'cmake' } | ";
    ps += L"Sort-Object LastWriteTime -Desc | Select-Object -First 1; ";
    ps += L"if ($exe) { & $exe.FullName } else { Write-Host 'Aucun executable trouve apres compilation.' }";
    return ps;
}

static void ShowRunErrorPopup(HWND owner, const std::wstring &title, const std::wstring &detail)
{
    std::wstring msg = title;
    if (!detail.empty())
        msg += L"\n" + detail;
    std::wstring *copy = new std::wstring(msg);
    PostMessageW(owner, WM_SHOW_RUN_ERROR_POPUP, 0, reinterpret_cast<LPARAM>(copy));
}

static std::string GetProjectTypeFromRoot(const std::filesystem::path &root)
{
    std::filesystem::path projPath = root / ".nebula" / "project.json";
    std::ifstream ifs(projPath, std::ios::binary);
    if (!ifs)
        return {};

    std::string contents((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    auto pos = contents.find("\"type\"");
    if (pos == std::string::npos)
        return {};
    pos = contents.find(':', pos);
    if (pos == std::string::npos)
        return {};
    pos = contents.find('"', pos);
    if (pos == std::string::npos)
        return {};
    auto end = contents.find('"', pos + 1);
    if (end == std::string::npos)
        return {};
    return contents.substr(pos + 1, end - pos - 1);
}

void Window::RunActiveProject()
{
    std::wstring root = GetExplorerManager().GetState().rootPath;
    Orion::Editor *activeEditor = GetEditor();
    std::wstring activeFile = activeEditor ? activeEditor->GetFilePath() : L"";

    ToolchainConfig tc{};
    const bool hasToolchain = LoadToolchainConfig(tc);
    const std::vector<wchar_t> envBlock = BuildEnvironmentBlock(hasToolchain ? &tc : nullptr);
    // For dev runs, prefer an external console window for full logs.

    std::filesystem::path rootPath(root);
    std::error_code ec;
    bool hasCMake = std::filesystem::exists(rootPath / "CMakeLists.txt", ec);
    bool hasSolution = HasSolutionFile(rootPath);

    if (root.empty() || (!hasCMake && !hasSolution))
    {
        if (activeFile.empty() || activeFile.rfind(L"__untitled__", 0) == 0)
        {
            MessageBoxW(hwnd_, L"Aucun projet dÃ©tectÃ© et aucun fichier sauvegardÃ© actif.", L"Run", MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (!IsCppLikeFile(activeFile))
        {
            MessageBoxW(hwnd_, L"Aucun projet C++ dÃ©tectÃ© et le fichier actif n'est pas C/C++.", L"Run", MB_OK | MB_ICONINFORMATION);
            return;
        }

        SimpleCompiler compiler = DetectSimpleCompiler();
        if (hasToolchain)
        {
            std::filesystem::path gpp = std::filesystem::path(tc.toolchainBin) / L"g++.exe";
            if (std::filesystem::exists(gpp, ec))
                compiler = SimpleCompiler::GPP;
        }
        if (compiler == SimpleCompiler::None)
        {
            MessageBoxW(hwnd_,
                        L"Aucun compilateur dÃ©tectÃ©.\nInstalle Visual Studio Build Tools (MSVC) ou MinGW-w64 (g++), puis relance.",
                        L"Run",
                        MB_OK | MB_ICONWARNING);
            return;
        }

        std::filesystem::path filePath(activeFile);
        std::filesystem::path buildDir = filePath.parent_path() / "build" / "single";
        std::filesystem::create_directories(buildDir, ec);

        std::filesystem::path outExe = buildDir / "app.exe";
        std::wstring cmd;
        std::wstring workDir = buildDir.wstring();

        if (compiler == SimpleCompiler::MSVC)
        {
            auto vcvars = FindVcVars64();
            if (vcvars.has_value())
            {
                cmd = L"\"\"" + vcvars->wstring() + L"\" && cl /nologo /EHsc /std:c++17 " +
                      QuotePath(filePath) + L" /Fe:" + QuotePath(outExe) + L"\"";
            }
            else
            {
                cmd = L"cl /nologo /EHsc /std:c++17 " + QuotePath(filePath) + L" /Fe:" + QuotePath(outExe);
            }
        }
        else
        {
            cmd = L"g++ -std=c++17 -g " + QuotePath(filePath) + L" -o " + QuotePath(outExe);
        }

        std::thread([cmd, buildDir, outExe, hwnd = hwnd_, envBlock]()
        {
            DWORD exitCode = 0;
            if (!RunCommandInNewConsole(cmd, buildDir, exitCode, &envBlock, true))
            {
                MessageBoxW(hwnd, L"Compilation echouee. Verifie la console.", L"Run", MB_OK | MB_ICONERROR);
                return;
            }

            if (!LaunchExecutable(outExe, true))
            {
                MessageBoxW(hwnd, L"Impossible de lancer l'executable.", L"Run", MB_OK | MB_ICONERROR);
                return;
            }
        }).detach();
        return;
    }

    if (!hasCMake)
    {
        MessageBoxW(hwnd_, L"Le lancement automatique supporte seulement CMake pour le moment.", L"Run", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const std::string projType = GetProjectTypeFromRoot(rootPath);
    const bool keepConsoleOpen = (projType == "cpp-console");
    const bool useNinja = hasToolchain;

    // Prefer a valid system CMake when the toolchain CMake is missing its share/ directory.
    std::wstring systemCMake = L"C:\\Program Files\\CMake\\bin\\cmake.exe";
    std::wstring systemCMakeX86 = L"C:\\Program Files (x86)\\CMake\\bin\\cmake.exe";
    std::wstring cmakeExe;
    if (hasToolchain)
    {
        std::error_code ec2;
        if (!tc.cmakeBin.empty())
        {
            std::filesystem::path cmakeBin = tc.cmakeBin;
            std::filesystem::path cmakePath = cmakeBin / "cmake.exe";
            std::filesystem::path shareDir = cmakeBin.parent_path() / "share";
            bool hasShare = false;
            if (std::filesystem::exists(shareDir, ec2))
            {
                for (const auto &entry : std::filesystem::directory_iterator(shareDir, ec2))
                {
                    if (!entry.is_directory(ec2))
                        continue;
                    std::wstring name = entry.path().filename().wstring();
                    if (name.rfind(L"cmake-", 0) == 0)
                    {
                        hasShare = true;
                        break;
                    }
                }
            }
            if (std::filesystem::exists(cmakePath, ec2) && hasShare)
                cmakeExe = cmakePath.wstring();
        }
    }
    if (cmakeExe.empty())
    {
        std::error_code ec3;
        if (std::filesystem::exists(systemCMake, ec3))
            cmakeExe = systemCMake;
        else if (std::filesystem::exists(systemCMakeX86, ec3))
            cmakeExe = systemCMakeX86;
    }

    // Route CMake output to Output panel (no extra terminal tabs).
    {
        TerminalPanel &terminal = GetTerminalPanel();
        terminal.ClearOutput();
        terminal.ShowOutput(true);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    std::thread([rootPath, hwnd = hwnd_, keepConsoleOpen, envBlock, useNinja, cmakeExe]()
    {
        Logger::Instance().Log(L"Run: CMake configure/build started.");

        std::filesystem::path buildDir = rootPath / (useNinja ? "build-ninja" : "build");
        DWORD exitCode = 0;
        std::wstring cmakeCmd = cmakeExe.empty() ? L"cmake" : QuotePath(cmakeExe);
        std::wstring configureCmd = cmakeCmd + L" -S " + QuotePath(rootPath) + L" -B " + QuotePath(buildDir);
        if (useNinja)
            configureCmd += L" -G \"Ninja\" -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++";

        std::wstring buildCmd = cmakeCmd + L" --build " + QuotePath(buildDir);
        if (!useNinja)
            buildCmd += L" --config Release";
        TerminalPanel &terminal = GetTerminalPanel();
        terminal.AppendOutputChunk(L"$ " + configureCmd + L"\n");
        std::wstring lastLine;
        if (!RunCommandAndCapture(configureCmd, rootPath, exitCode, &envBlock, hwnd, &lastLine))
        {
            Logger::Instance().Log(L"Run: CMake configure failed.");
            ShowRunErrorPopup(hwnd, L"Configuration CMake echouee", lastLine);
            return;
        }

        terminal.AppendOutputChunk(L"$ " + buildCmd + L"\n");
        lastLine.clear();
        if (!RunCommandAndCapture(buildCmd, rootPath, exitCode, &envBlock, hwnd, &lastLine))
        {
            Logger::Instance().Log(L"Run: Build failed.");
            ShowRunErrorPopup(hwnd, L"Compilation echouee", lastLine);
            return;
        }

        std::filesystem::path exe = FindNewestExecutable(buildDir / "Release");
        if (exe.empty())
            exe = FindNewestExecutable(buildDir / "Debug");
        if (exe.empty())
            exe = FindNewestExecutable(buildDir);

        if (exe.empty())
        {
            Logger::Instance().Log(L"Run: No executable found after build.");
            ShowRunErrorPopup(hwnd, L"Aucun executable apres compilation", L"Verifie le panneau Sortie");
            return;
        }

        if (!LaunchExecutable(exe, keepConsoleOpen))
        {
            Logger::Instance().Log(L"Run: Failed to launch executable.");
            MessageBoxW(hwnd, L"Impossible de lancer l'exÃ©cutable.", L"Run", MB_OK | MB_ICONERROR);
            return;
        }

        Logger::Instance().Log(L"Run: Executable launched.");
    }).detach();
}

bool Window::HandleCommandLineArgs()
{
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return false;

    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i)
        args.emplace_back(argv[i]);

    LocalFree(argv);

    bool openedFromArgs = false;
    auto openDirectory = [&](const std::filesystem::path &path)
    {
        std::wstring folder = path.wstring();
        GetExplorerManager().Initialize(folder);
        GetExplorerManager().SetVisible(true);
        InvalidateRect(hwnd_, nullptr, FALSE);
        openedFromArgs = true;
    };

    auto openFile = [&](const std::filesystem::path &path)
    {
        std::filesystem::path parent = path.parent_path();
        if (!parent.empty())
            openDirectory(parent);
        OpenFileInNewTab(path.wstring(), -1);
        InvalidateRect(hwnd_, nullptr, FALSE);
        openedFromArgs = true;
    };

    for (size_t i = 0; i < args.size(); ++i)
    {
        const std::wstring &arg = args[i];

        if ((arg == L"--project" || arg == L"--folder" || arg == L"-p") && i + 1 < args.size())
        {
            std::filesystem::path path(args[i + 1]);
            if (std::filesystem::exists(path))
                openDirectory(path);
            i++;
            continue;
        }

        if ((arg == L"--file" || arg == L"-f") && i + 1 < args.size())
        {
            std::filesystem::path path(args[i + 1]);
            if (std::filesystem::exists(path))
                openFile(path);
            i++;
            continue;
        }

        std::filesystem::path path(arg);
        if (!std::filesystem::exists(path))
            continue;

        if (std::filesystem::is_directory(path))
        {
            openDirectory(path);
        }
        else
        {
            openFile(path);
        }
    }

    if (!openedFromArgs)
    {
        wchar_t cwdBuf[MAX_PATH] = {0};
        DWORD cwdLen = GetCurrentDirectoryW(MAX_PATH, cwdBuf);
        if (cwdLen > 0)
        {
            std::filesystem::path cwdPath(cwdBuf);
            std::filesystem::path exePath;
            wchar_t exeBuf[MAX_PATH] = {0};
            if (GetModuleFileNameW(NULL, exeBuf, MAX_PATH) > 0)
                exePath = std::filesystem::path(exeBuf).parent_path();

            std::error_code ec;
            if (!exePath.empty() && std::filesystem::exists(cwdPath, ec))
            {
                std::filesystem::path normCwd = std::filesystem::weakly_canonical(cwdPath, ec);
                std::filesystem::path normExe = std::filesystem::weakly_canonical(exePath, ec);
                if (ec)
                {
                    normCwd = cwdPath;
                    normExe = exePath;
                }

                if (normCwd != normExe)
                {
                    openDirectory(normCwd);
                }
            }
        }
    }

    if (openedFromArgs)
    {
        skipNewProjectOverlayOnce_ = true;
        HideNewProjectOverlay();
    }
    return openedFromArgs;
}

