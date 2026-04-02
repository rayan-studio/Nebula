#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

class ClaudeCliBridge
{
public:
    struct AuthInfo
    {
        bool available = false;
        bool loggedIn = false;
        std::wstring authMethod;
        std::wstring email;
        std::wstring orgId;
        std::wstring orgName;
        std::wstring subscriptionType;
    };

    struct ToolEvent
    {
        std::wstring toolId;
        std::wstring toolName;
        std::wstring title;
        std::wstring details;
        bool success = true;
    };

    enum class AuthState
    {
        NotConfigured,
        Checking,
        Ready,
        NeedsLogin,
        Error,
    };

    struct RequestOptions
    {
        std::wstring executablePath;
        std::wstring workingDirectory;
        std::wstring resumeSessionId;
        std::wstring prompt;
    };

    ClaudeCliBridge();
    ~ClaudeCliBridge();

    void CheckAuthStatusAsync(const std::wstring &executablePath, const std::wstring &workingDirectory);
    bool StartRequest(const RequestOptions &options);
    void Cancel();

    bool IsBusy() const { return requestRunning_.load(); }
    AuthState GetAuthState() const { return authState_.load(); }
    static std::wstring QuoteArg(const std::wstring &value);

    std::function<void(AuthState, const std::wstring &)> onAuthStatus;
    std::function<void(const AuthInfo &)> onAuthInfo;
    std::function<void()> onRequestStarted;
    std::function<void(const std::wstring &)> onTextDelta;
    std::function<void(const ToolEvent &)> onToolEvent;
    std::function<void(const std::wstring &, const std::wstring &)> onRequestFinished;
    std::function<void(const std::wstring &)> onError;

private:
    static std::wstring Trim(const std::wstring &text);
    static std::wstring Utf8ToWide(const std::string &text);
    static std::string WideToUtf8(const std::wstring &text);
    static bool EndsWithInsensitive(const std::wstring &value, const wchar_t *suffix);
    static std::wstring BuildCommandLine(const std::wstring &executablePath,
                                         const std::vector<std::wstring> &args,
                                         std::wstring &outApplicationName,
                                         bool &outUsesCmdWrapper);
    static bool RunProcessCapture(const std::wstring &applicationName,
                                  std::wstring commandLine,
                                  const std::wstring &workingDirectory,
                                  const std::string *stdinUtf8,
                                  DWORD &outExitCode,
                                  std::string &outStdout,
                                  std::function<void(const std::string &)> onStdoutChunk,
                                  std::atomic<bool> *cancelFlag,
                                  HANDLE *outProcessHandle = nullptr);

    void JoinThreads();
    void EmitError(const std::wstring &message);
    void RequestWorkerMain(RequestOptions options);

    std::thread authThread_;
    std::thread requestThread_;

    std::atomic<bool> authRunning_{false};
    std::atomic<bool> requestRunning_{false};
    std::atomic<bool> cancelRequest_{false};
    std::atomic<AuthState> authState_{AuthState::NotConfigured};

    HANDLE activeProcessHandle_ = nullptr;
    CRITICAL_SECTION processLock_{};
};
