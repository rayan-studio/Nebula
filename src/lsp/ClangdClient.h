#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include "LspManager.h"

namespace Lsp {

class ClangdClient {
public:
    static ClangdClient& Instance();

    using DiagCallback = std::function<void(const std::wstring& filePath, int tabIndex, HWND hwnd, const std::vector<Diagnostic>&)>;

    // Find and start clangd. Returns false if clangd not found.
    bool Start(const std::wstring& projectRoot);
    bool IsRunning() const;
    bool IsReady() const;
    // Returns both flags under a single mutex acquisition (avoids double syscall on hot paths).
    void GetRunningState(bool& running, bool& ready) const;
    void GetRunningState(bool& running, bool& ready, std::wstring& failureReason) const;
    void Stop();

    // Call these when a file is opened or modified
    void DidOpen(const std::wstring& filePath, const std::string& utf8Content, int version = 1);
    void DidChange(const std::wstring& filePath, const std::string& utf8Content, int version);
    void DidSave(const std::wstring& filePath);

    // hwnd + tabIndex are passed back in the callback so we can post WM_LSP_DIAGNOSTICS
    void SetContext(const std::wstring& filePath, HWND hwnd, int tabIndex);

    void SetDiagnosticsCallback(DiagCallback cb);

    // Find clangd / MinGW executables and includes
    static std::wstring FindClangd();
    static std::wstring FindMinGW();
    static std::vector<std::string> FindMinGWIncludes();

    // Returns the per-project directory where compile_commands.json is stored
    // (inside %LOCALAPPDATA%, never in the project tree).
    static std::wstring GetCompileCommandsDir(const std::wstring& projectRoot);

    // Stop the current clangd and restart it fresh for projectRoot.
    // Useful after compile_commands.json has been updated.
    bool RestartForProject(const std::wstring& projectRoot);

    // If projectRoot has a CMakeLists.txt and a build dir, runs cmake in the
    // background with CMAKE_EXPORT_COMPILE_COMMANDS=ON, then notifies clangd.
    void EnsureCompileCommandsAsync(const std::wstring& projectRoot);

    // Blocking definition request via clangd (returns nullopt on timeout/error).
    std::optional<Location> RequestDefinitionSync(const std::wstring& filePath,
                                                   int line, int col,
                                                   int timeoutMs = 2000);

    // Blocking completion request via clangd (returns empty on timeout/error).
    std::vector<CompletionItem> RequestCompletionsSync(const std::wstring& filePath,
                                                        int line, int col,
                                                        int timeoutMs = 500);

private:
    ClangdClient() = default;

    struct OpenDocumentState {
        std::string content;
        int version = 1;
    };

    HANDLE hStdinWr_  = nullptr;
    HANDLE hStdoutRd_ = nullptr;
    HANDLE hProcess_  = nullptr;

    std::thread readerThread_;
    std::atomic<bool> running_{false};
    mutable std::mutex lifecycleMutex_;
    mutable std::mutex failureMutex_;
    std::mutex sendMutex_;

    std::atomic<bool> initialized_{false};
    std::wstring projectRoot_;
    std::wstring lastFailureReason_;
    std::atomic<DWORD> initializeRequestedTick_{0};

    DiagCallback diagCb_;

    struct FileCtx {
        HWND hwnd = nullptr;
        int tabIndex = -1;
        std::wstring originalPath;
    };
    std::mutex ctxMutex_;
    std::unordered_map<std::wstring, FileCtx> fileContexts_;
    std::mutex docsMutex_;
    std::unordered_map<std::wstring, OpenDocumentState> openDocuments_;
    std::unordered_set<std::wstring> openedInSession_;
    bool replayOpenDocumentsOnInit_ = false;

    // Pending notifications buffered until after initialization
    struct PendingNotification {
        std::string json;
    };
    std::mutex pendingMutex_;
    std::vector<PendingNotification> pendingNotifications_;

    // Pending LSP requests (id → callback)
    struct PendingRequest {
        std::function<void(const std::string&)> callback;
    };
    mutable std::mutex requestsMutex_;
    std::unordered_map<int, PendingRequest> pendingRequests_;
    std::atomic<int> nextRequestId_{2}; // 1 is reserved for initialize

    bool StartLocked(const std::wstring& projectRoot);
    bool IsRunningLocked() const;
    void StopLocked();

    void ReaderLoop();
    void Send(const std::string& json);
    void SendOrBuffer(const std::string& json);
    void FlushPending();
    void ReplayOpenDocuments();
    void HandleMessage(const std::string& json);
    void SendInitialize();
    void SendInitialized();

    static std::string FilePathToUri(const std::wstring& path);
    static std::wstring UriToFilePath(const std::string& uri);
    static std::string WideToUtf8(const std::wstring& w);
    static std::wstring Utf8ToWide(const std::string& s);
    static std::string EscapeJsonString(const std::string& s);

    // Minimal JSON helpers
    static std::string  JsonGetString(const std::string& json, const std::string& key);
    static int          JsonGetInt   (const std::string& json, const std::string& key, int def = -1);
    static std::string  JsonGetObject(const std::string& json, const std::string& key);
    static std::string  JsonGetArray (const std::string& json, const std::string& key);
    static std::vector<std::string> JsonSplitObjects(const std::string& arrayContent);
};

} // namespace Lsp
