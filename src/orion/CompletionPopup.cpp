#include "orion/CompletionPopup.h"
#include <algorithm>
#include <dwrite_1.h>

namespace Orion
{
    CompletionPopup::CompletionPopup() {}
    CompletionPopup::~CompletionPopup() {}

    void CompletionPopup::Show()
    {
        visible_ = true;
        selected_ = 0;
    }
    void CompletionPopup::Hide()
    {
        visible_ = false;
    }

    void CompletionPopup::SetItems(const std::vector<std::wstring>& items)
    {
        items_ = items;
        if (selected_ >= (int)items_.size()) selected_ = 0;
        if (!visible_ && !items_.empty()) Show();
        if (items_.empty()) Hide();
    }

    const std::wstring& CompletionPopup::GetSelectedItem() const
    {
        static std::wstring empty;
        if (items_.empty()) return empty;
        int idx = selected_;
        if (idx < 0) idx = 0;
        int maxIdx = (int)items_.size() - 1;
        if (idx > maxIdx) idx = maxIdx;
        return items_[idx];
    }

    void CompletionPopup::UpdateLayout(float x, float y, float maxWidth, float itemHeight)
    {
        maxWidth_ = maxWidth;
        itemHeight_ = itemHeight;
        float h = itemHeight_ * (std::min)((int)items_.size(), 10);
        rect_ = D2D1::RectF(x, y, x + maxWidth_, y + h);
    }

    void CompletionPopup::Draw(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite)
    {
        if (!visible_ || items_.empty()) return;

        ID2D1SolidColorBrush* bg = nullptr;
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.10f, 0.10f, 0.95f), &bg);
        if (bg)
        {
            ctx->FillRectangle(rect_, bg);
            bg->Release();
        }

        ID2D1SolidColorBrush* textBrush = nullptr;
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.9f, 0.9f, 0.9f), &textBrush);

        IDWriteTextFormat* tf = nullptr;
        if (dwrite)
        {
            dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &tf);
        }

        float y = rect_.top;
        int maxShown = (int)((rect_.bottom - rect_.top) / itemHeight_);
        for (int i = 0; i < (int)items_.size() && i < maxShown; ++i)
        {
            if (i == selected_)
            {
                ID2D1SolidColorBrush* sel = nullptr;
                ctx->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.4f, 0.8f, 0.9f), &sel);
                if (sel) { ctx->FillRectangle(D2D1::RectF(rect_.left, y, rect_.right, y + itemHeight_), sel); sel->Release(); }
            }

            if (tf && textBrush)
            {
                D2D1_RECT_F r = D2D1::RectF(rect_.left + 6.0f, y + 2.0f, rect_.right - 6.0f, y + itemHeight_ - 2.0f);
                ctx->DrawTextW(items_[i].c_str(), (UINT32)items_[i].size(), tf, r, textBrush);
            }

            y += itemHeight_;
        }

        if (textBrush) textBrush->Release();
        if (tf) tf->Release();
    }

    void CompletionPopup::OnKeyDown(WPARAM key)
    {
        if (!visible_) return;
        switch (key)
        {
        case VK_UP:
            if (selected_ > 0) selected_--;
            break;
        case VK_DOWN:
            if (selected_ < (int)items_.size() - 1) selected_++;
            break;
        case VK_RETURN:
            // leave acceptance to caller (they will query GetSelectedItem())
            Hide();
            break;
        case VK_ESCAPE:
            Hide();
            break;
        }
    }

    void CompletionPopup::OnLeftButtonDown(POINT pt)
    {
        if (!visible_) return;
        if (pt.x < (int)rect_.left || pt.x > (int)rect_.right || pt.y < (int)rect_.top || pt.y > (int)rect_.bottom)
            return;
        int idx = (int)((pt.y - rect_.top) / itemHeight_);
        if (idx >= 0 && idx < (int)items_.size())
        {
            selected_ = idx;
            Hide();
        }
    }

    bool CompletionPopup::IsPointInPopup(POINT pt) const
    {
        if (!visible_) return false;
        return pt.x >= rect_.left && pt.x <= rect_.right && pt.y >= rect_.top && pt.y <= rect_.bottom;
    }

} // namespace Orion
