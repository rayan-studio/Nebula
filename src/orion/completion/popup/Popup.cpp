#include "Popup.h"
#include <algorithm>
#include <dwrite_1.h>
#include <cmath>

namespace Orion
{
    CompletionPopup::CompletionPopup() {}
    CompletionPopup::~CompletionPopup() { ReleaseResources(); }

    void CompletionPopup::ReleaseResources()
    {
        auto safeRelease = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
        safeRelease(brShadow_);   safeRelease(brBg_);       safeRelease(brBorder_);
        safeRelease(brDivider_);  safeRelease(brText_);     safeRelease(brDim_);
        safeRelease(brDesc_);     safeRelease(brIconStd_);  safeRelease(brIconProj_);
        safeRelease(brIconDef_);  safeRelease(brSelection_);safeRelease(brScrollbar_);
        safeRelease(tfMain_);     safeRelease(tfDesc_);     safeRelease(tfIcon_);
    }

    void CompletionPopup::EnsureResources(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite)
    {
        // Brushes
        if (!brShadow_)    ctx->CreateSolidColorBrush(D2D1::ColorF(0.0f,  0.0f,  0.0f,  0.25f), &brShadow_);
        if (!brBg_)        ctx->CreateSolidColorBrush(D2D1::ColorF(0.095f,0.100f,0.112f,0.98f),  &brBg_);
        if (!brBorder_)    ctx->CreateSolidColorBrush(D2D1::ColorF(0.28f, 0.32f, 0.40f, 0.75f),  &brBorder_);
        if (!brDivider_)   ctx->CreateSolidColorBrush(D2D1::ColorF(0.22f, 0.24f, 0.28f, 0.50f),  &brDivider_);
        if (!brText_)      ctx->CreateSolidColorBrush(D2D1::ColorF(0.92f, 0.93f, 0.95f, 1.0f),   &brText_);
        if (!brDim_)       ctx->CreateSolidColorBrush(D2D1::ColorF(0.72f, 0.74f, 0.78f, 1.0f),   &brDim_);
        if (!brDesc_)      ctx->CreateSolidColorBrush(D2D1::ColorF(0.50f, 0.53f, 0.58f, 1.0f),   &brDesc_);
        if (!brIconStd_)   ctx->CreateSolidColorBrush(D2D1::ColorF(0.36f, 0.63f, 0.90f, 0.90f),  &brIconStd_);
        if (!brIconProj_)  ctx->CreateSolidColorBrush(D2D1::ColorF(0.50f, 0.80f, 0.55f, 0.90f),  &brIconProj_);
        if (!brIconDef_)   ctx->CreateSolidColorBrush(D2D1::ColorF(0.70f, 0.55f, 0.85f, 0.90f),  &brIconDef_);
        if (!brSelection_) ctx->CreateSolidColorBrush(D2D1::ColorF(0.18f, 0.27f, 0.42f, 0.95f),  &brSelection_);
        if (!brScrollbar_) ctx->CreateSolidColorBrush(D2D1::ColorF(0.32f, 0.34f, 0.38f, 0.85f),  &brScrollbar_);

        // Text formats
        if (dwrite && !tfMain_)
        {
            dwrite->CreateTextFormat(L"JetBrains Mono", nullptr,
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 12.5f, L"en-us", &tfMain_);
            if (tfMain_) {
                tfMain_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                tfMain_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                tfMain_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            }
        }
        if (dwrite && !tfDesc_)
        {
            dwrite->CreateTextFormat(L"Segoe UI", nullptr,
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 10.5f, L"en-us", &tfDesc_);
            if (tfDesc_) {
                tfDesc_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                tfDesc_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                tfDesc_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            }
        }
        if (dwrite && !tfIcon_)
        {
            dwrite->CreateTextFormat(L"JetBrains Mono", nullptr,
                DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"en-us", &tfIcon_);
            if (tfIcon_) {
                tfIcon_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                tfIcon_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                tfIcon_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            }
        }
    }

