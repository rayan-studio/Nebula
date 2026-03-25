#include "Editor.h"

#include <filesystem>
#include <regex>
#include <optional>
#include <sstream>
#include <thread>
#include <cstdio>
#include <cctype>
#include <algorithm>

// Ensure Windows min/max macros don't interfere with std::min/std::max
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

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
            // Common build subdirectory names (CLion uses cmake-build-*, VS uses out/, etc.)
            static const std::vector<std::wstring> kBuildDirs = {
                L"cmake-build-debug", L"cmake-build-release",
                L"cmake-build-relwithdebinfo", L"cmake-build-minsizerel",
                L"build", L"Build", L"out", L"Out", L"_build", L".build",
                L".clangd",
                L"x64", L"x86",
            };

            std::filesystem::path dir = filePath.parent_path();
            std::error_code ec;
            while (!dir.empty())
            {
                // Check directly in this directory
                if (std::filesystem::exists(dir / "compile_commands.json", ec))
                    return dir;

                // Check in common build subdirectories
                for (const auto &bd : kBuildDirs)
                {
                    std::filesystem::path candidate = dir / bd / "compile_commands.json";
                    if (std::filesystem::exists(candidate, ec))
                        return dir / bd;
                }

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
            out.isHint  = (d.severity == Lsp::DiagnosticSeverity::Info);
            out.message = d.message;
            out.suggestion = d.suggestion;
            converted.push_back(std::move(out));
        }

        std::lock_guard<std::mutex> lock(diagnosticsState_->mutex);
        diagnosticsState_->diagnostics = std::move(converted);
    }
}
