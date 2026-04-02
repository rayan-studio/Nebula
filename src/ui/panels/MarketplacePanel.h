#pragma once
#include "Panel.h"
#include "LibraryDatabase.h"
#include "ui/components/input/TextInput.h"
#include <memory>
#include <vector>

// ============================================================================
// Marketplace Panel - Modern C++ Library Marketplace UI
// ============================================================================

struct LibraryCard {
    LibraryInfo library;
    D2D1_RECT_F bounds = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F iconRect = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F headerRect = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F ratingRect = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F descRect = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F tagsRect = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F installButtonBounds = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F uninstallButtonBounds = D2D1::RectF(0, 0, 0, 0);
    bool isHoveringCard = false;
    bool isHoveringInstallBtn = false;
    bool isHoveringUninstallBtn = false;
};

class MarketplacePanel : public Panel {
public:
    MarketplacePanel();

    // Panel interface
    void Initialize() override;
    void Draw(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite, HWND hwnd) override;
    void UpdateLayout(HWND hwnd) override;

    void OnMouseMove(HWND hwnd, POINT clientPoint) override;
    void OnLeftButtonDown(HWND hwnd, POINT clientPoint) override;
    void OnLeftButtonUp(HWND hwnd) override;
    void OnMouseWheel(HWND hwnd, int delta) override;
    void OnChar(wchar_t ch) override;
    void OnKeyDown(WPARAM key) override;

    bool HandleSearchChar(wchar_t ch);
    bool HandleSearchKeyDown(WPARAM key);
    bool IsSearchInputFocused() const;
    void UnfocusSearchInput();

private:
    // Drawing helpers
    void DrawSearchBar(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite);
    void DrawLibraryCard(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite,
                        LibraryCard& card);
    void DrawCardIcon(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite,
                     const D2D1_RECT_F& rect, const std::wstring& icon);
    void DrawCardHeader(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite,
                       const D2D1_RECT_F& rect, const LibraryInfo* lib);
    void DrawRatingStars(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite,
                        const D2D1_RECT_F& rect, float rating, int downloads);
    void DrawDescription(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite,
                        const D2D1_RECT_F& rect, const std::wstring& desc);
    void DrawCardStatus(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite,
                       const LibraryInfo& lib, float x, float y, float width);
    void DrawTags(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite,
                 const D2D1_RECT_F& rect, const LibraryInfo* lib);
    void DrawCardCategory(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite,
                         const std::wstring& category, D2D1_COLOR_F color,
                         float x, float y);
    void DrawInstallButton(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite,
                          const D2D1_RECT_F& bounds, bool hover,
                          const std::wstring& label = L"Install",
                          bool enabled = true);
    void DrawInstalledButton(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite,
                            const D2D1_RECT_F& bounds, bool hover);
    void DrawButton(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite,
                   const D2D1_RECT_F& bounds, const std::wstring& text,
                   bool isHovering, bool isActive);

    void UpdateCardLayout();
    void HandleInstallLibrary(HWND hwnd, LibraryCard* card);
    void HandleUninstallLibrary(HWND hwnd, LibraryCard* card);

    std::vector<LibraryCard> cards_;
    float scrollOffset_ = 0.0f;
    std::wstring searchQuery_;
    D2D1_RECT_F searchBarBounds_;
    D2D1_RECT_F allFilterBounds_;
    D2D1_RECT_F installedFilterBounds_;
    std::wstring lastKnownRootPath_;  // detect project change
    unsigned long long dataRevision_ = 0;
    bool isHoveringAllFilter_ = false;
    bool isHoveringInstalledFilter_ = false;
    bool showInstalledOnly_ = false;

    // Search input component
    TextInput searchInput_;

    // Stored hwnd for callbacks
    HWND hwnd_ = nullptr;

    // Modern design constants
    const float SEARCH_BAR_HEIGHT = 36.0f;
    const float CARD_HEIGHT = 220.0f;
    const float CARD_PADDING = 12.0f;
    const float ICON_SIZE = 48.0f;
    const float CARD_CORNER_RADIUS = 8.0f;
};
