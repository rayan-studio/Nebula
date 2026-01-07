#include "ExplorerPanel.h"
#include "core/explorer/Explorer.h"
#include "utils/logger/Logger.h"

// ============================================================================
// ExplorerPanel - Wrapper around existing ExplorerManager
// ============================================================================

ExplorerPanel::ExplorerPanel()
    : Panel(PanelId::Explorer)
{
    config_ = PanelConfig(
        PanelId::Explorer,
        L"\uE8B7",      // Folder icon
        L"Explorer",
        true,           // Show in sidebar
        false,          // Not pinned to bottom
        0               // Order (first)
    );
    title_ = L"EXPLORER";
}

void ExplorerPanel::Initialize()
{
    // ExplorerManager is already initialized via its singleton
}

void ExplorerPanel::Draw(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite, HWND hwnd)
{
    GetExplorerManager().Draw(ctx, dwrite, hwnd);
}

void ExplorerPanel::UpdateLayout(HWND hwnd)
{
    // If we're actively resizing, push our logicalWidth to ExplorerManager BEFORE it recalculates
    if (state_.isResizing)
    {
        GetExplorerManager().SetLogicalWidth(state_.logicalWidth);
    }
    
    GetExplorerManager().UpdateLayout(hwnd);
    
    // Sync ALL state from ExplorerManager so Panel resize logic works
    auto& expState = GetExplorerManager().GetState();
    visible_ = GetExplorerManager().IsVisible();
    
    // Only sync dimensions if not actively resizing (to preserve Panel's calculated width)
    if (!state_.isResizing)
    {
        state_.logicalWidth = expState.logicalWidth;
    }
    
    state_.physicalWidth = expState.physicalWidth;
    state_.minWidth = expState.minWidth;
    state_.maxWidth = expState.maxWidth;
    state_.leftEdge = expState.leftEdge;
    state_.rightEdge = expState.rightEdge;
    state_.topEdge = expState.topEdge;
    state_.bottomEdge = expState.bottomEdge;
    state_.titleHeight = expState.titleHeight;
    state_.topPadding = expState.topPadding;
}

void ExplorerPanel::OnMouseMove(HWND hwnd, POINT clientPoint)
{
    // Sync state from ExplorerManager (so resize zone check works)
    auto& expState = GetExplorerManager().GetState();
    visible_ = GetExplorerManager().IsVisible();
    
    // Don't overwrite logicalWidth if we're actively resizing!
    if (!state_.isResizing)
    {
        state_.logicalWidth = expState.logicalWidth;
        state_.physicalWidth = expState.physicalWidth;
        state_.rightEdge = expState.rightEdge;
    }
    state_.leftEdge = expState.leftEdge;
    state_.topEdge = expState.topEdge;
    state_.bottomEdge = expState.bottomEdge;
    state_.titleHeight = expState.titleHeight;
    state_.topPadding = expState.topPadding;
    state_.minWidth = expState.minWidth;
    state_.maxWidth = expState.maxWidth;
    
    // Handle resize (from Panel base)
    HandleResizeMouseMove(hwnd, clientPoint);
    
    // Sync resize state to ExplorerManager (for border highlight)
    expState.isHoveringResizeZone = state_.isHoveringResizeZone;
    expState.isResizing = state_.isResizing;
    
    // If actively resizing, sync the new width to ExplorerManager and update layout
    if (state_.isResizing)
    {
        GetExplorerManager().SetLogicalWidth(state_.logicalWidth);
        GetExplorerManager().UpdateLayout(hwnd);
        // Also update our local state from the new layout
        state_.physicalWidth = expState.physicalWidth;
        state_.rightEdge = expState.rightEdge;
        return;
    }
    
    // If hovering resize zone, don't call ExplorerManager (prevents cursor override)
    if (state_.isHoveringResizeZone)
    {
        return;
    }
    
    // Normal Explorer interaction
    GetExplorerManager().OnMouseMove(hwnd, clientPoint);
}

