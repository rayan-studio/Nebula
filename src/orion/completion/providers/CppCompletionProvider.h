#pragma once
#include "../CompletionService.h"
#include <vector>
#include <string>

namespace Orion::Completion
{
    class CppCompletionProvider : public ICompletionProvider
    {
    public:
        CppCompletionProvider();
        bool CanProvide(const CompletionContext& ctx) override;
        std::vector<CompletionItem> GetCompletions(const CompletionContext& ctx) override;

    private:
        void BuildHeaderIndex();
        std::vector<std::wstring> GetIncludeSuggestions(const std::wstring& prefix) const;
        std::vector<std::wstring> headerIndex_;
    };
}
