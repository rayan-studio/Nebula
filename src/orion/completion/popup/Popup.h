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
        struct PopupItem
        {
            std::wstring label;
            std::wstring description; // right-side hint (e.g. "std", "project", "sdk")
        };

        CompletionPopup();
        ~CompletionPopup();

        void Show();
        void Hide();
        bool IsVisible() const { return visible_; }

        void SetItems(const std::vector<PopupItem>& items);
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
        std::vector<PopupItem> items_;
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

        // Cached D2D/DWrite resources — created once, reused every frame
        ID2D1SolidColorBrush* brShadow_    = nullptr;
        ID2D1SolidColorBrush* brBg_        = nullptr;
        ID2D1SolidColorBrush* brBorder_    = nullptr;
        ID2D1SolidColorBrush* brDivider_   = nullptr;
        ID2D1SolidColorBrush* brText_      = nullptr;
        ID2D1SolidColorBrush* brDim_       = nullptr;
        ID2D1SolidColorBrush* brDesc_      = nullptr;
        ID2D1SolidColorBrush* brIconStd_   = nullptr;
        ID2D1SolidColorBrush* brIconProj_  = nullptr;
        ID2D1SolidColorBrush* brIconDef_   = nullptr;
        ID2D1SolidColorBrush* brSelection_ = nullptr;
        ID2D1SolidColorBrush* brScrollbar_ = nullptr;
        IDWriteTextFormat*    tfMain_      = nullptr;
        IDWriteTextFormat*    tfDesc_      = nullptr;
        IDWriteTextFormat*    tfIcon_      = nullptr;

        void EnsureResources(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite);
        void ReleaseResources();
    };

} // namespace Orion
