#include "CppCompletionProvider.h"
#include <filesystem>
#include <algorithm>
#include <windows.h>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <sstream>
#include <cwctype>
#include "lsp/LspManager.h"

namespace Orion::Completion
{
    namespace
    {
        std::vector<std::wstring> g_headerIndex;
        std::mutex g_headerMutex;
        std::atomic<bool> g_headerReady{false};
        std::atomic<bool> g_headerLoading{false};

        static std::vector<int> ParseVersion(const std::wstring &v)
        {
            std::vector<int> out;
            std::wstringstream ss(v);
            std::wstring seg;
            while (std::getline(ss, seg, L'.'))
            {
                try
                {
                    out.push_back(std::stoi(seg));
                }
                catch (...)
                {
                    out.push_back(0);
                }
            }
            return out;
        }

        static bool VersionLess(const std::wstring &a, const std::wstring &b)
        {
            auto va = ParseVersion(a);
            auto vb = ParseVersion(b);
            size_t n = (std::max)(va.size(), vb.size());
            va.resize(n, 0);
            vb.resize(n, 0);
            for (size_t i = 0; i < n; ++i)
            {
                if (va[i] < vb[i]) return true;
                if (va[i] > vb[i]) return false;
            }
            return false;
        }

        static std::optional<std::filesystem::path> FindNewestSubdir(const std::filesystem::path &root)
        {
            std::error_code ec;
            if (!std::filesystem::exists(root, ec))
                return std::nullopt;
            std::filesystem::path best;
            std::wstring bestName;
            for (const auto &entry : std::filesystem::directory_iterator(root, ec))
            {
                if (!entry.is_directory(ec))
                    continue;
                std::wstring name = entry.path().filename().wstring();
                if (best.empty() || VersionLess(bestName, name))
                {
                    best = entry.path();
                    bestName = name;
                }
            }
            if (best.empty())
                return std::nullopt;
            return best;
        }

        static void AppendIncludeRoots(std::vector<std::filesystem::path> &roots)
        {
            std::wstring projectRoot = Lsp::LspManager::Instance().GetProjectRoot();
            if (!projectRoot.empty())
            {
                roots.push_back(projectRoot);
                roots.push_back(std::filesystem::path(projectRoot) / "src");
                roots.push_back(std::filesystem::path(projectRoot) / "include");
                roots.push_back(std::filesystem::path(projectRoot) / "external");
            }

            // MSVC include roots
            const std::filesystem::path vsBases[] = {
                L"C:\\Program Files\\Microsoft Visual Studio\\18\\Community",
                L"C:\\Program Files\\Microsoft Visual Studio\\18\\Professional",
                L"C:\\Program Files\\Microsoft Visual Studio\\18\\Enterprise",
                L"C:\\Program Files\\Microsoft Visual Studio\\18\\BuildTools",
                L"C:\\Program Files\\Microsoft Visual Studio\\2026\\Community",
                L"C:\\Program Files\\Microsoft Visual Studio\\2026\\Professional",
                L"C:\\Program Files\\Microsoft Visual Studio\\2026\\Enterprise",
                L"C:\\Program Files\\Microsoft Visual Studio\\2026\\BuildTools",
                L"C:\\Program Files\\Microsoft Visual Studio\\2022\\Community",
                L"C:\\Program Files\\Microsoft Visual Studio\\2022\\Professional",
                L"C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise",
                L"C:\\Program Files\\Microsoft Visual Studio\\2022\\BuildTools",
            };

            for (const auto &base : vsBases)
            {
                std::filesystem::path msvcRoot = base / "VC" / "Tools" / "MSVC";
                auto newest = FindNewestSubdir(msvcRoot);
                if (newest.has_value())
                {
                    std::filesystem::path inc = *newest / "include";
                    roots.push_back(inc);
                }
            }

            // Windows Kits include
            std::filesystem::path kitsRoot = L"C:\\Program Files (x86)\\Windows Kits\\10\\Include";
            auto newestKit = FindNewestSubdir(kitsRoot);
            if (newestKit.has_value())
            {
                roots.push_back(*newestKit / "ucrt");
                roots.push_back(*newestKit / "um");
                roots.push_back(*newestKit / "shared");
                roots.push_back(*newestKit / "winrt");
            }
        }

        static bool IsPlainHeaderName(const std::wstring &name)
        {
            if (name.empty())
                return false;
            for (wchar_t c : name)
            {
                if (!(iswalnum(c) || c == L'_' || c == L'-' || c == L'.' || c == L'/'))
                    return false;
            }
            return true;
        }

