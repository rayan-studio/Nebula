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
#include "ui/theme/Theme.h"
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

    void Editor::ApplyUiTheme(bool lightMode)
    {
        const UI::Theme::Palette &palette = UI::Theme::GetPalette();

        theme_.background = UI::Theme::ChromeBackground();
        theme_.gutterBackground = theme_.background;
        theme_.text = palette.inputText;
        theme_.caret = palette.inputCaret;
        theme_.lineNumberText = UI::Theme::MutedText();
        theme_.activeLineNumberText = theme_.text;
        theme_.selection = palette.inputSelection;

        if (lightMode)
        {
            theme_.activeLineBackground = D2D1::ColorF(0.88f, 0.92f, 0.98f, 1.0f);

            theme_.keyword = D2D1::ColorF(0.16f, 0.34f, 0.74f, 1.0f);
            theme_.string = D2D1::ColorF(0.66f, 0.28f, 0.12f, 1.0f);
            theme_.comment = D2D1::ColorF(0.22f, 0.50f, 0.29f, 1.0f);
            theme_.number = D2D1::ColorF(0.57f, 0.38f, 0.10f, 1.0f);
            theme_.function = D2D1::ColorF(0.47f, 0.22f, 0.68f, 1.0f);
            theme_.type = D2D1::ColorF(0.06f, 0.49f, 0.64f, 1.0f);
            theme_.operator_ = D2D1::ColorF(0.20f, 0.24f, 0.30f, 1.0f);
            theme_.variable = D2D1::ColorF(0.09f, 0.31f, 0.61f, 1.0f);
        }
        else
        {
            // Match JetBrains expUI dark editor palette more closely.
            theme_.background = D2D1::ColorF(24.0f / 255.0f, 26.0f / 255.0f, 29.0f / 255.0f, 1.0f);
            theme_.gutterBackground = theme_.background;
            theme_.text = D2D1::ColorF(188.0f / 255.0f, 190.0f / 255.0f, 196.0f / 255.0f, 1.0f);             // bcbec4
            theme_.caret = D2D1::ColorF(206.0f / 255.0f, 208.0f / 255.0f, 214.0f / 255.0f, 1.0f);            // ced0d6
            theme_.lineNumberText = D2D1::ColorF(75.0f / 255.0f, 80.0f / 255.0f, 89.0f / 255.0f, 1.0f);      // 4b5059
            theme_.activeLineNumberText = D2D1::ColorF(161.0f / 255.0f, 163.0f / 255.0f, 171.0f / 255.0f, 1.0f); // a1a3ab
            theme_.selection = D2D1::ColorF(53.0f / 255.0f, 83.0f / 255.0f, 143.0f / 255.0f, 0.55f);         // 35538f
            theme_.activeLineBackground = D2D1::ColorF(38.0f / 255.0f, 40.0f / 255.0f, 46.0f / 255.0f, 1.0f); // 26282e

            theme_.keyword = D2D1::ColorF(207.0f / 255.0f, 142.0f / 255.0f, 109.0f / 255.0f, 1.0f);          // cf8e6d
            theme_.string = D2D1::ColorF(106.0f / 255.0f, 171.0f / 255.0f, 115.0f / 255.0f, 1.0f);           // 6aab73
            theme_.comment = D2D1::ColorF(122.0f / 255.0f, 126.0f / 255.0f, 133.0f / 255.0f, 1.0f);          // 7a7e85
            theme_.number = D2D1::ColorF(42.0f / 255.0f, 172.0f / 255.0f, 184.0f / 255.0f, 1.0f);            // 2aacb8
            theme_.function = D2D1::ColorF(86.0f / 255.0f, 168.0f / 255.0f, 245.0f / 255.0f, 1.0f);          // 56a8f5
            theme_.type = D2D1::ColorF(86.0f / 255.0f, 168.0f / 255.0f, 245.0f / 255.0f, 1.0f);              // inferred from JetBrains C++ dark rendering
            theme_.operator_ = D2D1::ColorF(188.0f / 255.0f, 190.0f / 255.0f, 196.0f / 255.0f, 1.0f);        // bcbec4
            theme_.variable = D2D1::ColorF(188.0f / 255.0f, 190.0f / 255.0f, 196.0f / 255.0f, 1.0f);         // bcbec4
        }
    }

    void Editor::SyncThemeFromUi()
    {
        const bool isLight = (UI::Theme::GetMode() == UI::Theme::Mode::Light);
        if (appliedUiThemeInitialized_ && appliedUiThemeIsLight_ == isLight)
            return;

        ApplyUiTheme(isLight);
        appliedUiThemeIsLight_ = isLight;
        appliedUiThemeInitialized_ = true;

        Rendering::SelectionConfig selectionConfig;
        selectionConfig.color = theme_.selection;
        selectionConfig.cornerRadius = 3.0f;
        selectionConfig.style = Rendering::SelectionStyle::RoundedSmart;
        selection_ = std::make_unique<Rendering::Selection>(selectionConfig);
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

        const bool isLight = (UI::Theme::GetMode() == UI::Theme::Mode::Light);
        ApplyUiTheme(isLight);
        appliedUiThemeInitialized_ = true;
        appliedUiThemeIsLight_ = isLight;

        Rendering::GuideStyle guideStyle;
        guideStyle.normalColor = D2D1::ColorF(theme_.lineNumberText.r, theme_.lineNumberText.g, theme_.lineNumberText.b, 0.32f);
        guideStyle.activeColor = D2D1::ColorF(theme_.text.r, theme_.text.g, theme_.text.b, 0.42f);
        guideStyle.lineWidth = 0.75f;
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
        markdownViewMode_ = MarkdownViewMode::Code;
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
        if (!isGitSplitDiffView_ && markdownViewMode_ != MarkdownViewMode::Split)
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
