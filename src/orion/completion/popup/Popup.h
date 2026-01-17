#pragma once
#include <Windows.h>
#include <vector>
#include <string>
#include <d2d1.h>
#include <dwrite.h>

namespace Orion
{
    class CompletionPopup
    {
    public:
        CompletionPopup();
        ~CompletionPopup();

        void Show();
        void Hide();
        bool IsVisible() const { return visible_; }

        void SetItems(const std::vector<std::wstring>& items);
        const std::wstring& GetSelectedItem() const;

        void UpdateLayout(float x, float y, float maxWidth, float itemHeight);
        void Draw(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite);

        // Input
        void OnKeyDown(WPARAM key);
        void OnLeftButtonDown(POINT pt);
        // Mouse interaction for popup
        void OnMouseMove(POINT pt);
        bool OnMouseWheel(int delta);
        void OnLeftButtonUp();
        bool IsPointInPopup(POINT pt) const;

    private:
        bool visible_ = false;
        std::vector<std::wstring> items_;
        int selected_ = 0;
        D2D1_RECT_F rect_;
        float itemHeight_ = 20.0f;
        float maxWidth_ = 300.0f;
        // Scrolling state when more items than visible
        int scrollIndex_ = 0;
        float scrollbarWidth_ = 12.0f;
        bool isDraggingThumb_ = false;
        int dragStartY_ = 0;
        int dragStartIndex_ = 0;
    };

} // namespace Orion
