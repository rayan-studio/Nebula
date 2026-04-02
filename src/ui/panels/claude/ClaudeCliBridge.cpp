#include "ClaudeCliBridge.h"

#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include <sstream>
#include <vector>

namespace
{
    std::wstring TrimWideLocal(const std::wstring &text)
    {
        size_t start = 0;
        while (start < text.size() && iswspace(text[start]))
            ++start;
        size_t end = text.size();
        while (end > start && iswspace(text[end - 1]))
            --end;
        return text.substr(start, end - start);
    }

    std::wstring Utf8ToWideLocal(const std::string &text)
    {
        if (text.empty())
            return {};

        int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0);
        if (len <= 0)
            len = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
        if (len <= 0)
            return {};

        std::wstring out((size_t)len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), len);
        return out;
    }

    std::string ExtractJsonStringValue(const std::string &json, const std::string &key)
    {
        const std::string needle = "\"" + key + "\"";
        size_t pos = json.find(needle);
        if (pos == std::string::npos)
            return {};

        pos = json.find(':', pos + needle.size());
        if (pos == std::string::npos)
            return {};

        pos = json.find('"', pos + 1);
        if (pos == std::string::npos)
            return {};

        std::string out;
        bool escape = false;
        for (size_t i = pos + 1; i < json.size(); ++i)
        {
            char ch = json[i];
            if (escape)
            {
                switch (ch)
                {
                case '"':
                case '\\':
                case '/':
                    out.push_back(ch);
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                default:
                    out.push_back(ch);
                    break;
                }
                escape = false;
                continue;
            }

            if (ch == '\\')
            {
                escape = true;
                continue;
            }

            if (ch == '"')
                break;

            out.push_back(ch);
        }

        return out;
    }

    std::string ExtractTopLevelType(const std::string &json)
    {
        return ExtractJsonStringValue(json, "type");
    }

    int ExtractJsonIntValue(const std::string &json, const std::string &key, int fallback = -1)
    {
        const std::string needle = "\"" + key + "\"";
        size_t pos = json.find(needle);
        if (pos == std::string::npos)
            return fallback;

        pos = json.find(':', pos + needle.size());
        if (pos == std::string::npos)
            return fallback;

        ++pos;
        while (pos < json.size() && isspace((unsigned char)json[pos]))
            ++pos;

        bool negative = false;
        if (pos < json.size() && json[pos] == '-')
        {
            negative = true;
            ++pos;
        }

        size_t start = pos;
        while (pos < json.size() && isdigit((unsigned char)json[pos]))
            ++pos;
        if (start == pos)
            return fallback;

        int value = atoi(json.substr(start, pos - start).c_str());
        return negative ? -value : value;
    }

    std::wstring ExtractPreferredPath(const std::string &inputJson)
    {
        for (const char *key : {"file_path", "path", "directory_path", "cwd"})
        {
            std::string value = ExtractJsonStringValue(inputJson, key);
            if (!value.empty())
                return Utf8ToWideLocal(value);
        }
        return {};
    }

    std::wstring SummarizeToolInput(const std::wstring &toolName, const std::string &inputJson)
    {
        const std::wstring name = TrimWideLocal(toolName);
        if (name.empty())
            return Utf8ToWideLocal(inputJson);

        if (_wcsicmp(name.c_str(), L"Bash") == 0)
        {
            std::string command = ExtractJsonStringValue(inputJson, "command");
            if (!command.empty())
                return Utf8ToWideLocal(command);
        }

        if (_wcsicmp(name.c_str(), L"Read") == 0 ||
            _wcsicmp(name.c_str(), L"Edit") == 0 ||
            _wcsicmp(name.c_str(), L"Write") == 0 ||
            _wcsicmp(name.c_str(), L"MultiEdit") == 0 ||
            _wcsicmp(name.c_str(), L"LS") == 0)
        {
            std::wstring path = ExtractPreferredPath(inputJson);
            if (!path.empty())
                return path;
        }

        if (_wcsicmp(name.c_str(), L"Glob") == 0 || _wcsicmp(name.c_str(), L"Grep") == 0)
        {
            std::wstring pattern = Utf8ToWideLocal(ExtractJsonStringValue(inputJson, "pattern"));
            std::wstring path = ExtractPreferredPath(inputJson);
            if (!pattern.empty() && !path.empty())
                return pattern + L"  in  " + path;
            if (!pattern.empty())
                return pattern;
            if (!path.empty())
                return path;
        }

        std::wstring path = ExtractPreferredPath(inputJson);
        if (!path.empty())
            return path;

        std::wstring fallback = Utf8ToWideLocal(inputJson);
        if (fallback.size() > 220)
            fallback = fallback.substr(0, 220) + L"...";
        return fallback;
    }

    std::wstring ToolTitleFromName(const std::wstring &toolName)
    {
        const std::wstring name = TrimWideLocal(toolName);
        if (_wcsicmp(name.c_str(), L"Bash") == 0)
            return L"Ran command";
        if (_wcsicmp(name.c_str(), L"Read") == 0)
            return L"Read file";
        if (_wcsicmp(name.c_str(), L"Edit") == 0 ||
            _wcsicmp(name.c_str(), L"Write") == 0 ||
            _wcsicmp(name.c_str(), L"MultiEdit") == 0)
            return L"Edited file";
        if (_wcsicmp(name.c_str(), L"Glob") == 0)
            return L"Matched files";
        if (_wcsicmp(name.c_str(), L"Grep") == 0)
            return L"Searched text";
        if (_wcsicmp(name.c_str(), L"LS") == 0)
            return L"Listed directory";
        return L"Used " + name;
    }

    bool FileExists(const std::wstring &path)
    {
        DWORD attrs = GetFileAttributesW(path.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }
}

