#include "./SearchBox.h"
#include <algorithm>
#include <regex>

namespace Orion
{
    SearchBox::SearchBox()
        : visible_(false)
        , inputFocused_(true)
        , caretPosition_(0)
        , caretVisible_(true)
        , lastBlinkTime_(GetTickCount())
        , currentMatchIndex_(-1)
        , hoverClose_(false)
        , hoverPrev_(false)
        , hoverNext_(false)
        , hoverCase_(false)
        , hoverWord_(false)
        , hoverRegex_(false)
    {
    }

    SearchBox::~SearchBox()
    {
    }

    void SearchBox::Show()
    {
        visible_ = true;
        inputFocused_ = true;
        caretVisible_ = true;
        lastBlinkTime_ = GetTickCount();
        // Select all text when opening
        caretPosition_ = static_cast<int>(searchText_.length());
    }

    void SearchBox::Hide()
    {
        visible_ = false;
        inputFocused_ = false;
        ClearMatches();
    }

    void SearchBox::UpdateLayout(float editorLeft, float editorTop, float editorWidth)
    {
        // Position search box at top-right of editor (only input field visible)
        float boxWidth = 400.0f;
        float boxHeight = 36.0f;
        float padding = 8.0f;

        boxRect_ = D2D1::RectF(
            editorLeft + editorWidth - boxWidth - padding,
            editorTop + padding,
            editorLeft + editorWidth - padding,
            editorTop + padding + boxHeight
        );

        // Input fills most of the box (no buttons)
        inputRect_ = D2D1::RectF(
            boxRect_.left + 8.0f,
            boxRect_.top + 4.0f,
            boxRect_.right - 8.0f,
            boxRect_.bottom - 4.0f
        );
    }