        static void AddHeadersFromRoot(const std::filesystem::path &root, std::vector<std::wstring> &out, size_t &count)
        {
            std::error_code ec;
            if (!std::filesystem::exists(root, ec))
                return;
            for (const auto &entry : std::filesystem::recursive_directory_iterator(root, ec))
            {
                if (ec)
                    break;
                if (!entry.is_regular_file(ec))
                    continue;
                auto ext = entry.path().extension().wstring();
                bool hasHeaderExt = (ext == L".h" || ext == L".hpp" || ext == L".hh" || ext == L".inc");
                bool hasNoExt = ext.empty();
                if (hasHeaderExt || hasNoExt)
                {
                    std::filesystem::path rel = std::filesystem::relative(entry.path(), root, ec);
                    std::wstring ws;
                    if (!ec)
                        ws = rel.generic_wstring();
                    else
                        ws = entry.path().filename().wstring();
                    if (hasHeaderExt || IsPlainHeaderName(ws))
                    {
                        out.push_back(ws);
                        count++;
                        if (count >= 8000)
                            return;
                    }
                }
            }
        }

        void BuildHeaderIndex(std::vector<std::wstring> &out)
        {
            out.clear();
            try {
                namespace fs = std::filesystem;
                std::vector<fs::path> roots;
                AppendIncludeRoots(roots);
                size_t count = 0;
                for (auto &r : roots)
                {
                    AddHeadersFromRoot(r, out, count);
                    if (count >= 8000)
                        break;
                }
                std::sort(out.begin(), out.end());
                out.erase(std::unique(out.begin(), out.end()), out.end());
            } catch (...) {
            }
        }

        void EnsureHeaderIndexAsync()
        {
            if (g_headerReady.load())
                return;

            bool expected = false;
            if (!g_headerLoading.compare_exchange_strong(expected, true))
                return;

            std::thread([]()
                        {
                            std::vector<std::wstring> local;
                            BuildHeaderIndex(local);
                            {
                                std::lock_guard<std::mutex> lk(g_headerMutex);
                                g_headerIndex = std::move(local);
                            }
                            g_headerReady.store(true);
                            g_headerLoading.store(false);
                        })
                .detach();
        }
    }

    CppCompletionProvider::CppCompletionProvider()
    {
        EnsureHeaderIndexAsync();
    }

    bool CppCompletionProvider::CanProvide(const CompletionContext& ctx)
    {
        auto ext = ctx.fileExt;
        return ext == L".c" || ext == L".cpp" || ext == L".h" || ext == L".hpp" || ext.empty();
    }

    std::vector<CompletionItem> CppCompletionProvider::GetCompletions(const CompletionContext& ctx)
    {
        if (ctx.line < 0 || ctx.line >= (int)ctx.lines.size())
            return {};
        const std::wstring& line = ctx.lines[ctx.line];
        size_t posInclude = line.rfind(L"#include", ctx.column);
        if (posInclude == std::wstring::npos)
            return {};
        size_t lt = line.find_last_of(L"<\"", ctx.column - 1);
        if (lt == std::wstring::npos || lt <= posInclude)
            return {};
        size_t start = lt + 1;
        size_t end = ctx.column;
        if (end < start) end = start;
        std::wstring prefix = line.substr(start, end - start);
        auto suggestions = GetIncludeSuggestions(prefix);
        std::vector<CompletionItem> items;
        for (const auto& s : suggestions)
            items.push_back({ s, s, false });
        return items;
    }

    std::vector<std::wstring> CppCompletionProvider::GetIncludeSuggestions(const std::wstring& prefix) const
    {
        EnsureHeaderIndexAsync();
        if (!g_headerReady.load())
            return {};

        std::vector<std::wstring> snapshot;
        {
            std::lock_guard<std::mutex> lk(g_headerMutex);
            snapshot = g_headerIndex;
        }

        std::vector<std::wstring> out;
        for (const auto& h : snapshot)
        {
            if (prefix.empty())
                out.push_back(h);
            else
            {
                std::wstring low = h;
                std::wstring lp = prefix;
                for (auto& c : low) c = towlower(c);
                for (auto& c : lp) c = towlower(c);
                if (low.rfind(lp, 0) == 0 || low.find(lp) != std::wstring::npos)
                    out.push_back(h);
            }
            if (out.size() >= 200) break;
        }
        return out;
    }
}