ClaudeCliBridge::ClaudeCliBridge()
{
    InitializeCriticalSection(&processLock_);
}

ClaudeCliBridge::~ClaudeCliBridge()
{
    Cancel();
    JoinThreads();
    DeleteCriticalSection(&processLock_);
}

void ClaudeCliBridge::JoinThreads()
{
    if (authThread_.joinable())
        authThread_.join();
    if (requestThread_.joinable())
        requestThread_.join();
}

std::wstring ClaudeCliBridge::Trim(const std::wstring &text)
{
    size_t start = 0;
    while (start < text.size() && iswspace(text[start]))
        ++start;
    size_t end = text.size();
    while (end > start && iswspace(text[end - 1]))
        --end;
    return text.substr(start, end - start);
}

std::wstring ClaudeCliBridge::QuoteArg(const std::wstring &value)
{
    if (value.empty())
        return L"\"\"";

    bool needsQuotes = false;
    for (wchar_t ch : value)
    {
        if (iswspace(ch) || ch == L'"')
        {
            needsQuotes = true;
            break;
        }
    }
    if (!needsQuotes)
        return value;

    std::wstring out;
    out.reserve(value.size() + 4);
    out.push_back(L'"');

    unsigned int backslashes = 0;
    for (wchar_t ch : value)
    {
        if (ch == L'\\')
        {
            ++backslashes;
            continue;
        }

        if (ch == L'"')
        {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
            continue;
        }

        if (backslashes > 0)
        {
            out.append(backslashes, L'\\');
            backslashes = 0;
        }
        out.push_back(ch);
    }

    if (backslashes > 0)
        out.append(backslashes * 2, L'\\');

    out.push_back(L'"');
    return out;
}

