#pragma once
#include "Panel.h"
#include "PanelManager.h"
#include "explorer/ExplorerPanel.h"
#include "git/GitPanel.h"
#include "search/SearchPanel.h"
#include "settings/SettingsPanel.h"
#include "MarketplacePanel.h"

// ============================================================================
// Panel System Initialization
// ============================================================================

// Initialize all panels and register them with PanelManager
void InitializePanelSystem()
{
    auto& manager = GetPanelManager();
    
    // Register Explorer panel
    auto explorerPanel = std::make_unique<ExplorerPanel>();
    manager.RegisterPanel(std::move(explorerPanel));
    
    // Register Search panel
    auto searchPanel = std::make_unique<SearchPanel>();
    manager.RegisterPanel(std::move(searchPanel));

    // Register Git panel
    auto gitPanel = std::make_unique<GitPanel>();
    manager.RegisterPanel(std::move(gitPanel));

    // Register Settings panel
    auto settingsPanel = std::make_unique<SettingsPanel>();
    manager.RegisterPanel(std::move(settingsPanel));
    
    // Register Marketplace panel
    auto marketplacePanel = std::make_unique<MarketplacePanel>();
    manager.RegisterPanel(std::move(marketplacePanel));
    
    // Set Explorer as the default active panel
    manager.SetActivePanel(PanelId::Explorer);
}

// Convenience functions
inline ExplorerPanel* GetExplorerPanelPtr()
{
    return GetPanelManager().GetPanelAs<ExplorerPanel>(PanelId::Explorer);
}

inline SearchPanel* GetSearchPanelPtr()
{
    return GetPanelManager().GetPanelAs<SearchPanel>(PanelId::Search);
}

inline MarketplacePanel* GetMarketplacePanelPtr()
{
    return GetPanelManager().GetPanelAs<MarketplacePanel>(PanelId::Marketplace);
}