    void SearchBox::Draw(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite)
    {
        if (!visible_) return;

        UpdateCaretBlink();

        // Note: outer box (shadow/background/border) intentionally omitted
        // to match the simplified single-input look used elsewhere.

        // Input field background
        ID2D1SolidColorBrush* inputBgBrush = nullptr;
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.1f, 0.1f, 0.1f), &inputBgBrush);
        if (inputBgBrush)
        {
            ctx->FillRoundedRectangle(D2D1::RoundedRect(inputRect_, 2.0f, 2.0f), inputBgBrush);
            inputBgBrush->Release();
        }

        // Input field border
        ID2D1SolidColorBrush* inputBorderBrush = nullptr;
        D2D1_COLOR_F borderColor = inputFocused_ ? D2D1::ColorF(0.2f, 0.4f, 0.8f) : D2D1::ColorF(0.25f, 0.25f, 0.25f);
        ctx->CreateSolidColorBrush(borderColor, &inputBorderBrush);
        if (inputBorderBrush)
        {
            ctx->DrawRoundedRectangle(D2D1::RoundedRect(inputRect_, 2.0f, 2.0f), inputBorderBrush, 1.0f);
            inputBorderBrush->Release();
        }

        // Input text
        if (!searchText_.empty())
        {
            IDWriteTextFormat* textFormat = nullptr;
            dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                    13.0f, L"en-us", &textFormat);
            if (textFormat)
            {
                textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

                ID2D1SolidColorBrush* textBrush = nullptr;
                ctx->CreateSolidColorBrush(D2D1::ColorF(0.9f, 0.9f, 0.9f), &textBrush);
                
                D2D1_RECT_F textRect = inputRect_;
                textRect.left += 6.0f;
                textRect.right -= 6.0f;
                
                ctx->DrawTextW(searchText_.c_str(), static_cast<UINT32>(searchText_.length()),
                              textFormat, textRect, textBrush);

                if (textBrush) textBrush->Release();
                textFormat->Release();
            }
        }
        else
        {
            // Placeholder text
            IDWriteTextFormat* placeholderFormat = nullptr;
            dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                    DWRITE_FONT_STYLE_ITALIC, DWRITE_FONT_STRETCH_NORMAL,
                                    13.0f, L"en-us", &placeholderFormat);
            if (placeholderFormat)
            {
                placeholderFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                placeholderFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

                ID2D1SolidColorBrush* placeholderBrush = nullptr;
                ctx->CreateSolidColorBrush(D2D1::ColorF(0.4f, 0.4f, 0.4f), &placeholderBrush);
                
                D2D1_RECT_F textRect = inputRect_;
                textRect.left += 6.0f;
                textRect.right -= 6.0f;
                
                const wchar_t* placeholder = L"Rechercher...";
                ctx->DrawTextW(placeholder, static_cast<UINT32>(wcslen(placeholder)),
                              placeholderFormat, textRect, placeholderBrush);

                if (placeholderBrush) placeholderBrush->Release();
                placeholderFormat->Release();
            }
        }

        // Caret
        if (inputFocused_ && caretVisible_)
        {
            ID2D1SolidColorBrush* caretBrush = nullptr;
            ctx->CreateSolidColorBrush(D2D1::ColorF(0.9f, 0.9f, 0.9f), &caretBrush);
            if (caretBrush)
            {
                // Simple caret at end of text for now
                float caretX = inputRect_.left + 6.0f;
                if (!searchText_.empty())
                {
                    IDWriteTextFormat* tf = nullptr;
                    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                            13.0f, L"en-us", &tf);
                    if (tf)
                    {
                        IDWriteTextLayout* layout = nullptr;
                        dwrite->CreateTextLayout(searchText_.c_str(), caretPosition_,
                                                tf, 1000.0f, 30.0f, &layout);
                        if (layout)
                        {
                            DWRITE_TEXT_METRICS metrics;
                            layout->GetMetrics(&metrics);
                            caretX += metrics.width;
                            layout->Release();
                        }
                        tf->Release();
                    }
                }
                
                D2D1_RECT_F caretRect = D2D1::RectF(
                    caretX, inputRect_.top + 4.0f,
                    caretX + 1.5f, inputRect_.bottom - 4.0f
                );
                ctx->FillRectangle(caretRect, caretBrush);
                caretBrush->Release();
            }
        }

        // No buttons or match counter in the simplified UI
    }

    void SearchBox::DrawButton(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite,
                               const D2D1_RECT_F& rect, const wchar_t* icon,
                               bool active, bool hovered)
    {
        // Background
        ID2D1SolidColorBrush* bgBrush = nullptr;
        D2D1_COLOR_F bgColor;
        if (active)
            bgColor = D2D1::ColorF(0.2f, 0.4f, 0.8f);
        else if (hovered)
            bgColor = D2D1::ColorF(0.25f, 0.25f, 0.25f);
        else
            bgColor = D2D1::ColorF(0.15f, 0.15f, 0.15f);
        
        ctx->CreateSolidColorBrush(bgColor, &bgBrush);
        if (bgBrush)
        {
            ctx->FillRoundedRectangle(D2D1::RoundedRect(rect, 2.0f, 2.0f), bgBrush);
            bgBrush->Release();
        }

        // Border
        if (hovered || active)
        {
            ID2D1SolidColorBrush* borderBrush = nullptr;
            ctx->CreateSolidColorBrush(D2D1::ColorF(0.4f, 0.4f, 0.4f), &borderBrush);
            if (borderBrush)
            {
                ctx->DrawRoundedRectangle(D2D1::RoundedRect(rect, 2.0f, 2.0f), borderBrush, 1.0f);
                borderBrush->Release();
            }
        }

        // Icon
        IDWriteTextFormat* iconFormat = nullptr;
        dwrite->CreateTextFormat(L"Segoe UI Symbol", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                12.0f, L"en-us", &iconFormat);
        if (iconFormat)
        {
            iconFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            iconFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

            ID2D1SolidColorBrush* iconBrush = nullptr;
            D2D1_COLOR_F iconColor = active ? D2D1::ColorF(1.0f, 1.0f, 1.0f) : D2D1::ColorF(0.7f, 0.7f, 0.7f);
            ctx->CreateSolidColorBrush(iconColor, &iconBrush);
            
            ctx->DrawTextW(icon, static_cast<UINT32>(wcslen(icon)),
                          iconFormat, rect, iconBrush);

            if (iconBrush) iconBrush->Release();
            iconFormat->Release();
        }
    }

    void SearchBox::OnChar(wchar_t ch)
    {
        if (!inputFocused_) return;
        if (ch < 32 || ch == 127) return; // Ignore control chars

        searchText_.insert(caretPosition_, 1, ch);
        caretPosition_++;
        caretVisible_ = true;
        lastBlinkTime_ = GetTickCount();
    }

    void SearchBox::OnKeyDown(WPARAM key)
    {
        if (!inputFocused_) return;

        switch (key)
        {
        case VK_BACK:
            if (caretPosition_ > 0)
            {
                searchText_.erase(caretPosition_ - 1, 1);
                caretPosition_--;
            }
            break;
        
        case VK_DELETE:
            if (caretPosition_ < static_cast<int>(searchText_.length()))
            {
                searchText_.erase(caretPosition_, 1);
            }
            break;
        
        case VK_LEFT:
            if (caretPosition_ > 0)
                caretPosition_--;
            break;
        
        case VK_RIGHT:
            if (caretPosition_ < static_cast<int>(searchText_.length()))
                caretPosition_++;
            break;
        
        case VK_HOME:
            caretPosition_ = 0;
            break;
        
        case VK_END:
            caretPosition_ = static_cast<int>(searchText_.length());
            break;
        
        case VK_RETURN:
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
                FindPrevious();
            else
                FindNext();
            break;
        
        case VK_ESCAPE:
            Hide();
            break;
        }

        caretVisible_ = true;
        lastBlinkTime_ = GetTickCount();
    }

    void SearchBox::OnLeftButtonDown(POINT pt)
    {
        // Click in input field -> focus it
        // Avoid casting D2D1_RECT_F to RECT (undefined interpretation of floats as LONGs).
        if (pt.x >= inputRect_.left && pt.x <= inputRect_.right &&
            pt.y >= inputRect_.top && pt.y <= inputRect_.bottom)
        {
            // Use setter to keep focus behavior consistent (caret blink, visibility)
            SetInputFocused(true);
            // TODO: Set caret position based on click location (requires text layout measurement)
        }
    }

    void SearchBox::SetInputFocused(bool focused)
    {
        inputFocused_ = focused;
        if (!inputFocused_)
        {
            caretVisible_ = false;
        }
        else
        {
            caretVisible_ = true;
            lastBlinkTime_ = GetTickCount();
        }
    }

    bool SearchBox::IsPointInSearchBox(POINT pt) const
    {
        if (!visible_) return false;

        // Consider clicks inside the input area or any visible control buttons only.
        auto inside = [&](const D2D1_RECT_F &r) -> bool {
            return (pt.x >= r.left && pt.x <= r.right && pt.y >= r.top && pt.y <= r.bottom);
        };

        if (inside(inputRect_)) return true;
        if (inside(closeButtonRect_)) return true;
        if (inside(prevButtonRect_)) return true;
        if (inside(nextButtonRect_)) return true;
        if (inside(caseButtonRect_)) return true;
        if (inside(wordButtonRect_)) return true;
        if (inside(regexButtonRect_)) return true;

        return false;
    }

    void SearchBox::SetSearchText(const std::wstring& text)
    {
        searchText_ = text;
        caretPosition_ = static_cast<int>(text.length());
    }

    void SearchBox::PerformSearch(const std::vector<std::wstring>& lines)
    {
        matches_.clear();
        currentMatchIndex_ = -1;

        if (searchText_.empty() || lines.empty())
            return;

        std::wstring searchStr = searchText_;
        if (!options_.caseSensitive)
        {
            std::transform(searchStr.begin(), searchStr.end(), searchStr.begin(), ::towlower);
        }

        // Simple string search (regex support can be added later)
        for (int lineIdx = 0; lineIdx < static_cast<int>(lines.size()); ++lineIdx)
        {
            std::wstring line = lines[lineIdx];
            if (!options_.caseSensitive)
            {
                std::transform(line.begin(), line.end(), line.begin(), ::towlower);
            }

            size_t pos = 0;
            while ((pos = line.find(searchStr, pos)) != std::wstring::npos)
            {
                // Check whole word if needed
                if (options_.wholeWord && !MatchesWholeWord(lines[lineIdx], pos, searchStr.length()))
                {
                    pos++;
                    continue;
                }

                SearchMatch match;
                match.line = lineIdx;
                match.startColumn = static_cast<int>(pos);
                match.endColumn = static_cast<int>(pos + searchStr.length());
                matches_.push_back(match);
                
                pos++;
            }
        }

        if (!matches_.empty())
        {
            currentMatchIndex_ = 0;
        }
    }

    bool SearchBox::MatchesWholeWord(const std::wstring& line, size_t pos, size_t len)
    {
        // Check if character before match is word boundary
        if (pos > 0)
        {
            wchar_t before = line[pos - 1];
            if (iswalnum(before) || before == L'_')
                return false;
        }

        // Check if character after match is word boundary
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
        if (matches_.empty()) return;
        
        currentMatchIndex_ = (currentMatchIndex_ + 1) % static_cast<int>(matches_.size());
    }

    void SearchBox::FindPrevious()
    {
        if (matches_.empty()) return;
        
        currentMatchIndex_--;
        if (currentMatchIndex_ < 0)
            currentMatchIndex_ = static_cast<int>(matches_.size()) - 1;
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
        DWORD current = GetTickCount();
        if (current - lastBlinkTime_ > 500)
        {
            caretVisible_ = !caretVisible_;
            lastBlinkTime_ = current;
        }
    }

} // namespace Orion
