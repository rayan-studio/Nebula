#include "Scrollbar.h"
#include <algorithm>
#include <cmath>

// Définir min/max macros si pas déjà fait
#undef min
#undef max

Scrollbar::Scrollbar() {
}

void Scrollbar::UpdateLayout(float left, float top, float width, float height, float contentHeight) {
    state_.viewportHeight = height;
    state_.contentHeight = contentHeight;
    
    state_.visible = contentHeight > height;
    
    if (!state_.visible) {
        state_.scrollOffset = 0.0f;
        return;
    }
    
    state_.trackLeft = left + width - SCROLLBAR_WIDTH;
    state_.trackTop = top;
    state_.trackRight = left + width;
    state_.trackBottom = top + height;
    
    UpdateThumbGeometry();
    ClampScrollOffset();
}

void Scrollbar::UpdateThumbGeometry() {
    if (!state_.visible) return;
    
    float ratio = state_.viewportHeight / state_.contentHeight;
    state_.thumbHeight = (std::max)(state_.viewportHeight * ratio, THUMB_MIN_HEIGHT);  // PARENTHÈSES autour de max
    
    float maxScroll = state_.contentHeight - state_.viewportHeight;
    float availableTrack = state_.viewportHeight - state_.thumbHeight;
    
    if (maxScroll > 0.0f) {
        state_.thumbPosition = (state_.scrollOffset / maxScroll) * availableTrack;
    } else {
        state_.thumbPosition = 0.0f;
    }
}

void Scrollbar::ClampScrollOffset() {
    float maxScroll = (std::max)(0.0f, state_.contentHeight - state_.viewportHeight);  // PARENTHÈSES
    // Utiliser min/max au lieu de clamp pour la compatibilité
    state_.scrollOffset = (std::max)(0.0f, (std::min)(state_.scrollOffset, maxScroll));
    UpdateThumbGeometry();
}

void Scrollbar::SetScrollOffset(float offset) {
    state_.scrollOffset = offset;
    ClampScrollOffset();
}

void Scrollbar::ScrollBy(float delta) {
    state_.scrollOffset += delta;
    ClampScrollOffset();
}

bool Scrollbar::OnMouseWheel(int delta) {
    if (!state_.visible) return false;
    
    // Vertical wheel mapping: positive delta -> scroll up (decrease offset)
    float scrollAmount = -(delta / 120.0f) * SCROLL_SPEED;
    ScrollBy(scrollAmount);
    return true;
}

bool Scrollbar::IsPointInThumb(POINT pt) const {
    if (!state_.visible) return false;
    
    float thumbTop = state_.trackTop + state_.thumbPosition;
    float thumbBottom = thumbTop + state_.thumbHeight;
    
    return pt.x >= state_.trackLeft && pt.x <= state_.trackRight &&
           pt.y >= thumbTop && pt.y <= thumbBottom;
}

bool Scrollbar::IsPointInTrack(POINT pt) const {
    if (!state_.visible) return false;
    
    return pt.x >= state_.trackLeft && pt.x <= state_.trackRight &&
           pt.y >= state_.trackTop && pt.y <= state_.trackBottom;
}

bool Scrollbar::OnMouseMove(POINT pt) {
    if (!state_.visible) return false;
    
    if (state_.isDragging) {
        int deltaY = pt.y - state_.dragStartY;
        
        float maxScroll = state_.contentHeight - state_.viewportHeight;
        float availableTrack = state_.viewportHeight - state_.thumbHeight;
        
        if (availableTrack > 0.0f) {
            float scrollDelta = (deltaY / availableTrack) * maxScroll;
            state_.scrollOffset = state_.dragStartOffset + scrollDelta;
            ClampScrollOffset();
        }
        
        return true;
    }
    
    bool wasHoveringThumb = state_.isHoveringThumb;
    bool wasHoveringTrack = state_.isHoveringTrack;
    
    state_.isHoveringThumb = IsPointInThumb(pt);
    state_.isHoveringTrack = IsPointInTrack(pt);
    
    return (state_.isHoveringThumb != wasHoveringThumb) || 
           (state_.isHoveringTrack != wasHoveringTrack);
}

bool Scrollbar::OnLeftButtonDown(POINT pt) {
    if (!state_.visible) return false;
    
    if (IsPointInThumb(pt)) {
        state_.isDragging = true;
        state_.dragStartY = pt.y;
        state_.dragStartOffset = state_.scrollOffset;
        return true;
    }
    
    if (IsPointInTrack(pt)) {
        float thumbTop = state_.trackTop + state_.thumbPosition;
        
        if (pt.y < thumbTop) {
            ScrollBy(-state_.viewportHeight * 0.8f);
        } else {
            ScrollBy(state_.viewportHeight * 0.8f);
        }
        return true;
    }
    
    return false;
}

bool Scrollbar::OnLeftButtonUp() {
    if (state_.isDragging) {
        state_.isDragging = false;
        return true;
    }
    return false;
}

void Scrollbar::Draw(ID2D1RenderTarget* ctx) {
    if (!state_.visible || !ctx) return;
    if (state_.thumbHeight <= 0.0f || state_.trackBottom <= state_.trackTop)
        return;
    
    // Pas de track visible - juste le thumb
    D2D1_COLOR_F thumbColor;
    
    if (state_.isDragging) {
        thumbColor = D2D1::ColorF(0.45f, 0.45f, 0.45f, 0.9f);  // Plus opaque quand on drag
    } else if (state_.isHoveringThumb) {
        thumbColor = D2D1::ColorF(0.35f, 0.35f, 0.35f, 0.7f);  // Moyennement visible au hover
    } else {
        thumbColor = D2D1::ColorF(0.25f, 0.25f, 0.25f, 0.4f);  // Subtil par défaut
    }
    
    ID2D1SolidColorBrush* thumbBrush = nullptr;
    HRESULT hr = ctx->CreateSolidColorBrush(thumbColor, &thumbBrush);
    if (FAILED(hr) || !thumbBrush)
        return;
    
    float thumbTop = state_.trackTop + state_.thumbPosition;
    float thumbBottom = thumbTop + state_.thumbHeight;
    if (thumbBottom <= thumbTop)
    {
        thumbBrush->Release();
        return;
    }
    
    // Thumb plus fin et arrondi
    D2D1_ROUNDED_RECT thumbRect = D2D1::RoundedRect(
        D2D1::RectF(
            state_.trackLeft + 4.0f,  // Plus centré
            thumbTop + 2.0f,
            state_.trackRight - 4.0f,
            thumbBottom - 2.0f
        ),
        3.0f,  // Coins plus arrondis
        3.0f
    );
    
    ctx->FillRoundedRectangle(thumbRect, thumbBrush);
    
    thumbBrush->Release();
}
