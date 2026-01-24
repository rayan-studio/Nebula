#include "TextInput.h"
#include <algorithm>

TextInput::TextInput()
{
    lastBlinkTime_ = GetTickCount();
}

void TextInput::SetText(const std::wstring& text)
{
    text_ = text;
    cursorPos_ = (int)text_.length();
    selectionStart_ = selectionEnd_ = cursorPos_;
}

void TextInput::SetFocused(bool focused)
{
    focused_ = focused;
    if (focused) {
        lastBlinkTime_ = GetTickCount();
        cursorVisible_ = true;
    }
}

bool TextInput::HitTest(POINT pt) const
{
    D2D1_RECT_F testRect = rect_;
    
    // If using SearchBox style, expand hit area to include outer box
    if (style_.useSearchBoxStyle) {
        testRect.left -= style_.outerPadding;
        testRect.right += style_.outerPadding;
        testRect.top -= style_.outerVerticalPadding;
        testRect.bottom += style_.outerVerticalPadding;
    }
    
    return (pt.x >= testRect.left && pt.x <= testRect.right &&
            pt.y >= testRect.top && pt.y <= testRect.bottom);
}

void TextInput::Draw(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite)
{
    lastDWrite_ = dwrite;
    // Use per-primitive AA to keep rounded corners clean
    D2D1_ANTIALIAS_MODE oldAA = ctx->GetAntialiasMode();
    ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (style_.useSearchBoxStyle) {
        DrawSearchBoxStyle(ctx, dwrite);
    } else {
        DrawStandardStyle(ctx, dwrite);
    }

    // Restore previous antialiasing mode
    ctx->SetAntialiasMode(oldAA);
}

void TextInput::DrawSearchBoxStyle(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite)
{
    // Previously this drew an outer box + shadow. Remove the external rectangle
    // to keep a single, clean input appearance (only inner input is drawn).
    DrawStandardStyle(ctx, dwrite);
}

