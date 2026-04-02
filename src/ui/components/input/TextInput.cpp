#include "TextInput.h"
#include <algorithm>
#include <cmath>

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
    D2D1_TEXT_ANTIALIAS_MODE oldTextAA = ctx->GetTextAntialiasMode();
    ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

    if (style_.useSearchBoxStyle) {
        DrawSearchBoxStyle(ctx, dwrite);
    } else {
        DrawStandardStyle(ctx, dwrite);
    }

    // Restore previous antialiasing mode
    ctx->SetAntialiasMode(oldAA);
    ctx->SetTextAntialiasMode(oldTextAA);
}

void TextInput::DrawSearchBoxStyle(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite)
{
    // Previously this drew an outer box + shadow. Remove the external rectangle
    // to keep a single, clean input appearance (only inner input is drawn).
    DrawStandardStyle(ctx, dwrite);
}

void TextInput::DrawStandardStyle(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite)
{
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

    // Snap geometry to pixel grid to avoid blurry 1px borders.
    D2D1_RECT_F snappedRect = D2D1::RectF(
        std::round(rect_.left),
        std::round(rect_.top),
        std::round(rect_.right),
        std::round(rect_.bottom));
    if (snappedRect.right <= snappedRect.left)
        snappedRect.right = snappedRect.left + 1.0f;
    if (snappedRect.bottom <= snappedRect.top)
        snappedRect.bottom = snappedRect.top + 1.0f;

    D2D1_RECT_F strokeRect = D2D1::RectF(
        snappedRect.left + 0.5f,
        snappedRect.top + 0.5f,
        snappedRect.right - 0.5f,
        snappedRect.bottom - 0.5f);
    if (strokeRect.right <= strokeRect.left)
        strokeRect.right = strokeRect.left + 1.0f;
    if (strokeRect.bottom <= strokeRect.top)
        strokeRect.bottom = strokeRect.top + 1.0f;

    const float fillRadius = style_.cornerRadius;
    const float strokeRadius = (std::max)(0.0f, style_.cornerRadius - 0.5f);
    ctx->FillRoundedRectangle(D2D1::RoundedRect(snappedRect, fillRadius, fillRadius), bgBrush);
    ctx->DrawRoundedRectangle(D2D1::RoundedRect(strokeRect, strokeRadius, strokeRadius), borderBrush, 1.0f);

    float iconWidth = icon_.empty() ? 0.0f : style_.iconPadding;
    
    // Design text layout to fit within the input area, accounting for padding and optional icon
    float textLeft = snappedRect.left + style_.paddingLeft + iconWidth; //  bord gauche du champ + padding + place de l’icône
    float textRight = snappedRect.right - style_.paddingRight; // bord droit du champ - padding


    const float verticalPadding = style_.padding;
    float topInset = style_.multiline ? verticalPadding * 0.55f : 0.0f;
    float bottomInset = style_.multiline ? verticalPadding * 0.45f : 0.0f;
    
    D2D1_RECT_F textClip = D2D1::RectF(textLeft
                                        , snappedRect.top + topInset
                                        , textRight
                                        , snappedRect.bottom - bottomInset);
    
    if (textClip.bottom < textClip.top)
        textClip.bottom = textClip.top;

    if (!icon_.empty()) {
        IDWriteTextFormat* iconFormat = nullptr;
        dwrite->CreateTextFormat(iconFont_.c_str(), NULL, DWRITE_FONT_WEIGHT_NORMAL,
                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                 style_.iconSize, L"en-us", &iconFormat);
        if (iconFormat) {
            iconFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            iconFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            D2D1_RECT_F iconRect = D2D1::RectF(snappedRect.left + 4.0f, snappedRect.top, snappedRect.left + style_.iconPadding, snappedRect.bottom);
            ctx->DrawTextW(icon_.c_str(), (UINT32)icon_.length(), iconFormat, iconRect, iconBrush);
            iconFormat->Release();
        }
    }

    IDWriteTextFormat* textFormat = nullptr;
    dwrite->CreateTextFormat(style_.fontFamily, style_.fontCollection, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             style_.fontSize, L"en-us", &textFormat);
    if (textFormat) {
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        textFormat->SetParagraphAlignment(style_.multiline ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR
                                                           : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        textFormat->SetWordWrapping(style_.multiline ? DWRITE_WORD_WRAPPING_WRAP
                                                     : DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    ctx->PushAxisAlignedClip(textClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    DWORD now = GetTickCount();
    if (now - lastBlinkTime_ > 500) {
        cursorVisible_ = !cursorVisible_;
        lastBlinkTime_ = now;
    }

    bool drawCaret = focused_ && (cursorVisible_ || HasSelection());
    float layoutWidth = (std::max)(1.0f, textClip.right - textClip.left);
    float layoutHeight = (std::max)(1.0f, textClip.bottom - textClip.top);

    if (text_.empty()) {
        if (textFormat) {
            D2D1_RECT_F placeholderRect = D2D1::RectF(
                textClip.left,
                textClip.top,
                style_.multiline ? textClip.right : (textClip.right + 500.0f),
                textClip.bottom);
            ctx->DrawTextW(placeholder_.c_str(), (UINT32)placeholder_.length(), textFormat, placeholderRect, placeholderBrush);
        }

        if (drawCaret) {
            float caretX = std::floor(textClip.left) + 0.5f;
            float caretTop = style_.multiline ? (textClip.top + 1.0f) : (snappedRect.top + 6.0f);
            float caretBottom = style_.multiline ? ((std::min)(textClip.bottom, textClip.top + style_.fontSize + 5.0f))
                                                 : (snappedRect.bottom - 6.0f);
            ctx->DrawLine(D2D1::Point2F(caretX, caretTop), D2D1::Point2F(caretX, caretBottom), cursorBrush, 1.0f);
        }
    } else if (textFormat) {
        IDWriteTextLayout* layout = nullptr;
        dwrite->CreateTextLayout(text_.c_str(), (UINT32)text_.length(), textFormat,
                                 style_.multiline ? layoutWidth : 10000.0f,
                                 layoutHeight, &layout);

        if (layout) {
            float originX = textClip.left - textOffsetX_;
            float originY = style_.multiline ? textClip.top : rect_.top;

            if (focused_ && HasSelection()) {
                int selStart = (std::min)(selectionStart_, selectionEnd_);
                int selEnd = (std::max)(selectionStart_, selectionEnd_);
                UINT32 selLen = (UINT32)(selEnd - selStart);
                if (selLen > 0) {
                    UINT32 hitCount = 0;
                    layout->HitTestTextRange((UINT32)selStart, selLen, originX, originY, nullptr, 0, &hitCount);
                    if (hitCount > 0) {
                        std::vector<DWRITE_HIT_TEST_METRICS> hits(hitCount);
                        if (SUCCEEDED(layout->HitTestTextRange((UINT32)selStart, selLen, originX, originY,
                                                               hits.data(), hitCount, &hitCount))) {
                            for (UINT32 i = 0; i < hitCount; ++i) {
                                const auto &h = hits[i];
                                D2D1_RECT_F selRect = D2D1::RectF(
                                    h.left,
                                    h.top,
                                    h.left + h.width,
                                    h.top + h.height);
                                ctx->FillRectangle(selRect, selectionBrush);
                            }
                        }
                    }
                }
            }

            ctx->DrawTextLayout(D2D1::Point2F(originX, originY), layout, textBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);

            if (drawCaret) {
                FLOAT cx = 0.0f, cy = 0.0f;
                DWRITE_HIT_TEST_METRICS hm = {};
                UINT32 textPos = (UINT32)(std::max)(0, (std::min)(cursorPos_, (int)text_.length()));
                if (SUCCEEDED(layout->HitTestTextPosition(textPos, FALSE, &cx, &cy, &hm))) {
                    float caretX = std::floor(originX + cx) + 0.5f;
                    float caretTop = style_.multiline ? (originY + cy) : (snappedRect.top + 6.0f);
                    float caretBottom = style_.multiline ? (caretTop + (std::max)(hm.height, style_.fontSize + 2.0f))
                                                         : (snappedRect.bottom - 6.0f);
                    ctx->DrawLine(D2D1::Point2F(caretX, caretTop), D2D1::Point2F(caretX, caretBottom), cursorBrush, 1.0f);
                }
            }

            layout->Release();
        }
    }

    ctx->PopAxisAlignedClip();

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

int TextInput::GetCharIndexAtPosition(IDWriteFactory* dwrite, float x, float y)
{
    if (text_.empty()) return 0;
    
    IDWriteTextFormat* tf = nullptr;
    dwrite->CreateTextFormat(style_.fontFamily, style_.fontCollection, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             style_.fontSize, L"en-us", &tf);
    if (!tf) return 0;

    tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    tf->SetParagraphAlignment(style_.multiline ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR
                                               : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    tf->SetWordWrapping(style_.multiline ? DWRITE_WORD_WRAPPING_WRAP
                                         : DWRITE_WORD_WRAPPING_NO_WRAP);
    
    IDWriteTextLayout* layout = nullptr;
    float iconWidth = icon_.empty() ? 0.0f : style_.iconPadding;
    float width = (std::max)(1.0f, (rect_.right - rect_.left) - style_.paddingLeft - style_.paddingRight - iconWidth);
    float height = (std::max)(1.0f, (rect_.bottom - rect_.top) - (style_.multiline ? style_.padding : 0.0f));
    dwrite->CreateTextLayout(text_.c_str(), (UINT32)text_.length(), tf,
                             style_.multiline ? width : 10000.0f,
                             style_.multiline ? height : 100.0f, &layout);
    
    int result = 0;
    if (layout) {
        BOOL isTrailingHit = FALSE;
        BOOL isInside = FALSE;
        DWRITE_HIT_TEST_METRICS hitMetrics = {};
        layout->HitTestPoint(x, style_.multiline ? y : 10.0f, &isTrailingHit, &isInside, &hitMetrics);
        result = (int)hitMetrics.textPosition + (isTrailingHit ? 1 : 0);
        if (!isInside) {
            if (x <= 0.0f || y <= 0.0f)
                result = 0;
            else
                result = (int)text_.length();
        }
        layout->Release();
    }
    
    tf->Release();
    return (std::min)(result, (int)text_.length());
}

bool TextInput::OnMouseMove(HWND hwnd, POINT pt)
{
    if (!selecting_)
        return false;

    float iconWidth = icon_.empty() ? 0.0f : style_.iconPadding;
    float textLeft = rect_.left + style_.paddingLeft + iconWidth;
    float textTop = rect_.top + (style_.multiline ? style_.padding * 0.55f : 0.0f);
    float x = pt.x - textLeft + textOffsetX_;
    float y = pt.y - textTop;

    int idx = 0;
    if (lastDWrite_)
        idx = GetCharIndexAtPosition(lastDWrite_, x, y);
    idx = (std::max)(0, (std::min)(idx, (int)text_.length()));

    bool changed = (idx != cursorPos_) || (idx != selectionEnd_);
    cursorPos_ = idx;
    selectionEnd_ = idx;
    if (changed)
        InvalidateRect(hwnd, nullptr, FALSE);
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
    float textLeft = rect_.left + style_.paddingLeft + iconWidth;
    float textTop = rect_.top + (style_.multiline ? style_.padding * 0.55f : 0.0f);
    float clickX = pt.x - textLeft + textOffsetX_;
    float clickY = pt.y - textTop;

    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    int clickIndex = 0;
    if (lastDWrite_)
        clickIndex = GetCharIndexAtPosition(lastDWrite_, clickX, clickY);
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
    else if (ch == 13 || ch == 10) {
        if (!style_.multiline)
            return false;
        if (HasSelection()) {
            DeleteSelection();
        }
        text_.insert(cursorPos_, 1, L'\n');
        cursorPos_++;
        selectionStart_ = selectionEnd_ = cursorPos_;
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
        if (style_.multiline)
            return true;
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
            if (style_.multiline)
            {
                std::wstring normalized;
                normalized.reserve(paste.size());
                for (size_t i = 0; i < paste.size(); ++i)
                {
                    wchar_t c = paste[i];
                    if (c == L'\r')
                    {
                        if (i + 1 < paste.size() && paste[i + 1] == L'\n')
                            i++;
                        normalized.push_back(L'\n');
                    }
                    else
                    {
                        normalized.push_back(c);
                    }
                }
                paste.swap(normalized);
            }
            else
            {
                paste.erase(std::remove(paste.begin(), paste.end(), L'\n'), paste.end());
                paste.erase(std::remove(paste.begin(), paste.end(), L'\r'), paste.end());
            }
            
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