std::string ClaudeCliBridge::WideToUtf8(const std::wstring &text)
{
    if (text.empty())
        return {};

    int len = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0)
        return {};

    std::string out((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring ClaudeCliBridge::Utf8ToWide(const std::string &text)
{
    if (text.empty())
        return {};

    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0);
    if (len <= 0)
        len = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    if (len <= 0)
        return {};

    std::wstring out((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), len);
    return out;
}

bool ClaudeCliBridge::EndsWithInsensitive(const std::wstring &value, const wchar_t *suffix)
{
    if (!suffix)
        return false;

    size_t suffixLen = wcslen(suffix);
    if (value.size() < suffixLen)
        return false;

    size_t start = value.size() - suffixLen;
    for (size_t i = 0; i < suffixLen; ++i)
    {
        if (towlower(value[start + i]) != towlower(suffix[i]))
            return false;
    }
    return true;
}

std::wstring ClaudeCliBridge::BuildCommandLine(const std::wstring &executablePath,
                                               const std::vector<std::wstring> &args,
                                               std::wstring &outApplicationName,
                                               bool &outUsesCmdWrapper)
{
    outApplicationName.clear();
    outUsesCmdWrapper = false;

    const bool needsCmdWrapper = EndsWithInsensitive(executablePath, L".cmd") ||
                                 EndsWithInsensitive(executablePath, L".bat");

    std::wstring joinedArgs;
    for (const std::wstring &arg : args)
    {
        if (!joinedArgs.empty())
            joinedArgs += L' ';
        joinedArgs += QuoteArg(arg);
    }

    if (!needsCmdWrapper)
    {
        outApplicationName = executablePath;
        std::wstring commandLine = QuoteArg(executablePath);
        if (!joinedArgs.empty())
        {
            commandLine += L' ';
            commandLine += joinedArgs;
        }
        return commandLine;
    }

    wchar_t cmdPath[MAX_PATH] = {};
    UINT len = GetSystemDirectoryW(cmdPath, MAX_PATH);
    std::wstring commandInterpreter;
    if (len > 0 && len < MAX_PATH)
    {
        commandInterpreter.assign(cmdPath, cmdPath + len);
        commandInterpreter += L"\\cmd.exe";
    }
    if (commandInterpreter.empty() || !FileExists(commandInterpreter))
        commandInterpreter = L"cmd.exe";

    outApplicationName = commandInterpreter;
    outUsesCmdWrapper = true;

    std::wstring inner = QuoteArg(executablePath);
    if (!joinedArgs.empty())
    {
        inner += L' ';
        inner += joinedArgs;
    }

    return QuoteArg(commandInterpreter) + L" /D /S /C " + QuoteArg(inner);
}

bool ClaudeCliBridge::RunProcessCapture(const std::wstring &applicationName,
                                        std::wstring commandLine,
                                        const std::wstring &workingDirectory,
                                        const std::string *stdinUtf8,
                                        DWORD &outExitCode,
                                        std::string &outStdout,
                                        std::function<void(const std::string &)> onStdoutChunk,
                                        std::atomic<bool> *cancelFlag,
                                        HANDLE *outProcessHandle)
{
    outExitCode = DWORD(-1);
    outStdout.clear();
    if (outProcessHandle)
        *outProcessHandle = nullptr;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdoutRead = nullptr;
    HANDLE stdoutWrite = nullptr;
    HANDLE stdinRead = nullptr;
    HANDLE stdinWrite = nullptr;

    if (!CreatePipe(&stdoutRead, &stdoutWrite, &sa, 0))
        return false;
    if (!SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0))
    {
        CloseHandle(stdoutRead);
        CloseHandle(stdoutWrite);
        return false;
    }

    if (!CreatePipe(&stdinRead, &stdinWrite, &sa, 0))
    {
        CloseHandle(stdoutRead);
        CloseHandle(stdoutWrite);
        return false;
    }
    if (!SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0))
    {
        CloseHandle(stdoutRead);
        CloseHandle(stdoutWrite);
        CloseHandle(stdinRead);
        CloseHandle(stdinWrite);
        return false;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = stdinRead;
    si.hStdOutput = stdoutWrite;
    si.hStdError = stdoutWrite;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdlineBuffer(commandLine.begin(), commandLine.end());
    cmdlineBuffer.push_back(L'\0');

    const wchar_t *workDirPtr = workingDirectory.empty() ? nullptr : workingDirectory.c_str();
    BOOL created = CreateProcessW(
        applicationName.empty() ? nullptr : applicationName.c_str(),
        cmdlineBuffer.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        nullptr,
        workDirPtr,
        &si,
        &pi);

    CloseHandle(stdinRead);
    CloseHandle(stdoutWrite);

    if (!created)
    {
        CloseHandle(stdoutRead);
        CloseHandle(stdinWrite);
        return false;
    }

    if (outProcessHandle)
        *outProcessHandle = pi.hProcess;

    if (stdinUtf8 && !stdinUtf8->empty())
    {
        DWORD written = 0;
        WriteFile(stdinWrite, stdinUtf8->data(), (DWORD)stdinUtf8->size(), &written, nullptr);
    }
    CloseHandle(stdinWrite);

    std::vector<char> buffer(4096);
    while (true)
    {
        if (cancelFlag && cancelFlag->load())
        {
            TerminateProcess(pi.hProcess, 1);
            break;
        }

        DWORD available = 0;
        if (!PeekNamedPipe(stdoutRead, nullptr, 0, nullptr, &available, nullptr))
            break;

        if (available == 0)
        {
            DWORD wait = WaitForSingleObject(pi.hProcess, 20);
            if (wait == WAIT_OBJECT_0)
            {
                DWORD finalAvailable = 0;
                if (!PeekNamedPipe(stdoutRead, nullptr, 0, nullptr, &finalAvailable, nullptr) || finalAvailable == 0)
                    break;
                available = finalAvailable;
            }
            else
            {
                continue;
            }
        }

        DWORD toRead = (std::min)(available, (DWORD)buffer.size());
        DWORD read = 0;
        if (!ReadFile(stdoutRead, buffer.data(), toRead, &read, nullptr) || read == 0)
            break;

        outStdout.append(buffer.data(), buffer.data() + read);
        if (onStdoutChunk)
            onStdoutChunk(std::string(buffer.data(), buffer.data() + read));
    }

    WaitForSingleObject(pi.hProcess, 5000);
    GetExitCodeProcess(pi.hProcess, &outExitCode);

    CloseHandle(stdoutRead);
    CloseHandle(pi.hThread);
    if (!outProcessHandle)
        CloseHandle(pi.hProcess);

    return true;
}

