#pragma once
#include "ui/panels/Panel.h"

// ============================================================================
// Explorer Panel - Wrapper around existing ExplorerManager
// ============================================================================

class ExplorerPanel : public Panel
{
public:
    ExplorerPanel();
    ~ExplorerPanel() override = default;
    
    // Panel interface - all delegate to ExplorerManager
    void Initialize() override;
    void Draw(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite, HWND hwnd) override;
    void UpdateLayout(HWND hwnd) override;
    void OnMouseMove(HWND hwnd, POINT clientPoint) override;
    void OnLeftButtonDown(HWND hwnd, POINT clientPoint) override;
    void OnLeftButtonUp(HWND hwnd) override;
    void OnRightButtonUp(HWND hwnd, POINT clientPoint) override;
    void OnMouseWheel(HWND hwnd, int delta) override;
    void OnChar(wchar_t ch) override;
    void OnKeyDown(WPARAM key) override;
    
    // Visibility synced with ExplorerManager
    void SetVisible(bool visible) override;
    void ToggleVisible() override;
    bool IsVisible() const override;
    
    // Delegate to ExplorerManager
    bool IsPointInPanel(POINT clientPoint) const override;
    bool IsPointInResizeZone(POINT clientPoint) const override;
    void ClearResizeHover(HWND hwnd) override;
};
