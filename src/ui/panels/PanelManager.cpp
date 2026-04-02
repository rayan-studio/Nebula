#include "PanelManager.h"
#include "helpers/window_helpers.h"
#include <algorithm>

PanelManager& PanelManager::Instance()
{
    static PanelManager instance;
    return instance;
}

void PanelManager::RegisterPanel(std::unique_ptr<Panel> panel)
{
    if (!panel) return;
    
    PanelId id = panel->GetId();
    Panel* rawPtr = panel.get();

    rawPtr->SetActive(false);
    rawPtr->SetVisible(false);
    
    panels_[id] = std::move(panel);
    panelList_.push_back(rawPtr);
    
    rawPtr->Initialize();
    
    if (!activePanel_)
    {
        activePanel_ = rawPtr;
        rawPtr->SetActive(true);
        rawPtr->SetVisible(true);
    }
}

Panel* PanelManager::GetPanel(PanelId id)
{
    auto it = panels_.find(id);
    if (it != panels_.end())
        return it->second.get();
    return nullptr;
}

std::vector<Panel*> PanelManager::GetSidebarPanels() const
{
    std::vector<Panel*> result;
    
    for (Panel* p : panelList_)
    {
        if (p->GetConfig().showInSidebar)
            result.push_back(p);
    }
    
    std::sort(result.begin(), result.end(), [](Panel* a, Panel* b) {
        const auto& ca = a->GetConfig();
        const auto& cb = b->GetConfig();
        if (ca.pinToBottom != cb.pinToBottom)
            return !ca.pinToBottom;
        return ca.order < cb.order;
    });
    
    return result;
}

void PanelManager::SetActivePanel(PanelId id)
{
    Panel* panel = GetPanel(id);
    if (!panel) return;

    const bool dockedRight = panel->IsDockedRight();
    for (Panel* candidate : panelList_)
    {
        if (!candidate || candidate == panel)
            continue;

        if (candidate->IsDockedRight() == dockedRight)
        {
            candidate->SetActive(false);
            candidate->SetVisible(false);
        }
    }
    
    activePanel_ = panel;
    activePanel_->SetActive(true);
    activePanel_->SetVisible(true);
}

Panel* PanelManager::GetActivePanel()
{
    if (activePanel_ && activePanel_->IsVisible())
        return activePanel_;

    if (Panel* right = GetVisiblePanel(true))
        return right;
    if (Panel* left = GetVisiblePanel(false))
        return left;

    return activePanel_;
}

PanelId PanelManager::GetActivePanelId() const
{
    if (activePanel_ && activePanel_->IsVisible())
        return activePanel_->GetId();

    if (Panel* right = GetVisiblePanel(true))
        return right->GetId();
    if (Panel* left = GetVisiblePanel(false))
        return left->GetId();

    return PanelId::Explorer;
}

bool PanelManager::IsPanelActive(PanelId id) const
{
    Panel* panel = const_cast<PanelManager*>(this)->GetPanel(id);
    return panel && panel->IsVisible() && panel->IsActive();
}

Panel* PanelManager::GetVisiblePanel(bool dockedRight) const
{
    for (Panel* panel : panelList_)
    {
        if (panel && panel->IsVisible() && panel->IsDockedRight() == dockedRight)
            return panel;
    }
    return nullptr;
}

Panel* PanelManager::GetPanelAtPoint(POINT clientPoint, bool includeResizeZone) const
{
    if (includeResizeZone)
    {
        for (Panel* panel : panelList_)
        {
            if (panel && panel->IsVisible() && panel->IsPointInResizeZone(clientPoint))
                return panel;
        }
    }

    for (Panel* panel : panelList_)
    {
        if (panel && panel->IsVisible() && panel->IsPointInPanel(clientPoint))
            return panel;
    }

    return nullptr;
}

Panel* PanelManager::GetResizingPanel() const
{
    for (Panel* panel : panelList_)
    {
        if (panel && panel->IsVisible() && panel->IsResizing())
            return panel;
    }
    return nullptr;
}

void PanelManager::DrawActivePanel(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite, HWND hwnd)
{
    for (Panel* panel : panelList_)
    {
        if (panel && panel->IsVisible())
            panel->Draw(ctx, dwrite, hwnd);
    }
}

void PanelManager::UpdateLayout(HWND hwnd)
{
    for (Panel* p : panelList_)
        p->UpdateLayout(hwnd);
}

void PanelManager::OnMouseMove(HWND hwnd, POINT clientPoint)
{
    if (Panel* panel = GetResizingPanel())
    {
        panel->OnMouseMove(hwnd, clientPoint);
        return;
    }

    if (Panel* panel = GetPanelAtPoint(clientPoint))
    {
        activePanel_ = panel;
        panel->OnMouseMove(hwnd, clientPoint);
        return;
    }

    if (Panel* panel = GetActivePanel())
        panel->OnMouseMove(hwnd, clientPoint);
}

void PanelManager::OnLeftButtonDown(HWND hwnd, POINT clientPoint)
{
    if (Panel* panel = GetPanelAtPoint(clientPoint))
    {
        activePanel_ = panel;
        panel->SetActive(true);
        panel->OnLeftButtonDown(hwnd, clientPoint);
        return;
    }
}

void PanelManager::OnLeftButtonUp(HWND hwnd)
{
    if (Panel* panel = GetResizingPanel())
    {
        panel->OnLeftButtonUp(hwnd);
        return;
    }

    if (Panel* panel = GetActivePanel())
        panel->OnLeftButtonUp(hwnd);
}

void PanelManager::OnRightButtonUp(HWND hwnd, POINT clientPoint)
{
    if (Panel* panel = GetPanelAtPoint(clientPoint, false))
    {
        activePanel_ = panel;
        panel->OnRightButtonUp(hwnd, clientPoint);
    }
}

void PanelManager::OnMouseWheel(HWND hwnd, int delta)
{
    if (Panel* panel = GetActivePanel())
        panel->OnMouseWheel(hwnd, delta);
}

void PanelManager::OnChar(wchar_t ch)
{
    if (activePanel_ && activePanel_->IsVisible())
        activePanel_->OnChar(ch);
}

void PanelManager::OnKeyDown(WPARAM key)
{
    if (activePanel_ && activePanel_->IsVisible())
        activePanel_->OnKeyDown(key);
}

bool PanelManager::IsAnyPanelResizing() const
{
    for (Panel* p : panelList_)
    {
        if (p->IsResizing())
            return true;
    }
    return false;
}

void PanelManager::ClearResizeHover(HWND hwnd)
{
    for (Panel* panel : panelList_)
    {
        if (panel && panel->IsVisible())
            panel->ClearResizeHover(hwnd);
    }
}

std::vector<PanelConfig> PanelManager::GetPanelConfigs() const
{
    std::vector<PanelConfig> configs;
    for (Panel* p : panelList_)
    {
        if (p->GetConfig().showInSidebar)
            configs.push_back(p->GetConfig());
    }
    
    std::sort(configs.begin(), configs.end(), [](const PanelConfig& a, const PanelConfig& b) {
        if (a.pinToBottom != b.pinToBottom)
            return !a.pinToBottom;
        return a.order < b.order;
    });
    
    return configs;
}
