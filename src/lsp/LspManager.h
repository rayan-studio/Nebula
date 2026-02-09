#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <mutex>
#include <windows.h>

namespace Lsp
{
    enum class DiagnosticSeverity
    {
        Error,
        Warning,
        Info
    };

    struct Diagnostic
    {
        int line = 0;
        int startCol = 0;
        int endCol = 0;
        DiagnosticSeverity severity = DiagnosticSeverity::Error;
        std::wstring message;
        std::wstring suggestion;
    };

    struct CompletionItem
    {
        std::wstring label;           // The text to insert
        std::wstring description;     // Short description
        std::wstring category;        // e.g. "std_container", "std_algorithm"
    };

    struct Location
    {
        std::wstring filePath;
        int line = 0;
        int column = 0;
    };

    class LspManager
    {
    public:
        static LspManager &Instance();

        void SetProjectRoot(const std::wstring &rootPath);
        void UpdateFile(const std::wstring &filePath, const std::vector<std::wstring> &lines);
        void RequestDiagnosticsAsync(const std::wstring &filePath,
                                     const std::vector<std::wstring> &lines,
                                     HWND hwnd,
                                     int tabIndex);

        std::optional<Location> GoToDefinition(const std::wstring &filePath,
                                               const std::wstring &lineText,
                                               int line,
                                               int column,
                                               const std::wstring &word);

        std::vector<Diagnostic> GetDiagnostics(const std::wstring &filePath) const;
        std::vector<CompletionItem> GetCompletions(const std::wstring &filePath,
                                                    const std::wstring &lineText,
                                                    int column) const;
        std::wstring GetProjectRoot() const;

    private:
        LspManager() = default;

        void StartProjectIndexAsync();
        void IndexFileSymbols(const std::wstring &filePath, const std::vector<std::wstring> &lines);
        std::vector<Diagnostic> AnalyzeDiagnostics(const std::wstring &filePath, const std::vector<std::wstring> &lines) const;
        std::optional<Location> ResolveIncludeAtCursor(const std::wstring &filePath,
                                                       const std::wstring &lineText,
                                                       int column) const;

        std::wstring projectRoot_;

        mutable std::mutex mutex_;
        std::unordered_map<std::wstring, std::vector<Diagnostic>> diagnostics_;
        std::unordered_map<std::wstring, std::vector<std::wstring>> fileSymbols_;
        std::unordered_map<std::wstring, std::unordered_map<std::wstring, std::vector<Location>>> fileSymbolDefLocs_;
        std::unordered_map<std::wstring, std::unordered_map<std::wstring, std::vector<Location>>> fileSymbolDeclLocs_;
        std::unordered_map<std::wstring, std::vector<std::wstring>> fileIncludes_;
        std::unordered_map<std::wstring, Location> symbolIndexDef_;
        std::unordered_map<std::wstring, Location> symbolIndexDecl_;
        std::unordered_map<std::wstring, DWORD> lastDiagTick_;
        bool indexing_ = false;
    };
}
