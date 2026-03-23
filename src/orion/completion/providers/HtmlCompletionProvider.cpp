#include "HtmlCompletionProvider.h"
#include <algorithm>

namespace Orion::Completion
{
    bool HtmlCompletionProvider::CanProvide(const CompletionContext& ctx)
    {
        return ctx.fileExt == L".html" || ctx.fileExt == L".htm";
    }

    std::vector<CompletionItem> HtmlCompletionProvider::GetCompletions(const CompletionContext& ctx)
    {
        if (ctx.line < 0 || ctx.line >= (int)ctx.lines.size())
            return {};
        const std::wstring& line = ctx.lines[ctx.line];
        auto parsed = ParseLineContext(line, ctx.column);
        std::vector<CompletionItem> items;
        if (parsed.afterExclamation)
        {
            items.push_back(GetHtml5Boilerplate());
            return items;
        }
        if (parsed.afterLessThan)
        {
            items = GetTagCompletions(parsed.prefix);
        }
        return items;
    }

    HtmlCompletionProvider::ParsedContext HtmlCompletionProvider::ParseLineContext(const std::wstring& line, int column)
    {
        ParsedContext result;
        int lt = column - 1;
        while (lt >= 0 && line[lt] != L'<')
            lt--;
        if (lt >= 0)
        {
            result.afterLessThan = true;
            int start = lt + 1;
            if (start < (int)line.size() && line[start] == L'/')
                start++;
            if (column > start)
                result.prefix = line.substr(start, column - start);
        }
        if (column > 0 && line[column - 1] == L'!')
        {
            result.afterExclamation = true;
        }
        return result;
    }

    std::vector<CompletionItem> HtmlCompletionProvider::GetTagCompletions(const std::wstring& prefix)
    {
        static const std::vector<std::wstring> htmlTags = {
            L"html", L"head", L"body", L"div", L"span", L"p", L"a", L"img",
            L"script", L"style", L"link", L"meta", L"title", L"h1", L"h2",
            L"h3", L"ul", L"ol", L"li", L"section", L"article", L"nav",
            L"header", L"footer", L"main", L"form", L"input", L"button",
            L"label", L"select", L"option", L"textarea", L"table", L"tr",
            L"td", L"th", L"thead", L"tbody", L"iframe", L"canvas", L"svg",
            L"figure", L"figcaption", L"blockquote", L"pre", L"code",
            L"br", L"hr"
        };
        std::vector<CompletionItem> items;
        std::wstring lowerPrefix = prefix;
        for (auto& c : lowerPrefix) c = towlower(c);
        for (const auto& tag : htmlTags)
        {
            if (prefix.empty() || tag.rfind(lowerPrefix, 0) == 0)
            {
                items.push_back({ tag, tag, L"html", false });
                if (items.size() >= 200) break;
            }
        }
        return items;
    }

    CompletionItem HtmlCompletionProvider::GetHtml5Boilerplate()
    {
        return { L"HTML5 boilerplate",
                 L"<!DOCTYPE html>\n<html>\n<head>\n  <meta charset=\"utf-8\">\n  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n  <title>Document</title>\n</head>\n<body>\n\n</body>\n</html>",
                 L"html", true };
    }
}
