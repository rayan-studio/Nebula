#pragma once
#include "../CompletionService.h"

namespace Orion::Completion
{
    class HtmlCompletionProvider : public ICompletionProvider
    {
    public:
        bool CanProvide(const CompletionContext& ctx) override;
        std::vector<CompletionItem> GetCompletions(const CompletionContext& ctx) override;

    private:
        struct ParsedContext
        {
            bool afterLessThan = false;
            bool afterExclamation = false;
            std::wstring prefix;
        };

        ParsedContext ParseLineContext(const std::wstring& line, int column);
        std::vector<CompletionItem> GetTagCompletions(const std::wstring& prefix);
        CompletionItem GetHtml5Boilerplate();
    };
}
