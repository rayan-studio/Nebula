#pragma once
#include <string>
#include <vector>
#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>

namespace Orion
{
    struct SearchMatch
    {
        int line;
        int startColumn;
        int endColumn;
    };

    struct SearchOptions
    {
        bool caseSensitive = false;
        bool wholeWord = false;
        bool useRegex = false;
    };

    class SearchBox
    {
    public:
        SearchBox();
        ~SearchBox();

        void Show();
        void Hide();
        bool IsVisible() const { return visible_; }

        void Draw(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite);
        void UpdateLayout(float editorLeft, float editorTop, float editorWidth);

        // Input handling
        void OnChar(wchar_t ch);
        void OnKeyDown(WPARAM key);
        void OnLeftButtonDown(POINT pt);
        bool IsPointInSearchBox(POINT pt) const;
        void SetInputFocused(bool focused);
        bool IsInputFocused() const { return inputFocused_; }

        // Search functionality
        void SetSearchText(const std::wstring& text);
        std::wstring GetSearchText() const { return searchText_; }
        
        const std::vector<SearchMatch>& GetMatches() const { return matches_; }
        int GetCurrentMatchIndex() const { return currentMatchIndex_; }
        
        void FindNext();
        void FindPrevious();
        void ClearMatches();

        // Options
        SearchOptions GetOptions() const { return options_; }
        void ToggleCaseSensitive();
        void ToggleWholeWord();
        void ToggleRegex();

        // Called by editor to perform search
        void PerformSearch(const std::vector<std::wstring>& lines);

    private:
        bool visible_;
        std::wstring searchText_;
        
        // UI bounds
        D2D1_RECT_F boxRect_;
        D2D1_RECT_F inputRect_;
        D2D1_RECT_F closeButtonRect_;
        D2D1_RECT_F prevButtonRect_;
        D2D1_RECT_F nextButtonRect_;
        D2D1_RECT_F caseButtonRect_;
        D2D1_RECT_F wordButtonRect_;
        D2D1_RECT_F regexButtonRect_;
        
        // State
        bool inputFocused_;
        int caretPosition_;
        bool caretVisible_;
        DWORD lastBlinkTime_;
        
        // Search results
        std::vector<SearchMatch> matches_;
        int currentMatchIndex_;
        SearchOptions options_;
        
        // Hover states
        bool hoverClose_;
        bool hoverPrev_;
        bool hoverNext_;
        bool hoverCase_;
        bool hoverWord_;
        bool hoverRegex_;
        
        // Helper methods
        void DrawButton(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite,
                       const D2D1_RECT_F& rect, const wchar_t* icon,
                       bool active, bool hovered);
        void UpdateCaretBlink();
        bool MatchesWholeWord(const std::wstring& line, size_t pos, size_t len);
    };

} // namespace Orion
