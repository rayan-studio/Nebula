#include "CppCompletionProvider.h"
#include <filesystem>
#include <algorithm>
#include <windows.h>
#include <string>

namespace Orion::Completion
{
    CppCompletionProvider::CppCompletionProvider()
    {
        BuildHeaderIndex();
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

    void CppCompletionProvider::BuildHeaderIndex()
    {
        headerIndex_.clear();
        try {
            namespace fs = std::filesystem;
            std::vector<fs::path> roots = { fs::path("src"), fs::path("external") };
            for (auto& r : roots)
            {
                if (!fs::exists(r)) continue;
                for (auto& p : fs::recursive_directory_iterator(r))
                {
                    if (!p.is_regular_file()) continue;
                    auto ext = p.path().extension().wstring();
                    if (ext == L".h" || ext == L".hpp" || ext == L".hh" || ext == L".inc")
                    {
                        try {
                            fs::path rel = fs::relative(p.path(), fs::current_path());
                            std::string s = rel.generic_string();
                            int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
                            std::wstring ws(wlen, L'\0');
                            if (wlen > 0)
                                MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), ws.data(), wlen);
                            headerIndex_.push_back(ws);
                        } catch (...) {
                            headerIndex_.push_back(p.path().filename().wstring());
                        }
                    }
                }
            }
            std::sort(headerIndex_.begin(), headerIndex_.end());
            headerIndex_.erase(std::unique(headerIndex_.begin(), headerIndex_.end()), headerIndex_.end());
        } catch (...) {
        }
    }

    std::vector<std::wstring> CppCompletionProvider::GetIncludeSuggestions(const std::wstring& prefix) const
    {
        std::vector<std::wstring> out;
        for (const auto& h : headerIndex_)
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
