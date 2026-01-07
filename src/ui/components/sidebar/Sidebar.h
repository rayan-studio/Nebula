#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <vector>
#include "ui/panels/Panel.h"

// ============================================================================
// Sidebar - Left navigation bar using PanelManager
// ============================================================================

// Sidebar item state for rendering
struct SidebarItemState {
    PanelId panelId;
    D2D1_RECT_F hitRect;      // Clickable area
    D2D1_RECT_F bgRect;       // Background rect (smaller, centered)
    bool isHovered = false;
    bool isActive = false;
};

class SidebarRenderer {
public:
    static SidebarRenderer& Instance();
    
    // Drawing
    void Draw(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite, HWND hwnd);
    
    // Event handling
    bool HandleLeftClick(HWND hwnd, POINT clientPoint);
    void UpdateHover(HWND hwnd, POINT clientPoint);
    
    // Get sidebar bounds
    int GetWidth() const { return logicalWidth_; }
    int GetPhysicalWidth(HWND hwnd) const;
    
    // Get hovered item (for cursor changes)
    int GetHoveredIndex() const { return hoveredIndex_; }

private:
    SidebarRenderer() = default;
    
    void UpdateItemRects(HWND hwnd);
    int HitTest(HWND hwnd, POINT clientPoint) const;
    
    // Layout
    int logicalWidth_ = 52;
    float itemSize_ = 40.0f;
    float iconSize_ = 20.0f;
    float bgSize_ = 32.0f;
    float spacing_ = 2.0f;
    float topPadding_ = 4.0f;
    float bottomPadding_ = 4.0f;
    
    // State
    int hoveredIndex_ = -1;
    std::vector<SidebarItemState> itemStates_;
};

// API functions - backward compatible
void DrawSidebarD2D(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite, HWND hwnd);
bool HandleSidebarLeftClick(HWND hwnd, POINT clientPoint);
void UpdateSidebarHover(HWND hwnd, POINT clientPoint);
