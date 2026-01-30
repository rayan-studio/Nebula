#include "Popup.h"
#include <algorithm>
#include <dwrite_1.h>
#include <cmath>

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
        scrollIndex_ = 0;
        if (!items_.empty())
            selected_ = 0;
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
        int maxShown = (std::min)((int)items_.size(), 10);
        float h = itemHeight_ * maxShown;
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
        if (maxShown <= 0) return;

        // clamp scrollIndex_
        int total = (int)items_.size();
        if (scrollIndex_ < 0) scrollIndex_ = 0;
        if (scrollIndex_ > (total - maxShown)) scrollIndex_ = (total - maxShown) < 0 ? 0 : (total - maxShown);

        int visibleCount = (std::min)(total, maxShown);
        float contentRight = rect_.right;
        bool needScrollbar = total > visibleCount;
        if (needScrollbar)
            contentRight -= scrollbarWidth_ + 4.0f; // reserve space for scrollbar

        for (int i = 0; i < visibleCount; ++i)
        {
            int idx = scrollIndex_ + i;
            float itemTop = rect_.top + i * itemHeight_;
            if (idx == selected_)
            {
                ID2D1SolidColorBrush* sel = nullptr;
                ctx->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.4f, 0.8f, 0.9f), &sel);
                if (sel) { ctx->FillRectangle(D2D1::RectF(rect_.left, itemTop, rect_.right, itemTop + itemHeight_), sel); sel->Release(); }
            }

            if (tf && textBrush)
            {
                D2D1_RECT_F r = D2D1::RectF(rect_.left + 6.0f, itemTop + 2.0f, contentRight - 6.0f, itemTop + itemHeight_ - 2.0f);
                ctx->DrawTextW(items_[idx].c_str(), (UINT32)items_[idx].size(), tf, r, textBrush);
            }
        }

        // Draw scrollbar if needed
        if (needScrollbar)
        {
            float trackLeft = rect_.right - scrollbarWidth_ - 2.0f;
            float trackTop = rect_.top + 2.0f;
            float trackRight = rect_.right - 2.0f;
            float trackBottom = rect_.bottom - 2.0f;
            float trackHeight = trackBottom - trackTop;

            // Track
            ID2D1SolidColorBrush* trackBrush = nullptr;
            ctx->CreateSolidColorBrush(D2D1::ColorF(0x1f1f20, 1.0f), &trackBrush);
            if (trackBrush)
            {
                ctx->FillRectangle(D2D1::RectF(trackLeft, trackTop, trackRight, trackBottom), trackBrush);
                trackBrush->Release();
            }

            // Thumb size/position
            float thumbMin = 18.0f;
            float thumbH = (visibleCount / (float)total) * trackHeight;
            if (thumbH < thumbMin) thumbH = thumbMin;
            float available = trackHeight - thumbH;
            float thumbTopPos = trackTop;
            if (total > visibleCount)
            {
                float ratio = (float)scrollIndex_ / (float)(total - visibleCount);
                thumbTopPos = trackTop + ratio * available;
            }

            ID2D1SolidColorBrush* thumbBrush = nullptr;
            ctx->CreateSolidColorBrush(D2D1::ColorF(0.25f, 0.25f, 0.25f, 0.9f), &thumbBrush);
            if (thumbBrush)
            {
                ctx->FillRectangle(D2D1::RectF(trackLeft + 2.0f, thumbTopPos, trackRight - 2.0f, thumbTopPos + thumbH), thumbBrush);
                thumbBrush->Release();
            }
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

        int maxShown = (int)((rect_.bottom - rect_.top) / itemHeight_);
        if (maxShown <= 0) return;
        int total = (int)items_.size();
        int visibleCount = (std::min)(total, maxShown);
        if (selected_ < scrollIndex_)
            scrollIndex_ = selected_;
        else if (selected_ >= scrollIndex_ + visibleCount)
            scrollIndex_ = selected_ - visibleCount + 1;
        if (scrollIndex_ < 0) scrollIndex_ = 0;
        if (total > visibleCount && scrollIndex_ > total - visibleCount)
            scrollIndex_ = total - visibleCount;
    }

    void CompletionPopup::OnLeftButtonDown(POINT pt)
    {
        if (!visible_) return;
        if (pt.x < (int)rect_.left || pt.x > (int)rect_.right || pt.y < (int)rect_.top || pt.y > (int)rect_.bottom)
            return;

        int maxShown = (int)((rect_.bottom - rect_.top) / itemHeight_);
        if (maxShown <= 0) return;
        int total = (int)items_.size();
        int visibleCount = (std::min)(total, maxShown);

        // Check for clicks on scrollbar area
        if (total > visibleCount)
        {
            float trackLeft = rect_.right - scrollbarWidth_ - 2.0f;
            float trackTop = rect_.top + 2.0f;
            float trackRight = rect_.right - 2.0f;
            float trackBottom = rect_.bottom - 2.0f;
            float trackHeight = trackBottom - trackTop;
            float thumbMin = 18.0f;
            float thumbH = (visibleCount / (float)total) * trackHeight;
            if (thumbH < thumbMin) thumbH = thumbMin;
            float available = trackHeight - thumbH;
            float thumbTopPos = trackTop;
            if (total > visibleCount)
            {
                float ratio = (float)scrollIndex_ / (float)(total - visibleCount);
                thumbTopPos = trackTop + ratio * available;
            }

            if (pt.x >= (int)trackLeft && pt.x <= (int)trackRight && pt.y >= (int)trackTop && pt.y <= (int)trackBottom)
            {
                // Clicked on thumb?
                if (pt.y >= (int)thumbTopPos && pt.y <= (int)(thumbTopPos + thumbH))
                {
                    isDraggingThumb_ = true;
                    dragStartY_ = pt.y;
                    dragStartIndex_ = scrollIndex_;
                }
                else
                {
                    // Clicked on track: page up/down
                    if (pt.y < (int)thumbTopPos)
                        scrollIndex_ = (std::max)(0, scrollIndex_ - visibleCount);
                    else
                        scrollIndex_ = (std::min)(total - visibleCount, scrollIndex_ + visibleCount);
                }
                return;
            }
        }

        // Click on item area
        int idx = scrollIndex_ + (int)((pt.y - rect_.top) / itemHeight_);
        if (idx >= 0 && idx < total)
        {
            selected_ = idx;
            Hide();
        }
    }

    void CompletionPopup::OnLeftButtonUp()
    {
        isDraggingThumb_ = false;
    }

    void CompletionPopup::OnMouseMove(POINT pt)
    {
        if (!visible_) return;

        int maxShown = (int)((rect_.bottom - rect_.top) / itemHeight_);
        if (maxShown <= 0) return;
        int total = (int)items_.size();
        int visibleCount = (std::min)(total, maxShown);

        // If dragging scrollbar thumb, update scrollIndex_
        if (isDraggingThumb_ && total > visibleCount)
        {
            float trackTop = rect_.top + 2.0f;
            float trackBottom = rect_.bottom - 2.0f;
            float trackHeight = trackBottom - trackTop;
            float thumbMin = 18.0f;
            float thumbH = (visibleCount / (float)total) * trackHeight;
            if (thumbH < thumbMin) thumbH = thumbMin;
            float available = trackHeight - thumbH;
            if (available <= 0) return;
            float deltaY = (float)(pt.y - dragStartY_);
            float frac = deltaY / available;
            int maxIndex = total - visibleCount;
            int newIndex = dragStartIndex_ + (int)std::roundf(frac * maxIndex);
            if (newIndex < 0) newIndex = 0;
            if (newIndex > maxIndex) newIndex = maxIndex;
            scrollIndex_ = newIndex;
            return;
        }

        // Otherwise, hover to select item
        if (pt.x >= (int)rect_.left && pt.x <= (int)rect_.right && pt.y >= (int)rect_.top && pt.y <= (int)rect_.bottom)
        {
            int idx = scrollIndex_ + (int)((pt.y - rect_.top) / itemHeight_);
            if (idx >= 0 && idx < total)
                selected_ = idx;
        }
    }

    bool CompletionPopup::OnMouseWheel(int delta)
    {
        if (!visible_) return false;
        int steps = delta / 120;
        if (steps == 0) return false;
        int maxShown = (int)((rect_.bottom - rect_.top) / itemHeight_);
        if (maxShown <= 0) return false;
        int total = (int)items_.size();
        int visibleCount = (std::min)(total, maxShown);
        if (total <= visibleCount) return false;
        int old = scrollIndex_;
        // scroll 3 items per wheel step for smoother navigation
        scrollIndex_ -= steps * 3;
        if (scrollIndex_ < 0) scrollIndex_ = 0;
        if (scrollIndex_ > total - visibleCount) scrollIndex_ = total - visibleCount;
        return old != scrollIndex_;
    }

    bool CompletionPopup::IsPointInPopup(POINT pt) const
    {
        if (!visible_) return false;
        return pt.x >= rect_.left && pt.x <= rect_.right && pt.y >= rect_.top && pt.y <= rect_.bottom;
    }

} // namespace Orion