void ClaudeCliBridge::EmitError(const std::wstring &message)
{
    if (onError)
        onError(message);
}

void ClaudeCliBridge::CheckAuthStatusAsync(const std::wstring &executablePath, const std::wstring &workingDirectory)
{
    const std::wstring trimmedExe = Trim(executablePath);
    if (trimmedExe.empty())
    {
        authState_.store(AuthState::NotConfigured);
        if (onAuthStatus)
            onAuthStatus(AuthState::NotConfigured, L"Claude CLI not configured.");
        return;
    }

    if (authThread_.joinable())
        authThread_.join();

    authRunning_.store(true);
    authState_.store(AuthState::Checking);
    if (onAuthStatus)
        onAuthStatus(AuthState::Checking, L"Checking Claude authentication...");

    authThread_ = std::thread([this, trimmedExe, workingDirectory]() {
        std::wstring applicationName;
        bool usesCmdWrapper = false;
        std::wstring commandLine = BuildCommandLine(trimmedExe,
            {L"auth", L"status", L"--text"},
            applicationName,
            usesCmdWrapper);

        DWORD exitCode = DWORD(-1);
        std::string stdoutText;
        if (!RunProcessCapture(applicationName, std::move(commandLine), workingDirectory, nullptr, exitCode, stdoutText, {}, nullptr, nullptr))
        {
            authRunning_.store(false);
            authState_.store(AuthState::Error);
            if (onAuthStatus)
                onAuthStatus(AuthState::Error, L"Unable to run Claude CLI. Check the configured path.");
            return;
        }

        const std::wstring detail = Trim(Utf8ToWide(stdoutText));
        AuthState nextState = AuthState::Error;
        std::wstring message = detail;

        if (exitCode == 0)
        {
            nextState = AuthState::Ready;
            if (message.empty())
                message = L"Claude CLI is ready.";
        }
        else if (exitCode == 1)
        {
            nextState = AuthState::NeedsLogin;
            if (message.empty())
                message = L"Claude CLI is installed, but no Claude account is connected yet.";
        }
        else
        {
            nextState = AuthState::Error;
            if (message.empty())
                message = L"Claude CLI returned an unexpected authentication status.";
        }

        authState_.store(nextState);
        authRunning_.store(false);
        if (onAuthStatus)
            onAuthStatus(nextState, message);
    });
}

bool ClaudeCliBridge::StartRequest(const RequestOptions &options)
{
    if (requestRunning_.exchange(true))
        return false;

    if (requestThread_.joinable())
        requestThread_.join();

    cancelRequest_.store(false);
    requestThread_ = std::thread(&ClaudeCliBridge::RequestWorkerMain, this, options);
    return true;
}

void ClaudeCliBridge::Cancel()
{
    cancelRequest_.store(true);

    EnterCriticalSection(&processLock_);
    HANDLE process = activeProcessHandle_;
    LeaveCriticalSection(&processLock_);

    if (process)
        TerminateProcess(process, 1);
}

