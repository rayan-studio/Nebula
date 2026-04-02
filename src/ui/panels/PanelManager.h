#pragma once
#include "Panel.h"
#include <memory>
#include <vector>
#include <unordered_map>

// ============================================================================
// Panel Manager - Manages all panels and their lifecycle
// ============================================================================

class PanelManager {
public:
    static PanelManager& Instance();
    
    // Panel registration
    void RegisterPanel(std::unique_ptr<Panel> panel);
    
    // Get panel by ID
    Panel* GetPanel(PanelId id);
    template<typename T>
    T* GetPanelAs(PanelId id) {
        return dynamic_cast<T*>(GetPanel(id));
    }
    
    // Get all registered panels
    const std::vector<Panel*>& GetAllPanels() const { return panelList_; }
    
    // Get panels for sidebar (sorted by order)
    std::vector<Panel*> GetSidebarPanels() const;
    
    // Active panel management
    void SetActivePanel(PanelId id);
    Panel* GetActivePanel();
    PanelId GetActivePanelId() const;
    bool IsPanelActive(PanelId id) const;
    Panel* GetVisiblePanel(bool dockedRight) const;
    Panel* GetPanelAtPoint(POINT clientPoint, bool includeResizeZone = true) const;
    Panel* GetResizingPanel() const;
    
    // Render visible panels
    void DrawActivePanel(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite, HWND hwnd);
    
    // Update layout for all panels
    void UpdateLayout(HWND hwnd);
    
    // Event dispatch to active panel
    void OnMouseMove(HWND hwnd, POINT clientPoint);
    void OnLeftButtonDown(HWND hwnd, POINT clientPoint);
    void OnLeftButtonUp(HWND hwnd);
    void OnRightButtonUp(HWND hwnd, POINT clientPoint);
    void OnMouseWheel(HWND hwnd, int delta);
    void OnChar(wchar_t ch);
    void OnKeyDown(WPARAM key);
    
    // Check if any panel is resizing
    bool IsAnyPanelResizing() const;
    
    // Clear resize hover state on active panel
    void ClearResizeHover(HWND hwnd);
    
    // Get all panel configs for sidebar rendering
    std::vector<PanelConfig> GetPanelConfigs() const;

private:
    PanelManager() = default;
    ~PanelManager() = default;
    PanelManager(const PanelManager&) = delete;
    PanelManager& operator=(const PanelManager&) = delete;
    
    std::unordered_map<PanelId, std::unique_ptr<Panel>> panels_;
    std::vector<Panel*> panelList_;
    Panel* activePanel_ = nullptr;
};

inline PanelManager& GetPanelManager() { return PanelManager::Instance(); }
