#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <functional>

// ============================================================================
// Panel System - Base abstract class for all sidebar panels
// ============================================================================

// Panel identifiers
enum class PanelId {
    Explorer,
    Search,
    Git,
    Claude,
    CodeMap,
    Settings,
    Marketplace,
    Count
};

// Configuration for a panel in the sidebar
struct PanelConfig {
    PanelId id;
    std::wstring icon;
    std::wstring tooltip;
    bool showInSidebar;
    bool pinToBottom;
    int order;
    
    PanelConfig() 
        : id(PanelId::Explorer)
        , icon(L"\uE8B7")
        , tooltip(L"Explorer")
        , showInSidebar(true)
        , pinToBottom(false)
        , order(0) {}
    
    PanelConfig(PanelId _id, const std::wstring& _icon, const std::wstring& _tooltip, 
                bool _show = true, bool _bottom = false, int _order = 0)
        : id(_id)
        , icon(_icon)
        , tooltip(_tooltip)
        , showInSidebar(_show)
        , pinToBottom(_bottom)
        , order(_order) {}
};

// Panel state
struct PanelState {
    // Dimensions
    int logicalWidth = 280;
    int physicalWidth = 280;
    int minWidth = 150;
    int maxWidth = 600;
    
    // Position and bounds
    float leftEdge = 0.0f;
    float rightEdge = 0.0f;
    float topEdge = 0.0f;
    float bottomEdge = 0.0f;
    
    // Resize state
    bool isResizing = false;
    bool isHoveringResizeZone = false;

    // Ancien (screen coords) - tu peux le garder si tu veux
    int resizeStartX = 0;

    // Nouveau (client coords) - requis par le Panel.cpp corrigé
    int resizeStartClientX = 0;

    int resizeStartWidth = 0;

    // Layout constants
    float titleHeight = 40.0f;
    float leftPadding = 12.0f;
    float topPadding = 6.0f;
};

// Base class for all panels
class Panel {
public:
    Panel(PanelId id);
    virtual ~Panel() = default;
    
    // Pure virtual methods that each panel must implement
    virtual void Initialize() = 0;
    virtual void Draw(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite, HWND hwnd) = 0;
    virtual void UpdateLayout(HWND hwnd) = 0;
    virtual void OnMouseMove(HWND hwnd, POINT clientPoint) = 0;
    virtual void OnLeftButtonDown(HWND hwnd, POINT clientPoint) = 0;
    virtual void OnLeftButtonUp(HWND hwnd) = 0;
    virtual void OnRightButtonUp(HWND /*hwnd*/, POINT /*clientPoint*/) {}
    virtual void OnMouseWheel(HWND /*hwnd*/, int /*delta*/) {}
    virtual void OnChar(wchar_t /*ch*/) {}
    virtual void OnKeyDown(WPARAM /*key*/) {}
    
    // Common panel interface
    PanelId GetId() const { return id_; }
    const PanelConfig& GetConfig() const { return config_; }
    PanelState& GetState() { return state_; }
    const PanelState& GetState() const { return state_; }
    
    // Visibility
    virtual bool IsVisible() const { return visible_; }
    virtual void SetVisible(bool v) { visible_ = v; }
    virtual void ToggleVisible() { visible_ = !visible_; }
    virtual bool IsDockedRight() const { return false; }
    
    // Active state
    bool IsActive() const { return active_; }
    void SetActive(bool a) { active_ = a; }
    
    // Title
    const std::wstring& GetTitle() const { return title_; }
    void SetTitle(const std::wstring& t) { title_ = t; }
    
    // Dimensions
    int GetLogicalWidth() const { return state_.logicalWidth; }
    int GetPhysicalWidth() const { return state_.physicalWidth; }
    float GetPhysicalRightEdge() const { return state_.rightEdge; }
    virtual void SetLogicalWidth(int width)
    {
        if (width < state_.minWidth)
            width = state_.minWidth;
        if (width > state_.maxWidth)
            width = state_.maxWidth;
        state_.logicalWidth = width;
    }
    
    // Hit testing and resize
    virtual bool IsPointInPanel(POINT clientPoint) const;
    virtual bool IsPointInResizeZone(POINT clientPoint) const;
    bool IsResizing() const { return state_.isResizing; }
    bool IsHoveringResizeZone() const { return state_.isHoveringResizeZone; }
    virtual void ClearResizeHover(HWND hwnd);

protected:
    // Returns true when the resize handle is on the left edge of the panel.
    virtual bool IsResizeHandleOnLeft() const { return false; }

    // Drawing helpers
    void DrawBackground(ID2D1RenderTarget* ctx);
    void DrawTitle(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite);
    void DrawRightBorder(ID2D1RenderTarget* ctx);
    void UpdateBaseLayout(HWND hwnd, float leftEdge);
    
    // Resize helpers - call these from derived class event handlers
    bool HandleResizeMouseMove(HWND hwnd, POINT clientPoint);
    bool HandleResizeLeftButtonDown(HWND hwnd, POINT clientPoint);
    bool HandleResizeLeftButtonUp(HWND hwnd);
    
    PanelId id_;
    PanelConfig config_;
    PanelState state_;
    std::wstring title_;
    bool visible_ = true;
    bool active_ = false;
    
    static const int RESIZE_ZONE_WIDTH = 6;
};
