#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <functional>
#include <vector>

// ============================================================================
// TextInput - Reusable text input component with selection support
// ============================================================================

class TextInput
{
public:
    TextInput();
    ~TextInput() = default;

    // Configuration
    void SetPlaceholder(const std::wstring &placeholder) { placeholder_ = placeholder; }
    void SetIcon(const std::wstring &icon) { icon_ = icon; }
    void SetIconFont(const std::wstring &fontName) { iconFont_ = fontName; }
    void SetText(const std::wstring &text);
    void SetFocused(bool focused);
    void SetRect(const D2D1_RECT_F &rect) { rect_ = rect; }

    // State getters
    const std::wstring &GetText() const { return text_; }
    bool IsFocused() const { return focused_; }
    bool HasSelection() const { return selectionStart_ != selectionEnd_; }
    D2D1_RECT_F GetRect() const { return rect_; }

    // Rendering
    void Draw(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite);
    // Internal drawing helpers
    void DrawSearchBoxStyle(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite);
    void DrawStandardStyle(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite);

    // Event handling - returns true if event was consumed
    bool OnMouseMove(HWND hwnd, POINT pt);
    bool OnLeftButtonDown(HWND hwnd, POINT pt);
    bool OnLeftButtonUp(HWND hwnd, POINT pt);
    bool OnChar(wchar_t ch);
    bool OnKeyDown(WPARAM key);

    // Hit testing
    bool HitTest(POINT pt) const;

    // Callbacks
    std::function<void(const std::wstring &)> onTextChanged;
    std::function<void()> onSubmit; // Called on Enter
    std::function<void()> onEscape; // Called on Escape

    // Style configuration
    struct Style
    {
        // Optional search-box outer style
        bool useSearchBoxStyle = false;
        float outerPadding = 8.0f;
        float outerVerticalPadding = 6.0f;
        D2D1_COLOR_F outerBoxColor = D2D1::ColorF(0.13f, 0.13f, 0.13f);
        D2D1_COLOR_F outerBorderColor = D2D1::ColorF(0.26f, 0.26f, 0.26f);
        D2D1_COLOR_F shadowColor = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.2f);
        float shadowOffset = 2.0f;
        float outerCornerRadius = 4.0f;
        D2D1_COLOR_F backgroundColor = D2D1::ColorF(0.10f, 0.10f, 0.10f);
        D2D1_COLOR_F borderColor = D2D1::ColorF(0.28f, 0.28f, 0.28f);
        D2D1_COLOR_F focusBorderColor = D2D1::ColorF(0.24f, 0.57f, 0.92f);
        D2D1_COLOR_F textColor = D2D1::ColorF(0.95f, 0.95f, 0.95f);
        D2D1_COLOR_F placeholderColor = D2D1::ColorF(0.58f, 0.58f, 0.58f);
        // editor.selectionBackground -> #3392ff44
        D2D1_COLOR_F selectionColor = D2D1::ColorF(0.2f, 0.572549f, 1.0f, 0.266667f);
        D2D1_COLOR_F cursorColor = D2D1::ColorF(0.95f, 0.95f, 0.95f);
        D2D1_COLOR_F iconColor = D2D1::ColorF(0.62f, 0.62f, 0.62f);
        const wchar_t *fontFamily = L"Segoe UI Variable Text";
        IDWriteFontCollection *fontCollection = nullptr; // non-owning
        float cornerRadius = 6.0f;
        float fontSize = 13.5f;
        float iconSize = 14.0f;
        float padding = 10.0f;
        float iconPadding = 28.0f; // Space for icon on left
        bool multiline = false;
    };

    Style &GetStyle() { return style_; }

private:
    // Text layout helpers
    int GetCharIndexAtPosition(IDWriteFactory *dwrite, float x, float y = 10.0f);
    float GetCharPosition(IDWriteFactory *dwrite, int index);
    void UpdateTextLayout(IDWriteFactory *dwrite);
    void DeleteSelection();
    void SelectAll();
    void CopyToClipboard();
    void PasteFromClipboard();
    void CutToClipboard();

    // State
    std::wstring text_;
    std::wstring placeholder_;
    std::wstring icon_;
    std::wstring iconFont_ = L"Segoe Fluent Icons";
    bool focused_ = false;

    // Cursor and selection
    int cursorPos_ = 0;
    int selectionStart_ = 0;
    int selectionEnd_ = 0;
    bool selecting_ = false;

    // Layout
    D2D1_RECT_F rect_ = {};
    float textOffsetX_ = 0.0f; // For scrolling long text

    // Style
    Style style_;

    // Cursor blink
    DWORD lastBlinkTime_ = 0;
    bool cursorVisible_ = true;

    // Non-owning pointer for hit-testing/caret metrics
    IDWriteFactory *lastDWrite_ = nullptr;
};