    void CompletionPopup::Show()
    {
        visible_ = true;
        selected_ = 0;
    }
    void CompletionPopup::Hide()
    {
        visible_ = false;
    }

    void CompletionPopup::SetItems(const std::vector<PopupItem>& items)
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
        return items_[idx].label;
    }

    void CompletionPopup::UpdateLayout(float x, float y, float maxWidth, float itemHeight)
    {
        maxWidth_ = maxWidth + 120.0f;
        itemHeight_ = itemHeight;
        int maxShown = (std::min)((int)items_.size(), 10);
        float h = itemHeight_ * maxShown;
        rect_ = D2D1::RectF(x, y, x + maxWidth_, y + h);
    }

    void CompletionPopup::Draw(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite)
    {
        if (!visible_ || items_.empty()) return;

        EnsureResources(ctx, dwrite);

        const float radius      = 6.0f;
        const float borderThick = 1.0f;
        const float iconAreaW   = 28.0f;
        const float padX        = 8.0f;
        const float descMaxW    = 70.0f;

        // ---- Shadow ----
        if (brShadow_)
        {
            D2D1_ROUNDED_RECT sr = D2D1::RoundedRect(
                D2D1::RectF(rect_.left + 2.0f, rect_.top + 3.0f, rect_.right + 2.0f, rect_.bottom + 3.0f),
                radius, radius);
            ctx->FillRoundedRectangle(sr, brShadow_);
        }

        // ---- Background ----
        if (brBg_)
            ctx->FillRoundedRectangle(D2D1::RoundedRect(rect_, radius, radius), brBg_);

        // ---- Border ----
        if (brBorder_)
        {
            D2D1_ANTIALIAS_MODE prev = ctx->GetAntialiasMode();
            ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            ctx->DrawRoundedRectangle(D2D1::RoundedRect(rect_, radius, radius), brBorder_, borderThick);
            ctx->SetAntialiasMode(prev);
        }

        // ---- Left icon divider line ----
        if (brDivider_)
        {
            float lx = rect_.left + iconAreaW;
            ctx->DrawLine(D2D1::Point2F(lx, rect_.top + 6.0f),
                          D2D1::Point2F(lx, rect_.bottom - 6.0f), brDivider_, 1.0f);
        }

        // ---- Scroll state ----
        int total    = (int)items_.size();
        int maxShown = (int)((rect_.bottom - rect_.top) / itemHeight_);
        if (maxShown <= 0) return;
        if (scrollIndex_ < 0) scrollIndex_ = 0;
        if (scrollIndex_ > total - maxShown) scrollIndex_ = (total - maxShown) < 0 ? 0 : (total - maxShown);

        int   visibleCount  = (std::min)(total, maxShown);
        bool  needScrollbar = total > visibleCount;
        float contentRight  = rect_.right - (needScrollbar ? scrollbarWidth_ + 6.0f : 4.0f);

        ctx->PushAxisAlignedClip(rect_, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        for (int i = 0; i < visibleCount; ++i)
        {
            int   idx     = scrollIndex_ + i;
            float itemTop = rect_.top + i * itemHeight_;
            bool  isSel   = (idx == selected_);

            // Selection highlight
            if (isSel && brSelection_)
            {
                D2D1_RECT_F sr = D2D1::RectF(rect_.left + 2.0f, itemTop + 1.5f,
                                              contentRight - 1.0f, itemTop + itemHeight_ - 1.5f);
                ctx->FillRoundedRectangle(D2D1::RoundedRect(sr, 4.0f, 4.0f), brSelection_);
            }

            const auto& item = items_[idx];

            // ---- Icon bracket (left column) ----
            if (tfIcon_ && dwrite)
            {
                bool isStd  = item.description == L"std" || item.description == L"sdk";
                bool isProj = item.description == L"project";
                ID2D1SolidColorBrush* iconBr = isStd ? brIconStd_ : (isProj ? brIconProj_ : brIconDef_);
                std::wstring iconSym = isStd ? L"<>" : (isProj ? L"\"\"" : L"#");

                D2D1_RECT_F ir = D2D1::RectF(rect_.left + 2.0f, itemTop,
                                              rect_.left + iconAreaW - 2.0f, itemTop + itemHeight_);
                if (iconBr)
                {
                    IDWriteTextLayout* iconLay = nullptr;
                    dwrite->CreateTextLayout(iconSym.c_str(), (UINT32)iconSym.size(),
                        tfIcon_, ir.right - ir.left, ir.bottom - ir.top, &iconLay);
                    if (iconLay)
                    {
                        iconLay->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                        ctx->DrawTextLayout(D2D1::Point2F(ir.left, ir.top), iconLay, iconBr);
                        iconLay->Release();
                    }
                }
            }

            if (tfMain_ && dwrite)
            {
                // ---- Label (center) ----
                float labelLeft  = rect_.left + iconAreaW + padX;
                float labelRight = contentRight - (item.description.empty() ? padX : descMaxW + padX);
                D2D1_RECT_F lr = D2D1::RectF(labelLeft, itemTop, labelRight, itemTop + itemHeight_);

                IDWriteTextLayout* lay = nullptr;
                dwrite->CreateTextLayout(item.label.c_str(), (UINT32)item.label.size(),
                    tfMain_, lr.right - lr.left, lr.bottom - lr.top, &lay);
                if (lay)
                {
                    DWRITE_TRIMMING trim = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
                    IDWriteInlineObject* ell = nullptr;
                    dwrite->CreateEllipsisTrimmingSign(tfMain_, &ell);
                    lay->SetTrimming(&trim, ell);
                    if (ell) ell->Release();

                    ctx->DrawTextLayout(D2D1::Point2F(lr.left, lr.top), lay,
                                        isSel ? brText_ : brDim_);
                    lay->Release();
                }

                // ---- Description right-side hint ----
                if (!item.description.empty() && tfDesc_ && brDesc_)
                {
                    D2D1_RECT_F dr = D2D1::RectF(contentRight - descMaxW - padX, itemTop,
                                                   contentRight - padX,            itemTop + itemHeight_);
                    IDWriteTextLayout* dlay = nullptr;
                    dwrite->CreateTextLayout(item.description.c_str(), (UINT32)item.description.size(),
                        tfDesc_, dr.right - dr.left, dr.bottom - dr.top, &dlay);
                    if (dlay)
                    {
                        dlay->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                        ctx->DrawTextLayout(D2D1::Point2F(dr.left, dr.top), dlay, brDesc_);
                        dlay->Release();
                    }
                }
            }
        }

        ctx->PopAxisAlignedClip();

        // ---- Scrollbar ----
        if (needScrollbar && brScrollbar_)
        {
            float trackLeft   = rect_.right - scrollbarWidth_ - 3.0f;
            float trackTop    = rect_.top    + 4.0f;
            float trackRight  = rect_.right  - 3.0f;
            float trackBottom = rect_.bottom - 4.0f;
            float trackH      = trackBottom - trackTop;

            float thumbMin = 16.0f;
            float thumbH   = (visibleCount / (float)total) * trackH;
            if (thumbH < thumbMin) thumbH = thumbMin;
            float avail  = trackH - thumbH;
            float thumbY = trackTop;
            if (total > visibleCount)
                thumbY = trackTop + ((float)scrollIndex_ / (float)(total - visibleCount)) * avail;

            ctx->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(trackLeft + 2.0f, thumbY,
                                               trackRight - 2.0f, thumbY + thumbH), 3.0f, 3.0f),
                brScrollbar_);
        }
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

        // Click on item area — just select (caller reads GetSelectedItem on accept)
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
            int newIndex = dragStartIndex_ + static_cast<int>(std::roundf(frac * maxIndex));
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
