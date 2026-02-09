#include "./SearchBox.h"
#include "ui/components/input/InputTheme.h"
#include <algorithm>
#include <cmath>
#include <regex>

namespace Orion
{
    namespace
    {
        static bool PointInRect(const D2D1_RECT_F &r, POINT pt)
        {
            return pt.x >= r.left && pt.x <= r.right && pt.y >= r.top && pt.y <= r.bottom;
        }
    }

    SearchBox::SearchBox()
        : visible_(false)
        , inputFocused_(true)
        , replaceFocused_(false)
        , activeField_(InputField::Search)
        , caretPosition_(0)
        , replaceCaretPosition_(0)
        , caretVisible_(true)
        , lastBlinkTime_(GetTickCount())
        , replaceRequested_(false)
        , currentMatchIndex_(-1)
        , hoverClose_(false)
        , hoverPrev_(false)
        , hoverNext_(false)
        , hoverCase_(false)
        , hoverWord_(false)
        , hoverRegex_(false)
    {
        auto styleInput = [](TextInput &input, const std::wstring &placeholder)
        {
            input.SetPlaceholder(placeholder);
            auto &style = input.GetStyle();
            style.useSearchBoxStyle = false;
            style.backgroundColor = UI::InputTheme::Background();
            style.borderColor = UI::InputTheme::Border();
            style.focusBorderColor = UI::InputTheme::FocusBorder();
            style.textColor = UI::InputTheme::Text();
            style.placeholderColor = UI::InputTheme::Placeholder();
            style.selectionColor = UI::InputTheme::Selection();
            style.cursorColor = UI::InputTheme::Caret();
            style.cornerRadius = UI::InputTheme::kCornerRadius;
            style.fontFamily = UI::InputTheme::kFontFamily;
            style.fontSize = UI::InputTheme::kFontSize;
            style.padding = UI::InputTheme::kHorizontalPadding;
        };

        styleInput(searchInput_, L"Rechercher...");
        styleInput(replaceInput_, L"Remplacer...");
        searchInput_.SetIcon(L"\uE721");
        searchInput_.SetIconFont(L"Segoe Fluent Icons");

        searchInput_.onTextChanged = [this](const std::wstring &text)
        {
            searchText_ = text;
            caretPosition_ = (int)searchText_.size();
        };
        replaceInput_.onTextChanged = [this](const std::wstring &text)
        {
            replaceText_ = text;
            replaceCaretPosition_ = (int)replaceText_.size();
        };

        searchInput_.onEscape = [this]()
        {
            Hide();
        };
        replaceInput_.onEscape = [this]()
        {
            Hide();
        };
        replaceInput_.onSubmit = [this]()
        {
            replaceRequested_ = true;
        };
    }

    SearchBox::~SearchBox()
    {
    }

    void SearchBox::Show()
    {
        visible_ = true;
        replaceRequested_ = false;
        searchInput_.SetText(searchText_);
        replaceInput_.SetText(replaceText_);
        SetInputFocused(true);
    }

    void SearchBox::Hide()
    {
        visible_ = false;
        inputFocused_ = false;
        replaceFocused_ = false;
        activeField_ = InputField::None;
        searchInput_.SetFocused(false);
        replaceInput_.SetFocused(false);
        ClearMatches();
    }

    void SearchBox::UpdateLayout(float editorLeft, float editorTop, float editorWidth)
    {
        const float boxWidth = 460.0f;
        const float boxHeight = 96.0f;
        const float padding = 8.0f;
        const float rowHeight = 32.0f;
        const float rowSpacing = 8.0f;
        const float innerPadding = 10.0f;

        boxRect_ = D2D1::RectF(
            editorLeft + editorWidth - boxWidth - padding,
            editorTop + padding,
            editorLeft + editorWidth - padding,
            editorTop + padding + boxHeight);

        inputRect_ = D2D1::RectF(
            boxRect_.left + innerPadding,
            boxRect_.top + innerPadding,
            boxRect_.right - innerPadding,
            boxRect_.top + innerPadding + rowHeight);

        const float replaceButtonWidth = 30.0f;
        const float replaceButtonGap = 6.0f;

        replaceInputRect_ = D2D1::RectF(
            boxRect_.left + innerPadding,
            inputRect_.bottom + rowSpacing,
            boxRect_.right - innerPadding - replaceButtonGap - replaceButtonWidth,
            inputRect_.bottom + rowSpacing + rowHeight);

        replaceButtonRect_ = D2D1::RectF(
            replaceInputRect_.right + replaceButtonGap,
            replaceInputRect_.top,
            replaceInputRect_.right + replaceButtonGap + replaceButtonWidth,
            replaceInputRect_.bottom);

        searchInput_.SetRect(inputRect_);
        replaceInput_.SetRect(replaceInputRect_);
    }

