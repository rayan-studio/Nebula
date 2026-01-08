#pragma once
#include <vector>
#include <string>
#include <memory>

namespace Orion::Completion
{
    struct CompletionItem
    {
        std::wstring label;
        std::wstring insertText;
        bool isSnippet = false;
    };

    struct CompletionContext
    {
        const std::wstring& filePath;
        const std::vector<std::wstring>& lines;
        int line;
        int column;
        std::wstring fileExt;
    };

    class ICompletionProvider
    {
    public:
        virtual ~ICompletionProvider() = default;
        virtual bool CanProvide(const CompletionContext& ctx) = 0;
        virtual std::vector<CompletionItem> GetCompletions(const CompletionContext& ctx) = 0;
    };

    class CompletionService
    {
    public:
        void RegisterProvider(std::unique_ptr<ICompletionProvider> provider);
        std::vector<CompletionItem> GetCompletions(const CompletionContext& ctx);

    private:
        std::vector<std::unique_ptr<ICompletionProvider>> providers_;
    };
}