void TextInput::DrawStandardStyle(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite)
{
    // Create brushes
    ID2D1SolidColorBrush* bgBrush = nullptr;
    ID2D1SolidColorBrush* borderBrush = nullptr;
    ID2D1SolidColorBrush* textBrush = nullptr;
    ID2D1SolidColorBrush* placeholderBrush = nullptr;
    ID2D1SolidColorBrush* selectionBrush = nullptr;
    ID2D1SolidColorBrush* cursorBrush = nullptr;
    ID2D1SolidColorBrush* iconBrush = nullptr;
    
    ctx->CreateSolidColorBrush(style_.backgroundColor, &bgBrush);
    ctx->CreateSolidColorBrush(focused_ ? style_.focusBorderColor : style_.borderColor, &borderBrush);
    ctx->CreateSolidColorBrush(style_.textColor, &textBrush);
    ctx->CreateSolidColorBrush(style_.placeholderColor, &placeholderBrush);
    ctx->CreateSolidColorBrush(style_.selectionColor, &selectionBrush);
    ctx->CreateSolidColorBrush(style_.cursorColor, &cursorBrush);
    ctx->CreateSolidColorBrush(style_.iconColor, &iconBrush);
    
    // Draw background
    D2D1_ROUNDED_RECT roundedRect = D2D1::RoundedRect(rect_, style_.cornerRadius, style_.cornerRadius);
    ctx->FillRoundedRectangle(roundedRect, bgBrush);
    ctx->DrawRoundedRectangle(roundedRect, borderBrush, 1.0f);
    
    // Calculate text area
    float iconWidth = icon_.empty() ? 0.0f : style_.iconPadding;
    float textLeft = rect_.left + style_.padding + iconWidth;
    float textRight = rect_.right - style_.padding;
    float textTop = rect_.top;
    float textBottom = rect_.bottom;
    
    // Draw icon if present
    if (!icon_.empty()) {
        IDWriteTextFormat* iconFormat = nullptr;
        dwrite->CreateTextFormat(iconFont_.c_str(), NULL, DWRITE_FONT_WEIGHT_NORMAL,
                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                 style_.iconSize, L"en-us", &iconFormat);
        if (iconFormat) {
            iconFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            iconFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            
            D2D1_RECT_F iconRect = D2D1::RectF(
                rect_.left + 4.0f,
                rect_.top,
                rect_.left + style_.iconPadding,
                rect_.bottom);
            ctx->DrawTextW(icon_.c_str(), (UINT32)icon_.length(), iconFormat, iconRect, iconBrush);
            iconFormat->Release();
        }
    }
    
    // Create text format
    IDWriteTextFormat* textFormat = nullptr;
    dwrite->CreateTextFormat(style_.fontFamily, style_.fontCollection, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             style_.fontSize, L"en-us", &textFormat);
    if (textFormat) {
        textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        textFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    
    // Clip text area
    D2D1_RECT_F textClip = D2D1::RectF(textLeft, textTop, textRight, textBottom);
    ctx->PushAxisAlignedClip(textClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    
    D2D1_RECT_F textRect = D2D1::RectF(textLeft - textOffsetX_, textTop, textRight + 500.0f, textBottom);
    
    if (text_.empty()) {
        // Draw placeholder
        if (textFormat) {
            ctx->DrawTextW(placeholder_.c_str(), (UINT32)placeholder_.length(), 
                          textFormat, textRect, placeholderBrush);
        }
    } else {
        // Draw selection background
        if (focused_ && HasSelection() && textFormat) {
            int selStart = (std::min)(selectionStart_, selectionEnd_);
            int selEnd = (std::max)(selectionStart_, selectionEnd_);
            
            float startX = GetCharPosition(dwrite, selStart);
            float endX = GetCharPosition(dwrite, selEnd);
            
            D2D1_RECT_F selRect = D2D1::RectF(
                textLeft + startX - textOffsetX_,
                rect_.top + 4.0f,
                textLeft + endX - textOffsetX_,
                rect_.bottom - 4.0f);
            ctx->FillRectangle(selRect, selectionBrush);
        }
        
        // Draw text
        if (textFormat) {
            ctx->DrawTextW(text_.c_str(), (UINT32)text_.length(), textFormat, textRect, textBrush);
        }
    }
    
    // Draw cursor
    if (focused_ && textFormat) {
        // Update blink
        DWORD now = GetTickCount();
        if (now - lastBlinkTime_ > 500) {
            cursorVisible_ = !cursorVisible_;
            lastBlinkTime_ = now;
        }
        
        if (cursorVisible_ || HasSelection()) {
            float cursorX = textLeft + GetCharPosition(dwrite, cursorPos_) - textOffsetX_;
            ctx->DrawLine(
                D2D1::Point2F(cursorX, rect_.top + 6.0f),
                D2D1::Point2F(cursorX, rect_.bottom - 6.0f),
                cursorBrush, 1.5f);
        }
    }
    
    ctx->PopAxisAlignedClip();
    
    // Cleanup
    if (textFormat) textFormat->Release();
    if (bgBrush) bgBrush->Release();
    if (borderBrush) borderBrush->Release();
    if (textBrush) textBrush->Release();
    if (placeholderBrush) placeholderBrush->Release();
    if (selectionBrush) selectionBrush->Release();
    if (cursorBrush) cursorBrush->Release();
    if (iconBrush) iconBrush->Release();
}

float TextInput::GetCharPosition(IDWriteFactory* dwrite, int index)
{
    if (index <= 0 || text_.empty()) return 0.0f;
    if (index > (int)text_.length()) index = (int)text_.length();
    
    IDWriteTextFormat* tf = nullptr;
    dwrite->CreateTextFormat(style_.fontFamily, style_.fontCollection, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             style_.fontSize, L"en-us", &tf);
    if (!tf) return 0.0f;
    
    IDWriteTextLayout* layout = nullptr;
    std::wstring sub = text_.substr(0, index);
    dwrite->CreateTextLayout(sub.c_str(), (UINT32)sub.length(), tf, 10000.0f, 100.0f, &layout);
    
    float result = 0.0f;
    if (layout) {
        DWRITE_TEXT_METRICS metrics;
        layout->GetMetrics(&metrics);
        result = metrics.widthIncludingTrailingWhitespace;
        layout->Release();
    }
    
    tf->Release();
    return result;
}

int TextInput::GetCharIndexAtPosition(IDWriteFactory* dwrite, float x)
{
    if (text_.empty()) return 0;
    
    IDWriteTextFormat* tf = nullptr;
    dwrite->CreateTextFormat(style_.fontFamily, style_.fontCollection, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             style_.fontSize, L"en-us", &tf);
    if (!tf) return 0;
    
    IDWriteTextLayout* layout = nullptr;
    dwrite->CreateTextLayout(text_.c_str(), (UINT32)text_.length(), tf, 10000.0f, 100.0f, &layout);
    
    int result = 0;
    if (layout) {
        BOOL isTrailingHit = FALSE;
        BOOL isInside = FALSE;
        DWRITE_HIT_TEST_METRICS hitMetrics;
        layout->HitTestPoint(x, 10.0f, &isTrailingHit, &isInside, &hitMetrics);
        result = hitMetrics.textPosition + (isTrailingHit ? 1 : 0);
        layout->Release();
    }
    
    tf->Release();
    return (std::min)(result, (int)text_.length());
}

bool TextInput::OnMouseMove(HWND hwnd, POINT pt)
{
    if (!selecting_) return false;
    return true;
}

bool TextInput::OnLeftButtonDown(HWND hwnd, POINT pt)
{
    if (!HitTest(pt)) {
        if (focused_) {
            focused_ = false;
            return true;
        }
        return false;
    }
    
    focused_ = true;
    lastBlinkTime_ = GetTickCount();
    cursorVisible_ = true;
    
    float iconWidth = icon_.empty() ? 0.0f : style_.iconPadding;
    float textLeft = rect_.left + style_.padding + iconWidth;
    float clickX = pt.x - textLeft + textOffsetX_;

    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    int clickIndex = 0;
    if (lastDWrite_)
        clickIndex = GetCharIndexAtPosition(lastDWrite_, clickX);
    if (clickIndex < 0) clickIndex = 0;
    if (clickIndex > (int)text_.length()) clickIndex = (int)text_.length();

    cursorPos_ = clickIndex;
    if (!shift) {
        selectionStart_ = selectionEnd_ = cursorPos_;
    } else {
        selectionEnd_ = cursorPos_;
    }
    selecting_ = true;
    SetCapture(hwnd);
    
    return true;
}

bool TextInput::OnLeftButtonUp(HWND hwnd, POINT pt)
{
    if (selecting_) {
        selecting_ = false;
        ReleaseCapture();
        return true;
    }
    return false;
}

bool TextInput::OnChar(wchar_t ch)
{
    if (!focused_) return false;
    
    if (ch == 8) {
        if (HasSelection()) {
            DeleteSelection();
        } else if (cursorPos_ > 0) {
            text_.erase(cursorPos_ - 1, 1);
            cursorPos_--;
            selectionStart_ = selectionEnd_ = cursorPos_;
        }
        if (onTextChanged) onTextChanged(text_);
        return true;
    }
    else if (ch == 127) {
        if (HasSelection()) {
            DeleteSelection();
        } else if (cursorPos_ > 0) {
            int pos = cursorPos_ - 1;
            while (pos > 0 && text_[pos - 1] == L' ') pos--;
            while (pos > 0 && text_[pos - 1] != L' ') pos--;
            text_.erase(pos, cursorPos_ - pos);
            cursorPos_ = pos;
            selectionStart_ = selectionEnd_ = cursorPos_;
        }
        if (onTextChanged) onTextChanged(text_);
        return true;
    }
    else if (ch == 1) {
        SelectAll();
        return true;
    }
    else if (ch == 3) {
        CopyToClipboard();
        return true;
    }
    else if (ch == 22) {
        PasteFromClipboard();
        if (onTextChanged) onTextChanged(text_);
        return true;
    }
    else if (ch == 24) {
        CutToClipboard();
        if (onTextChanged) onTextChanged(text_);
        return true;
    }
    else if (ch >= 32) {
        if (HasSelection()) {
            DeleteSelection();
        }
        text_.insert(cursorPos_, 1, ch);
        cursorPos_++;
        selectionStart_ = selectionEnd_ = cursorPos_;
        if (onTextChanged) onTextChanged(text_);
        return true;
    }
    
    return false;
}

bool TextInput::OnKeyDown(WPARAM key)
{
    if (!focused_) return false;
    
    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    
    lastBlinkTime_ = GetTickCount();
    cursorVisible_ = true;
    
    if (key == VK_LEFT) {
        if (ctrl) {
            int pos = cursorPos_;
            while (pos > 0 && text_[pos - 1] == L' ') pos--;
            while (pos > 0 && text_[pos - 1] != L' ') pos--;
            cursorPos_ = pos;
        } else if (cursorPos_ > 0) {
            cursorPos_--;
        }
        if (!shift) selectionStart_ = cursorPos_;
        selectionEnd_ = cursorPos_;
        return true;
    }
    else if (key == VK_RIGHT) {
        if (ctrl) {
            int pos = cursorPos_;
            int len = (int)text_.length();
            while (pos < len && text_[pos] != L' ') pos++;
            while (pos < len && text_[pos] == L' ') pos++;
            cursorPos_ = pos;
        } else if (cursorPos_ < (int)text_.length()) {
            cursorPos_++;
        }
        if (!shift) selectionStart_ = cursorPos_;
        selectionEnd_ = cursorPos_;
        return true;
    }
    else if (key == VK_HOME) {
        cursorPos_ = 0;
        if (!shift) selectionStart_ = cursorPos_;
        selectionEnd_ = cursorPos_;
        return true;
    }
    else if (key == VK_END) {
        cursorPos_ = (int)text_.length();
        if (!shift) selectionStart_ = cursorPos_;
        selectionEnd_ = cursorPos_;
        return true;
    }
    else if (key == VK_DELETE) {
        if (HasSelection()) {
            DeleteSelection();
        } else if (cursorPos_ < (int)text_.length()) {
            if (ctrl) {
                int pos = cursorPos_;
                int len = (int)text_.length();
                while (pos < len && text_[pos] != L' ') pos++;
                while (pos < len && text_[pos] == L' ') pos++;
                text_.erase(cursorPos_, pos - cursorPos_);
            } else {
                text_.erase(cursorPos_, 1);
            }
        }
        if (onTextChanged) onTextChanged(text_);
        return true;
    }
    else if (key == VK_RETURN) {
        if (onSubmit) onSubmit();
        return true;
    }
    else if (key == VK_ESCAPE) {
        if (onEscape) onEscape();
        return true;
    }
    
    return false;
}

void TextInput::DeleteSelection()
{
    if (!HasSelection()) return;
    
    int start = (std::min)(selectionStart_, selectionEnd_);
    int end = (std::max)(selectionStart_, selectionEnd_);
    text_.erase(start, end - start);
    cursorPos_ = start;
    selectionStart_ = selectionEnd_ = cursorPos_;
}

void TextInput::SelectAll()
{
    selectionStart_ = 0;
    selectionEnd_ = (int)text_.length();
    cursorPos_ = selectionEnd_;
}

void TextInput::CopyToClipboard()
{
    if (!HasSelection()) return;
    
    int start = (std::min)(selectionStart_, selectionEnd_);
    int end = (std::max)(selectionStart_, selectionEnd_);
    std::wstring selected = text_.substr(start, end - start);
    
    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        size_t size = (selected.length() + 1) * sizeof(wchar_t);
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, size);
        if (hMem) {
            wchar_t* pMem = (wchar_t*)GlobalLock(hMem);
            if (pMem) {
                wcscpy_s(pMem, selected.length() + 1, selected.c_str());
                GlobalUnlock(hMem);
                SetClipboardData(CF_UNICODETEXT, hMem);
            }
        }
        CloseClipboard();
    }
}

void TextInput::PasteFromClipboard()
{
    if (!OpenClipboard(NULL)) return;
    
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (hData) {
        wchar_t* pText = (wchar_t*)GlobalLock(hData);
        if (pText) {
            if (HasSelection()) {
                DeleteSelection();
            }
            std::wstring paste = pText;
            paste.erase(std::remove(paste.begin(), paste.end(), L'\n'), paste.end());
            paste.erase(std::remove(paste.begin(), paste.end(), L'\r'), paste.end());
            
            text_.insert(cursorPos_, paste);
            cursorPos_ += (int)paste.length();
            selectionStart_ = selectionEnd_ = cursorPos_;
            GlobalUnlock(hData);
        }
    }
    CloseClipboard();
}

void TextInput::CutToClipboard()
{
    CopyToClipboard();
    DeleteSelection();
}
