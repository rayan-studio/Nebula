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
#include <cmath>

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
        constexpr float kGitSplitMinPaneWidth = 140.0f;
        constexpr float kGitSplitDividerHitHalfWidth = 5.0f;
    }

    static bool CaretPosLess(const CaretPosition &a, const CaretPosition &b)
    {
        if (a.line != b.line)
            return a.line < b.line;
        return a.column < b.column;
    }

    static bool CaretPosEqual(const CaretPosition &a, const CaretPosition &b)
    {
        return a.line == b.line && a.column == b.column;
    }

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
        if (isGitSplitDiffView_)
        {
            state_.caretVisible = false;
            return false;
        }
        bool wasVisible = state_.caretVisible;
        Caret::UpdateCaretBlink(state_);
        return wasVisible != state_.caretVisible;
    }

    void Editor::SetGitSplitDiffView(const std::vector<GitSplitDiffRow> &rows)
    {
        gitSplitDiffRows_ = rows;
        isGitSplitDiffView_ = !gitSplitDiffRows_.empty();
        state_.hasSelection = false;
        secondaryCarets_.clear();
        dragSelecting_ = false;
        gitSplitDividerDragging_ = false;
        gitSplitDividerRatio_ = 0.5f;
        state_.caretVisible = !isGitSplitDiffView_;
        state_.scrollOffsetX = 0.0f;
        pendingRevealCaret_ = false;
        if (isGitSplitDiffView_)
        {
            scrollbar_.SetScrollOffset(0.0f);
            state_.scrollOffsetY = scrollbar_.GetScrollOffset();
        }
    }

    void Editor::ClearGitSplitDiffView()
    {
        isGitSplitDiffView_ = false;
        gitSplitDiffRows_.clear();
        gitSplitDividerDragging_ = false;
        state_.caretVisible = true;
    }

    float Editor::GetGitSplitContentRight() const
    {
        return state_.rightEdge - (scrollbar_.IsVisible() ? 14.0f : 0.0f);
    }

    float Editor::GetGitSplitDividerX() const
    {
        const float contentLeft = state_.leftEdge;
        const float contentRight = GetGitSplitContentRight();
        const float fullWidth = contentRight - contentLeft;
        if (fullWidth <= 0.0f)
            return contentLeft;

        const float minX = contentLeft + kGitSplitMinPaneWidth;
        const float maxX = contentRight - kGitSplitMinPaneWidth;
        const float ratio = (std::max)(0.1f, (std::min)(0.9f, gitSplitDividerRatio_));
        const float rawX = contentLeft + (fullWidth * ratio);
        if (maxX <= minX)
            return contentLeft + fullWidth * 0.5f;
        return (std::max)(minX, (std::min)(maxX, rawX));
    }

    bool Editor::IsPointOnGitSplitDivider(POINT pt) const
    {
        if (!isGitSplitDiffView_)
            return false;
        if (pt.y < (int)state_.topEdge || pt.y > (int)state_.bottomEdge)
            return false;
        const float dividerX = GetGitSplitDividerX();
        return std::fabs((float)pt.x - dividerX) <= kGitSplitDividerHitHalfWidth;
    }

    void Editor::NormalizeSecondaryCarets()
    {
        if (state_.lines.empty())
        {
            secondaryCarets_.clear();
            return;
        }

        for (auto &c : secondaryCarets_)
        {
            c.line = (std::max)(0, (std::min)(c.line, (int)state_.lines.size() - 1));
            int maxCol = (int)state_.lines[(size_t)c.line].size();
            c.column = (std::max)(0, (std::min)(c.column, maxCol));
        }

        std::sort(secondaryCarets_.begin(), secondaryCarets_.end(), CaretPosLess);
        secondaryCarets_.erase(std::unique(secondaryCarets_.begin(), secondaryCarets_.end(), CaretPosEqual), secondaryCarets_.end());

        secondaryCarets_.erase(
            std::remove_if(
                secondaryCarets_.begin(),
                secondaryCarets_.end(),
                [this](const CaretPosition &c)
                {
                    return c.line == state_.caret.line && c.column == state_.caret.column;
                }),
            secondaryCarets_.end());
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

        if (previewD2DBitmap_)
        {
            previewD2DBitmap_->Release();
            previewD2DBitmap_ = nullptr;
        }

        if (previewBitmap_)
        {
            DeleteObject(previewBitmap_);
            previewBitmap_ = nullptr;
        }
    }
}
