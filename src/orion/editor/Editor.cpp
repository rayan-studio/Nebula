#include "Editor.h"

#include "../syntax/Highlighter.h"
#include "../completion/popup/Popup.h"

#include "../completion/CompletionService.h"
#include "../completion/providers/HtmlCompletionProvider.h"
#include "../completion/providers/CppCompletionProvider.h"

#include "orion/geometry/IndentationHelper.h"
#include "orion/rendering/GuideRenderer.h"
#include "orion/selection/Selection.h"
#include "orion/caret/Caret.h"

// Ensure Windows min/max macros don't interfere with std::min/std::max
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

namespace Orion
{
    Editor::Editor()
    {
        state_.lastBlinkTime = GetTickCount();
        highlighter_ = new ::Orion::Syntax::Highlighter();
        cachedTextFormat_ = nullptr;
        completionPopup_ = new CompletionPopup();

        // completion service
        completionService_ = std::make_unique<Completion::CompletionService>();
        completionService_->RegisterProvider(std::make_unique<Completion::HtmlCompletionProvider>());
        completionService_->RegisterProvider(std::make_unique<Completion::CppCompletionProvider>());

        Geometry::IndentConfig indentConfig = Geometry::IndentConfig{4, 8.0f};
        indentHelper_ = std::make_unique<Geometry::IndentationHelper>(indentConfig);

        Rendering::GuideStyle guideStyle;
        guideStyle.normalColor = D2D1::ColorF(0.3f, 0.3f, 0.35f, 0.5f);
        guideStyle.activeColor = D2D1::ColorF(0.4f, 0.4f, 0.5f, 0.7f);
        guideRenderer_ = std::make_unique<Rendering::GuideRenderer>(indentConfig, guideStyle);

        Rendering::SelectionConfig selectionConfig;
        selectionConfig.color = theme_.selection;
        selectionConfig.cornerRadius = 3.0f;
        selectionConfig.style = Rendering::SelectionStyle::RoundedSmart;
        selection_ = std::make_unique<Rendering::Selection>(selectionConfig);

        diagnosticsState_ = std::make_shared<DiagnosticsState>();
    }

    bool Editor::UpdateCaretBlink()
    {
        bool wasVisible = state_.caretVisible;
        Caret::UpdateCaretBlink(state_);
        return wasVisible != state_.caretVisible;
    }

    Editor::~Editor()
    {
        if (customFontCollection_)
        {
            customFontCollection_->Release();
            customFontCollection_ = nullptr;
        }

        if (fontLoader_)
        {
            if (fontCollectionRegisteredFactory_)
            {
                fontCollectionRegisteredFactory_->UnregisterFontCollectionLoader(fontLoader_);
                fontCollectionRegisteredFactory_->Release();
                fontCollectionRegisteredFactory_ = nullptr;
            }
            fontLoader_->Release();
            fontLoader_ = nullptr;
        }

        if (highlighter_)
        {
            delete highlighter_;
            highlighter_ = nullptr;
        }

        if (cachedTextFormat_)
        {
            cachedTextFormat_->Release();
            cachedTextFormat_ = nullptr;
        }

        if (completionPopup_)
        {
            delete completionPopup_;
            completionPopup_ = nullptr;
        }

        // Release any cached brushes used for syntax highlighting
        for (auto &p : brushCache_)
        {
            if (p.second)
                p.second->Release();
        }
        brushCache_.clear();
    }
}
