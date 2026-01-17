#include "../CompletionService.h"

namespace Orion::Completion
{
    void CompletionService::RegisterProvider(std::unique_ptr<ICompletionProvider> provider)
    {
        providers_.push_back(std::move(provider));
    }

    std::vector<CompletionItem> CompletionService::GetCompletions(const CompletionContext& ctx)
    {
        std::vector<CompletionItem> results;
        for (auto& provider : providers_)
        {
            if (!provider->CanProvide(ctx))
                continue;
            auto items = provider->GetCompletions(ctx);
            if (!items.empty())
            {
                // If first item is a snippet, prefer it and return immediately
                if (items.size() > 0 && items[0].isSnippet)
                {
                    return items;
                }
            }
            results.insert(results.end(), items.begin(), items.end());
        }
        return results;
    }
}
