#include "Editor.h"

#include <filesystem>
#include <regex>
#include <optional>
#include <sstream>
#include <thread>
#include <cstdio>
#include <cctype>
#include <algorithm>

namespace Orion
{
    namespace
    {
        std::string WideToUtf8(const std::wstring &w)
        {
            if (w.empty())
                return {};
            int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (len <= 1)
                return {};
            std::string out(static_cast<size_t>(len - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
            return out;
        }

        std::wstring Utf8ToWide(const std::string &s)
        {
            if (s.empty())
                return {};
            int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
            if (len <= 1)
                return {};
            std::wstring out(static_cast<size_t>(len - 1), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
            return out;
        }

        std::string ToLower(std::string value)
        {
            for (char &c : value)
                c = static_cast<char>(tolower(c));
            return value;
        }

        bool EndsWith(const std::string &value, const std::string &suffix)
        {
            if (suffix.size() > value.size())
                return false;
            return std::equal(suffix.rbegin(), suffix.rend(), value.rbegin());
        }

        bool SupportsClangdExtension(const std::wstring &ext)
        {
            return ext == L".c" || ext == L".cc" || ext == L".cpp" || ext == L".cxx" ||
                   ext == L".h" || ext == L".hpp" || ext == L".hh";
        }

        std::optional<std::filesystem::path> FindCompilationDatabase(const std::filesystem::path &filePath)
        {
            std::filesystem::path dir = filePath.parent_path();
            std::error_code ec;
            while (!dir.empty())
            {
                if (std::filesystem::exists(dir / "compile_commands.json", ec))
                    return dir;
                if (dir == dir.root_path())
                    break;
                dir = dir.parent_path();
            }
            return std::nullopt;
        }

        void InvalidateMainWindow()
        {
            HWND wnd = FindWindowW(L"NebulaTextWindowClass", NULL);
            if (wnd)
                InvalidateRect(wnd, nullptr, FALSE);
        }
    }

    void Editor::RunClangdDiagnosticsAsync()
    {
        if (!diagnosticsState_)
            return;

        if (state_.filePath.empty())
        {
            ClearDiagnostics();
            return;
        }

        std::wstring ext = GetFileExtension();
        if (!SupportsClangdExtension(ext))
        {
            ClearDiagnostics();
            return;
        }

        std::filesystem::path filePath(state_.filePath);
        auto compileDir = FindCompilationDatabase(filePath);
        if (!compileDir.has_value())
        {
            ClearDiagnostics();
            return;
        }

        auto diagnosticsState = diagnosticsState_;
        int token = ++diagnosticsState->token;
        std::string fileUtf8 = WideToUtf8(filePath.wstring());
        std::string compileDirUtf8 = WideToUtf8(compileDir->wstring());

        {
            std::lock_guard<std::mutex> lock(diagnosticsState->mutex);
            diagnosticsState->diagnostics.clear();
        }

        std::thread([diagnosticsState, token, fileUtf8, compileDirUtf8]()
                    {
                        std::vector<Diagnostic> parsed;

                        if (fileUtf8.empty() || compileDirUtf8.empty())
                            return;

                        std::string cmd = "clangd --check=\"" + fileUtf8 + "\" --compile-commands-dir=\"" + compileDirUtf8 + "\" 2>&1";
                        FILE *pipe = _popen(cmd.c_str(), "r");
                        if (!pipe)
                            return;

                        std::regex diagRegex(R"(^(.+):(\d+):(\d+):\s*(error|warning|note):\s*(.*)$)");
                        std::string line;
                        char buffer[2048];
                        while (fgets(buffer, sizeof(buffer), pipe))
                        {
                            line.assign(buffer);
                            if (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                                line.erase(line.find_last_not_of("\r\n") + 1);

                            std::smatch match;
                            if (!std::regex_match(line, match, diagRegex))
                                continue;

                            std::string path = ToLower(match[1].str());
                            std::string fileLower = ToLower(fileUtf8);
                            if (!(path == fileLower || EndsWith(path, fileLower)))
                                continue;

                            int lineNumber = std::max(0, std::stoi(match[2].str()) - 1);
                            int column = std::max(0, std::stoi(match[3].str()) - 1);
                            std::string severity = match[4].str();

                            Diagnostic diag;
                            diag.line = lineNumber;
                            diag.startCol = column;
                            diag.endCol = column + 1;
                            diag.isError = (severity == "error");
                            diag.message = Utf8ToWide(match[5].str());
                            parsed.push_back(std::move(diag));
                        }

                        _pclose(pipe);

                        if (diagnosticsState->token.load() != token)
                            return;

                        {
                            std::lock_guard<std::mutex> lock(diagnosticsState->mutex);
                            diagnosticsState->diagnostics = std::move(parsed);
                        }

                        InvalidateMainWindow();
                    })
            .detach();
    }

    std::vector<Editor::Diagnostic> Editor::GetDiagnostics() const
    {
        if (!diagnosticsState_)
            return {};
        std::lock_guard<std::mutex> lock(diagnosticsState_->mutex);
        return diagnosticsState_->diagnostics;
    }

    void Editor::ClearDiagnostics()
    {
        if (!diagnosticsState_)
            return;
        std::lock_guard<std::mutex> lock(diagnosticsState_->mutex);
        diagnosticsState_->diagnostics.clear();
    }

    void Editor::SetDiagnostics(const std::vector<Lsp::Diagnostic> &diags)
    {
        if (!diagnosticsState_)
            return;

        std::vector<Diagnostic> converted;
        converted.reserve(diags.size());
        for (const auto &d : diags)
        {
            Diagnostic out;
            out.line = d.line;
            out.startCol = d.startCol;
            out.endCol = d.endCol;
            out.isError = (d.severity == Lsp::DiagnosticSeverity::Error);
            out.message = d.message;
            out.suggestion = d.suggestion;
            converted.push_back(std::move(out));
        }

        std::lock_guard<std::mutex> lock(diagnosticsState_->mutex);
        diagnosticsState_->diagnostics = std::move(converted);
    }
}
