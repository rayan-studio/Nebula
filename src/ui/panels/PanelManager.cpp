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
    
    if (activePanel_ && activePanel_ != panel)
    {
        activePanel_->SetActive(false);
        activePanel_->SetVisible(false);
    }
    
    activePanel_ = panel;
    activePanel_->SetActive(true);
    activePanel_->SetVisible(true);
}

PanelId PanelManager::GetActivePanelId() const
{
    if (activePanel_)
        return activePanel_->GetId();
    return PanelId::Explorer;
}

bool PanelManager::IsPanelActive(PanelId id) const
{
    return activePanel_ && activePanel_->GetId() == id;
}

void PanelManager::DrawActivePanel(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite, HWND hwnd)
{
    if (activePanel_ && activePanel_->IsVisible())
        activePanel_->Draw(ctx, dwrite, hwnd);
}

void PanelManager::UpdateLayout(HWND hwnd)
{
    for (Panel* p : panelList_)
        p->UpdateLayout(hwnd);
}

void PanelManager::OnMouseMove(HWND hwnd, POINT clientPoint)
{
    if (activePanel_ && activePanel_->IsVisible())
        activePanel_->OnMouseMove(hwnd, clientPoint);
}

void PanelManager::OnLeftButtonDown(HWND hwnd, POINT clientPoint)
{
    // If click lands inside a panel that isn't active, activate it first
    for (Panel* p : panelList_)
    {
        if (p->IsVisible() && p->IsPointInPanel(clientPoint) && p != activePanel_)
        {
            if (activePanel_)
            {
                activePanel_->SetActive(false);
                activePanel_->SetVisible(false);
            }
            activePanel_ = p;
            activePanel_->SetActive(true);
            activePanel_->SetVisible(true);
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        }
    }

    if (activePanel_ && activePanel_->IsVisible())
        activePanel_->OnLeftButtonDown(hwnd, clientPoint);
}

void PanelManager::OnLeftButtonUp(HWND hwnd)
{
    if (activePanel_ && activePanel_->IsVisible())
        activePanel_->OnLeftButtonUp(hwnd);
}

void PanelManager::OnRightButtonUp(HWND hwnd, POINT clientPoint)
{
    if (activePanel_ && activePanel_->IsVisible())
        activePanel_->OnRightButtonUp(hwnd, clientPoint);
}

void PanelManager::OnMouseWheel(HWND hwnd, int delta)
{
    if (activePanel_ && activePanel_->IsVisible())
        activePanel_->OnMouseWheel(hwnd, delta);
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
    if (activePanel_ && activePanel_->IsVisible())
        activePanel_->ClearResizeHover(hwnd);
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