void ExplorerPanel::OnLeftButtonDown(HWND hwnd, POINT clientPoint)
{
    // Sync state from ExplorerManager first (so resize zone check works)
    auto& expState = GetExplorerManager().GetState();
    visible_ = GetExplorerManager().IsVisible();
    state_.logicalWidth = expState.logicalWidth;
    state_.physicalWidth = expState.physicalWidth;
    state_.leftEdge = expState.leftEdge;
    state_.rightEdge = expState.rightEdge;
    state_.topEdge = expState.topEdge;
    state_.bottomEdge = expState.bottomEdge;
    state_.titleHeight = expState.titleHeight;
    state_.topPadding = expState.topPadding;
    state_.minWidth = expState.minWidth;
    state_.maxWidth = expState.maxWidth;
    
    // Handle resize first (from Panel base) - same pattern as SearchPanel
    if (HandleResizeLeftButtonDown(hwnd, clientPoint))
    {
        // Sync resize state to ExplorerManager
        expState.isResizing = state_.isResizing;
        return;
    }
    
    GetExplorerManager().OnLeftButtonDown(hwnd, clientPoint);
}

void ExplorerPanel::OnLeftButtonUp(HWND hwnd)
{
    // Handle resize first (from Panel base)
    if (HandleResizeLeftButtonUp(hwnd))
    {
        // Sync resize state and width to ExplorerManager
        auto& expState = GetExplorerManager().GetState();
        expState.isResizing = state_.isResizing;
        GetExplorerManager().SetLogicalWidth(state_.logicalWidth);
        GetExplorerManager().UpdateLayout(hwnd);
        return;
    }
    
    GetExplorerManager().OnLeftButtonUp(hwnd);
}

void ExplorerPanel::OnRightButtonUp(HWND hwnd, POINT clientPoint)
{
    GetExplorerManager().OnRightButtonUp(hwnd, clientPoint);
}

void ExplorerPanel::OnMouseWheel(HWND hwnd, int delta)
{
    GetExplorerManager().OnMouseWheel(hwnd, delta);
}

void ExplorerPanel::OnChar(wchar_t ch)
{
    if (GetExplorerManager().IsInlineInputVisible()) {
        GetExplorerManager().OnCharInline(ch);
    } else if (GetExplorerManager().IsSearchMode()) {
        GetExplorerManager().OnCharSearch(ch);
    }
}

void ExplorerPanel::OnKeyDown(WPARAM key)
{
    if (GetExplorerManager().IsInlineInputVisible()) {
        GetExplorerManager().OnKeyDownInline(key);
    } else if (GetExplorerManager().IsSearchMode()) {
        GetExplorerManager().OnKeyDownSearch(key);
    }
}

void ExplorerPanel::SetVisible(bool visible)
{
    Panel::SetVisible(visible);
    GetExplorerManager().SetVisible(visible);
}

void ExplorerPanel::ToggleVisible()
{
    GetExplorerManager().ToggleVisible();
    Panel::SetVisible(GetExplorerManager().IsVisible());
}

bool ExplorerPanel::IsVisible() const
{
    return GetExplorerManager().IsVisible();
}

bool ExplorerPanel::IsPointInPanel(POINT clientPoint) const
{
    return GetExplorerManager().IsPointInExplorer(clientPoint);
}

bool ExplorerPanel::IsPointInResizeZone(POINT clientPoint) const
{
    // Use synchronized state from Panel base class
    if (!visible_ || state_.physicalWidth <= 0)
        return false;
    
    float rightEdge = state_.rightEdge;
    // Match base Panel resize hit area: start below the title area
    float contentTop = state_.topEdge + state_.titleHeight + state_.topPadding;
    
    return (clientPoint.x >= (int)(rightEdge - RESIZE_ZONE_WIDTH) &&
            clientPoint.x <= (int)(rightEdge + RESIZE_ZONE_WIDTH) &&
            clientPoint.y >= (int)contentTop &&
            clientPoint.y <= (int)state_.bottomEdge);
}

void ExplorerPanel::ClearResizeHover(HWND hwnd)
{
    // Clear Panel's resize hover state
    Panel::ClearResizeHover(hwnd);
    
    // Also sync to ExplorerManager so its border drawing is updated
    auto& expState = GetExplorerManager().GetState();
    expState.isHoveringResizeZone = false;
}