void ClaudeCliBridge::RequestWorkerMain(RequestOptions options)
{
    if (onRequestStarted)
        onRequestStarted();

    const std::wstring exePath = Trim(options.executablePath);
    if (exePath.empty())
    {
        requestRunning_.store(false);
        EmitError(L"Claude CLI not configured.");
        return;
    }

    std::vector<std::wstring> args = {
        L"-p",
        L"--output-format", L"stream-json",
        L"--verbose",
        L"--include-partial-messages",
        L"--max-turns", L"6",
        L"--tools", L"Read,Bash,Edit,Write,MultiEdit,Glob,Grep,LS",
        L"--allowedTools", L"Read,Bash,Edit,Write,MultiEdit,Glob,Grep,LS"
    };

    if (!Trim(options.resumeSessionId).empty())
    {
        args.push_back(L"--resume");
        args.push_back(Trim(options.resumeSessionId));
    }

    std::wstring applicationName;
    bool usesCmdWrapper = false;
    std::wstring commandLine = BuildCommandLine(exePath, args, applicationName, usesCmdWrapper);
    std::string stdinUtf8 = WideToUtf8(options.prompt);

    DWORD exitCode = DWORD(-1);
    std::string stdoutText;
    HANDLE processHandle = nullptr;
    std::wstring sessionId;
    std::wstring fallbackResult;
    std::string pendingLine;
    struct PendingToolBlock
    {
        std::wstring id;
        std::wstring name;
        std::string inputJson;
    };
    std::unordered_map<int, PendingToolBlock> pendingToolBlocks;

    auto processJsonLine = [this, &sessionId, &fallbackResult, &pendingToolBlocks](const std::string &line) {
        if (line.empty())
            return;

        std::string type = ExtractTopLevelType(line);
        if (type == "stream_event")
        {
            if (line.find("\"content_block_start\"") != std::string::npos &&
                (line.find("\"content_block\":{\"type\":\"tool_use\"") != std::string::npos ||
                 line.find("\"content_block\":{\"type\":\"server_tool_use\"") != std::string::npos))
            {
                PendingToolBlock block;
                block.id = Utf8ToWide(ExtractJsonStringValue(line, "id"));
                block.name = Utf8ToWide(ExtractJsonStringValue(line, "name"));
                int index = ExtractJsonIntValue(line, "index");
                if (index >= 0)
                    pendingToolBlocks[index] = std::move(block);
            }
            else if (line.find("\"content_block_delta\"") != std::string::npos &&
                     line.find("\"input_json_delta\"") != std::string::npos)
            {
                int index = ExtractJsonIntValue(line, "index");
                auto it = pendingToolBlocks.find(index);
                if (it != pendingToolBlocks.end())
                    it->second.inputJson += ExtractJsonStringValue(line, "partial_json");
            }
            else if (line.find("\"content_block_stop\"") != std::string::npos)
            {
                int index = ExtractJsonIntValue(line, "index");
                auto it = pendingToolBlocks.find(index);
                if (it != pendingToolBlocks.end())
                {
                    ToolEvent event;
                    event.toolId = it->second.id;
                    event.toolName = it->second.name;
                    event.title = ToolTitleFromName(it->second.name);
                    event.details = SummarizeToolInput(it->second.name, it->second.inputJson);
                    event.success = true;
                    if (onToolEvent)
                        onToolEvent(event);
                    pendingToolBlocks.erase(it);
                }
            }

            if (line.find("\"text_delta\"") != std::string::npos)
            {
                std::string delta = ExtractJsonStringValue(line, "text");
                if (!delta.empty() && onTextDelta)
                    onTextDelta(Utf8ToWide(delta));
            }
        }
        else if (type == "result" || line.find("\"result\"") != std::string::npos)
        {
            std::string maybeSession = ExtractJsonStringValue(line, "session_id");
            if (!maybeSession.empty())
                sessionId = Utf8ToWide(maybeSession);

            std::string resultText = ExtractJsonStringValue(line, "result");
            if (!resultText.empty())
                fallbackResult = Utf8ToWide(resultText);
        }
    };
    auto consumeStdoutChunk = [processJsonLine, &pendingLine](const std::string &chunk) mutable {
        for (char ch : chunk)
        {
            if (ch == '\r')
                continue;
            if (ch != '\n')
            {
                pendingLine.push_back(ch);
                continue;
            }

            processJsonLine(pendingLine);
            pendingLine.clear();
        }
    };
    if (!RunProcessCapture(applicationName, std::move(commandLine), options.workingDirectory, &stdinUtf8,
                           exitCode, stdoutText, consumeStdoutChunk, &cancelRequest_, &processHandle))
    {
        requestRunning_.store(false);
        EmitError(L"Unable to start Claude CLI. Check the configured path.");
        return;
    }

    EnterCriticalSection(&processLock_);
    activeProcessHandle_ = processHandle;
    LeaveCriticalSection(&processLock_);

    if (!pendingLine.empty())
        processJsonLine(pendingLine);

    EnterCriticalSection(&processLock_);
    if (activeProcessHandle_)
    {
        CloseHandle(activeProcessHandle_);
        activeProcessHandle_ = nullptr;
    }
    LeaveCriticalSection(&processLock_);

    const bool wasCancelled = cancelRequest_.load();
    cancelRequest_.store(false);
    requestRunning_.store(false);

    if (wasCancelled)
        return;

    if (exitCode != 0)
    {
        std::wstring errorText = Trim(Utf8ToWide(stdoutText));
        if (errorText.empty())
            errorText = L"Claude CLI request failed.";
        EmitError(errorText);
        return;
    }

    if (onRequestFinished)
        onRequestFinished(sessionId, fallbackResult);
}