    void SearchBox::Draw(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite)
    {
        if (!visible_)
            return;

        D2D1_RECT_F panelRect = D2D1::RectF(
            std::round(boxRect_.left),
            std::round(boxRect_.top),
            std::round(boxRect_.right),
            std::round(boxRect_.bottom));

        ID2D1SolidColorBrush *panelBg = nullptr;
        ID2D1SolidColorBrush *panelBorder = nullptr;
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.07f, 0.07f, 0.07f, 0.98f), &panelBg);
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.19f, 0.19f, 0.19f, 1.0f), &panelBorder);

        const float panelRadius = 8.0f;
        if (panelBg)
        {
            ctx->FillRoundedRectangle(D2D1::RoundedRect(panelRect, panelRadius, panelRadius), panelBg);
            panelBg->Release();
        }
        if (panelBorder)
        {
            D2D1_RECT_F borderRect = D2D1::RectF(
                panelRect.left + 0.5f,
                panelRect.top + 0.5f,
                panelRect.right - 0.5f,
                panelRect.bottom - 0.5f);
            ctx->DrawRoundedRectangle(D2D1::RoundedRect(borderRect, panelRadius - 0.5f, panelRadius - 0.5f), panelBorder, 1.0f);
            panelBorder->Release();
        }

        searchInput_.Draw(ctx, dwrite);
        replaceInput_.Draw(ctx, dwrite);

        if (!searchText_.empty())
        {
            const int total = (int)matches_.size();
            const int current = (total > 0 && currentMatchIndex_ >= 0) ? (currentMatchIndex_ + 1) : 0;
            wchar_t buf[32];
            swprintf_s(buf, L"%d/%d", current, total);

            IDWriteTextFormat *countFormat = nullptr;
            dwrite->CreateTextFormat(
                UI::InputTheme::kFontFamily,
                nullptr,
                DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                11.0f,
                L"en-us",
                &countFormat);
            if (countFormat)
            {
                countFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                countFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

                ID2D1SolidColorBrush *countBrush = nullptr;
                ctx->CreateSolidColorBrush(UI::InputTheme::Placeholder(), &countBrush);
                if (countBrush)
                {
                    D2D1_RECT_F countRect = inputRect_;
                    countRect.right -= UI::InputTheme::kHorizontalPadding;
                    countRect.left = countRect.right - 88.0f;
                    ctx->DrawTextW(buf, (UINT32)wcslen(buf), countFormat, countRect, countBrush);
                    countBrush->Release();
                }
                countFormat->Release();
            }
        }

        DrawButton(ctx, dwrite, replaceButtonRect_, L"\u2192", false, hoverReplaceButton_);
    }

    void SearchBox::DrawButton(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite,
                               const D2D1_RECT_F &rect, const wchar_t *icon,
                               bool active, bool hovered)
    {
        const float radius = UI::InputTheme::kCornerRadius;

        ID2D1SolidColorBrush *bgBrush = nullptr;
        D2D1_COLOR_F bgColor;
        if (active)
            bgColor = UI::InputTheme::FocusBorder();
        else if (hovered)
            bgColor = D2D1::ColorF(0.20f, 0.20f, 0.20f);
        else
            bgColor = D2D1::ColorF(0.14f, 0.14f, 0.14f);

        ctx->CreateSolidColorBrush(bgColor, &bgBrush);
        if (bgBrush)
        {
            ctx->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), bgBrush);
            bgBrush->Release();
        }

        ID2D1SolidColorBrush *borderBrush = nullptr;
        ctx->CreateSolidColorBrush(active ? UI::InputTheme::FocusBorder() : UI::InputTheme::Border(), &borderBrush);
        if (borderBrush)
        {
            ctx->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), borderBrush, 1.0f);
            borderBrush->Release();
        }

        IDWriteTextFormat *iconFormat = nullptr;
        dwrite->CreateTextFormat(
            L"Segoe UI Symbol",
            nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            12.0f,
            L"en-us",
            &iconFormat);
        if (iconFormat)
        {
            iconFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            iconFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

            ID2D1SolidColorBrush *iconBrush = nullptr;
            ctx->CreateSolidColorBrush(active ? UI::InputTheme::Text() : D2D1::ColorF(0.74f, 0.74f, 0.74f), &iconBrush);
            if (iconBrush)
            {
                ctx->DrawTextW(icon, (UINT32)wcslen(icon), iconFormat, rect, iconBrush);
                iconBrush->Release();
            }
            iconFormat->Release();
        }
    }

    void SearchBox::OnChar(wchar_t ch)
    {
        if (!visible_ || !IsInputFocused())
            return;

        if (activeField_ == InputField::Replace)
            replaceInput_.OnChar(ch);
        else
            searchInput_.OnChar(ch);

        searchText_ = searchInput_.GetText();
        replaceText_ = replaceInput_.GetText();
        caretPosition_ = (int)searchText_.size();
        replaceCaretPosition_ = (int)replaceText_.size();
    }

    void SearchBox::OnKeyDown(WPARAM key)
    {
        if (!visible_ || !IsInputFocused())
            return;

        if (key == VK_TAB)
        {
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
                SetInputFocused(true);
            else
                SetReplaceFocused(true);
            return;
        }

        if (key == VK_ESCAPE)
        {
            Hide();
            return;
        }

        if (activeField_ == InputField::Replace)
            replaceInput_.OnKeyDown(key);
        else
            searchInput_.OnKeyDown(key);

        searchText_ = searchInput_.GetText();
        replaceText_ = replaceInput_.GetText();
        caretPosition_ = (int)searchText_.size();
        replaceCaretPosition_ = (int)replaceText_.size();
    }

    void SearchBox::OnLeftButtonDown(HWND hwnd, POINT pt)
    {
        if (!visible_)
            return;

        hoverReplaceButton_ = PointInRect(replaceButtonRect_, pt);
        if (hoverReplaceButton_)
        {
            SetReplaceFocused(true);
            replaceRequested_ = true;
            return;
        }

        if (searchInput_.HitTest(pt))
        {
            SetInputFocused(true);
            searchInput_.OnLeftButtonDown(hwnd, pt);
            return;
        }

        if (replaceInput_.HitTest(pt))
        {
            SetReplaceFocused(true);
            replaceInput_.OnLeftButtonDown(hwnd, pt);
            return;
        }

        if (!PointInRect(boxRect_, pt))
        {
            searchInput_.SetFocused(false);
            replaceInput_.SetFocused(false);
            inputFocused_ = false;
            replaceFocused_ = false;
            activeField_ = InputField::None;
        }
    }

    bool SearchBox::OnMouseMove(HWND hwnd, POINT pt)
    {
        if (!visible_)
            return false;

        bool changed = false;
        bool prevHoverReplace = hoverReplaceButton_;
        hoverReplaceButton_ = PointInRect(replaceButtonRect_, pt);
        if (prevHoverReplace != hoverReplaceButton_)
            changed = true;

        if (searchInput_.OnMouseMove(hwnd, pt))
            changed = true;
        if (replaceInput_.OnMouseMove(hwnd, pt))
            changed = true;

        return changed;
    }

    bool SearchBox::OnLeftButtonUp(HWND hwnd, POINT pt)
    {
        if (!visible_)
            return false;

        bool consumed = false;
        if (searchInput_.OnLeftButtonUp(hwnd, pt))
            consumed = true;
        if (replaceInput_.OnLeftButtonUp(hwnd, pt))
            consumed = true;
        return consumed;
    }

    void SearchBox::SetInputFocused(bool focused)
    {
        inputFocused_ = focused;
        replaceFocused_ = false;
        activeField_ = focused ? InputField::Search : InputField::None;
        searchInput_.SetFocused(focused);
        replaceInput_.SetFocused(false);
    }

    void SearchBox::SetReplaceFocused(bool focused)
    {
        replaceFocused_ = focused;
        inputFocused_ = false;
        activeField_ = focused ? InputField::Replace : InputField::None;
        replaceInput_.SetFocused(focused);
        searchInput_.SetFocused(false);
    }

    bool SearchBox::IsPointInSearchBox(POINT pt) const
    {
        if (!visible_)
            return false;

        return PointInRect(boxRect_, pt) ||
               searchInput_.HitTest(pt) ||
               replaceInput_.HitTest(pt) ||
               PointInRect(replaceButtonRect_, pt);
    }

    void SearchBox::SetSearchText(const std::wstring &text)
    {
        searchText_ = text;
        searchInput_.SetText(text);
        caretPosition_ = (int)searchText_.size();
    }

    void SearchBox::SetReplaceText(const std::wstring &text)
    {
        replaceText_ = text;
        replaceInput_.SetText(text);
        replaceCaretPosition_ = (int)replaceText_.size();
    }

    void SearchBox::SetCurrentMatchIndex(int idx)
    {
        if (idx < 0 || idx >= (int)matches_.size())
        {
            currentMatchIndex_ = matches_.empty() ? -1 : 0;
            return;
        }
        currentMatchIndex_ = idx;
    }

    void SearchBox::PerformSearch(const std::vector<std::wstring> &lines)
    {
        searchText_ = searchInput_.GetText();
        replaceText_ = replaceInput_.GetText();
        matches_.clear();
        currentMatchIndex_ = -1;

        if (searchText_.empty() || lines.empty())
            return;

        std::wstring searchStr = searchText_;
        if (!options_.caseSensitive)
            std::transform(searchStr.begin(), searchStr.end(), searchStr.begin(), ::towlower);

        for (int lineIdx = 0; lineIdx < (int)lines.size(); ++lineIdx)
        {
            std::wstring line = lines[lineIdx];
            if (!options_.caseSensitive)
                std::transform(line.begin(), line.end(), line.begin(), ::towlower);

            size_t pos = 0;
            while ((pos = line.find(searchStr, pos)) != std::wstring::npos)
            {
                if (options_.wholeWord && !MatchesWholeWord(lines[lineIdx], pos, searchStr.length()))
                {
                    pos++;
                    continue;
                }

                SearchMatch match;
                match.line = lineIdx;
                match.startColumn = (int)pos;
                match.endColumn = (int)(pos + searchStr.length());
                matches_.push_back(match);
                pos++;
            }
        }

        if (!matches_.empty())
            currentMatchIndex_ = 0;
    }

    bool SearchBox::MatchesWholeWord(const std::wstring &line, size_t pos, size_t len)
    {
        if (pos > 0)
        {
            wchar_t before = line[pos - 1];
            if (iswalnum(before) || before == L'_')
                return false;
        }

        if (pos + len < line.length())
        {
            wchar_t after = line[pos + len];
            if (iswalnum(after) || after == L'_')
                return false;
        }

        return true;
    }

    void SearchBox::FindNext()
    {
        if (matches_.empty())
            return;

        currentMatchIndex_ = (currentMatchIndex_ + 1) % (int)matches_.size();
    }

    void SearchBox::FindPrevious()
    {
        if (matches_.empty())
            return;

        currentMatchIndex_--;
        if (currentMatchIndex_ < 0)
            currentMatchIndex_ = (int)matches_.size() - 1;
    }

    void SearchBox::ClearMatches()
    {
        matches_.clear();
        currentMatchIndex_ = -1;
    }

    void SearchBox::ToggleCaseSensitive()
    {
        options_.caseSensitive = !options_.caseSensitive;
    }

    void SearchBox::ToggleWholeWord()
    {
        options_.wholeWord = !options_.wholeWord;
    }

    void SearchBox::ToggleRegex()
    {
        options_.useRegex = !options_.useRegex;
    }

    void SearchBox::UpdateCaretBlink()
    {
        // Caret blinking is handled by TextInput.
    }

    bool SearchBox::ConsumeReplaceRequest()
    {
        if (!replaceRequested_)
            return false;

        replaceRequested_ = false;
        return true;
    }
} // namespace Orion
