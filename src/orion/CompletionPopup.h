#pragma once
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
        bool IsPointInPopup(POINT pt) const;

    private:
        bool visible_ = false;
        std::vector<std::wstring> items_;
        int selected_ = 0;
        D2D1_RECT_F rect_;
        float itemHeight_ = 20.0f;
        float maxWidth_ = 300.0f;
    };

} // namespace Orion
