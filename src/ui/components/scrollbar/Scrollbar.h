#pragma once
#include <windows.h>
#include <d2d1.h>

struct ScrollbarState
{
    float scrollOffset = 0.0f;   // Position du scroll (en pixels)
    float contentHeight = 0.0f;  // Hauteur totale du contenu
    float viewportHeight = 0.0f; // Hauteur visible
    float thumbHeight = 0.0f;    // Hauteur du thumb
    float thumbPosition = 0.0f;  // Position du thumb
    float trackLeft = 0.0f;
    float trackTop = 0.0f;
    float trackRight = 0.0f;
    float trackBottom = 0.0f;
    bool isDragging = false;
    int dragStartY = 0;
    float dragStartOffset = 0.0f;
    bool isHoveringThumb = false;
    bool isHoveringTrack = false;
    bool visible = false;

};

class Scrollbar
{
public:
    Scrollbar();

    void UpdateLayout(float left, float top, float width, float height, float contentHeight);
    void Draw(ID2D1RenderTarget *ctx);

    bool OnMouseMove(POINT pt);
    bool OnLeftButtonDown(POINT pt);
    bool OnLeftButtonUp();
    bool OnMouseWheel(int delta);

    float GetScrollOffset() const { return state_.scrollOffset; }
    bool IsVisible() const { return state_.visible; }
    bool IsDragging() const { return state_.isDragging; }

    void SetScrollOffset(float offset);
    void ScrollBy(float delta);
bool IsHoveringThumb() const { return state_.isHoveringThumb; }
    bool IsHoveringTrack() const { return state_.isHoveringTrack; }
private:
    void UpdateThumbGeometry();
    void ClampScrollOffset();
    bool IsPointInThumb(POINT pt) const;
    bool IsPointInTrack(POINT pt) const;

    ScrollbarState state_;

    static constexpr float SCROLLBAR_WIDTH = 14.0f;
    static constexpr float THUMB_MIN_HEIGHT = 30.0f;
    static constexpr float SCROLL_SPEED = 40.0f; // pixels per wheel notch
};